/**
 * TDD layer M3.b sip.js integration — SipInboundBridge.
 *
 * Wires the ui-mirrored `SipUaHandle.onIncomingCall` channel into the
 * existing `IncomingCallController.reportPush` entry point AND
 * implements the M3.b `IncomingCallSignaling` seam over a Call-ID →
 * `IncomingCallHandle` registry. Same control flow as PushKit/FCM —
 * just sourced from an in-process UA delivery instead of an OS push.
 */
import {SipInboundBridge} from '../sipIncomingSignaling';
import {
  IncomingCallHandle,
  SipCallHandle,
  SipUaHandle,
  SipUaStateChange,
} from '../../sip/sipUa';

class FakeIncomingHandle implements IncomingCallHandle {
  info: IncomingCallHandle['info'];
  accept = jest.fn(
    async (): Promise<SipCallHandle> => ({
      onStateChange: () => {},
      hangup: async () => {},
      setMute: () => {},
    }),
  );
  reject = jest.fn(async (_cause?: 'busy' | 'declined') => {});
  constructor(callId: string, fromFlat: string) {
    this.info = {fromUri: `sip:${fromFlat}@x`, fromFlat, callId};
  }
}

class FakeSipUa implements SipUaHandle {
  private incomingCb: (h: IncomingCallHandle) => void = () => {};
  start = jest.fn(async () => {});
  stop = jest.fn(async () => {});
  onStateChange = jest.fn((_cb: (c: SipUaStateChange) => void): void => {});
  placeCall = jest.fn((_: string): SipCallHandle => {
    throw new Error('not used in inbound tests');
  });
  onIncomingCall = jest.fn((cb: (h: IncomingCallHandle) => void): void => {
    this.incomingCb = cb;
  });
  /** Test driver — simulate the UA delivering a fresh INVITE. */
  deliver(handle: IncomingCallHandle): void {
    this.incomingCb(handle);
  }
}

function setup() {
  const ua = new FakeSipUa();
  const controller = {reportPush: jest.fn()};
  const bridge = new SipInboundBridge(ua, controller);
  return {bridge, ua, controller};
}

describe('SipInboundBridge — UA→controller wiring', () => {
  it('subscribes to ua.onIncomingCall on construction', () => {
    const {ua} = setup();
    expect(ua.onIncomingCall).toHaveBeenCalledTimes(1);
  });

  it('reports an inbound INVITE to the controller as an incoming-call push', () => {
    const {ua, controller} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');

    ua.deliver(inv);

    expect(controller.reportPush).toHaveBeenCalledWith({
      type: 'incoming-call',
      callId: 'call-7',
      callerFlat: 'B-204',
      callerName: 'B-204',
    });
  });
});

describe('SipInboundBridge — IncomingCallSignaling', () => {
  it('answer(callId) accepts the matching INVITE', async () => {
    const {bridge, ua} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');
    ua.deliver(inv);

    await bridge.answer('call-7');

    expect(inv.accept).toHaveBeenCalledTimes(1);
  });

  it('answer(unknown callId) is a no-op — no throw', async () => {
    const {bridge} = setup();
    await expect(bridge.answer('nope')).resolves.toBeUndefined();
  });

  it('reject(callId, 603) rejects with cause "declined"', () => {
    const {bridge, ua} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');
    ua.deliver(inv);

    bridge.reject('call-7', 603);

    expect(inv.reject).toHaveBeenCalledWith('declined');
  });

  it('reject(callId, 486) rejects with cause "busy"', () => {
    const {bridge, ua} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');
    ua.deliver(inv);

    bridge.reject('call-7', 486);

    expect(inv.reject).toHaveBeenCalledWith('busy');
  });

  it('reject(unknown callId) is a silent no-op', () => {
    const {bridge} = setup();
    expect(() => bridge.reject('nope', 603)).not.toThrow();
  });

  it('removes the handle from the registry after accept', async () => {
    const {bridge, ua} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');
    ua.deliver(inv);

    await bridge.answer('call-7');
    await bridge.answer('call-7'); // second accept must NOT re-call

    expect(inv.accept).toHaveBeenCalledTimes(1);
  });

  it('removes the handle from the registry after reject', () => {
    const {bridge, ua} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');
    ua.deliver(inv);

    bridge.reject('call-7', 603);
    bridge.reject('call-7', 603); // second reject must NOT re-call

    expect(inv.reject).toHaveBeenCalledTimes(1);
  });
});

// ── Concurrent-call busy gate ──────────────────────────────────────────
//
// Web softphone: `SipService.onIncoming` short-circuits with
// `void h.reject('busy')` when there's already an active call or
// pending incoming. Mobile mirrors that via the bridge's `isBusy`
// predicate so a second INVITE during an in-progress call never
// reaches the IncomingCallController (which would otherwise overwrite
// state via the narrowed PR #164 ringReducer).

function busySetup(isBusy: () => boolean) {
  const ua = new FakeSipUa();
  const controller = {reportPush: jest.fn()};
  const bridge = new SipInboundBridge(ua, controller, isBusy);
  return {bridge, ua, controller};
}

describe('SipInboundBridge — busy gate', () => {
  it('rejects with busy when isBusy returns true and never invokes the controller', () => {
    const {ua, controller} = busySetup(() => true);
    const inv = new FakeIncomingHandle('call-7', 'B-204');

    ua.deliver(inv);

    expect(inv.reject).toHaveBeenCalledWith('busy');
    expect(controller.reportPush).not.toHaveBeenCalled();
  });

  it('lets the INVITE through when isBusy returns false', () => {
    const {ua, controller} = busySetup(() => false);
    const inv = new FakeIncomingHandle('call-7', 'B-204');

    ua.deliver(inv);

    expect(inv.reject).not.toHaveBeenCalled();
    expect(controller.reportPush).toHaveBeenCalledTimes(1);
  });

  it('lets the INVITE through when no isBusy is configured (default)', () => {
    // Backwards-compatible: existing call sites that built a bridge
    // without the gate keep working.
    const {ua, controller} = setup();
    const inv = new FakeIncomingHandle('call-7', 'B-204');

    ua.deliver(inv);

    expect(inv.reject).not.toHaveBeenCalled();
    expect(controller.reportPush).toHaveBeenCalledTimes(1);
  });

  it('does not register the rejected handle (later answer/reject is a no-op)', async () => {
    const {bridge, ua} = busySetup(() => true);
    const inv = new FakeIncomingHandle('call-7', 'B-204');
    ua.deliver(inv);

    await bridge.answer('call-7');

    expect(inv.accept).not.toHaveBeenCalled();
  });
});
