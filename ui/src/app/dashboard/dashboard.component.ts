import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';

import { PubsubsvcService, CallState } from 'src/common/pubsubsvc.service';
import { SipService } from 'src/common/sip.service';
import { Subscriber } from 'src/common/app-globals';

// Dashboard: SIP registration state + connect/disconnect; one-click
// society conference (joins sip:conf@pbx.local). Push toggle and
// device pickers moved to /main/settings in slice 5.
@Component({
    selector: 'app-dashboard',
    templateUrl: './dashboard.component.html',
    styleUrls: ['./dashboard.component.scss'],
})
export class DashboardComponent implements OnInit, OnDestroy {

    subscriber?: Subscriber;
    callState: CallState = { kind: 'idle' };

    private subs: Subscription[] = [];

    constructor(
        private pubsub: PubsubsvcService,
        private sip:   SipService,
    ) {}

    ngOnInit(): void {
        this.subs.push(this.pubsub.onSubscriber.subscribe(s => this.subscriber = s));
        this.subs.push(this.pubsub.onCallState .subscribe(s => this.callState  = s));

        // Auto-connect on land. Matches consumer-app norm (login =
        // online); without it the resident has to click CONNECT before
        // any directory CALL button is enabled, which surprised every
        // first-time user (caught live 2026-05-17).
        //
        // SipService.connect() is idempotent: it early-returns if a UA
        // handle already exists. So if the user navigates away from
        // /dashboard and back, this is a no-op. Same for the explicit
        // CONNECT button — it stays wired for the rare case of a
        // post-failure manual retry. DISCONNECT remains as the
        // "go offline" / pause-notifications gesture.
        void this.sip.connect();
    }

    ngOnDestroy(): void {
        for (const s of this.subs) s.unsubscribe();
    }

    canConnect():    boolean { return this.callState.kind === 'idle' || this.callState.kind === 'failed'; }
    canDisconnect(): boolean {
        return this.callState.kind === 'registering' || this.callState.kind === 'registered';
    }

    statusLabel(): string {
        switch (this.callState.kind) {
            case 'idle':         return 'Disconnected';
            case 'registering':  return 'Connecting…';
            case 'registered':   return 'Connected · ready for calls';
            case 'incoming':     return `Incoming call from ${this.callState.fromFlat}`;
            case 'outgoing':     return `Calling ${this.callState.toFlat}…`;
            case 'in-call':      return `On call with ${this.callState.peerFlat}`;
            case 'failed':       return `Connection failed: ${this.callState.reason}`;
        }
    }

    statusClass(): string {
        switch (this.callState.kind) {
            case 'registered': return 'status status-ok';
            case 'failed':     return 'status status-err';
            case 'idle':       return 'status status-idle';
            default:           return 'status status-busy';
        }
    }

    async onConnect():    Promise<void> { await this.sip.connect(); }
    async onDisconnect(): Promise<void> { await this.sip.disconnect(); }

    canJoinConference(): boolean { return this.callState.kind === 'registered'; }
    onJoinConference(): void     { this.sip.joinConference(); }
}
