import {
    AfterViewInit, Component, ElementRef, OnDestroy, OnInit, ViewChild,
} from '@angular/core';
import { Subscription, interval } from 'rxjs';

import { PubsubsvcService, CallState } from 'src/common/pubsubsvc.service';
import { SipService } from 'src/common/sip.service';

// Fixed-position overlay shown whenever there is an active call. The
// remote audio element is the *only* place where the remote stream is
// attached — keeping it on a shell-level component means the audio
// keeps playing as the user navigates between child routes.

@Component({
    selector: 'app-call-panel',
    templateUrl: './call-panel.component.html',
    styleUrls: ['./call-panel.component.scss'],
})
export class CallPanelComponent implements OnInit, OnDestroy, AfterViewInit {

    callState: CallState = { kind: 'idle' };
    muted = false;
    elapsed = '00:00';
    private startedAt = 0;

    @ViewChild('remoteAudio') remoteAudio?: ElementRef<HTMLAudioElement>;

    private subs: Subscription[] = [];

    constructor(private pubsub: PubsubsvcService, private sip: SipService) {}

    ngOnInit(): void {
        this.subs.push(this.pubsub.onCallState.subscribe(s => this.onCallStateChange(s)));
        this.subs.push(interval(1000).subscribe(() => this.updateElapsed()));
    }

    ngAfterViewInit(): void {
        if (this.callState.kind === 'in-call') this.bindRemoteStream();
    }

    ngOnDestroy(): void { for (const s of this.subs) s.unsubscribe(); }

    visible(): boolean {
        const k = this.callState.kind;
        return k === 'outgoing' || k === 'incoming' || k === 'in-call';
    }

    peerLabel(): string {
        switch (this.callState.kind) {
            case 'outgoing': return this.callState.toFlat;
            case 'incoming': return this.callState.fromFlat;
            case 'in-call':  return this.callState.peerFlat;
            default:         return '';
        }
    }

    statusLabel(): string {
        switch (this.callState.kind) {
            case 'outgoing': return 'Calling…';
            case 'incoming': return 'Incoming call';
            case 'in-call':  return this.elapsed;
            default:         return '';
        }
    }

    async onHangup(): Promise<void> { await this.sip.hangup(); }

    onToggleMute(): void {
        this.muted = !this.muted;
        this.sip.setMute(this.muted);
    }

    private onCallStateChange(s: CallState): void {
        this.callState = s;
        if (s.kind === 'in-call') {
            this.startedAt = s.startedAt;
            this.muted     = false;
            // ViewChild may not be resolved yet on first state change
            // — guarded by the AfterViewInit re-bind path.
            setTimeout(() => this.bindRemoteStream(), 0);
        } else if (s.kind !== 'outgoing' && s.kind !== 'incoming') {
            this.startedAt = 0;
            if (this.remoteAudio) this.remoteAudio.nativeElement.srcObject = null;
        }
    }

    private bindRemoteStream(): void {
        const stream = this.sip.getRemoteStream();
        const el     = this.remoteAudio?.nativeElement;
        if (!stream || !el) return;
        if (el.srcObject !== stream) {
            el.srcObject = stream;
            el.play().catch(() => { /* autoplay gating; user clicked Call already */ });
        }
    }

    private updateElapsed(): void {
        if (this.startedAt === 0) { this.elapsed = '00:00'; return; }
        const total = Math.floor((Date.now() - this.startedAt) / 1000);
        const mm = Math.floor(total / 60).toString().padStart(2, '0');
        const ss = (total % 60).toString().padStart(2, '0');
        this.elapsed = `${mm}:${ss}`;
    }
}
