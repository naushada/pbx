/**
 * RegistrationContext — exposes the sip.js UA's current state + a
 * manual reconnect handle to any post-login screen.
 *
 * Web softphone's Dashboard subscribes to `PubsubsvcService.onCallState`
 * and renders "Connected", "Connecting…", "Disconnected" + has a
 * Connect button for manual retry after a `failed`/`idle` state.
 * Mobile mirrors both pieces here: `state` for the badge + `reconnect`
 * for the retry button.
 *
 * Default values render an offline badge and a no-op reconnect so
 * isolated screen tests that don't wrap in `RegistrationProvider`
 * still render without crashing.
 */
import {createContext, useContext} from 'react';
import {SipUaState} from '../sip/sipUa';

/**
 * Mirrors `SipUaState` plus an `'unknown'` sentinel for the period
 * before the UA has emitted anything (App boot, or a freshly-built
 * env between createSipEnv() and ua.start()).
 */
export type RegistrationState = SipUaState | 'unknown';

export interface RegistrationContextValue {
  state: RegistrationState;
  /**
   * Re-issue `sipUaHandle.start()` on the current env. Idempotent —
   * a no-op if there is no env (pre-login) and tolerant of an
   * already-started UA (sip.js's `UserAgent.start()` early-returns
   * when the transport is already up). Errors are swallowed; the
   * state stream surfaces the result via the badge.
   */
  reconnect: () => Promise<void>;
}

const DEFAULT: RegistrationContextValue = {
  state: 'unknown',
  reconnect: async () => {
    /* default no-op — context not provided in test */
  },
};

const RegistrationContext = createContext<RegistrationContextValue>(DEFAULT);

export const RegistrationProvider = RegistrationContext.Provider;

export function useRegistration(): RegistrationContextValue {
  return useContext(RegistrationContext);
}
