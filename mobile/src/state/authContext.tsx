/**
 * AuthContext — App-wide login state.
 *
 * Holds the *current* `Session | null` so the App shell can build (and
 * tear down) the sip.js call engine in lock-step with login / logout.
 * `LoginScreen` and `RegisterScreen` call `setSession(newSession)`
 * immediately after `sessionStore.save(...)`; logout (when added) calls
 * `setSession(null)`.
 *
 * Default value is a no-op so tests that don't wrap in `AuthProvider`
 * still render — every existing screen test relies on that.
 */
import {createContext, useContext} from 'react';
import {Session} from '../api/types';

export interface AuthContextValue {
  session: Session | null;
  setSession: (session: Session | null) => void;
}

const AuthContext = createContext<AuthContextValue>({
  session: null,
  setSession: () => {
    /* no-op default — safe in tests / pre-wrap render */
  },
});

export const AuthProvider = AuthContext.Provider;

export function useAuth(): AuthContextValue {
  return useContext(AuthContext);
}
