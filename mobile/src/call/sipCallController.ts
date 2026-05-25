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
} from '../sip/callState';
import {callTargetUri} from '../sip/sipUri';
import {
  SipCallHandle,
  SipCallStateChange,
  SipUaHandle,
} from '../sip/sipUa';
import {CallController} from './callController';

export class SipCallController implements CallController {
  private call: Call = idleCall;
  private muted = false;
  private readonly listeners = new Set<(call: Call) => void>();

  private currentSipCall: SipCallHandle | null = null;
  private weHungUp = false;

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
    // First-call-wins: the reducer returns identity when not idle, so
    // we check beforehand to avoid wasted INVITE traffic.
    if (this.call.state !== 'idle') return;

    this.weHungUp = false;
    const handle = this.ua.placeCall(target);
    this.currentSipCall = handle;
    handle.onStateChange(change => this.onSipState(change));
    this.dispatch({type: 'dial', target});
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
      case 'ended':
        // If we initiated the hangup the reducer is already 'ended';
        // suppress to avoid stomping the "hung up" reason with
        // "remote hung up".
        if (!this.weHungUp) {
          this.dispatch({type: 'remoteHangup'});
        }
        this.currentSipCall = null;
        break;
      case 'failed':
        this.dispatch({
          type: 'failed',
          reason: change.detail ?? 'failed',
        });
        this.currentSipCall = null;
        break;
    }
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
