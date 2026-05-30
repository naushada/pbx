/**
 * Unit test — createSipEnv wires a logged-in Session into a working
 * sip.js call engine + IncomingCallController + SipInboundBridge.
 *
 * Drives the factory with a fake SipUaFactory so no real WebSocket
 * opens; asserts the UA opts that are derived from the session and
 * that the IncomingCallController.signaling round-trips through the
 * inbound bridge once both are constructed.
 */
import {createSipEnv} from '../createSipEnv';
import {Session} from '../../api/types';
import {
  IncomingCallHandle,
  SipCallHandle,
  SipUaFactory,
  SipUaHandle,
  SipUaOpts,
  SipUaStateChange,
} from '../../sip/sipUa';

class FakeSipUa implements SipUaHandle {
  opts: SipUaOpts;
  incomingCb: ((h: IncomingCallHandle) => void) | null = null;
  start = jest.fn(async () => {});
  stop = jest.fn(async () => {});
  onStateChange = jest.fn((_cb: (c: SipUaStateChange) => void): void => {});
  placeCall = jest.fn((_t: string): SipCallHandle => {
    throw new Error('placeCall not exercised in createSipEnv test');
  });
  onIncomingCall = jest.fn((cb: (h: IncomingCallHandle) => void): void => {
    this.incomingCb = cb;
  });
  constructor(opts: SipUaOpts) {
    this.opts = opts;
  }
}

function fakeFactory(): {factory: SipUaFactory; built: FakeSipUa[]} {
  const built: FakeSipUa[] = [];
  const factory: SipUaFactory = {
    create: opts => {
      const ua = new FakeSipUa(opts);
      built.push(ua);
      return ua;
    },
  };
  return {factory, built};
}

function session(overrides: Partial<Session['subscriber']> = {}): Session {
  return {
    token: 'tok-7',
    subscriber: {
      societyId: 'soc1',
      flatNumber: 'A-101',
      sipUsername: 'u_abc',
      displayName: 'Asha Rao',
      role: 'resident',
      ...overrides,
    },
  };
}

