/**
 * SIP request-URI for an outbound call — TDD layer M2.a.
 *
 * Calling a flat sends `INVITE sip:<flat>@<realm>`. The realm host must
 * be RFC 3261-valid — its top label starts with a letter and carries no
 * underscores (see PR #76 / #117). `pbx.local` is the fixed,
 * underscore-free realm the whole stack already uses.
 */
const DEFAULT_REALM = 'pbx.local';

/** A dialable flat: starts alphanumeric, then alphanumerics / `.` `_` `-`. */
const FLAT_RE = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;

/** True when `flat` is safe to place in a SIP URI user-part. */
export function isDialableFlat(flat: string): boolean {
  return FLAT_RE.test(flat.trim());
}

/**
 * Build the SIP request-URI for calling `flat`. Throws if `flat` is not
 * a dialable flat number (empty, or characters that would break the URI).
 */
export function callTargetUri(
  flat: string,
  realm: string = DEFAULT_REALM,
): string {
  const f = flat.trim();
  if (!isDialableFlat(f)) {
    throw new Error(`not a dialable flat number: "${flat}"`);
  }
  return `sip:${f}@${realm}`;
}
