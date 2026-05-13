import { Inject, Injectable } from '@angular/core';

import { environment } from 'src/environments/environment';
import { AuthService } from './auth.service';
import { PubsubsvcService } from './pubsubsvc.service';
import { RingtoneService } from './ringtone.service';
import {
    SIP_UA_FACTORY, SipUaFactory, SipUaHandle, SipUaStateChange,
    SipCallHandle, SipCallStateChange,
    IncomingCallHandle,
} from './sip-ua';

// Front-door for SIP signalling. Drives the injected SipUaFactory and
// translates UA state events into the high-level CallState exposed to
// the rest of the app via PubsubsvcService.onCallState.
//
// State flow (slice 2 — call setup is slice 3):
//   idle ── connect() ──► registering ── REGISTER 200 ──► registered
//                                             │
//                                             └── 4xx / transport drop ──► failed
//   any state ── disconnect() ──► idle

@Injectable({ providedIn: 'root' })
export class SipService {

    private handle?: SipUaHandle;
    private stopping = false;

    // Slice 3: active call (either direction once established).
    private call?: { handle: SipCallHandle; peerFlat: string; callId: string };
    // Slice 4: pending incoming INVITE, before the user accepts/rejects.
    private incoming?: { handle: IncomingCallHandle };
    private registered = false;

    constructor(
        @Inject(SIP_UA_FACTORY) private factory: SipUaFactory,
        private auth: AuthService,
        private pubsub: PubsubsvcService,
        private ringtone: RingtoneService,
    ) {}

    async connect(): Promise<void> {
        if (this.handle) return;                  // already connecting / connected

        const sub   = this.auth.getSubscriber();
        const token = this.auth.getToken();
        if (!sub || !token) {
            this.pubsub.emit_callState({ kind: 'failed', reason: 'not_authenticated' });
            return;
        }

        const wsUrl = this.makeWsUrl(token);
        const uri   = `sip:${sub.sipUser}@pbx.${sub.societyId}`;

        this.stopping = false;
        this.pubsub.emit_callState({ kind: 'registering' });

        this.handle = this.factory.create({
            uri, wsUrl,
            authUser:    sub.sipUser,
            displayName: sub.displayName,
        });
        this.handle.onStateChange(c => this.onUaState(c));
        this.handle.onIncomingCall(h => this.onIncoming(h));

        try {
            await this.handle.start();
        } catch (e) {
            const reason = (e instanceof Error) ? e.message : 'start_failed';
            this.pubsub.emit_callState({ kind: 'failed', reason });
            this.handle = undefined;
        }
    }

    async disconnect(): Promise<void> {
        if (!this.handle) return;
        this.stopping = true;
        try { await this.handle.stop(); } finally {
            this.handle = undefined;
            this.pubsub.emit_callState({ kind: 'idle' });
        }
    }

    private onUaState(c: SipUaStateChange): void {
        switch (c.state) {
            case 'starting':
            case 'started':
            case 'registering':
                this.registered = false;
                this.pubsub.emit_callState({ kind: 'registering' });
                break;
            case 'registered':
                this.registered = true;
                this.pubsub.emit_callState({ kind: 'registered' });
                break;
            case 'unregistered':
                this.registered = false;
                if (this.stopping) this.pubsub.emit_callState({ kind: 'idle' });
                break;
            case 'terminated':
                this.registered = false;
                if (!this.stopping) {
                    this.pubsub.emit_callState({
                        kind:   'failed',
                        reason: c.detail ?? 'terminated',
                    });
                }
                break;
        }
    }

    // ─── Slice 3: outbound call ──────────────────────────────────────

    /**
     * Place an INVITE to a flat in the same society. Caller must already
     * be `registered`; otherwise emits a failed CallState and returns.
     * Idempotent — if a call is already up, the new request is ignored.
     */
    placeCall(targetFlat: string): void { this.dial(targetFlat, targetFlat); }

    /**
     * Join the society's shared conference room (Asterisk ConfBridge,
     * one room per society reached via the well-known user `conf`).
     * Other residents join the same URI to be merged in.
     */
    joinConference(): void { this.dial('conf', 'Conference'); }

