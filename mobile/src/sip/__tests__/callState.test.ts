/**
 * TDD layer M2.a — call state machine.
 */
import {callReducer, idleCall, isCallActive} from '../callState';

describe('callReducer', () => {
  it('idle + dial → calling, with the target recorded', () => {
    const c = callReducer(idleCall, {
      type: 'dial',
      target: 'sip:A-101@pbx.local',
    });
    expect(c.state).toBe('calling');
    expect(c.target).toBe('sip:A-101@pbx.local');
    expect(c.endReason).toBeNull();
  });

  it('calling + answered → connected', () => {
    let c = callReducer(idleCall, {type: 'dial', target: 't'});
    c = callReducer(c, {type: 'answered'});
    expect(c.state).toBe('connected');
  });

  it('a second dial while a call is in flight is a no-op', () => {
    const calling = callReducer(idleCall, {type: 'dial', target: 't1'});
    const again = callReducer(calling, {type: 'dial', target: 't2'});
    expect(again).toBe(calling); // unchanged reference
    expect(again.target).toBe('t1');
  });

  it('connected + remoteHangup → ended', () => {
    let c = callReducer(idleCall, {type: 'dial', target: 't'});
    c = callReducer(c, {type: 'answered'});
    c = callReducer(c, {type: 'remoteHangup'});
    expect(c.state).toBe('ended');
    expect(c.endReason).toBe('remote hung up');
  });

  it('calling + localHangup → ended', () => {
    let c = callReducer(idleCall, {type: 'dial', target: 't'});
    c = callReducer(c, {type: 'localHangup'});
    expect(c.state).toBe('ended');
    expect(c.endReason).toBe('hung up');
  });

  it('calling + rejected → ended, carrying the reason', () => {
    let c = callReducer(idleCall, {type: 'dial', target: 't'});
    c = callReducer(c, {type: 'rejected', reason: 'busy'});
    expect(c.state).toBe('ended');
    expect(c.endReason).toBe('busy');
  });

  it('ignores answered unless the call is calling', () => {
    expect(callReducer(idleCall, {type: 'answered'})).toBe(idleCall);
  });

  it('a hangup after the call already ended is a no-op', () => {
    let c = callReducer(idleCall, {type: 'dial', target: 't'});
    c = callReducer(c, {type: 'localHangup'});
    expect(callReducer(c, {type: 'remoteHangup'})).toBe(c);
  });

  // Narrowed re-dial guard — was `state !== 'idle'`, blocks post-ended
  // re-dial. Now uses `isCallActive`, allowing a fresh call after the
  // previous one ended.
  it('allows a fresh dial after a previous call ended', () => {
    let c = callReducer(idleCall, {type: 'dial', target: 'A'});
    c = callReducer(c, {type: 'localHangup'}); // ended
    const next = callReducer(c, {type: 'dial', target: 'B'});
    expect(next.state).toBe('calling');
    expect(next.target).toBe('B');
    expect(next.endReason).toBeNull();
  });

  it('blocks a dial while a call is still calling or connected', () => {
    const calling = callReducer(idleCall, {type: 'dial', target: 'A'});
    expect(callReducer(calling, {type: 'dial', target: 'B'})).toBe(calling);

    const connected = callReducer(calling, {type: 'answered'});
    expect(callReducer(connected, {type: 'dial', target: 'C'})).toBe(connected);
  });

  describe("'adopted' event (inbound-call handoff)", () => {
    it('jumps from idle straight to connected with the target', () => {
      const c = callReducer(idleCall, {
        type: 'adopted',
        target: 'sip:B-204@pbx.local',
      });
      expect(c.state).toBe('connected');
      expect(c.target).toBe('sip:B-204@pbx.local');
      expect(c.endReason).toBeNull();
    });

    it('also allowed after a previous call ended', () => {
      let c = callReducer(idleCall, {type: 'dial', target: 'A'});
      c = callReducer(c, {type: 'localHangup'});
      const adopted = callReducer(c, {type: 'adopted', target: 'sip:X@pbx'});
      expect(adopted.state).toBe('connected');
    });

    it('refuses to overwrite a live call', () => {
      const calling = callReducer(idleCall, {type: 'dial', target: 'A'});
      expect(
        callReducer(calling, {type: 'adopted', target: 'sip:B@pbx'}),
      ).toBe(calling);

      const connected = callReducer(calling, {type: 'answered'});
      expect(
        callReducer(connected, {type: 'adopted', target: 'sip:C@pbx'}),
      ).toBe(connected);
    });

    it('a localHangup ends the adopted call cleanly', () => {
      const c = callReducer(idleCall, {type: 'adopted', target: 'sip:X@pbx'});
      const ended = callReducer(c, {type: 'localHangup'});
      expect(ended.state).toBe('ended');
      expect(ended.endReason).toBe('hung up');
    });
  });
});

describe('isCallActive', () => {
  it('is true while calling or connected, false otherwise', () => {
    expect(isCallActive(idleCall)).toBe(false);
    const calling = callReducer(idleCall, {type: 'dial', target: 't'});
    expect(isCallActive(calling)).toBe(true);
    expect(isCallActive(callReducer(calling, {type: 'answered'}))).toBe(true);
    expect(isCallActive(callReducer(calling, {type: 'localHangup'}))).toBe(
      false,
    );
  });
});
