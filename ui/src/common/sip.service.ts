import { Inject, Injectable } from '@angular/core';

import { environment } from 'src/environments/environment';
import { AuthService } from './auth.service';
import { PubsubsvcService } from './pubsubsvc.service';
import {
    SIP_UA_FACTORY, SipUaFactory, SipUaHandle, SipUaStateChange,
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

    constructor(
        @Inject(SIP_UA_FACTORY) private factory: SipUaFactory,
        private auth: AuthService,
        private pubsub: PubsubsvcService,
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
                this.pubsub.emit_callState({ kind: 'registering' });
                break;
            case 'registered':
                this.pubsub.emit_callState({ kind: 'registered' });
                break;
            case 'unregistered':
                if (this.stopping) this.pubsub.emit_callState({ kind: 'idle' });
                break;
            case 'terminated':
                if (!this.stopping) {
                    this.pubsub.emit_callState({
                        kind:   'failed',
                        reason: c.detail ?? 'terminated',
                    });
                }
                break;
        }
    }

    /**
     * Builds the /sip-ws WebSocket URL with the bearer token as a query
     * param. Respects environment.cloudOrigin (empty → same-origin).
     * Same-origin construction is window-dependent and only safe in
     * the browser context; SipService is a browser-only singleton.
     */
    private makeWsUrl(bearer: string): string {
        const origin = environment.cloudOrigin
            ? environment.cloudOrigin
            : `${window.location.protocol}//${window.location.host}`;
        const wsOrigin = origin.replace(/^http/, 'ws');
        return `${wsOrigin}${environment.sipWsPath}?token=${encodeURIComponent(bearer)}`;
    }
}
