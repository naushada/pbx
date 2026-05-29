/**
 * TDD layer M3.b — IncomingCallController.
 *
 * The incoming-call glue, driven against mocked native modules: a
 * wake-up push rings via CallKit, accept opens the tunnel + answers the
 * INVITE + connects media, decline sends a SIP reject, and an
 * unanswered ring times out as a missed call. CallKit, WebRTC and SIP
 * are all injected fakes here — the REAL native behaviour is proven
 * only at M4 (Detox) and M5 (device matrix). See mobile-app-tdd.md.
 */
import {IncomingCallController} from '../incomingCallController';

interface PendingTimer {
  fn: () => void;
  ms: number;
}

/** Injected clock — the missed-call timeout fires only when told to. */
class FakeClock {
  pending: PendingTimer[] = [];
  setTimer = (fn: () => void, ms: number): PendingTimer => {
    const timer: PendingTimer = {fn, ms};
    this.pending.push(timer);
    return timer;
  };
  clearTimer = (handle: unknown): void => {
    this.pending = this.pending.filter(t => t !== handle);
  };
  /** Run and drop the oldest pending timer. */
  runNext(): void {
    const timer = this.pending.shift();
    if (timer) timer.fn();
  }
}

const RING_TIMEOUT_MS = 30000;

/** A well-formed APNs/FCM incoming-call wake-up payload. */
function wakeUpPush(overrides: Record<string, unknown> = {}) {
  return {
    type: 'incoming-call',
    callId: 'call-7',
    callerFlat: 'B204',
    callerName: 'Asha Rao',
    ...overrides,
  };
}

function setup() {
  const clock = new FakeClock();
  const callKit = {
    displayIncomingCall: jest.fn(),
    endCall: jest.fn(),
  };
  const signaling = {
    answer: jest.fn(async () => {}),
    reject: jest.fn(),
  };
  const ensureConnected = jest.fn(async () => {});
  const connectMedia = jest.fn(async () => {});
  const logMissedCall = jest.fn();
  const controller = new IncomingCallController({
    callKit,
    signaling,
    ensureConnected,
    connectMedia,
    logMissedCall,
    ringTimeoutMs: RING_TIMEOUT_MS,
    setTimer: clock.setTimer,
    clearTimer: clock.clearTimer,
  });
  return {
    controller,
    clock,
    callKit,
    signaling,
    ensureConnected,
    connectMedia,
    logMissedCall,
  };
}

