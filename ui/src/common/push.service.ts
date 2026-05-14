import { Injectable } from '@angular/core';
import { BehaviorSubject, firstValueFrom } from 'rxjs';

import { HttpsvcService } from './httpsvc.service';
import { PushSubscriptionPayload } from './app-globals';

// Web Push subscription manager. Owns the lifecycle:
//   1. fetch VAPID public key from cloud
//   2. ensure the SW is registered (and active)
//   3. request Notification permission (browser gesture-gated)
//   4. pushManager.subscribe + POST the result to the cloud
//
// Re-running enable() is safe — it short-circuits when already
// subscribed and re-posts the existing subscription so a cloud-side
// DB wipe can recover.

export type PushState =
    | 'unsupported'   // browser missing serviceWorker or PushManager
    | 'denied'        // user said no (Notification.permission === 'denied')
    | 'disabled'      // permission grantable, no subscription yet
    | 'enabled';      // permission + active subscription

// Relative path so the registration resolves against <base href="/webui/">.
// An absolute "/sw.js" would resolve to the document root which the
// webservice doesn't serve (the SPA + assets live under /webui/).
const SW_URL = 'sw.js';

@Injectable({ providedIn: 'root' })
export class PushService {

    private state$ = new BehaviorSubject<PushState>('disabled');
    public readonly onState = this.state$.asObservable();

    // No side-effects in the constructor — callers (DashboardComponent
    // via ngOnInit) explicitly invoke refresh() once the view is ready
    // and after the user has signed in. Keeps test timing deterministic.
    constructor(private http: HttpsvcService) {}

    /** Recompute and emit the current PushState. */
    async refresh(): Promise<PushState> {
        if (!('serviceWorker' in navigator) || !('PushManager' in window)) {
            this.state$.next('unsupported');
            return 'unsupported';
        }
        if (Notification.permission === 'denied') {
            this.state$.next('denied');
            return 'denied';
        }
        const reg = await this.swRegistration();
        const sub = reg ? await reg.pushManager.getSubscription() : null;
        const next: PushState = sub ? 'enabled' : 'disabled';
        this.state$.next(next);
        return next;
    }

    async enable(): Promise<PushState> {
        const supported = await this.refresh();
        if (supported === 'unsupported' || supported === 'denied') return supported;

        // Notification permission is gesture-gated; this call must be
        // triggered by a user action (a button click in the UI).
        if (Notification.permission === 'default') {
            const decision = await Notification.requestPermission();
            if (decision !== 'granted') {
                this.state$.next(decision === 'denied' ? 'denied' : 'disabled');
                return this.state$.value;
            }
        }

        const reg = await this.ensureRegistration();
        let sub  = await reg.pushManager.getSubscription();
        if (!sub) {
            const vapid = await firstValueFrom(this.http.getVapidPublicKey());
            sub = await reg.pushManager.subscribe({
                userVisibleOnly:      true,
                applicationServerKey: urlBase64ToUint8Array(vapid.key),
            });
        }

        await firstValueFrom(this.http.registerPushSubscription(toPayload(sub)));
        this.state$.next('enabled');
        return 'enabled';
    }

    async disable(): Promise<PushState> {
        const reg = await this.swRegistration();
        const sub = reg ? await reg.pushManager.getSubscription() : null;
        if (sub) {
            try { await sub.unsubscribe(); } catch { /* cloud handles 410 */ }
        }
        this.state$.next('disabled');
        return 'disabled';
    }

    private async ensureRegistration(): Promise<ServiceWorkerRegistration> {
        const existing = await this.swRegistration();
        if (existing) return existing;
        return navigator.serviceWorker.register(SW_URL);
    }

    private async swRegistration(): Promise<ServiceWorkerRegistration | undefined> {
        if (!('serviceWorker' in navigator)) return undefined;
        try { return await navigator.serviceWorker.getRegistration(SW_URL); }
        catch { return undefined; }
    }
}

// ─── helpers ─────────────────────────────────────────────────────────

function urlBase64ToUint8Array(b64: string): Uint8Array {
    const padded = b64 + '=='.slice(0, (4 - (b64.length % 4)) % 4);
    const std    = padded.replace(/-/g, '+').replace(/_/g, '/');
    const raw    = atob(std);
    const out    = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
    return out;
}

function toPayload(sub: PushSubscription): PushSubscriptionPayload {
    const p256dh = sub.getKey('p256dh');
    const auth   = sub.getKey('auth');
    if (!p256dh || !auth) throw new Error('PushService: subscription missing p256dh/auth');
    return {
        endpoint: sub.endpoint,
        keys: {
            p256dh: arrayBufferToBase64Url(p256dh),
            auth:   arrayBufferToBase64Url(auth),
        },
    };
}

function arrayBufferToBase64Url(buf: ArrayBuffer): string {
    const bytes = new Uint8Array(buf);
    let bin = '';
    for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return btoa(bin).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
