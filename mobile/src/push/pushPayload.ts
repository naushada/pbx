/**
 * Incoming-call wake-up push parsing — TDD layer M3.a.
 *
 * When a call is inbound for this resident, the cloud sends a push —
 * an APNs PushKit payload (iOS) or an FCM data message (Android). The
 * OS hands the app a flat key/value object; this turns it into a typed
 * `WakeUpCall`, or `null` for anything malformed.
 *
 * A malformed push must never crash the handler — on iOS especially, a
 * thrown error in the PushKit handler is penalised by the OS. Hence the
 * null-return contract rather than exceptions.
 */

export interface WakeUpCall {
  /** Correlates the push with the SIP dialog. */
  callId: string;
  /** The caller's flat number — shown on the incoming-call screen. */
  callerFlat: string;
  /** The caller's display name; falls back to the flat number. */
  callerName: string;
}

export function parseWakeUp(payload: unknown): WakeUpCall | null {
  if (typeof payload !== 'object' || payload === null) {
    return null;
  }
  const p = payload as Record<string, unknown>;
  if (p.type !== 'incoming-call') {
    return null;
  }
  // FCM data-message values arrive as strings; APNs payloads likewise
  // carry these as strings — anything else is malformed.
  const {callId, callerFlat} = p;
  if (typeof callId !== 'string' || callId === '') {
    return null;
  }
  if (typeof callerFlat !== 'string' || callerFlat === '') {
    return null;
  }
  const callerName =
    typeof p.callerName === 'string' && p.callerName !== ''
      ? p.callerName
      : callerFlat;
  return {callId, callerFlat, callerName};
}
