import { Injectable, NgZone, OnDestroy } from '@angular/core';
import { Router } from '@angular/router';
import { Subscription } from 'rxjs';

import { AuthService }       from './auth.service';
import { HttpsvcService }    from './httpsvc.service';
import { PubsubsvcService }  from './pubsubsvc.service';

// Session keep-alive + idle timeout.
//
// Subscribes to PubsubsvcService.onSubscriber once, then auto-starts
// whenever a subscriber appears (login or rehydrate) and auto-stops
// when the subscriber goes away (logout or our own timeout). Callers
// don't need to remember to invoke start()/stop().
//
// While a subscriber is set:
//   - Heartbeat: GET /api/v1/ping every PING_INTERVAL_MS.
//     After PING_FAIL_THRESHOLD consecutive failures we treat the
//     cloud as unreachable and time the session out.
//   - Idle: user interaction (mousemove, keydown, touchstart, scroll)
//     stamps a `lastActivity` timestamp. If the heartbeat tick sees
//     more than IDLE_TIMEOUT_MS since lastActivity, we time out.
//
// Timing out clears the session and navigates to /login?reason=timeout
// so the login page can show a "Session timed out" banner.
//
// Activity listeners run outside the Angular zone so the constant
// mousemove stream doesn't trigger change detection — Angular's
// default would re-run CD on every event, which is wasteful here.

const PING_INTERVAL_MS    = 30_000;            // 30 seconds
const PING_FAIL_THRESHOLD = 3;                 // 3 missed pings ⇒ timeout
const IDLE_TIMEOUT_MS     = 30 * 60 * 1000;    // 30 minutes
const ACTIVITY_EVENTS     = ['mousemove', 'keydown', 'touchstart', 'scroll'];

@Injectable({ providedIn: 'root' })
export class KeepaliveService implements OnDestroy {

    private timer?: ReturnType<typeof setInterval>;
    private lastActivity = Date.now();
    private failures     = 0;
    private subSub?:  Subscription;
    private boundOnActivity = () => { this.lastActivity = Date.now(); };

    constructor(
        private auth:   AuthService,
        private http:   HttpsvcService,
        private router: Router,
        private pubsub: PubsubsvcService,
        private zone:   NgZone,
    ) {
        // BehaviorSubject — fires immediately with the current value,
        // so if AuthService rehydrated a session at boot we'll start
        // right away without the caller having to ask.
        this.subSub = this.pubsub.onSubscriber.subscribe((s) => {
            if (s) this.start();
            else   this.stop();
        });
    }

    ngOnDestroy(): void {
        this.subSub?.unsubscribe();
        this.stop();
    }

    // ── private ──────────────────────────────────────────────────────

    private start(): void {
        if (this.timer) return;
        this.lastActivity = Date.now();
        this.failures     = 0;

        this.zone.runOutsideAngular(() => {
            ACTIVITY_EVENTS.forEach((ev) =>
                window.addEventListener(ev, this.boundOnActivity, { passive: true }));
            this.timer = setInterval(() => this.tick(), PING_INTERVAL_MS);
        });
    }

    private stop(): void {
        if (this.timer) {
            clearInterval(this.timer);
            this.timer = undefined;
        }
        ACTIVITY_EVENTS.forEach((ev) =>
            window.removeEventListener(ev, this.boundOnActivity));
    }

    private tick(): void {
        // Idle check first — cheaper than a ping round-trip.
        if (Date.now() - this.lastActivity > IDLE_TIMEOUT_MS) {
            this.timeout('idle');
            return;
        }

        // Heartbeat. Runs inside the zone so the HTTP error path can
        // trigger Angular's router cleanly.
        this.zone.run(() => {
            this.http.ping().subscribe({
                next: () => { this.failures = 0; },
                error: () => {
                    this.failures += 1;
                    if (this.failures >= PING_FAIL_THRESHOLD) {
                        this.timeout('unreachable');
                    }
                },
            });
        });
    }

    private timeout(_reason: 'idle' | 'unreachable'): void {
        // clearSession() emits subscriber=undefined which will route
        // back through our subscription and call stop().
        this.auth.clearSession();
        this.router.navigate(['/login'], { queryParams: { reason: 'timeout' } });
    }
}
