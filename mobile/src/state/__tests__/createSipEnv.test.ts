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
});