describe('createSipEnv', () => {
  it('builds the UA from the session — sip URI, ws URL with token, display name', () => {
    const {factory, built} = fakeFactory();

    createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://pabx.example.com',
      factory,
    });

    expect(built).toHaveLength(1);
    expect(built[0].opts.uri).toBe('sip:u_abc@soc1.pbx.local');
    expect(built[0].opts.wsUrl).toBe(
      'wss://pabx.example.com/sip-ws?token=tok-7',
    );
    expect(built[0].opts.authUser).toBe('u_abc');
    expect(built[0].opts.displayName).toBe('Asha Rao');
  });

  it('keeps "ws://" for an http cloudBaseUrl (dev / non-TLS)', () => {
    const {factory, built} = fakeFactory();

    createSipEnv({
      session: session(),
      cloudBaseUrl: 'http://localhost:8080',
      factory,
    });

    expect(built[0].opts.wsUrl).toBe(
      'ws://localhost:8080/sip-ws?token=tok-7',
    );
  });

  it('exposes a SipCallController whose placeCall hits the UA', () => {
    const {factory, built} = fakeFactory();
    built; // capture by reference below
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://pabx.example.com',
      factory,
    });
    // SipCallController's placeCall will throw via the fake UA — we
    // only care that it routes through.
    expect(() => env.callController.placeCall('A-101')).toThrow(
      /placeCall not exercised/,
    );
    expect(built[0].placeCall).toHaveBeenCalledTimes(1);
  });

  it('wires SipInboundBridge so the IncomingCallController.signaling resolves to it', () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://pabx.example.com',
      factory,
    });

    // ua.onIncomingCall was subscribed by the bridge.
    expect(built[0].onIncomingCall).toHaveBeenCalledTimes(1);
    expect(built[0].incomingCb).not.toBeNull();

    // Deliver a fake INVITE through the UA → controller goes ringing.
    const handle: IncomingCallHandle = {
      info: {fromUri: 'sip:B-204@x', fromFlat: 'B-204', callId: 'call-7'},
      accept: jest.fn(async (): Promise<SipCallHandle> => ({
        onStateChange: () => {},
        hangup: async () => {},
        setMute: () => {},
      })),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(handle);

    expect(env.incomingCallController.getCall()?.state).toBe('ringing');
    expect(env.incomingCallController.getCall()?.callId).toBe('call-7');
  });

  it('cleanup stops the UA', async () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    await env.cleanup();

    expect(built[0].stop).toHaveBeenCalledTimes(1);
  });

  it('cleanup swallows UA stop errors (always resolves)', async () => {
    const {factory, built} = fakeFactory();
    built; // captured below
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });
    built[0].stop = jest.fn(async () => {
      throw new Error('transport already dead');
    });

    await expect(env.cleanup()).resolves.toBeUndefined();
  });

  // ── Guard kiosk auto-answer (DESIGN.md §9) ─────────────────────────
  //
  // The controller takes a `shouldAutoAnswer` predicate; createSipEnv
  // wires it to AND together the session's `autoAnswer` flag with the
  // role being `'guard'`. Mirrors `SipService.onIncoming` in the web
  // softphone — autoAnswer on a resident is silently ignored.

  it('auto-answers an incoming call for a guard with autoAnswer=true', async () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session({role: 'guard', autoAnswer: true}),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const acceptedHandle: SipCallHandle = {
      onStateChange: () => {},
      hangup: async () => {},
      setMute: () => {},
    };
    const inboundHandle: IncomingCallHandle = {
      info: {fromUri: 'sip:B-204@x', fromFlat: 'B-204', callId: 'call-G1'},
      accept: jest.fn(async () => acceptedHandle),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(inboundHandle);
    // Settle the controller's auto-`accept()` Promise chain.
    await Promise.resolve();
    await Promise.resolve();

    expect(inboundHandle.accept).toHaveBeenCalledTimes(1);
    expect(env.incomingCallController.getCall()?.state).not.toBe('ringing');
  });

  it('rings normally for a resident even if their autoAnswer is somehow true', () => {
    // Defence-in-depth: createSipEnv AND-s the role check. A back-end
    // bug or stale token shouldn't let a resident's flag silently
    // auto-accept calls on their phone.
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session({role: 'resident', autoAnswer: true}),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const inboundHandle: IncomingCallHandle = {
      info: {fromUri: 'sip:B-204@x', fromFlat: 'B-204', callId: 'call-R1'},
      accept: jest.fn(async () => ({
        onStateChange: () => {},
        hangup: async () => {},
        setMute: () => {},
      })),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(inboundHandle);

    expect(inboundHandle.accept).not.toHaveBeenCalled();
    expect(env.incomingCallController.getCall()?.state).toBe('ringing');
  });

  it('rings normally for a guard whose autoAnswer is undefined', () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session({role: 'guard'}), // autoAnswer omitted
      cloudBaseUrl: 'https://x',
      factory,
    });

    const inboundHandle: IncomingCallHandle = {
      info: {fromUri: 'sip:B-204@x', fromFlat: 'B-204', callId: 'call-G2'},
      accept: jest.fn(async () => ({
        onStateChange: () => {},
        hangup: async () => {},
        setMute: () => {},
      })),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(inboundHandle);

    expect(inboundHandle.accept).not.toHaveBeenCalled();
    expect(env.incomingCallController.getCall()?.state).toBe('ringing');
  });

  // ── Concurrent-call busy gate ───────────────────────────────────────
  //
  // The bridge's `isBusy` predicate is wired here to AND the OUTBOUND
  // (SipCallController) and INBOUND (IncomingCallController) views so
  // a second INVITE during an in-progress call is dropped with 486
  // Busy. Direct unit coverage of the predicate's truth table.

  function inboundInvite(callId: string): IncomingCallHandle {
    return {
      info: {fromUri: 'sip:Z@x', fromFlat: 'Z-001', callId},
      accept: jest.fn(async () => ({
        onStateChange: () => {},
        hangup: async () => {},
        setMute: () => {},
      })),
      reject: jest.fn(async () => {}),
    };
  }

  it('busy-rejects a second INVITE while the previous inbound is still ringing', () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const first = inboundInvite('call-A');
    built[0].incomingCb!(first);
    expect(env.incomingCallController.getCall()?.state).toBe('ringing');

    const second = inboundInvite('call-B');
    built[0].incomingCb!(second);

    expect(second.reject).toHaveBeenCalledWith('busy');
    expect(second.accept).not.toHaveBeenCalled();
    // First is unaffected — still the active ring.
    expect(env.incomingCallController.getCall()?.callId).toBe('call-A');
  });

  it('busy-rejects a second INVITE while the previous inbound is accepted', async () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const first = inboundInvite('call-A');
    built[0].incomingCb!(first);
    await env.incomingCallController.accept();
    expect(env.incomingCallController.getCall()?.state).toBe('accepted');

    const second = inboundInvite('call-B');
    built[0].incomingCb!(second);

    expect(second.reject).toHaveBeenCalledWith('busy');
  });

  it('allows a new INVITE after the previous ring was declined', () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    built[0].incomingCb!(inboundInvite('call-A'));
    env.incomingCallController.decline();
    expect(env.incomingCallController.getCall()?.state).toBe('declined');

    const second = inboundInvite('call-B');
    built[0].incomingCb!(second);

    expect(second.reject).not.toHaveBeenCalled();
    expect(env.incomingCallController.getCall()?.callId).toBe('call-B');
    expect(env.incomingCallController.getCall()?.state).toBe('ringing');
  });

  it('busy-rejects an INVITE while an outbound call is in progress', () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    // Stage an outbound call so SipCallController.getCall().state !== 'idle'.
    const outbound: SipCallHandle = {
      onStateChange: () => {},
      hangup: async () => {},
      setMute: () => {},
    };
    (built[0].placeCall as jest.Mock).mockImplementation(() => outbound);
    env.callController.placeCall('B-204');
    expect(env.callController.getCall().state).not.toBe('idle');

    const inv = inboundInvite('call-A');
    built[0].incomingCb!(inv);

    expect(inv.reject).toHaveBeenCalledWith('busy');
    expect(env.incomingCallController.getCall()).toBeNull();
  });

  // ── Inbound-call adoption + post-end busy-gate clear ──────────────
  //
  // The bridge's `onAnswered` hook hands the accepted SipCallHandle to
  // SipCallController via `adoptInboundCall`, which puts callController
  // into 'connected' so `<InCallPanel />` renders and `hangup()`
  // works for the inbound case the same way it does for outbound.
  //
  // When that call eventually ends, createSipEnv's
  // `callController.subscribe(call => if (state === 'ended')
  // incomingCallController.clear())` wiring drops the lingering
  // IncomingCall state so the busy gate clears and the next inbound
  // INVITE rings normally.

  function answeredHandle(): SipCallHandle {
    const listeners: Array<(c: {state: string}) => void> = [];
    return {
      onStateChange: cb =>
        listeners.push(cb as (c: {state: string}) => void),
      hangup: jest.fn(async () => {
        listeners.forEach(cb => cb({state: 'ended'}));
      }),
      setMute: jest.fn(),
    };
  }

  it('adopts an accepted inbound call into SipCallController', async () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const accepted = answeredHandle();
    const inv: IncomingCallHandle = {
      info: {fromUri: 'sip:B-204@x', fromFlat: 'B-204', callId: 'call-A'},
      accept: jest.fn(async () => accepted),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(inv);
    await env.incomingCallController.accept();

    expect(env.callController.getCall().state).toBe('connected');
    expect(env.callController.getCall().target).toBe('sip:B-204@pbx.local');
  });

  it('hanging up the adopted call clears the inbound state + frees the busy gate', async () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const accepted = answeredHandle();
    const inv: IncomingCallHandle = {
      info: {fromUri: 'sip:B-204@x', fromFlat: 'B-204', callId: 'call-A'},
      accept: jest.fn(async () => accepted),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(inv);
    await env.incomingCallController.accept();
    expect(env.callController.getCall().state).toBe('connected');

    // Hangup drives FakeAnsweredHandle's 'ended' emission, which the
    // controller's onSipState turns into a 'remoteHangup' dispatch
    // because we didn't go through the controller's local hangup path.
    env.callController.hangup();

    expect(env.callController.getCall().state).toBe('ended');
    // createSipEnv's subscribe → clear() wiring fired.
    expect(env.incomingCallController.getCall()).toBeNull();

    // A new INVITE should now ring (busy gate is clear).
    const next: IncomingCallHandle = {
      info: {fromUri: 'sip:C-301@x', fromFlat: 'C-301', callId: 'call-B'},
      accept: jest.fn(async () => answeredHandle()),
      reject: jest.fn(async () => {}),
    };
    built[0].incomingCb!(next);

    expect(next.reject).not.toHaveBeenCalled();
    expect(env.incomingCallController.getCall()?.callId).toBe('call-B');
  });

  it('allows a fresh outbound dial after the previous call ended', () => {
    const {factory, built} = fakeFactory();
    const env = createSipEnv({
      session: session(),
      cloudBaseUrl: 'https://x',
      factory,
    });

    const first: SipCallHandle = {
      onStateChange: () => {},
      hangup: async () => {},
      setMute: () => {},
    };
    const second: SipCallHandle = {
      onStateChange: () => {},
      hangup: async () => {},
      setMute: () => {},
    };
    const placeCallMock = built[0].placeCall as jest.Mock;
    placeCallMock.mockImplementationOnce(() => first);
    placeCallMock.mockImplementationOnce(() => second);
    env.callController.placeCall('B-204');
    env.callController.hangup();
    expect(env.callController.getCall().state).toBe('ended');

    env.callController.placeCall('C-301');

    expect(placeCallMock).toHaveBeenCalledTimes(2);
    expect(env.callController.getCall().state).toBe('calling');
  });
});
