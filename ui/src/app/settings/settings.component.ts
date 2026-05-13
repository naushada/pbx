import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';

import { PushService, PushState } from 'src/common/push.service';
import { DeviceService } from 'src/common/device.service';

@Component({
    selector: 'app-settings',
    templateUrl: './settings.component.html',
    styleUrls: ['./settings.component.scss'],
})
export class SettingsComponent implements OnInit, OnDestroy {

    pushState: PushState = 'disabled';
    pushBusy   = false;
    pushError  = '';

    mics:     MediaDeviceInfo[] = [];
    speakers: MediaDeviceInfo[] = [];
    selectedMic     = '';
    selectedSpeaker = '';
    devicesError    = '';

    private subs: Subscription[] = [];

    constructor(private push: PushService, private devices: DeviceService) {}

    ngOnInit(): void {
        this.subs.push(this.push.onState.subscribe(s => this.pushState = s));
        this.push.refresh().catch(() => { /* state shows the failure */ });
        this.selectedMic     = this.devices.getSavedMic()     ?? '';
        this.selectedSpeaker = this.devices.getSavedSpeaker() ?? '';
        this.loadDevices();
    }

    ngOnDestroy(): void { for (const s of this.subs) s.unsubscribe(); }

    async loadDevices(): Promise<void> {
        try {
            // enumerateDevices() returns labels only after mic permission
            // has been granted; this prompts the user once.
            await navigator.mediaDevices.getUserMedia({ audio: true }).catch(() => undefined);
            this.mics     = await this.devices.listMics();
            this.speakers = await this.devices.listSpeakers();
        } catch (e) {
            this.devicesError = (e instanceof Error) ? e.message : 'failed to load devices';
        }
    }

    onMicChange(id: string):     void { this.devices.saveMic(id     || undefined);     this.selectedMic     = id; }
    onSpeakerChange(id: string): void { this.devices.saveSpeaker(id || undefined);     this.selectedSpeaker = id; }

    // ─── Push state surface (same shape as the previous dashboard card) ──

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
