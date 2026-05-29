/**
 * Build the live call engine for a logged-in session.
 *
 * One factory that resolves the IncomingCallController ↔ SipInboundBridge
 * forward-reference cycle: the controller's `signaling` seam is a
 * thunk that delegates to the bridge once both are constructed.
 *
 * Returned `cleanup` stops the UA — call it when the session ends
 * (logout, session expiry, app suspend).
 */
import {Session} from '../api/types';
import {SipJsUaFactory} from '../sip/sipJsUaFactory';
import {SipUaHandle, SipUaFactory} from '../sip/sipUa';
import {SipCallController} from '../call/sipCallController';
import {SipInboundBridge} from '../call/sipIncomingSignaling';
import {
  CallKitBridge,
  IncomingCallController,
  IncomingCallSignaling,
} from '../call/incomingCallController';
import {IncomingCall} from '../sip/incomingCall';

/** Just the slice of `react-native-callkeep` we currently need. */
const NOOP_CALL_KIT: CallKitBridge = {
  // Real CallKit / ConnectionService binding is a follow-up. The
  // in-app `IncomingCallOverlay` already renders the ringing UI by
  // subscribing to IncomingCallController.subscribe(), so the engine
  // works foreground-to-foreground without a native UI yet.
  displayIncomingCall: () => {},
  endCall: () => {},
};

const NOOP_MISSED_CALL = (_call: IncomingCall): void => {
  // Future: persist to a missed-call history (#TBD).
};

export interface CreateSipEnvOpts {
  session: Session;
  /** Override for tests; defaults to the real Heroku cloud. */
  cloudBaseUrl: string;
  /** Override for tests; defaults to `SipJsUaFactory`. */
  factory?: SipUaFactory;
  callKit?: CallKitBridge;
  logMissedCall?: (call: IncomingCall) => void;
}

export interface SipEnv {
  callController: SipCallController;
  incomingCallController: IncomingCallController;
  sipUaHandle: SipUaHandle;
  /** Stop UA + unregister. Idempotent; always resolves. */
  cleanup: () => Promise<void>;
}

export function createSipEnv(opts: CreateSipEnvOpts): SipEnv {
  const factory = opts.factory ?? new SipJsUaFactory();
  const wsBase = httpToWs(opts.cloudBaseUrl);
  const realm = sipRealmFor(opts.session);
  const ua = factory.create({
    uri: `sip:${opts.session.subscriber.sipUsername}@${realm}`,
    wsUrl: `${wsBase}/sip-ws?token=${encodeURIComponent(opts.session.token)}`,
    authUser: opts.session.subscriber.sipUsername,
    displayName: opts.session.subscriber.displayName,
  });

  const callController = new SipCallController(ua);

  // Forward reference: IncomingCallController.signaling needs the
  // bridge, but the bridge constructor wants the controller for
  // `reportPush`. The signaling thunk delegates once both exist.
  let bridge: SipInboundBridge | null = null;
  const signaling: IncomingCallSignaling = {
    answer: callId => bridge!.answer(callId),
    reject: (callId, code) => bridge!.reject(callId, code),
  };

  const incomingCallController = new IncomingCallController({
    callKit: opts.callKit ?? NOOP_CALL_KIT,
    signaling,
    // sip.js owns the transport; once the UA is started, it stays
    // connected — these hooks are placeholders for future expansion
    // (e.g., re-arm a dropped tunnel before answer).
    ensureConnected: () => Promise.resolve(),
    connectMedia: () => Promise.resolve(),
    logMissedCall: opts.logMissedCall ?? NOOP_MISSED_CALL,
    // Guard kiosk auto-answer (DESIGN.md §9). Mirrors the web
    // softphone's `SipService.onIncoming` check. AND-ed with the role
    // here so `autoAnswer` on a resident is silently ignored.
    shouldAutoAnswer: () =>
      opts.session.subscriber.autoAnswer === true &&
      opts.session.subscriber.role === 'guard',
  });

  bridge = new SipInboundBridge(ua, incomingCallController);

  return {
    callController,
    incomingCallController,
    sipUaHandle: ua,
    cleanup: async () => {
      try {
        await ua.stop();
      } catch {
        /* logged by SipJsUaHandle */
      }
    },
  };
}

function httpToWs(origin: string): string {
  return origin.replace(/^http(s?):\/\//, (_match, s) => `ws${s}://`);
}

/**
 * Derive the SIP realm from the session. We don't yet expose
 * `sipRealm` on the subscriber doc; fall back to a `<society>.pbx.local`
 * convention that matches the agent's default (see
 * `PjsipProvisioner.set_sip_realm`).
 */
function sipRealmFor(session: Session): string {
  return `${session.subscriber.societyId}.pbx.local`;
}
