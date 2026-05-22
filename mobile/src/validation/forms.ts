/**
 * Form validation — TDD layer M1.a.
 *
 * Pure functions, no React. Client-side validation only blocks the
 * obvious (empty required fields, weak new password, malformed email);
 * the cloud remains the authority for everything else.
 */

export interface LoginFormInput {
  society: string;
  flat: string;
  password: string;
}

export interface RegisterFormInput {
  society: string;
  flat: string;
  residentName: string;
  password: string;
  mobile: string; // optional field — '' when the user left it blank
  email: string; //  optional field — '' when the user left it blank
}

export type FieldErrors = Partial<Record<string, string>>;

export type ValidationResult =
  | {ok: true}
  | {ok: false; errors: FieldErrors};

/** Minimum length for a NEW password (registration only). */
export const PASSWORD_MIN_LENGTH = 8;

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

export function validateLoginForm(input: LoginFormInput): ValidationResult {
  const errors: FieldErrors = {};
  if (!input.society.trim()) {
    errors.society = 'Society is required';
  }
  if (!input.flat.trim()) {
    errors.flat = 'Flat number is required';
  }
  // Login does not enforce the password policy — an existing password
  // predates the policy; only require that something was entered.
  if (!input.password) {
    errors.password = 'Password is required';
  }
  return finish(errors);
}

export function validateRegisterForm(
  input: RegisterFormInput,
): ValidationResult {
  const errors: FieldErrors = {};
  if (!input.society.trim()) {
    errors.society = 'Society is required';
  }
  if (!input.flat.trim()) {
    errors.flat = 'Flat number is required';
  }
  if (!input.residentName.trim()) {
    errors.residentName = 'Resident name is required';
  }
  if (!input.password) {
    errors.password = 'Password is required';
  } else if (input.password.length < PASSWORD_MIN_LENGTH) {
    errors.password = `Password must be at least ${PASSWORD_MIN_LENGTH} characters`;
  }
  // Mobile and email are optional — validated only when provided.
  if (input.email.trim() && !EMAIL_RE.test(input.email.trim())) {
    errors.email = 'Enter a valid email address';
  }
  return finish(errors);
}

function finish(errors: FieldErrors): ValidationResult {
  return Object.keys(errors).length === 0
    ? {ok: true}
    : {ok: false, errors};
}
