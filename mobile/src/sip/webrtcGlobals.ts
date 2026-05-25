/**
 * Install `react-native-webrtc`'s WebRTC primitives as globals so
 * sip.js's bundled platform/web `SessionDescriptionHandler` can find
 * them.
 *
 * sip.js's default SDH calls `new RTCPeerConnection(...)`,
 * `new RTCSessionDescription(...)`, `new MediaStream()`, and
 * `navigator.mediaDevices.getUserMedia(...)` as if running in a
 * browser. In RN those types are imported, not global — this shim
 * bridges the two so we can reuse sip.js's working SDH verbatim
 * instead of writing a custom one.
 *
 * Side-effect import. Must run once at app startup, BEFORE any
 * sip.js module is imported / executed:
 *
 *   // mobile/index.js (RN entry point)
 *   import './src/sip/webrtcGlobals';
 *   ...rest of bootstrap
 *
 * No-op in Jest (`react-native-webrtc` is mocked in jest.setup.ts) —
 * tests never hit the real WebRTC primitives anyway.
 */
import {
  RTCPeerConnection,
  RTCSessionDescription,
  RTCIceCandidate,
  MediaStream,
  mediaDevices,
} from 'react-native-webrtc';

type WebrtcGlobals = {
  RTCPeerConnection: typeof RTCPeerConnection;
  RTCSessionDescription: typeof RTCSessionDescription;
  RTCIceCandidate: typeof RTCIceCandidate;
  MediaStream: typeof MediaStream;
};

const g = globalThis as unknown as WebrtcGlobals & {
  navigator?: {mediaDevices?: typeof mediaDevices};
};

g.RTCPeerConnection = RTCPeerConnection;
g.RTCSessionDescription = RTCSessionDescription;
g.RTCIceCandidate = RTCIceCandidate;
g.MediaStream = MediaStream;

if (!g.navigator) {
  (g as {navigator: object}).navigator = {};
}
g.navigator!.mediaDevices = mediaDevices;
