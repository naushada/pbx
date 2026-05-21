/**
 * TDD layer M1.a — form validation.
 */
import {
  validateLoginForm,
  validateRegisterForm,
  PASSWORD_MIN_LENGTH,
} from '../forms';

describe('validateLoginForm', () => {
  const good = {society: 'SUNSET', flat: 'A-101', password: 'anything'};

  it('accepts a fully-filled form', () => {
    expect(validateLoginForm(good)).toEqual({ok: true});
  });

  it('rejects empty society / flat / password', () => {
    const r = validateLoginForm({society: '  ', flat: '', password: ''});
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.errors.society).toBeDefined();
      expect(r.errors.flat).toBeDefined();
      expect(r.errors.password).toBeDefined();
    }
  });

  it('does NOT enforce the password policy on login', () => {
    // An existing password may predate the policy — logging in must
    // not be blocked by length.
    expect(validateLoginForm({...good, password: 'x'})).toEqual({ok: true});
  });
});

describe('validateRegisterForm', () => {
  const good = {
    society: 'SUNSET',
    flat: 'A-101',
    residentName: 'Asha Rao',
    password: 'longenough',
    mobile: '',
    email: '',
  };

  it('accepts the required fields with optional mobile/email blank', () => {
    expect(validateRegisterForm(good)).toEqual({ok: true});
  });

  it('requires society, flat, resident name and password', () => {
    const r = validateRegisterForm({
      society: '',
      flat: ' ',
      residentName: '',
      password: '',
      mobile: '',
      email: '',
    });
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(Object.keys(r.errors).sort()).toEqual([
        'flat',
        'password',
        'residentName',
        'society',
      ]);
    }
  });

  it('rejects a new password shorter than the policy minimum', () => {
    const r = validateRegisterForm({...good, password: 'short'});
    expect(r.ok).toBe(false);
    if (!r.ok) {
      expect(r.errors.password).toContain(String(PASSWORD_MIN_LENGTH));
    }
  });

  it('accepts a blank email but rejects a malformed one', () => {
    expect(validateRegisterForm({...good, email: ''}).ok).toBe(true);
    expect(validateRegisterForm({...good, email: 'resident@example.com'}).ok).toBe(
      true,
    );
    const bad = validateRegisterForm({...good, email: 'not-an-email'});
    expect(bad.ok).toBe(false);
    if (!bad.ok) {
      expect(bad.errors.email).toBeDefined();
    }
  });
});