    private dial(sipUser: string, displayLabel: string): void {
        if (this.call)        return;                          // already on a call
        if (!this.handle)     {
            this.pubsub.emit_callState({ kind: 'failed', reason: 'not_connected' });
            return;
        }
        if (!this.registered) {
            this.pubsub.emit_callState({ kind: 'failed', reason: 'not_registered' });
            return;
        }
        const sub = this.auth.getSubscriber();
        if (!sub) return;

        const targetUri = `sip:${sipUser}@pbx.${sub.societyId}`;
        const callId    = newCallId();

        let callHandle: SipCallHandle;
        try {
            callHandle = this.handle.placeCall(targetUri);
        } catch (e) {
            this.pubsub.emit_callState({
                kind:   'failed',
                reason: (e instanceof Error) ? e.message : 'invite_failed',
            });
            return;
        }

        this.call = { handle: callHandle, peerFlat: displayLabel, callId };
        this.pubsub.emit_callState({ kind: 'outgoing', toFlat: displayLabel, callId });

        callHandle.onStateChange(c => this.onCallState(c));
    }

    /** Hang up the active call (BYE if up, CANCEL otherwise). */
    async hangup(): Promise<void> {
        const c = this.call;
        if (!c) return;
        try { await c.handle.hangup(); } finally {
            this.call = undefined;
            // If the registerer is still up, we go back to 'registered';
            // otherwise we're idle.
            this.pubsub.emit_callState(
                this.registered ? { kind: 'registered' } : { kind: 'idle' },
            );
        }
    }

    setMute(mute: boolean): void { this.call?.handle.setMute(mute); }

    getRemoteStream(): MediaStream | undefined {
        return this.call?.handle.getRemoteStream();
    }

    // ─── Slice 4: inbound call ───────────────────────────────────────

    private onIncoming(h: IncomingCallHandle): void {
        // One-call-at-a-time policy. A second arrival is auto-rejected
        // as busy so the caller doesn't ring forever.
        if (this.call || this.incoming) {
            void h.reject('busy');
            return;
        }
        this.incoming = { handle: h };
        this.pubsub.emit_callState({
            kind:     'incoming',
            fromFlat: h.info.fromFlat,
            callId:   h.info.callId,
        });
        void this.ringtone.start();
    }

    async acceptIncoming(): Promise<void> {
        const inc = this.incoming;
        if (!inc) return;
        this.incoming = undefined;
        this.ringtone.stop();

        let callHandle: SipCallHandle;
        try {
            callHandle = await inc.handle.accept();
        } catch (e) {
            this.pubsub.emit_callState({
                kind:   'failed',
                reason: (e instanceof Error) ? e.message : 'accept_failed',
            });
            return;
        }

        const peerFlat = inc.handle.info.fromFlat;
        const callId   = inc.handle.info.callId;
        this.call = { handle: callHandle, peerFlat, callId };
        this.pubsub.emit_callState({
            kind: 'in-call', peerFlat, callId, startedAt: Date.now(),
        });
        callHandle.onStateChange(c => this.onCallState(c));
    }

    async rejectIncoming(): Promise<void> {
        const inc = this.incoming;
        if (!inc) return;
        this.incoming = undefined;
        this.ringtone.stop();
        try { await inc.handle.reject('declined'); } catch { /* logged */ }
        this.pubsub.emit_callState(
            this.registered ? { kind: 'registered' } : { kind: 'idle' },
        );
    }

    private onCallState(c: SipCallStateChange): void {
        if (!this.call) return;
        const { peerFlat, callId } = this.call;

        switch (c.state) {
            case 'calling':
            case 'progressing':
                this.pubsub.emit_callState({ kind: 'outgoing', toFlat: peerFlat, callId });
                break;
            case 'in-call':
                this.pubsub.emit_callState({
                    kind:      'in-call',
                    peerFlat,  callId,
                    startedAt: Date.now(),
                });
                break;
            case 'ended':
                this.call = undefined;
                this.pubsub.emit_callState(
                    this.registered ? { kind: 'registered' } : { kind: 'idle' },
                );
                break;
            case 'failed':
                this.call = undefined;
                this.pubsub.emit_callState({
                    kind:   'failed',
                    reason: c.detail ?? 'call_failed',
                });
                break;
        }
    }

    /**
     * Builds the /sip-ws WebSocket URL with the bearer token as a query
     * param. Respects environment.cloudOrigin (empty → same-origin).
     * Same-origin construction is window-dependent and only safe in
     * the browser context; SipService is a browser-only singleton.
     */
    /** RFC 7989-ish call-id: short random hex. Cloud also assigns one in CDR. */
    /* (helper kept module-local; see newCallId() below) */
    private makeWsUrl(bearer: string): string {
        const origin = environment.cloudOrigin
            ? environment.cloudOrigin
            : `${window.location.protocol}//${window.location.host}`;
        const wsOrigin = origin.replace(/^http/, 'ws');
        return `${wsOrigin}${environment.sipWsPath}?token=${encodeURIComponent(bearer)}`;
    }
}

function newCallId(): string {
    const bytes = new Uint8Array(8);
    crypto.getRandomValues(bytes);
    return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
}
