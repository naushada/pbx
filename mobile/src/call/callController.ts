/**
 * CallController — TDD layer M2.d.
 *
 * The seam between the UI and the call engine. The Dial / in-call
 * screens depend only on this interface — they place calls, hang up,
 * mute, and subscribe to call-state changes through it, never touching
 * SIP or WebRTC directly.
 *
 * `StubCallController` (below) drives the call STATE MACHINE only — no
 * real SIP, no audio. It lets the Dial UI be built, tested, and demoed
 * before the sip.js engine lands; the sip.js-backed CallController
 * (the integration slice) replaces it without any screen change.
 */
import {Call, CallEvent, callReducer, idleCall} from '../sip/callState';
import {callTargetUri} from '../sip/sipUri';

export interface CallController {
  /** The current call snapshot. */
  getCall(): Call;
  /** Subscribe to call-state changes; returns an unsubscribe function. */
  subscribe(listener: (call: Call) => void): () => void;
  /** Place a call to a flat number (ignored if it is not dialable). */
  placeCall(flatNumber: string): void;
  /** Hang up the active call. */
  hangup(): void;
  /** Mute or unmute the local microphone. */
  setMuted(muted: boolean): void;
  /** Whether the microphone is currently muted. */
  isMuted(): boolean;
}

export class StubCallController implements CallController {
  private call: Call = idleCall;
  private muted = false;
  private readonly listeners = new Set<(call: Call) => void>();

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
      return; // not a dialable flat — ignore
    }
    this.dispatch({type: 'dial', target});
  }

  hangup(): void {
    this.dispatch({type: 'localHangup'});
  }

  setMuted(muted: boolean): void {
    this.muted = muted;
  }

  isMuted(): boolean {
    return this.muted;
  }

  private dispatch(event: CallEvent): void {
    const next = callReducer(this.call, event);
    if (next === this.call) return; // no-op transition
    this.call = next;
    for (const listener of this.listeners) {
      listener(next);
    }
  }
}
