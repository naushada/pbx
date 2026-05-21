/**
 * FakeCloud — TDD layer M0.3.
 *
 * An in-memory stand-in for the onprem-pbx cloud control plane. Unit
 * and integration tests run against this instead of a real backend,
 * so they are fast, hermetic, and need no network.
 *
 * It models the REST behaviour the mobile app depends on
 * (docs/design/mobile-app.md §5): society-name resolution, login, and
 * ungated self-registration. The `/sip-ws` tunnel double is added in
 * TDD layer M2, when the calling code first needs it.
 *
 * `register()` is intentionally **ungated** — a new account is usable
 * immediately. That mirrors the v1 product decision and its documented
 * risk (mobile-app.md §9); tests assert that behaviour so a future
 * move to a gated model is a deliberate, test-visible change.
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

export type FakeCloudErrorCode =
  | 'UNKNOWN_SOCIETY'
  | 'INVALID_CREDENTIALS';

/** A typed error so callers (and tests) can branch on `code`. */
export class FakeCloudError extends Error {
  constructor(
    public readonly code: FakeCloudErrorCode,
    message: string,
  ) {
    super(message);
    this.name = 'FakeCloudError';
  }
}

export interface RegisterInput {
  societyName: string;
  flatNumber: string;
  residentName: string;
  password: string;
  mobile?: string;
  email?: string;
}

interface StoredSub extends Subscriber {
  password: string;
}

export class FakeCloud {
  private readonly societies = new Map<string, string>(); // name → societyId
  private readonly subs: StoredSub[] = [];
  private seq = 0;

  /** Seed an existing society (residents can only join societies that exist). */
  seedSociety(name: string, societyId: string): void {
    this.societies.set(name.trim().toLowerCase(), societyId);
  }

  /** Seed a pre-existing subscriber (e.g. an admin-imported resident). */
  seedSubscriber(sub: Subscriber, password: string): void {
    this.subs.push({...sub, password});
  }

  /** Resolve a society name/code → societyId. Throws UNKNOWN_SOCIETY. */
  resolveSociety(name: string): string {
    const id = this.societies.get(name.trim().toLowerCase());
    if (id === undefined) {
      throw new FakeCloudError('UNKNOWN_SOCIETY', `no society named "${name}"`);
    }
    return id;
  }

  /** POST /api/v1/subscriber/login. Throws UNKNOWN_SOCIETY / INVALID_CREDENTIALS. */
  login(societyName: string, flatNumber: string, password: string): Session {
    const societyId = this.resolveSociety(societyName);
    const sub = this.subs.find(
      s =>
        s.societyId === societyId &&
        s.flatNumber === flatNumber &&
        s.password === password,
    );
    if (sub === undefined) {
      throw new FakeCloudError(
        'INVALID_CREDENTIALS',
        'wrong flat number or password',
      );
    }
    return {token: this.issueToken(sub), subscriber: strip(sub)};
  }

  /** POST /api/v1/subscriber/register — ungated; account is active at once. */
  register(input: RegisterInput): Session {
    const societyId = this.resolveSociety(input.societyName);
    const sub: StoredSub = {
      societyId,
      flatNumber: input.flatNumber,
      sipUsername: this.mintSipUsername(societyId, input.flatNumber),
      displayName: input.residentName,
      role: 'resident',
      password: input.password,
    };
    this.subs.push(sub);
    return {token: this.issueToken(sub), subscriber: strip(sub)};
  }

  /** Test observability — how many subscribers exist. */
  subscriberCount(): number {
    return this.subs.length;
  }

  private issueToken(sub: StoredSub): string {
    return `faketoken.${sub.sipUsername}.${++this.seq}`;
  }

  /**
   * Mint a unique, SIP-safe sipUsername — `<societyId>-<flat>-<n>`,
   * lower-cased, non-alphanumerics collapsed to '-' (mobile-app.md §5.2).
   */
  private mintSipUsername(societyId: string, flat: string): string {
    const safe = (v: string) =>
      v.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
    return `${safe(societyId)}-${safe(flat)}-${++this.seq}`;
  }
}

/** Drop the password before a subscriber ever leaves the fake. */
function strip(sub: StoredSub): Subscriber {
  const {password: _password, ...rest} = sub;
  return rest;
}
