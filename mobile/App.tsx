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
import {
  RegistrationProvider,
  RegistrationState,
} from './src/state/registrationContext';
import {SessionStore} from './src/session/sessionStore';
import {defaultClient} from './src/api/defaultClient';
import {Session} from './src/api/types';
import {CLOUD_BASE_URL} from './src/config';
import {createSipEnv, SipEnv} from './src/state/createSipEnv';
import {IncomingCallOverlay} from './src/screens/IncomingCallOverlay';
import {MetricsManager} from './src/metrics';

export default function App(): React.JSX.Element {
  const [session, setSessionState] = useState<Session | null>(null);
  // Tracks the live sip env so we can tear it down deterministically
  // on logout / session change without leaking the UA's WebSocket.
  const envRef = useRef<SipEnv | null>(null);
  const [env, setEnv] = useState<SipEnv | null>(null);
  // Remembers who was logged in so we can emit a logout metric when the
  // session clears (by then `session` is already null).
  const loggedInRef = useRef<{societyId: string; flatNumber: string} | null>(null);
  // Surfaced via RegistrationContext — the badge in each post-login
  // screen header reads from this. 'unknown' until the UA emits.
  const [registration, setRegistration] = useState<RegistrationState>('unknown');

  // Initialize metrics manager and wire it to the cloud transport.
  useEffect(() => {
    const metricsManager = MetricsManager.getInstance();
    metricsManager.initialize();
    // Events are POSTed to the cloud (POST /api/v1/metrics) in batches;
    // failures keep events buffered for the next flush.
    metricsManager.setSink(events => defaultClient.postMetrics(events));
    metricsManager.trackEvent('app_start');

    return () => {
      // Best-effort flush of anything still buffered on teardown.
      void metricsManager.flush();
    };
  }, []);

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
      // Reset registration to 'unknown' for the new UA so a stale
      // 'registered' from the previous session can't briefly show.
      setRegistration('unknown');
      next.sipUaHandle.onStateChange(change => {
        if (cancelled) return;
        setRegistration(change.state);
      });
      next.sipUaHandle.start().catch(() => {
        /* state stream will surface 'terminated' */
      });
      if (cancelled) {
        next.cleanup();
        return;
      }
      envRef.current = next;
      setEnv(next);
      
      // Track login when session is created
      const {societyId, flatNumber} = session.subscriber;
      MetricsManager.getInstance().trackLogin(societyId, flatNumber);
      loggedInRef.current = {societyId, flatNumber};
    } else {
      // Track logout when session is cleared (use the remembered identity).
      if (loggedInRef.current) {
        const {societyId, flatNumber} = loggedInRef.current;
        const metrics = MetricsManager.getInstance();
        metrics.trackLogout(societyId, flatNumber);
        loggedInRef.current = null;
        void metrics.flush(); // ship the session's events on logout
      }
      setRegistration('unknown');
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

  const registrationCtx = useMemo(
    () => ({
      state: registration,
      // Re-issue start() on the current UA — idempotent at the sip.js
      // layer. Errors are swallowed; the state stream surfaces the
      // result via the badge.
      reconnect: async (): Promise<void> => {
        const live = envRef.current;
        if (!live) return;
        try {
          await live.sipUaHandle.start();
        } catch {
          /* state stream will surface 'terminated' */
        }
      },
    }),
    [registration],
  );

  return (
    <SafeAreaProvider>
      <StatusBar barStyle="light-content" />
      <AuthProvider value={{session, setSession: setSessionState}}>
        <RegistrationProvider value={registrationCtx}>
          <DepsProvider value={deps}>
            <RootNavigator />
            {env && (
              <IncomingCallOverlay
                controller={env.incomingCallController}
              />
            )}
          </DepsProvider>
        </RegistrationProvider>
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
