/**
 * MediaSession — TDD layer M2.c.
 *
 * The WebRTC half of a call: it wraps a `react-native-webrtc`
 * `RTCPeerConnection` — acquires the mic, produces the local SDP offer
 * (Opus-first), applies the remote answer, shuttles ICE candidates, and
 * tears everything down on hang-up.
 *
 * The peer connection and the mic are both injected, so the offer /
 * answer / ICE / teardown flow is unit-testable with a mock — no real
 * WebRTC engine, no device.
 *
 * The sip.js `SessionDescriptionHandler` that the `UserAgent` calls
 * delegates to this class — that thin adapter is wired in the sip.js
 * integration slice (which needs a real sip.js install to verify).
 */
import {preferOpus} from './sdp';

export interface SdpDescription {
  type: 'offer' | 'answer';
  sdp: string;
}

export interface MediaTrack {
  stop(): void;
}

export interface MediaStreamLike {
  getTracks(): MediaTrack[];
}

/** The slice of `RTCPeerConnection` MediaSession depends on. */
export interface PeerConnection {
  addTrack(track: MediaTrack, stream: MediaStreamLike): void;
  createOffer(options?: object): Promise<SdpDescription>;
  setLocalDescription(desc: SdpDescription): Promise<void>;
  setRemoteDescription(desc: SdpDescription): Promise<void>;
  addIceCandidate(candidate: object): Promise<void>;
  close(): void;
  onicecandidate: ((event: {candidate: object | null}) => void) | null;
  ontrack: ((event: {streams: MediaStreamLike[]}) => void) | null;
}

export interface MediaSessionDeps {
  /** Create a fresh peer connection (production: `new RTCPeerConnection`). */
  createPeerConnection: () => PeerConnection;
  /** Acquire the microphone (production: `mediaDevices.getUserMedia`). */
  getMicStream: () => Promise<MediaStreamLike>;
}

export class MediaSession {
  private pc: PeerConnection | null = null;
  private localStream: MediaStreamLike | null = null;
  private iceHandler: ((candidate: object) => void) | null = null;
  private remoteStreamHandler: ((stream: MediaStreamLike) => void) | null =
    null;

  constructor(private readonly deps: MediaSessionDeps) {}

  /** Local ICE candidates — the SIP layer forwards these to the peer. */
  onIceCandidate(cb: (candidate: object) => void): void {
    this.iceHandler = cb;
  }

  /** The remote audio stream, once it arrives. */
  onRemoteStream(cb: (stream: MediaStreamLike) => void): void {
    this.remoteStreamHandler = cb;
  }

  /**
   * Acquire the mic, build the peer connection, and return the local
   * SDP offer — Opus-first, since the bridge cannot transcode Opus.
   */
  async createOffer(): Promise<string> {
    const pc = this.deps.createPeerConnection();
    this.pc = pc;

    pc.onicecandidate = event => {
      if (event.candidate) this.iceHandler?.(event.candidate);
    };
    pc.ontrack = event => {
      const stream = event.streams[0];
      if (stream) this.remoteStreamHandler?.(stream);
    };

    const stream = await this.deps.getMicStream();
    this.localStream = stream;
    for (const track of stream.getTracks()) {
      pc.addTrack(track, stream);
    }

    const offer = await pc.createOffer();
    const local: SdpDescription = {type: 'offer', sdp: preferOpus(offer.sdp)};
    await pc.setLocalDescription(local);
    return local.sdp;
  }

  /** Apply the callee's SDP answer to complete negotiation. */
  async acceptAnswer(sdp: string): Promise<void> {
    if (!this.pc) {
      throw new Error('MediaSession.acceptAnswer: no offer created yet');
    }
    await this.pc.setRemoteDescription({type: 'answer', sdp});
  }

  /** Feed a remote ICE candidate into the peer connection. */
  async addRemoteIceCandidate(candidate: object): Promise<void> {
    if (!this.pc) return;
    await this.pc.addIceCandidate(candidate);
  }

  /** Tear down — close the peer connection and release the microphone. */
  close(): void {
    if (this.localStream) {
      for (const track of this.localStream.getTracks()) {
        track.stop();
      }
      this.localStream = null;
    }
    if (this.pc) {
      this.pc.close();
      this.pc = null;
    }
  }
}
