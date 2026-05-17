import { ComponentFixture, TestBed } from '@angular/core/testing';
import { Subject } from 'rxjs';

import { DashboardComponent } from './dashboard.component';
import { PubsubsvcService, CallState } from 'src/common/pubsubsvc.service';
import { SipService } from 'src/common/sip.service';
import { Subscriber } from 'src/common/app-globals';

// We don't drive the real SipService here — it pulls in sip.js, an
// `AuthService` with HttpClient deps, and a UA factory. The dashboard's
// contract with SipService is just `connect()` / `disconnect()` /
// `joinConference()`, so a hand-rolled stub keeps the test surface
// tight and the failure mode obvious if anything else gets bolted on
// later.

class StubSipService {
    connectCalls = 0;
    disconnectCalls = 0;
    joinConferenceCalls = 0;

    connect():        Promise<void> { this.connectCalls++;        return Promise.resolve(); }
    disconnect():     Promise<void> { this.disconnectCalls++;     return Promise.resolve(); }
    joinConference(): void          { this.joinConferenceCalls++;                          }
}

class StubPubsub {
    // Subjects so the dashboard's subscriptions land on real observables
    // and ngOnDestroy's unsubscribe path is exercised.
    onSubscriber = new Subject<Subscriber>();
    onCallState  = new Subject<CallState>();
}

describe('DashboardComponent', () => {

    let fixture: ComponentFixture<DashboardComponent>;
    let comp:    DashboardComponent;
    let sip:     StubSipService;

    beforeEach(async () => {
        sip = new StubSipService();
        await TestBed.configureTestingModule({
            declarations: [DashboardComponent],
            providers: [
                { provide: SipService,        useValue: sip            },
                { provide: PubsubsvcService,  useClass: StubPubsub     },
            ],
        }).compileComponents();

        fixture = TestBed.createComponent(DashboardComponent);
        comp    = fixture.componentInstance;
    });

    it('auto-calls sip.connect() on init (login = online)', () => {
        expect(sip.connectCalls).toBe(0);
        fixture.detectChanges();        // triggers ngOnInit
        expect(sip.connectCalls).toBe(1)
            ;
        // Regression guard for the behaviour landed 2026-05-17 — without
        // this, the resident has to click CONNECT before any Directory
        // CALL button is enabled, which surprised every first-time
        // user. SipService.connect() is itself idempotent so a second
        // ngOnInit (navigate-away-and-back) is harmless.
    });

    it('disconnect button still triggers sip.disconnect()', async () => {
        fixture.detectChanges();
        await comp.onDisconnect();
        expect(sip.disconnectCalls).toBe(1)
            ; // explicit "go offline" path stays wired
    });

    it('join-conference button calls sip.joinConference()', () => {
        fixture.detectChanges();
        comp.onJoinConference();
        expect(sip.joinConferenceCalls).toBe(1);
    });
});
