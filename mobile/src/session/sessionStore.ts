/**
 * SessionStore — TDD layer M1.b.
 *
 * Persists the auth `Session` in the device secure store (iOS Keychain
 * / Android Keystore) via `react-native-keychain`. Only the token and
 * subscriber profile are stored — the password is never persisted (it
 * is not part of `Session`).
 */
import * as Keychain from 'react-native-keychain';
import {Session} from '../api/types';

const SERVICE = 'onprem-pbx.session';
const ACCOUNT = 'session';

export const SessionStore = {
  /** Persist the session, replacing any previous one. */
  async save(session: Session): Promise<void> {
    await Keychain.setGenericPassword(ACCOUNT, JSON.stringify(session), {
      service: SERVICE,
    });
  },

  /** Load the stored session, or null if none / unreadable. */
  async load(): Promise<Session | null> {
    const creds = await Keychain.getGenericPassword({service: SERVICE});
    if (!creds) {
      return null;
    }
    try {
      return JSON.parse(creds.password) as Session;
    } catch {
      return null;
    }
  },

  /** Drop the stored session — used on sign-out and on any API 401. */
  async clear(): Promise<void> {
    await Keychain.resetGenericPassword({service: SERVICE});
  },
};

export type SessionStoreApi = typeof SessionStore;
