/**
 * Shared API types — TDD layer M1.
 *
 * `Subscriber` / `Session` are the shapes the cloud returns from
 * login and registration. `ApiError` is the single typed error every
 * cloud call raises, so callers branch on `code`, never on a raw
 * string or HTTP status.
 */

export interface Subscriber {
  societyId: string;
  flatNumber: string;
  sipUsername: string;
  displayName: string;
  role: string;
}

export interface Session {
  token: string;
  subscriber: Subscriber;
}

export type ApiErrorCode =
  | 'INVALID_CREDENTIALS' // wrong flat / password (HTTP 401)
  | 'UNKNOWN_SOCIETY' //     no such society (HTTP 404)
  | 'DUPLICATE' //           account already exists (HTTP 409)
  | 'RATE_LIMITED' //        too many attempts (HTTP 429)
  | 'NETWORK' //             transport failed — offline / unreachable
  | 'SERVER'; //             5xx or a malformed response

/** The one error type every CloudClient call rejects with. */
export class ApiError extends Error {
  constructor(
    public readonly code: ApiErrorCode,
    message: string,
  ) {
    super(message);
    this.name = 'ApiError';
  }

  /** A message safe to render to the user as-is. */
  get userMessage(): string {
    switch (this.code) {
      case 'INVALID_CREDENTIALS':
        return 'Wrong flat number or password.';
      case 'UNKNOWN_SOCIETY':
        return 'We could not find that society.';
      case 'DUPLICATE':
        return 'An account for this flat already exists.';
      case 'RATE_LIMITED':
        return 'Too many attempts — please try again shortly.';
      case 'NETWORK':
        return 'Network error — check your connection and try again.';
      case 'SERVER':
        return 'Something went wrong on our side. Please try again.';
    }
  }
}
