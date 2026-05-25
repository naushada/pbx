/**
 * TDD layer M2 — sip.js integration slice.
 *
 * `SipCallController` adapts the proven `SipUaHandle` seam (mirrored
 * verbatim from ui/sip-ua-sipjs.ts) into the mobile-side `CallController`
 * interface the screens already depend on. The state-machine driving
 * the Dial UI is unchanged (`callReducer` from M2.a); this adapter
 * just maps the sip.js call lifecycle into reducer events.
 */
import {SipCallController} from '../sipCallController';
import {
  IncomingCallHandle,
  SipCallHandle,
  SipCallStateChange,
  SipUaHandle,
  SipUaStateChange,
} from '../../sip/sipUa';

class FakeSipCallHandle implements SipCallHandle {
  hangup = jest.fn(async () => {});
  setMute = jest.fn();
  private listeners: Array<(c: SipCallStateChange) => void> = [];

  onStateChange(cb: (c: SipCallStateChange) => void): void {
    this.listeners.push(cb);
  }

  /** Test driver — push a sip.js-shaped state event to subscribers. */
  emit(change: SipCallStateChange): void {
    for (const cb of this.listeners) cb(change);
  }
}

class FakeSipUa implements SipUaHandle {
  callHandle = new FakeSipCallHandle();
  placeCall = jest.fn((_target: string): SipCallHandle => this.callHandle);
  stop = jest.fn(async () => {});
  start = jest.fn(async () => {});
  onStateChange = jest.fn((_cb: (c: SipUaStateChange) => void): void => {});
  onIncomingCall = jest.fn(
    (_cb: (h: IncomingCallHandle) => void): void => {},
  );
}

function setup() {
  const ua = new FakeSipUa();
  const controller = new SipCallController(ua);
  return {controller, ua};
}

describe('SipCallController.placeCall', () => {
  it('builds the SIP URI and hands it to the UA', () => {
    const {controller, ua} = setup();

    controller.placeCall('A-101');

    expect(ua.placeCall).toHaveBeenCalledTimes(1);
    expect(ua.placeCall.mock.calls[0][0]).toMatch(/^sip:A-101@/);
  });

  it('flips to "calling" with the target right away', () => {
    const {controller} = setup();

    controller.placeCall('A-101');

    expect(controller.getCall().state).toBe('calling');
    expect(controller.getCall().target).toMatch(/^sip:A-101@/);
  });

  it('flips to "connected" when sip emits "in-call"', () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');

    ua.callHandle.emit({state: 'in-call'});

    expect(controller.getCall().state).toBe('connected');
  });

  it('flips to "ended" with "remote hung up" when peer ends', () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');
    ua.callHandle.emit({state: 'in-call'});

    ua.callHandle.emit({state: 'ended'});

    const call = controller.getCall();
    expect(call.state).toBe('ended');
    expect(call.endReason).toBe('remote hung up');
  });

  it('flips to "ended" with the sip detail when sip fails', () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');

    ua.callHandle.emit({state: 'failed', detail: 'sip_486'});

    const call = controller.getCall();
    expect(call.state).toBe('ended');
    expect(call.endReason).toBe('sip_486');
  });

  it('ignores an undialable flat — no SIP placeCall, no state change', () => {
    const {controller, ua} = setup();

    controller.placeCall('not a flat number');

    expect(ua.placeCall).not.toHaveBeenCalled();
    expect(controller.getCall().state).toBe('idle');
  });

  it('first-call-wins: a second placeCall during an active call is ignored', () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');

    controller.placeCall('B-204');

    expect(ua.placeCall).toHaveBeenCalledTimes(1);
    expect(controller.getCall().target).toMatch(/A-101/);
  });
});

describe('SipCallController.hangup', () => {
  it('sends BYE/CANCEL via the sip call handle and ends locally', async () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');
    ua.callHandle.emit({state: 'in-call'});

    controller.hangup();

    expect(ua.callHandle.hangup).toHaveBeenCalledTimes(1);
    expect(controller.getCall().state).toBe('ended');
    expect(controller.getCall().endReason).toBe('hung up');
  });

  it('treats the subsequent sip "ended" event as a no-op (no double-ended)', () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');
    ua.callHandle.emit({state: 'in-call'});
    controller.hangup();
    const reasonBefore = controller.getCall().endReason;

    ua.callHandle.emit({state: 'ended'});

    expect(controller.getCall().endReason).toBe(reasonBefore);
  });

  it('is a safe no-op when idle', () => {
    const {controller, ua} = setup();

    controller.hangup();

    expect(ua.callHandle.hangup).not.toHaveBeenCalled();
    expect(controller.getCall().state).toBe('idle');
  });
});

describe('SipCallController.setMuted', () => {
  it('mutes the sip handle when a call is in flight', () => {
    const {controller, ua} = setup();
    controller.placeCall('A-101');

    controller.setMuted(true);

    expect(ua.callHandle.setMute).toHaveBeenCalledWith(true);
    expect(controller.isMuted()).toBe(true);
  });

  it('remembers the mute flag even with no active sip call', () => {
    const {controller} = setup();

    controller.setMuted(true);

    expect(controller.isMuted()).toBe(true);
  });
});

describe('SipCallController.subscribe', () => {
  it('notifies subscribers on every state transition', () => {
    const {controller, ua} = setup();
    const seen: string[] = [];
    controller.subscribe(c => seen.push(c.state));

    controller.placeCall('A-101');
    ua.callHandle.emit({state: 'in-call'});
    ua.callHandle.emit({state: 'ended'});

    expect(seen).toEqual(['calling', 'connected', 'ended']);
  });

  it('returns an unsubscribe function', () => {
    const {controller, ua} = setup();
    const cb = jest.fn();
    const unsubscribe = controller.subscribe(cb);

    unsubscribe();
    controller.placeCall('A-101');
    ua.callHandle.emit({state: 'in-call'});

    expect(cb).not.toHaveBeenCalled();
  });
});
