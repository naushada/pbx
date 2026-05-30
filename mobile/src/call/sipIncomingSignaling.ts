/**
 * SipInboundBridge — TDD M3.b sip.js integration slice.
 *
 * Two cohesive jobs:
 *
 *   1. Wire the ui-mirrored `SipUaHandle.onIncomingCall` channel into
 *      `IncomingCallController.reportPush`. In foreground operation
 *      there is no real APNs/FCM wake-up — the UA's INVITE delivery is
 *      the trigger, so we synthesise the same `incoming-call` payload
 *      shape the controller already understands.
 *
 *   2. Implement `IncomingCallSignaling` (the M3.b seam) over a
 *      Call-ID → `IncomingCallHandle` registry. `accept` / `reject` on
 *      the controller resolve to the matching sip.js Invitation here.
 *
 * Background/killed wake-up via PushKit / FCM lands in a follow-up; the
 * same `IncomingCallController.reportPush` is the entry point, so the
 * downstream control flow doesn't change when push lands.
 */
import {IncomingCallSignaling} from './incomingCallController';
import {IncomingCallHandle, SipCallHandle, SipUaHandle} from '../sip/sipUa';

/** Just the slice of IncomingCallController this bridge needs. */
interface IncomingCallReporter {
  reportPush(payload: unknown): unknown;
}

/** SIP 603 Decline — explicit "no" from the resident. */
const SIP_DECLINE = 603;

export class SipInboundBridge implements IncomingCallSignaling {
  private readonly pending = new Map<string, IncomingCallHandle>();

  constructor(
    ua: SipUaHandle,
    private readonly controller: IncomingCallReporter,
    /**
     * Concurrent-call busy gate (defence-in-depth for ringReducer). When
     * provided AND returns true at INVITE time, the new call is rejected
     * with SIP 486 Busy Here and `reportPush` is never invoked — same
     * shape as the web softphone's `SipService.onIncoming`:
     *
     *     if (this.call || this.incoming) {
     *         void h.reject('busy');
     *         return;
     *     }
     *
     * Without this gate, a second INVITE arriving while a previous call
     * is active would overwrite the `IncomingCall` state via the
     * narrowed (PR #164) ringReducer and ring the user — never what we
     * want.
     */
    private readonly isBusy?: () => boolean,
    /**
     * Fires once the answered INVITE produces an active `SipCallHandle`.
     * `createSipEnv` wires this to `SipCallController.adoptInboundCall`
     * so the in-call panel + hangup path light up for the inbound case
     * the same way they do for outbound. Without this, an accepted
     * inbound call has no UI to hang up — the user is silently stuck.
     */
    private readonly onAnswered?: (
      handle: SipCallHandle,
      peerFlat: string,
    ) => void,
  ) {
    ua.onIncomingCall(handle => this.onInvite(handle));
  }

  async answer(callId: string): Promise<void> {
    const handle = this.pending.get(callId);
    if (!handle) return;
    this.pending.delete(callId);
    const peerFlat = handle.info.fromFlat;
    const accepted = await handle.accept();
    this.onAnswered?.(accepted, peerFlat);
  }

  reject(callId: string, code: number): void {
    const handle = this.pending.get(callId);
    if (!handle) return;
    this.pending.delete(callId);
    const cause: 'busy' | 'declined' =
      code === SIP_DECLINE ? 'declined' : 'busy';
    handle.reject(cause).catch(() => {
      /* logged by SipJsUaHandle */
    });
  }

  private onInvite(handle: IncomingCallHandle): void {
    // Busy gate first — never touch `pending` or `reportPush` if the
    // line is already occupied; the new dialog is dropped right back
    // out via SIP 486 Busy Here.
    if (this.isBusy?.()) {
      handle.reject('busy').catch(() => {
        /* logged by SipJsUaHandle */
      });
      return;
    }
    this.pending.set(handle.info.callId, handle);
    // Synthesise the same payload PushKit/FCM will deliver later.
    // We don't get a display-name from the SIP URI, so we mirror flat
    // into callerName — parseWakeUp's own fallback if callerName is
    // missing already does this, but we set it for symmetry.
    this.controller.reportPush({
      type: 'incoming-call',
      callId: handle.info.callId,
      callerFlat: handle.info.fromFlat,
      callerName: handle.info.fromFlat,
    });
  }
}
