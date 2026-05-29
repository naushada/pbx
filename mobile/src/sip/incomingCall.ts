/**
 * Incoming-call model — TDD layer M3.b.
 *
 * A pure reducer over one INBOUND call's ring lifecycle. Kept separate
 * from the outbound `callState.ts` machine on purpose: a ring is not a
 * call until it is accepted, and the two have disjoint event sets.
 * `IncomingCallController` reconciles the two models so the single
 * phone line is never double-used.
 *
 * No I/O, no React, no native modules — just transitions.
 */
export type RingState = 'ringing' | 'accepted' | 'declined' | 'missed';

export type RingEvent =
  | {type: 'ring'; callId: string; callerFlat: string; callerName: string}
  | {type: 'accept'}
  | {type: 'decline'}
  | {type: 'timeout'}
  | {type: 'remoteCancel'};

export interface IncomingCall {
  state: RingState;
  /** Correlates the ring with the SIP dialog. */
  callId: string;
  /** The caller's flat number — the CallKit handle. */
  callerFlat: string;
  /** The caller's display name; falls back to the flat number. */
  callerName: string;
}

/**
 * Pure transition. `null` is "no inbound call". An event that does not
 * apply to the current state returns the call **unchanged** (same
 * reference, or the same `null`) — callers use identity to detect a
 * no-op.
 */
export function ringReducer(
  call: IncomingCall | null,
  event: RingEvent,
): IncomingCall | null {
  switch (event.type) {
    case 'ring':
      // Idempotent duplicate-push guard: while a call is ACTIVELY
      // ringing, a second push is a no-op — the line is single-
      // occupancy. After that ring resolves (decline / missed / accept),
      // the next push CAN ring — otherwise the line is stuck after the
      // first call. Mirrors the web softphone, where
      // `SipService.rejectIncoming` clears `this.incoming = undefined`
      // so the next INVITE can ring.
      if (call !== null && call.state === 'ringing') return call;
      return {
        state: 'ringing',
        callId: event.callId,
        callerFlat: event.callerFlat,
        callerName: event.callerName,
      };

    case 'accept':
      if (call === null || call.state !== 'ringing') return call;
      return {...call, state: 'accepted'};

    case 'decline':
      if (call === null || call.state !== 'ringing') return call;
      return {...call, state: 'declined'};

    case 'timeout':
    case 'remoteCancel':
      // An unanswered ring — whether it timed out locally or the caller
      // gave up — is a missed call.
      if (call === null || call.state !== 'ringing') return call;
      return {...call, state: 'missed'};
  }
}
