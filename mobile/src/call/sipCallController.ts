/**
 * SipCallController — TDD layer M2 sip.js integration slice.
 *
 * Concrete `CallController` (mobile-side M2.d seam) backed by the
 * ui-mirrored `SipUaHandle`. The Dial UI's state machine is unchanged:
 * we just translate sip.js call-lifecycle events into the same
 * `callReducer` events the `StubCallController` was driving.
 *
 * Construction is cheap (it doesn't open any sockets); the
 * `SipUaHandle` it receives is responsible for `start()` / `stop()`
 * lifecycle — typically wired in the app shell right after login.
 *
 * The `IncomingCallSignaling` half of the sip.js integration lives in
 * `sipIncomingSignaling.ts`; it consumes `SipUaHandle.onIncomingCall`.
 */
import {
  Call,
  CallEvent,
  callReducer,
  idleCall,
  isCallActive,
} from '../sip/callState';
import {callTargetUri} from '../sip/sipUri';
import {
  SipCallHandle,
  SipCallStateChange,
  SipUaHandle,
} from '../sip/sipUa';
import {CallController} from './callController';
import {MetricsManager} from '../metrics';

export class SipCallController implements CallController {
  private call: Call = idleCall;
  private muted = false;
  private readonly listeners = new Set<(call: Call) => void>();

  private currentSipCall: SipCallHandle | null = null;
  private weHungUp = false;
  // Per-call metric bookkeeping; the event is emitted once when the call ends.
  private callStartedAt: number | null = null;
  private callDirection: 'outbound' | 'inbound' | null = null;
  private callPeer: string | null = null;

  constructor(private readonly ua: SipUaHandle) {}

  getCall(): Call {
    return this.call;
  }

  subscribe(listener: (call: Call) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  placeCall(flatNumber: string): void {
    let target: string;
    try {
      target = callTargetUri(flatNumber);
    } catch {
      return; // not a dialable flat — same contract as StubCallController
    }
    // Block ONLY while a call actively occupies the line. The previous
    // `state !== 'idle'` check also blocked a fresh dial after a
    // previous call ended — `isCallActive` allows the re-dial path
    // (matches the narrowed reducer guard above).
    if (isCallActive(this.call)) return;

    this.weHungUp = false;
    const handle = this.ua.placeCall(target);
    this.currentSipCall = handle;
    handle.onStateChange(change => this.onSipState(change));
    this.dispatch({type: 'dial', target});
    // Begin metric bookkeeping; emitted once the call ends.
    this.callStartedAt = Date.now();
    this.callDirection = 'outbound';
    this.callPeer = flatNumber;
  }

  /**
   * Hand an already-answered inbound `SipCallHandle` to the controller
   * so the in-call panel + hangup path light up for the inbound case
   * the same way they do for outbound. Wired by `createSipEnv` via
   * `SipInboundBridge.onAnswered`. The `target` URI is constructed for
   * display only (`InCallPanel.flatOf` extracts the user-part); no
   * dial happens — the SIP layer is already established.
   *
   * No-op if the controller already has an active call (defence-in-
   * depth — the bridge's busy gate normally prevents this).
   */
  adoptInboundCall(handle: SipCallHandle, peerFlat: string): void {
    if (isCallActive(this.call)) return;
    this.weHungUp = false;
    this.currentSipCall = handle;
    handle.onStateChange(change => this.onSipState(change));
    this.dispatch({type: 'adopted', target: `sip:${peerFlat}@pbx.local`});
    this.callStartedAt = Date.now();
    this.callDirection = 'inbound';
    this.callPeer = peerFlat;
  }

  hangup(): void {
    if (!this.currentSipCall) return;
    this.weHungUp = true;
    // Fire-and-forget: sip.js's hangup returns a Promise but the UI
    // doesn't await it; we transition local state immediately.
    this.currentSipCall.hangup().catch(() => {
      /* logged by SipJsCallHandle */
    });
    this.dispatch({type: 'localHangup'});
  }

  setMuted(muted: boolean): void {
    this.muted = muted;
    this.currentSipCall?.setMute(muted);
  }

  isMuted(): boolean {
    return this.muted;
  }

  private onSipState(change: SipCallStateChange): void {
    switch (change.state) {
      case 'calling':
      case 'progressing':
        // Already dispatched 'dial' synchronously in placeCall; the
        // reducer is in 'calling'. Nothing new to emit here.
        break;
      case 'in-call':
        this.dispatch({type: 'answered'});
        break;
      case 'ended': {
        // If we initiated the hangup the reducer is already 'ended';
        // suppress to avoid stomping the "hung up" reason with
        // "remote hung up".
        if (!this.weHungUp) {
          this.dispatch({type: 'remoteHangup'});
        }
        this.currentSipCall = null;
        this.trackCallEnd(true);
        break;
      }
      case 'failed': {
        this.dispatch({
          type: 'failed',
          reason: change.detail ?? 'failed',
        });
        this.currentSipCall = null;
        this.trackCallEnd(false);
        break;
      }
    }
  }

  // Emit a single call metric when the active call ends, then reset bookkeeping.
  private trackCallEnd(success: boolean): void {
    if (this.callDirection === null) return; // nothing was tracked
    const peer = this.callPeer ?? 'unknown';
    const duration =
      this.callStartedAt != null ? Date.now() - this.callStartedAt : undefined;
    const metrics = MetricsManager.getInstance();
    if (this.callDirection === 'inbound') {
      metrics.trackInboundCall(peer, duration, success);
    } else {
      metrics.trackOutboundCall(peer, duration, success);
    }
    this.callStartedAt = null;
    this.callDirection = null;
    this.callPeer = null;
  }

  private dispatch(event: CallEvent): void {
    const next = callReducer(this.call, event);
    if (next === this.call) return; // reducer reported a no-op
    this.call = next;
    for (const listener of this.listeners) {
      listener(next);
    }
  }
}
