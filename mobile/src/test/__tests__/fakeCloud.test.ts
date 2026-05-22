/**
 * TDD layer M0.3 — test of the FakeCloud double itself.
 *
 * The fake backend is test infrastructure that M1+ relies on, so its
 * own behaviour is pinned down here before anything is built on it.
 */
import {FakeCloud, FakeCloudError} from '../fakeCloud';

function seeded(): FakeCloud {
  const cloud = new FakeCloud();
  cloud.seedSociety('SUNSET', 'soc_sunset');
  cloud.seedSubscriber(
    {
      societyId: 'soc_sunset',
      flatNumber: 'A-101',
      sipUsername: 'a101',
      displayName: 'Resident A101',
      role: 'resident',
    },
    'pw-a101',
  );
  return cloud;
}

describe('FakeCloud — society resolution', () => {
  it('resolves a seeded society name to its id (case-insensitive)', () => {
    expect(seeded().resolveSociety('sunset')).toBe('soc_sunset');
  });

  it('throws UNKNOWN_SOCIETY for a society that does not exist', () => {
    try {
      seeded().resolveSociety('NOWHERE');
      fail('expected FakeCloudError');
    } catch (e) {
      expect(e).toBeInstanceOf(FakeCloudError);
      expect((e as FakeCloudError).code).toBe('UNKNOWN_SOCIETY');
    }
  });
});

describe('FakeCloud — login', () => {
  it('returns a token and subscriber for correct credentials', () => {
    const s = seeded().login('SUNSET', 'A-101', 'pw-a101');
    expect(s.token).toBeTruthy();
    expect(s.subscriber.sipUsername).toBe('a101');
    expect(s.subscriber.flatNumber).toBe('A-101');
  });

  it('never returns the password on the subscriber', () => {
    const s = seeded().login('SUNSET', 'A-101', 'pw-a101');
    expect((s.subscriber as Record<string, unknown>).password).toBeUndefined();
  });

  it('throws INVALID_CREDENTIALS for a wrong password', () => {
    try {
      seeded().login('SUNSET', 'A-101', 'wrong');
      fail('expected FakeCloudError');
    } catch (e) {
      expect((e as FakeCloudError).code).toBe('INVALID_CREDENTIALS');
    }
  });

  it('throws UNKNOWN_SOCIETY when the society does not exist', () => {
    try {
      seeded().login('NOWHERE', 'A-101', 'pw-a101');
      fail('expected FakeCloudError');
    } catch (e) {
      expect((e as FakeCloudError).code).toBe('UNKNOWN_SOCIETY');
    }
  });
});

describe('FakeCloud — registration (ungated)', () => {
  it('creates an immediately-usable account and auto-logs-in', () => {
    const cloud = seeded();
    const before = cloud.subscriberCount();

    const s = cloud.register({
      societyName: 'SUNSET',
      flatNumber: 'B-204',
      residentName: 'Asha Rao',
      password: 'pw-asha',
    });

    expect(cloud.subscriberCount()).toBe(before + 1);
    expect(s.token).toBeTruthy(); // auto-login — no second login needed
    expect(s.subscriber.sipUsername).toMatch(/soc-sunset-b-204/);
    expect(s.subscriber.role).toBe('resident');
  });

  it('lets the new account log in right away — no approval step', () => {
    const cloud = seeded();
    cloud.register({
      societyName: 'SUNSET',
      flatNumber: 'B-204',
      residentName: 'Asha Rao',
      password: 'pw-asha',
    });
    expect(() => cloud.login('SUNSET', 'B-204', 'pw-asha')).not.toThrow();
  });

  it('rejects registration into a society that does not exist', () => {
    try {
      seeded().register({
        societyName: 'NOWHERE',
        flatNumber: 'B-204',
        residentName: 'Asha Rao',
        password: 'pw-asha',
      });
      fail('expected FakeCloudError');
    } catch (e) {
      expect((e as FakeCloudError).code).toBe('UNKNOWN_SOCIETY');
    }
  });
});
