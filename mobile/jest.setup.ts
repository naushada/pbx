/**
 * Jest global setup — TDD layer M0.4 (native-module mocks).
 *
 * Jest runs in pure Node; it cannot load the native (Obj-C / Java)
 * halves of React Native modules. Every native module the app will
 * touch is faked here so unit and component tests run without a
 * device or simulator.
 *
 * These fakes deliberately expose only the surface the app calls.
 * The REAL behaviour of these modules is exercised only by Detox
 * (TDD layer M4) and the manual device matrix (M5) — see
 * docs/design/mobile-app-tdd.md. A green `npm test` says nothing
 * about CallKit/WebRTC/Keychain actually working on a device.
 */

// ── react-native-keychain — secure token storage (used from M1) ───────────
jest.mock('react-native-keychain', () => {
  let store: {username: string; password: string} | false = false;
  return {
    setGenericPassword: jest.fn(async (username: string, password: string) => {
      store = {username, password};
      return true;
    }),
    getGenericPassword: jest.fn(async () => store),
    resetGenericPassword: jest.fn(async () => {
      store = false;
      return true;
    }),
    ACCESSIBLE: {WHEN_UNLOCKED: 'AccessibleWhenUnlocked'},
  };
});

// ── react-native-webrtc — media engine (used from M2) ─────────────────────
jest.mock('react-native-webrtc', () => ({
  RTCPeerConnection: jest.fn(),
  RTCSessionDescription: jest.fn(),
  RTCIceCandidate: jest.fn(),
  MediaStream: jest.fn(),
  mediaDevices: {
    getUserMedia: jest.fn(async () => ({getTracks: () => []})),
  },
}));

// ── react-native-callkeep — CallKit / ConnectionService (used from M2/M3) ─
jest.mock('react-native-callkeep', () => ({
  __esModule: true,
  default: {
    setup: jest.fn(async () => {}),
    displayIncomingCall: jest.fn(),
    startCall: jest.fn(),
    endCall: jest.fn(),
    endAllCalls: jest.fn(),
    addEventListener: jest.fn(),
    removeEventListener: jest.fn(),
  },
}));