describe('IncomingCallController', () => {
  it('reports a wake-up push to CallKit as a ringing call', () => {
    const {controller, callKit} = setup();

    const call = controller.reportPush(wakeUpPush());

    expect(call).toEqual({
      state: 'ringing',
      callId: 'call-7',
      callerFlat: 'B204',
      callerName: 'Asha Rao',
    });
    expect(callKit.displayIncomingCall).toHaveBeenCalledWith(
      'call-7',
      'B204',
      'Asha Rao',
    );
  });

  it('ignores a malformed push — no ring, no CallKit', () => {
    const {controller, callKit} = setup();

    const call = controller.reportPush({type: 'something-else'});

    expect(call).toBeNull();
    expect(controller.getCall()).toBeNull();
    expect(callKit.displayIncomingCall).not.toHaveBeenCalled();
  });

  it('on accept: opens the tunnel, answers the INVITE, then connects media', async () => {
    const {controller, ensureConnected, signaling, connectMedia} = setup();
    controller.reportPush(wakeUpPush());

    await controller.accept();

    expect(ensureConnected).toHaveBeenCalledTimes(1);
    expect(signaling.answer).toHaveBeenCalledWith('call-7');
    expect(connectMedia).toHaveBeenCalledTimes(1);
    // tunnel up → INVITE answered → media wired, strictly in that order.
    expect(ensureConnected.mock.invocationCallOrder[0]).toBeLessThan(
      signaling.answer.mock.invocationCallOrder[0],
    );
    expect(signaling.answer.mock.invocationCallOrder[0]).toBeLessThan(
      connectMedia.mock.invocationCallOrder[0],
    );
    expect(controller.getCall()?.state).toBe('accepted');
  });

  it('on decline: sends a SIP 603 reject and dismisses the CallKit UI', () => {
    const {controller, signaling, callKit} = setup();
    controller.reportPush(wakeUpPush());

    controller.decline();

    expect(signaling.reject).toHaveBeenCalledWith('call-7', 603);
    expect(callKit.endCall).toHaveBeenCalledWith('call-7');
    expect(controller.getCall()?.state).toBe('declined');
  });

  it('an unanswered ring times out as a missed call and is logged', () => {
    const {controller, clock, callKit, logMissedCall} = setup();
    controller.reportPush(wakeUpPush());

    expect(clock.pending[0].ms).toBe(RING_TIMEOUT_MS);
    clock.runNext(); // the ring timeout elapses

    expect(controller.getCall()?.state).toBe('missed');
    expect(callKit.endCall).toHaveBeenCalledWith('call-7');
    expect(logMissedCall).toHaveBeenCalledWith(
      expect.objectContaining({state: 'missed', callId: 'call-7'}),
    );
  });

  it('accepting cancels the missed-call timeout', async () => {
    const {controller, clock} = setup();
    controller.reportPush(wakeUpPush());

    await controller.accept();

    expect(clock.pending).toHaveLength(0);
  });

  it('rings from a wake-up push even with the app killed (headless entry)', () => {
    // Simulates the PushKit / FCM headless entry point: a freshly
    // constructed controller, no screens mounted, no prior bootstrap.
    const {controller, callKit} = setup();

    const call = controller.reportPush(wakeUpPush());

    expect(call?.state).toBe('ringing');
    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(1);
  });

  it('ignores a duplicate push while a call is already ringing', () => {
    const {controller, callKit} = setup();
    controller.reportPush(wakeUpPush());

    controller.reportPush(wakeUpPush({callId: 'call-9'}));

    // First-ring-wins: the second push neither re-rings nor re-arms.
    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(1);
    expect(controller.getCall()?.callId).toBe('call-7');
  });

  // ── Guard kiosk auto-answer (DESIGN.md §9) ─────────────────────────
  //
  // Mirrors the web softphone's `SipService.onIncoming` short-circuit:
  // when the subscriber is a guard with `autoAnswer === true`, the
  // controller goes straight from "push arrived" → answered, skipping
  // the CallKit ring UI + the missed-call timer entirely. Defence-in-
  // depth is enforced at the createSipEnv layer (it AND-s the role
  // check); the controller just takes a `shouldAutoAnswer` predicate.

  function autoAnswerSetup(predicate: () => boolean) {
    const clock = new FakeClock();
    const callKit = {
      displayIncomingCall: jest.fn(),
      endCall: jest.fn(),
    };
    const signaling = {
      answer: jest.fn(async () => {}),
      reject: jest.fn(),
    };
    const ensureConnected = jest.fn(async () => {});
    const connectMedia = jest.fn(async () => {});
    const controller = new IncomingCallController({
      callKit,
      signaling,
      ensureConnected,
      connectMedia,
      logMissedCall: jest.fn(),
      ringTimeoutMs: RING_TIMEOUT_MS,
      setTimer: clock.setTimer,
      clearTimer: clock.clearTimer,
      shouldAutoAnswer: predicate,
    });
    return {controller, clock, callKit, signaling, ensureConnected, connectMedia};
  }

  it('auto-answers when shouldAutoAnswer returns true — no CallKit ring, no timer', async () => {
    const {controller, clock, callKit, signaling, ensureConnected, connectMedia} =
      autoAnswerSetup(() => true);

    controller.reportPush(wakeUpPush());
    // Drain any pending microtasks so `void this.accept()` runs to
    // completion within the test's deterministic horizon.
    await Promise.resolve();
    await Promise.resolve();

    expect(callKit.displayIncomingCall).not.toHaveBeenCalled();
    expect(clock.pending.length).toBe(0);
    expect(ensureConnected).toHaveBeenCalledTimes(1);
    expect(signaling.answer).toHaveBeenCalledWith('call-7');
    expect(connectMedia).toHaveBeenCalledTimes(1);
  });

  it('rings normally when shouldAutoAnswer returns false (e.g. resident)', () => {
    const {controller, clock, callKit, signaling} =
      autoAnswerSetup(() => false);

    controller.reportPush(wakeUpPush());

    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(1);
    expect(clock.pending.length).toBe(1);
    expect(signaling.answer).not.toHaveBeenCalled();
  });

  // Future test: per-call re-evaluation across consecutive rings. Held
  // back because the existing `ringReducer` is first-ring-wins
  // regardless of state (mobile/src/sip/incomingCall.ts:42-51) — after
  // a `decline()` the call object stays in `state: 'declined'` and a
  // second `reportPush` is a no-op. This diverges from the web
  // softphone (`SipService.rejectIncoming` clears `this.incoming =
  // undefined`). Tracked as a separate follow-up; doesn't affect the
  // auto-answer wiring on a fresh controller (covered by the two
  // tests above).
});
