/**
 * App root — auth-aware shell.
 *
 * Tracks the current `Session | null` via `AuthContext`:
 *   - boot   → tries `sessionStore.load()` once; if a token is on disk,
 *              the call engine spins up before DialScreen mounts.
 *   - login  → LoginScreen / RegisterScreen call `setSession(...)` after
 *              `sessionStore.save(...)` (see those screens).
 *   - logout → caller invokes `setSession(null)` (logout UI is a
 *              follow-up; the wiring is here so the tear-down path
 *              works the day it lands).
 *
 * While `session` is non-null the tree gets a `DepsProvider` whose
 * `callController` is the real sip.js-backed one (`SipCallController`)
 * and `incomingCallController` is the M3.b machine bound to the same
 * UA via `SipInboundBridge`. `IncomingCallOverlay` paints over the
 * navigator so an inbound INVITE rings on any screen.
 *
 * When `session` is null we use the default `DepsContext` value —
 * `StubCallController`, no engine running — which is correct for the
 * Login / Register screens.
 */
import React, {useEffect, useMemo, useRef, useState} from 'react';
import {StatusBar} from 'react-native';
import {SafeAreaProvider} from 'react-native-safe-area-context';
import {RootNavigator} from './src/navigation/RootNavigator';
import {AuthProvider} from './src/state/authContext';
import {AppDeps, DepsProvider} from './src/state/deps';
import {SessionStore} from './src/session/sessionStore';
import {defaultClient} from './src/api/defaultClient';
import {Session} from './src/api/types';
import {CLOUD_BASE_URL} from './src/config';
import {createSipEnv, SipEnv} from './src/state/createSipEnv';
import {IncomingCallOverlay} from './src/screens/IncomingCallOverlay';

export default function App(): React.JSX.Element {
  const [session, setSessionState] = useState<Session | null>(null);
  // Tracks the live sip env so we can tear it down deterministically
  // on logout / session change without leaking the UA's WebSocket.
  const envRef = useRef<SipEnv | null>(null);
  const [env, setEnv] = useState<SipEnv | null>(null);

  // Boot: try to restore a persisted session before the first render of
  // the navigator. SessionStore.load() never throws — null on miss.
  useEffect(() => {
    let cancelled = false;
    SessionStore.load().then(persisted => {
      if (!cancelled && persisted) {
        setSessionState(persisted);
      }
    });
    return () => {
      cancelled = true;
    };
  }, []);

  // Build / rebuild the sip env in lock-step with `session`. Tear down
  // the previous one before standing up the next so we never run two
  // UAs at once.
  useEffect(() => {
    let cancelled = false;
    const prev = envRef.current;
    envRef.current = null;
    setEnv(null);
    if (prev) {
      prev.cleanup();
    }
    if (session) {
      const next = createSipEnv({session, cloudBaseUrl: CLOUD_BASE_URL});
      next.sipUaHandle.start().catch(() => {
        /* state stream will surface 'terminated' */
      });
      if (cancelled) {
        next.cleanup();
        return;
      }
      envRef.current = next;
      setEnv(next);
    }
    return () => {
      cancelled = true;
    };
  }, [session]);

  // Tear down on unmount — last-line safety; in practice the previous
  // useEffect cleans up when `session` flips to null on logout.
  useEffect(
    () => () => {
      envRef.current?.cleanup();
      envRef.current = null;
    },
    [],
  );

  const deps: AppDeps = useMemo(() => {
    const base: AppDeps = {
      client: defaultClient,
      sessionStore: SessionStore,
      // Sentinel: the default DepsContext value carries StubCallController.
      // We construct an explicit one for the post-login tree so the
      // sip engine actually drives the screens.
      callController: env?.callController ?? defaultCallController(),
    };
    if (env) {
      base.incomingCallController = env.incomingCallController;
    }
    return base;
  }, [env]);

  return (
    <SafeAreaProvider>
      <StatusBar barStyle="light-content" />
      <AuthProvider value={{session, setSession: setSessionState}}>
        <DepsProvider value={deps}>
          <RootNavigator />
          {env && (
            <IncomingCallOverlay
              controller={env.incomingCallController}
            />
          )}
        </DepsProvider>
      </AuthProvider>
    </SafeAreaProvider>
  );
}

// Memo-stable default so identity-equal `deps` references don't force
// a re-render before any session exists.
let _defaultCallController: import('./src/call/callController').CallController | null =
  null;
function defaultCallController() {
  if (!_defaultCallController) {
    // Lazy require to keep the test surface clean — pure JS, no side
    // effects, but the import chain is large enough that lazying it
    // shaves a frame on boot.
    const {
      StubCallController,
    } = require('./src/call/callController') as typeof import('./src/call/callController');
    _defaultCallController = new StubCallController();
  }
  return _defaultCallController!;
}
