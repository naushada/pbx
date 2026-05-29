/**
 * IncomingCallController — TDD layer M3.b.
 *
 * The glue that turns a wake-up push into a ringing call and drives
 * accept / decline. It sits between three native-ish concerns — all
 * injected as seams so the control flow is unit-testable with no
 * device:
 *
 *   - CallKit / ConnectionService — the OS incoming-call UI
 *     (`react-native-callkeep`).
 *   - SIP signalling — answering / rejecting the INVITE (the sip.js
 *     `UserAgent`, wired in the integration slice).
 *   - The SIP tunnel + WebRTC media path (M2.b `SipTunnel`, M2.c
 *     `MediaSession`).
 *
 * `reportPush` is deliberately self-contained: it is also the headless
 * entry point a PushKit / FCM handler invokes when the app is not
 * running, so it must work on a freshly constructed controller with no
 * screens mounted.
 */
import {parseWakeUp} from '../push/pushPayload';
import {IncomingCall, RingEvent, ringReducer} from '../sip/incomingCall';

/** CallKit (iOS) / ConnectionService (Android), via react-native-callkeep. */
export interface CallKitBridge {
  /** Show the OS incoming-call UI. `handle` is the caller's flat number. */
  displayIncomingCall(callId: string, handle: string, callerName: string): void;
  /** Dismiss the OS call UI. */
  endCall(callId: string): void;
}

/** Answering / rejecting the SIP INVITE — backed by the sip.js UserAgent. */
export interface IncomingCallSignaling {
  /** Answer the INVITE — sends SIP 200 OK. */
  answer(callId: string): Promise<void>;
  /** Reject the INVITE with a SIP status code (603 Decline, 486 Busy). */
  reject(callId: string, code: number): void;
}

type TimerHandle = unknown;

export interface IncomingCallDeps {
  callKit: CallKitBridge;
  signaling: IncomingCallSignaling;
  /** Open the SIP tunnel if it is not already up. */
  ensureConnected: () => Promise<void>;
  /** Wire the WebRTC media path once the INVITE is answered. */
  connectMedia: () => Promise<void>;
  /** Record a ring that was never answered. */
  logMissedCall: (call: IncomingCall) => void;
  /** How long an unanswered ring lasts before it is missed (ms). */
  ringTimeoutMs?: number;
  /** Injectable timer — defaults to setTimeout/clearTimeout. */
  setTimer?: (fn: () => void, ms: number) => TimerHandle;
  clearTimer?: (handle: TimerHandle) => void;
  /**
   * Guard kiosk auto-answer predicate (DESIGN.md §9). When provided
   * AND returns true, `reportPush` skips the CallKit ring UI + the
   * missed-call timer and goes straight through `accept()` — same
   * shape as the web softphone's `SipService.onIncoming` auto-accept
   * (`sub.autoAnswer && sub.role === 'guard'`). Evaluated per-call so
   * a session flip between guard / resident takes effect on the next
   * ring without rebuilding the controller. Defence-in-depth: callers
   * are responsible for AND-ing in the role check, never just the flag.
   */
  shouldAutoAnswer?: () => boolean;
}

/** SIP 603 Decline — the resident explicitly declined the call. */
const SIP_DECLINE = 603;
/** A ring left unanswered this long is a missed call. */
const DEFAULT_RING_TIMEOUT_MS = 30000;

export class IncomingCallController {
  private call: IncomingCall | null = null;
  private ringTimer: TimerHandle | null = null;
  private readonly listeners = new Set<(call: IncomingCall | null) => void>();

  private readonly ringTimeoutMs: number;
  private readonly setTimer: (fn: () => void, ms: number) => TimerHandle;
  private readonly clearTimer: (handle: TimerHandle) => void;

  constructor(private readonly deps: IncomingCallDeps) {
    this.ringTimeoutMs = deps.ringTimeoutMs ?? DEFAULT_RING_TIMEOUT_MS;
    this.setTimer =
      deps.setTimer ?? ((fn, ms) => setTimeout(fn, ms) as unknown);
    this.clearTimer =
      deps.clearTimer ?? (handle => clearTimeout(handle as never));
  }

  /** The current inbound call, or null when nothing is ringing. */
  getCall(): IncomingCall | null {
    return this.call;
  }

  /** Subscribe to ring-state changes; returns an unsubscribe function. */
  subscribe(listener: (call: IncomingCall | null) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /**
   * Entry point for a wake-up push — also the headless entry point a
   * PushKit / FCM handler invokes when the app is not running. Parses
   * the payload, rings via CallKit, and arms the missed-call timeout.
   * Returns the ringing call, or null if the payload was malformed.
   *
   * A malformed payload never throws: a thrown error in an OS push
   * handler is penalised (especially on iOS PushKit).
   */
  reportPush(payload: unknown): IncomingCall | null {
    const wake = parseWakeUp(payload);
    if (wake === null) {
      return null;
    }
    const before = this.call;
    this.dispatch({
      type: 'ring',
      callId: wake.callId,
      callerFlat: wake.callerFlat,
      callerName: wake.callerName,
    });
    const ringing = this.call;
    // No new ring — a malformed-through-reducer or a duplicate push
    // while one is already in flight. Do not re-display or re-arm.
    if (ringing === null || ringing === before) {
      return ringing;
    }
    // Guard kiosk auto-answer (DESIGN.md §9): skip the CallKit ring UI
    // + the missed-call timer and go straight through accept(). The
    // accept() dispatch transitions state synchronously before its
    // first await, so React batches both updates and listeners never
    // see a "ringing" frame — the overlay's Vibration alerter never
    // arms either.
    if (this.deps.shouldAutoAnswer?.()) {
      void this.accept();
      return ringing;
    }
    this.deps.callKit.displayIncomingCall(
      ringing.callId,
      ringing.callerFlat,
      ringing.callerName,
    );
    this.ringTimer = this.setTimer(() => {
      this.ringTimer = null;
      this.onTimeout();
    }, this.ringTimeoutMs);
    return ringing;
  }

  /**
   * Accept the ringing call: open the tunnel if needed, answer the
   * INVITE, then connect media — strictly in that order.
   */
  async accept(): Promise<void> {
    const call = this.call;
    if (call === null || call.state !== 'ringing') {
      return;
    }
    this.clearRingTimer();
    this.dispatch({type: 'accept'});
    await this.deps.ensureConnected();
    await this.deps.signaling.answer(call.callId);
    await this.deps.connectMedia();
  }

  /** Decline the ringing call — SIP 603, then dismiss the CallKit UI. */
  decline(): void {
    const call = this.call;
    if (call === null || call.state !== 'ringing') {
      return;
    }
    this.clearRingTimer();
    this.dispatch({type: 'decline'});
    this.deps.signaling.reject(call.callId, SIP_DECLINE);
    this.deps.callKit.endCall(call.callId);
  }

  private onTimeout(): void {
    const call = this.call;
    if (call === null || call.state !== 'ringing') {
      return;
    }
    this.dispatch({type: 'timeout'});
    this.deps.callKit.endCall(call.callId);
    const missed = this.call;
    if (missed !== null) {
      this.deps.logMissedCall(missed);
    }
  }

  private clearRingTimer(): void {
    if (this.ringTimer !== null) {
      this.clearTimer(this.ringTimer);
      this.ringTimer = null;
    }
  }

  private dispatch(event: RingEvent): void {
    const next = ringReducer(this.call, event);
    if (next === this.call) {
      return; // no-op transition
    }
    this.call = next;
    for (const listener of this.listeners) {
      listener(next);
    }
  }
}
