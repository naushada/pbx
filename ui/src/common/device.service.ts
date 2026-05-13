import { Injectable } from '@angular/core';

// Persists the user's preferred mic + speaker (and exposes the list of
// available devices). The actual wiring of the saved id into the SIP
// call's audio constraints is on the to-do list for slice 6 — for now
// the SettingsComponent uses this to save the selection and we'll
// thread it through SipUaOpts in a follow-up. Saving up-front means
// nothing is lost when the seam is upgraded.

const STORAGE_MIC      = 'pbxui:device-mic';
const STORAGE_SPEAKER  = 'pbxui:device-speaker';

@Injectable({ providedIn: 'root' })
export class DeviceService {

    async listMics(): Promise<MediaDeviceInfo[]> {
        const all = await this.list();
        return all.filter(d => d.kind === 'audioinput');
    }

    async listSpeakers(): Promise<MediaDeviceInfo[]> {
        const all = await this.list();
        return all.filter(d => d.kind === 'audiooutput');
    }

    getSavedMic():     string | undefined { return localStorage.getItem(STORAGE_MIC)     ?? undefined; }
    getSavedSpeaker(): string | undefined { return localStorage.getItem(STORAGE_SPEAKER) ?? undefined; }

    saveMic(id: string | undefined): void {
        if (id) localStorage.setItem(STORAGE_MIC, id);
        else    localStorage.removeItem(STORAGE_MIC);
    }
    saveSpeaker(id: string | undefined): void {
        if (id) localStorage.setItem(STORAGE_SPEAKER, id);
        else    localStorage.removeItem(STORAGE_SPEAKER);
    }

    private async list(): Promise<MediaDeviceInfo[]> {
        if (!navigator.mediaDevices?.enumerateDevices) return [];
        try { return await navigator.mediaDevices.enumerateDevices(); }
        catch { return []; }
    }
}
