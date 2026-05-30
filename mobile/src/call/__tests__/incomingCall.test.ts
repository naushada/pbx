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

  it('rings again after a previously declined call ends', () => {
    // Surfaced as a hold-back in PR #163: the older `ringReducer`'s
    // first-ring-wins was too broad and blocked the next ring forever
    // after the first call resolved. Web's `SipService.rejectIncoming`
    // clears `this.incoming = undefined`, so a second INVITE rings;
    // mobile now matches that behaviour via the narrowed reducer guard
    // (only blocks while state === 'ringing').
    const {controller, callKit} = setup();
    controller.reportPush(wakeUpPush({callId: 'call-A'}));
    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(1);

    controller.decline();
    expect(controller.getCall()?.state).toBe('declined');

    controller.reportPush(wakeUpPush({callId: 'call-B'}));

    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(2);
    expect(controller.getCall()?.state).toBe('ringing');
    expect(controller.getCall()?.callId).toBe('call-B');
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

  it('re-evaluates shouldAutoAnswer per call (session flip takes effect)', async () => {
    let autoAnswer = false;
    const {controller, callKit, signaling} =
      autoAnswerSetup(() => autoAnswer);

    // Ring 1 — resident, normal ring.
    controller.reportPush(wakeUpPush({callId: 'call-A'}));
    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(1);
    expect(signaling.answer).not.toHaveBeenCalled();
    controller.decline();

    // Ring 2 — session flips to guard kiosk, auto-answers (this also
    // exercises the rearm-after-decline path the narrowed reducer
    // enables).
    autoAnswer = true;
    controller.reportPush(wakeUpPush({callId: 'call-B'}));
    await Promise.resolve();
    await Promise.resolve();

    expect(signaling.answer).toHaveBeenCalledWith('call-B');
    // Display count unchanged from ring 1 — ring 2 skipped the UI.
    expect(callKit.displayIncomingCall).toHaveBeenCalledTimes(1);
  });

  // ── clear() — drop a lingering post-accept / post-decline state ────
  //
  // `createSipEnv` calls this when SipCallController reports the call
  // has ended, so the busy gate doesn't latch on the stale 'accepted'
  // state forever. Tested in isolation here.

  describe('clear()', () => {
    it('drops a lingering accepted call and notifies subscribers', async () => {
      const {controller} = setup();
      controller.reportPush(wakeUpPush());
      await controller.accept();
      expect(controller.getCall()?.state).toBe('accepted');
      const seen: Array<unknown> = [];
      controller.subscribe(c => seen.push(c));

      controller.clear();

      expect(controller.getCall()).toBeNull();
      expect(seen).toEqual([null]);
    });

    it('drops a lingering declined call', () => {
      const {controller} = setup();
      controller.reportPush(wakeUpPush());
      controller.decline();
      expect(controller.getCall()?.state).toBe('declined');

      controller.clear();

      expect(controller.getCall()).toBeNull();
    });

    it('is a no-op while a call is actively ringing', () => {
      const {controller} = setup();
      controller.reportPush(wakeUpPush());
      const before = controller.getCall();

      controller.clear();

      expect(controller.getCall()).toBe(before);
    });

    it('is a no-op when there is no call to begin with', () => {
      const {controller} = setup();
      const cb = jest.fn();
      controller.subscribe(cb);

      controller.clear();

      expect(controller.getCall()).toBeNull();
      expect(cb).not.toHaveBeenCalled();
    });
  });
});
