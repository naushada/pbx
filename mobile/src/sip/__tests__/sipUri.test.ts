/**
 * TDD layer M2.a — SIP request-URI builder.
 */
import {callTargetUri, isDialableFlat} from '../sipUri';

describe('callTargetUri', () => {
  it('builds sip:<flat>@pbx.local for a flat number', () => {
    expect(callTargetUri('A-101')).toBe('sip:A-101@pbx.local');
  });

  it('trims surrounding whitespace from the flat', () => {
    expect(callTargetUri('  A-101 ')).toBe('sip:A-101@pbx.local');
  });

  it('honours an explicit realm', () => {
    expect(callTargetUri('101', 'pbx.example')).toBe('sip:101@pbx.example');
  });

  it('rejects an empty or whitespace-only flat', () => {
    expect(() => callTargetUri('')).toThrow();
    expect(() => callTargetUri('   ')).toThrow();
  });

  it('rejects a flat with characters that would break the URI', () => {
    expect(() => callTargetUri('a@b')).toThrow();
    expect(() => callTargetUri('a b')).toThrow();
    expect(() => callTargetUri('a/b')).toThrow();
  });
});

describe('isDialableFlat', () => {
  it('accepts typical flat numbers', () => {
    expect(isDialableFlat('A-101')).toBe(true);
    expect(isDialableFlat('101')).toBe(true);
    expect(isDialableFlat('  B-204 ')).toBe(true);
  });

  it('rejects empty or malformed input', () => {
    expect(isDialableFlat('')).toBe(false);
    expect(isDialableFlat('-101')).toBe(false);
    expect(isDialableFlat('a/b')).toBe(false);
  });
});
