/**
 * Dependency seam — TDD layer M1.c.
 *
 * Screens receive their `navigation` from React Navigation, but their
 * cloud client and session store come from this context. Production
 * uses the real defaults (the context's default value), so `App` needs
 * no provider; tests wrap a screen in `<DepsProvider>` with fakes.
 */
import {createContext, useContext} from 'react';
import {CloudClient} from '../api/cloudClient';
import {defaultClient} from '../api/defaultClient';
import {SessionStore, SessionStoreApi} from '../session/sessionStore';
import {CallController, StubCallController} from '../call/callController';

export interface AppDeps {
  client: CloudClient;
  sessionStore: SessionStoreApi;
  callController: CallController;
}

const DepsContext = createContext<AppDeps>({
  client: defaultClient,
  sessionStore: SessionStore,
  // Drives the Dial UI's state machine only — replaced by the
  // sip.js-backed CallController once the call engine lands.
  callController: new StubCallController(),
});

/** Test seam — wrap a screen to inject fake deps. */
export const DepsProvider = DepsContext.Provider;

/** Screens call this for their cloud client + session store. */
export function useDeps(): AppDeps {
  return useContext(DepsContext);
}
