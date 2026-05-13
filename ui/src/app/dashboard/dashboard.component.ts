import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';

import { PubsubsvcService, CallState } from 'src/common/pubsubsvc.service';
import { SipService } from 'src/common/sip.service';
import { PushService, PushState } from 'src/common/push.service';
import { Subscriber } from 'src/common/app-globals';

// Dashboard: SIP registration state + connect/disconnect; push-
// notification toggle so the user can opt-in to wakeup on incoming
// calls while the tab is backgrounded.
@Component({
    selector: 'app-dashboard',
    templateUrl: './dashboard.component.html',
    styleUrls: ['./dashboard.component.scss'],
})
export class DashboardComponent implements OnInit, OnDestroy {

    subscriber?: Subscriber;
    callState: CallState = { kind: 'idle' };
    pushState: PushState = 'disabled';
    pushBusy   = false;
    pushError  = '';

    private subs: Subscription[] = [];

    constructor(
        private pubsub: PubsubsvcService,
        private sip:   SipService,
        private push:  PushService,
    ) {}

    ngOnInit(): void {
        this.subs.push(this.pubsub.onSubscriber.subscribe(s => this.subscriber = s));
        this.subs.push(this.pubsub.onCallState .subscribe(s => this.callState  = s));
        this.subs.push(this.push.onState.subscribe(s => this.pushState = s));
        // Resolve actual push state once the view is up.
        this.push.refresh().catch(() => { /* surfaced via state */ });
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

    canEnablePush():  boolean { return this.pushState === 'disabled'; }
    canDisablePush(): boolean { return this.pushState === 'enabled'; }

    pushStateLabel(): string {
        switch (this.pushState) {
            case 'enabled':     return 'Enabled — you\'ll be alerted of incoming calls.';
            case 'disabled':    return 'Disabled — incoming calls will only ring while this tab is open.';
            case 'denied':      return 'Blocked by browser — change the site permission to enable.';
            case 'unsupported': return 'This browser doesn\'t support Web Push.';
        }
    }

    async onEnablePush(): Promise<void> {
        this.pushBusy = true; this.pushError = '';
        try { await this.push.enable(); }
        catch (e) { this.pushError = (e instanceof Error) ? e.message : 'failed to enable push'; }
        finally   { this.pushBusy = false; }
    }

    async onDisablePush(): Promise<void> {
        this.pushBusy = true; this.pushError = '';
        try { await this.push.disable(); }
        catch (e) { this.pushError = (e instanceof Error) ? e.message : 'failed to disable push'; }
        finally   { this.pushBusy = false; }
    }
}
