/**
 * Call state machine — TDD layer M2.a.
 *
 * A pure reducer over one outbound call's lifecycle. The SIP/WebRTC
 * layer (M2.b/c) feeds it events; the Dial screen (M2.d) renders its
 * state. No I/O, no React — just transitions.
 */
export type CallState = 'idle' | 'calling' | 'connected' | 'ended';

export type CallEvent =
  | {type: 'dial'; target: string}
  /**
   * Adopt an already-answered inbound call into the SipCallController.
   * Goes idle (or post-ended) → connected directly, skipping the
   * `calling` transition that an outbound dial walks through. The
   * inbound-side `IncomingCallController` has already taken the user
   * through Accept; this hands the resulting `SipCallHandle` to the
   * controller so the in-call panel + hangup path light up for the
   * inbound case the same way they do for outbound.
   */
  | {type: 'adopted'; target: string}
  | {type: 'answered'}
  | {type: 'localHangup'}
  | {type: 'remoteHangup'}
  | {type: 'rejected'; reason: string}
  | {type: 'failed'; reason: string};

export interface Call {
  state: CallState;
  /** The dialed target URI — set on `dial`, kept until the next `dial`. */
  target: string | null;
  /** Why the call ended — null until it does. */
  endReason: string | null;
}

export const idleCall: Call = {state: 'idle', target: null, endReason: null};

/**
 * Pure transition. An event that does not apply to the current state
 * returns the call **unchanged** (same reference) — callers can use
 * identity to detect a no-op.
 */
export function callReducer(call: Call, event: CallEvent): Call {
  switch (event.type) {
    case 'dial':
      // Block ONLY while a call is actively occupying the line. Was
      // `state !== 'idle'`, but that also blocked a fresh dial after a
      // previous call ended — so the user could only ever place ONE
      // call per app launch. `isCallActive` allows the post-`ended`
      // re-dial path.
      if (isCallActive(call)) return call;
      return {state: 'calling', target: event.target, endReason: null};

    case 'adopted':
      // Same gate as dial — don't overwrite a live call; allow from
      // idle or post-`ended`.
      if (isCallActive(call)) return call;
      return {state: 'connected', target: event.target, endReason: null};

    case 'answered':
      if (call.state !== 'calling') return call;
      return {...call, state: 'connected'};

    case 'localHangup':
    case 'remoteHangup':
      if (call.state !== 'calling' && call.state !== 'connected') return call;
      return {
        ...call,
        state: 'ended',
        endReason: event.type === 'localHangup' ? 'hung up' : 'remote hung up',
      };

    case 'rejected':
      if (call.state !== 'calling') return call;
      return {...call, state: 'ended', endReason: event.reason};

    case 'failed':
      if (call.state === 'idle' || call.state === 'ended') return call;
      return {...call, state: 'ended', endReason: event.reason};
  }
}

/** True while a call occupies the line (no new call may start). */
export function isCallActive(call: Call): boolean {
  return call.state === 'calling' || call.state === 'connected';
}
