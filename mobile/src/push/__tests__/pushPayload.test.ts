/**
 * TDD layer M3.a — incoming-call wake-up push parsing.
 */
import {parseWakeUp} from '../pushPayload';

describe('parseWakeUp', () => {
  it('parses a well-formed wake-up payload', () => {
    expect(
      parseWakeUp({
        type: 'incoming-call',
        callId: 'call-7',
        callerFlat: 'A-101',
        callerName: 'Resident A101',
      }),
    ).toEqual({
      callId: 'call-7',
      callerFlat: 'A-101',
      callerName: 'Resident A101',
    });
  });

  it('defaults callerName to the flat number when it is absent', () => {
    expect(
      parseWakeUp({
        type: 'incoming-call',
        callId: 'call-7',
        callerFlat: 'A-101',
      }),
    ).toEqual({callId: 'call-7', callerFlat: 'A-101', callerName: 'A-101'});
  });

  it('returns null for a payload that is not an incoming-call', () => {
    expect(parseWakeUp({type: 'something-else', callId: 'x'})).toBeNull();
  });

  it('returns null when callId or callerFlat is missing or empty', () => {
    expect(parseWakeUp({type: 'incoming-call', callerFlat: 'A-101'})).toBeNull();
    expect(parseWakeUp({type: 'incoming-call', callId: 'call-7'})).toBeNull();
    expect(
      parseWakeUp({type: 'incoming-call', callId: '', callerFlat: 'A-101'}),
    ).toBeNull();
  });

  it('returns null for non-object input — never throws', () => {
    expect(parseWakeUp(null)).toBeNull();
    expect(parseWakeUp(undefined)).toBeNull();
    expect(parseWakeUp('incoming-call')).toBeNull();
    expect(parseWakeUp(42)).toBeNull();
  });
});
