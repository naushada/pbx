import { InjectionToken } from '@angular/core';

// SIP UA seam — keeps SIP.js (and any future replacement) at arm's
// length from SipService so the state machine can be unit-tested with
// a fake factory. Production wires SipJsUaFactory; tests wire
// FakeSipUaFactory (see sip.service.spec.ts).
//
// Mirrors the C++ side's ITransport/ITransportFactory pattern used in
// CloudConnector + AceSslTransportFactory.

// One of these is delivered for every state change emitted by the UA.
//
//   'starting'      — transport.start() in flight
//   'started'       — transport open, ready to REGISTER
//   'registering'   — REGISTER sent, awaiting 200
//   'registered'    — final 200 received; we're reachable
//   'unregistered'  — REGISTER explicitly cancelled (after stop)
//   'terminated'    — transport closed / fatal error; reason in `detail`
export type SipUaState =
    | 'starting' | 'started' | 'registering' | 'registered'
    | 'unregistered' | 'terminated';

export interface SipUaStateChange {
    state:  SipUaState;
    detail?: string;
}

export interface SipUaOpts {
    /** Full SIP URI for this UA (e.g. "sip:A-204@pbx.<societyId>"). */
    uri: string;

    /**
     * Full WS URL to the cloud's /sip-ws endpoint. The bearer token
     * goes here as a `?token=<bearer>` query param (browsers cannot
     * set Authorization on WS upgrades; cookies are the alternative).
     * The query string lives only on the wire, which is TLS — same
     * trust boundary as the REST API.
     */
    wsUrl: string;

    /** "A-204" — the digest username if the cloud re-challenges. */
    authUser: string;

    /** Display-name for the SIP URI's display field ("Alice Resident"). */
    displayName?: string;
}

export interface SipUaHandle {
    /** Bring transport up + perform initial REGISTER. */
    start(): Promise<void>;
    /** Unregister + close transport. Always resolves; errors are logged. */
    stop():  Promise<void>;
    /** Subscribe to state changes (multi-subscriber). */
    onStateChange(cb: (change: SipUaStateChange) => void): void;

    /**
     * Place an outbound INVITE to `targetUri`. Returns immediately with
     * a SipCallHandle whose state stream tracks INVITE → answer → BYE.
     * Throws if the UA is not yet registered.
     */
    placeCall(targetUri: string): SipCallHandle;
}

// ─── Per-call lifecycle ───────────────────────────────────────────────
//
//   calling      — INVITE sent, awaiting any non-100 response
//   progressing  — provisional response (180 ringing, 183 early media)
//   in-call      — 200 OK received, ACK sent
//   ended        — clean BYE (either side)
//   failed       — 4xx/5xx/6xx, transport drop, ICE failure, etc.
export type SipCallState =
    | 'calling' | 'progressing' | 'in-call' | 'ended' | 'failed';

export interface SipCallStateChange {
    state:  SipCallState;
    detail?: string;
}

export interface SipCallHandle {
    /** Subscribe to call-state transitions. */
    onStateChange(cb: (c: SipCallStateChange) => void): void;

    /** Send BYE / CANCEL; tears down the session. Always resolves. */
    hangup(): Promise<void>;

    /** Mute / unmute the local mic. */
    setMute(mute: boolean): void;

    /**
     * Remote audio stream once SDP negotiation completes. May be
     * `undefined` while the call is still ringing — callers should
     * re-check on state transitions to 'in-call'.
     */
    getRemoteStream(): MediaStream | undefined;
}

export interface SipUaFactory {
    create(opts: SipUaOpts): SipUaHandle;
}

export const SIP_UA_FACTORY = new InjectionToken<SipUaFactory>('SipUaFactory');
