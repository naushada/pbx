/**
 * RegistrationContext — exposes the sip.js UA's current state to any
 * post-login screen.
 *
 * Web softphone's Dashboard subscribes to `PubsubsvcService.onCallState`
 * and renders "Connected", "Connecting…", "Disconnected". Mobile has
 * no equivalent surface — without it the user can't tell if they're
 * actually online + reachable for incoming calls. This context puts
 * the same information one `useRegistration()` call away.
 *
 * Default value is `'unknown'` so an isolated screen test that
 * doesn't wrap in `RegistrationProvider` still renders without
 * crashing (the badge falls back to neutral grey).
 */
import {createContext, useContext} from 'react';
import {SipUaState} from '../sip/sipUa';

/**
 * Mirrors `SipUaState` plus an `'unknown'` sentinel for the period
 * before the UA has emitted anything (App boot, or a freshly-built
 * env between createSipEnv() and ua.start()).
 */
export type RegistrationState = SipUaState | 'unknown';

const RegistrationContext = createContext<RegistrationState>('unknown');

export const RegistrationProvider = RegistrationContext.Provider;

export function useRegistration(): RegistrationState {
  return useContext(RegistrationContext);
}
