/**
 * TDD layer M2.c — MediaSession.
 *
 * The peer connection and the mic are mocked, so offer / answer / ICE /
 * teardown are exercised with no real WebRTC engine.
 */
import {
  MediaSession,
  MediaStreamLike,
  MediaTrack,
  PeerConnection,
  SdpDescription,
} from '../mediaSession';

class FakeTrack implements MediaTrack {
  stopped = false;
  stop(): void {
    this.stopped = true;
  }
}

class FakeStream implements MediaStreamLike {
  constructor(readonly tracks: FakeTrack[]) {}
  getTracks(): MediaTrack[] {
    return this.tracks;
  }
}

// An SDP whose audio m-line offers Opus (111) LAST — so the test can
// confirm MediaSession ran preferOpus before setLocalDescription.
const RAW_OFFER = [
  'v=0',
  'm=audio 9 UDP/TLS/RTP/SAVPF 0 8 111',
  'a=rtpmap:0 PCMU/8000',
  'a=rtpmap:111 opus/48000/2',
].join('\r\n');

class FakePeerConnection implements PeerConnection {
  addedTracks: MediaTrack[] = [];
  createOfferCalls = 0;
  localDesc: SdpDescription | null = null;
  remoteDesc: SdpDescription | null = null;
  iceAdded: object[] = [];
  closed = false;
  onicecandidate: ((event: {candidate: object | null}) => void) | null = null;
  ontrack: ((event: {streams: MediaStreamLike[]}) => void) | null = null;

  addTrack(track: MediaTrack): void {
    this.addedTracks.push(track);
  }
  async createOffer(): Promise<SdpDescription> {
    this.createOfferCalls += 1;
    return {type: 'offer', sdp: RAW_OFFER};
  }
  async setLocalDescription(desc: SdpDescription): Promise<void> {
    this.localDesc = desc;
  }
  async setRemoteDescription(desc: SdpDescription): Promise<void> {
    this.remoteDesc = desc;
  }
  async addIceCandidate(candidate: object): Promise<void> {
    this.iceAdded.push(candidate);
  }
  close(): void {
    this.closed = true;
  }
  fireIce(candidate: object | null): void {
    this.onicecandidate?.({candidate});
  }
  fireTrack(stream: MediaStreamLike): void {
    this.ontrack?.({streams: [stream]});
  }
}

function setup() {
  const pc = new FakePeerConnection();
  const micTracks = [new FakeTrack(), new FakeTrack()];
  const mic = new FakeStream(micTracks);
  const session = new MediaSession({
    createPeerConnection: () => pc,
    getMicStream: async () => mic,
  });
  return {session, pc, mic, micTracks};
}

describe('MediaSession.createOffer', () => {
  it('adds the mic tracks and produces a local offer', async () => {
    const {session, pc, micTracks} = setup();
    const sdp = await session.createOffer();
    expect(pc.createOfferCalls).toBe(1);
    expect(pc.addedTracks).toEqual(micTracks);
    expect(pc.localDesc?.type).toBe('offer');
    expect(typeof sdp).toBe('string');
  });

  it('offers Opus first (preferOpus applied before setLocalDescription)', async () => {
    const {session, pc} = setup();
    const sdp = await session.createOffer();
    const mline = sdp.split('\r\n').find(l => l.startsWith('m=audio'));
    expect(mline).toBe('m=audio 9 UDP/TLS/RTP/SAVPF 111 0 8');
    expect(pc.localDesc?.sdp).toBe(sdp);
  });
});

describe('MediaSession.acceptAnswer', () => {
  it('applies the remote answer to the peer connection', async () => {
    const {session, pc} = setup();
    await session.createOffer();
    await session.acceptAnswer('answer-sdp');
    expect(pc.remoteDesc).toEqual({type: 'answer', sdp: 'answer-sdp'});
  });

  it('throws if no offer has been created yet', async () => {
    const {session} = setup();
    await expect(session.acceptAnswer('answer-sdp')).rejects.toThrow(
      /no offer/i,
    );
  });
});

describe('MediaSession ICE + remote stream', () => {
  it('forwards local ICE candidates to the handler (skipping the null end marker)', async () => {
    const {session, pc} = setup();
    const seen: object[] = [];
    session.onIceCandidate(c => seen.push(c));
    await session.createOffer();
    pc.fireIce({candidate: 'a=candidate:1 ...'});
    pc.fireIce(null); // end-of-candidates — must not be forwarded
    expect(seen).toEqual([{candidate: 'a=candidate:1 ...'}]);
  });

  it('feeds remote ICE candidates into the peer connection', async () => {
    const {session, pc} = setup();
    await session.createOffer();
    await session.addRemoteIceCandidate({candidate: 'remote-1'});
    expect(pc.iceAdded).toEqual([{candidate: 'remote-1'}]);
  });

  it('surfaces the remote stream', async () => {
    const {session, pc} = setup();
    let remote: MediaStreamLike | null = null;
    session.onRemoteStream(s => {
      remote = s;
    });
    await session.createOffer();
    const stream = new FakeStream([new FakeTrack()]);
    pc.fireTrack(stream);
    expect(remote).toBe(stream);
  });
});

describe('MediaSession.close', () => {
  it('closes the peer connection and stops the mic tracks', async () => {
    const {session, pc, micTracks} = setup();
    await session.createOffer();
    session.close();
    expect(pc.closed).toBe(true);
    expect(micTracks.every(t => t.stopped)).toBe(true);
  });

  it('is safe to call before any offer', () => {
    const {session, pc} = setup();
    expect(() => session.close()).not.toThrow();
    expect(pc.closed).toBe(false); // no peer connection was created
  });
});
