import { Injectable } from '@angular/core';
import { Subscriber } from './app-globals';
import { PubsubsvcService } from './pubsubsvc.service';

// Persistent session: bearer token + cached Subscriber. Backed by
// localStorage so the softphone stays signed-in across reloads — push
// wakeup needs the SW to be able to bring the app back to a logged-in
// state. Anyone with cross-origin XSS can read the token; that risk is
// accepted (same trade-off xpmile makes).
//
// On every load, the constructor rehydrates state and re-emits the
// subscriber via PubsubsvcService so components don't each have to
// poke localStorage.

const STORAGE_TOKEN      = 'pbxui:auth-token';
const STORAGE_SUBSCRIBER = 'pbxui:subscriber';

@Injectable({ providedIn: 'root' })
export class AuthService {

    private token?: string;
    private subscriber?: Subscriber;

    constructor(private pubsub: PubsubsvcService) {
        this.rehydrate();
    }

    private rehydrate(): void {
        try {
            const raw = localStorage.getItem(STORAGE_SUBSCRIBER);
            const tok = localStorage.getItem(STORAGE_TOKEN);
            if (tok && raw) {
                this.token      = tok;
                this.subscriber = JSON.parse(raw) as Subscriber;
                this.pubsub.emit_subscriber(this.subscriber);
            }
        } catch {
            // Corrupt cache — clear it; the user will be sent back to /login
            // by the AuthGuard on the next navigation.
            this.clearSession();
        }
    }

    setSession(token: string, subscriber: Subscriber): void {
        this.token      = token;
        this.subscriber = subscriber;
        localStorage.setItem(STORAGE_TOKEN, token);
        localStorage.setItem(STORAGE_SUBSCRIBER, JSON.stringify(subscriber));
        this.pubsub.emit_subscriber(subscriber);
    }

    clearSession(): void {
        this.token      = undefined;
        this.subscriber = undefined;
        localStorage.removeItem(STORAGE_TOKEN);
        localStorage.removeItem(STORAGE_SUBSCRIBER);
        this.pubsub.emit_subscriber(undefined);
    }

    getToken(): string | undefined      { return this.token; }
    getSubscriber(): Subscriber | undefined { return this.subscriber; }
    isAuthenticated(): boolean          { return !!this.token; }
}
