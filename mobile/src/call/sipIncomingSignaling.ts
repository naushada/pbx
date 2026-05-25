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
import {IncomingCallHandle, SipUaHandle} from '../sip/sipUa';

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
  ) {
    ua.onIncomingCall(handle => this.onInvite(handle));
  }

  async answer(callId: string): Promise<void> {
    const handle = this.pending.get(callId);
    if (!handle) return;
    this.pending.delete(callId);
    await handle.accept();
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
