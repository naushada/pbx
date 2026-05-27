/**
 * Concrete `SipUaFactory` backed by sip.js. Implementation lives in the
 * shared module so the Angular web softphone uses the same wiring.
 *
 * `sip.js/lib/platform/web`'s default SDH calls `RTCPeerConnection`,
 * `RTCSessionDescription`, `MediaStream`, etc. as globals. In RN those
 * come from `react-native-webrtc`; see `webrtcGlobals.ts` which must
 * be imported at app startup (before this module).
 */
export { SipJsUaFactory } from '../../../shared/sip-ua/sip-ua-sipjs';
