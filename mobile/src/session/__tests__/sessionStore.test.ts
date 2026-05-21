/**
 * TDD layer M1.b — SessionStore.
 *
 * `react-native-keychain` is faked in jest.setup.ts; these tests
 * exercise the store's contract against that fake. The fake keeps one
 * in-memory credential, so each test starts by clearing it.
 */
import {SessionStore} from '../sessionStore';
import {Session} from '../../api/types';

const SESSION: Session = {
  token: 'tok-123',
  subscriber: {
    societyId: 'soc_sunset',
    flatNumber: 'A-101',
    sipUsername: 'a101',
    displayName: 'Resident A101',
    role: 'resident',
  },
};

beforeEach(async () => {
  await SessionStore.clear();
});

describe('SessionStore', () => {
  it('persists a session and reads it back (restores on launch)', async () => {
    await SessionStore.save(SESSION);
    expect(await SessionStore.load()).toEqual(SESSION);
  });

  it('returns null when nothing has been stored', async () => {
    expect(await SessionStore.load()).toBeNull();
  });

  it('clear() removes the stored session', async () => {
    await SessionStore.save(SESSION);
    await SessionStore.clear();
    expect(await SessionStore.load()).toBeNull();
  });

  it('stores the token but never a password', async () => {
    await SessionStore.save(SESSION);
    const loaded = await SessionStore.load();
    expect(loaded?.token).toBe('tok-123');
    // Session carries no password field — pin that nothing leaks one.
    expect(JSON.stringify(loaded)).not.toMatch(/password/i);
  });
});
