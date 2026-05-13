import { ComponentFixture, TestBed, fakeAsync, tick } from '@angular/core/testing';
import { ReactiveFormsModule } from '@angular/forms';
import { HttpClientTestingModule, HttpTestingController } from '@angular/common/http/testing';
import { ClarityModule } from '@clr/angular';
import { NoopAnimationsModule } from '@angular/platform-browser/animations';

import { DirectoryComponent } from './directory.component';
import { AuthService } from 'src/common/auth.service';
import { SipService } from 'src/common/sip.service';
import { PubsubsvcService } from 'src/common/pubsubsvc.service';
import {
    SIP_UA_FACTORY, SipUaFactory, SipUaHandle, SipUaOpts, SipUaStateChange,
    SipCallHandle, SipCallStateChange,
} from 'src/common/sip-ua';
import { DirectoryEntry, UriMap } from 'src/common/app-globals';

// Minimal fakes so SipService can be constructed without sip.js.
class _FakeCall implements SipCallHandle {
    onStateChange(_: (c: SipCallStateChange) => void): void {}
    async hangup(): Promise<void> {}
    setMute(_: boolean): void {}
    getRemoteStream(): MediaStream | undefined { return undefined; }
}
class _FakeUa implements SipUaHandle {
    onStateChange(_: (c: SipUaStateChange) => void): void {}
    async start(): Promise<void> {}
    async stop():  Promise<void> {}
    placeCall(_: string): SipCallHandle { return new _FakeCall(); }
}
class _FakeFactory implements SipUaFactory {
    create(_: SipUaOpts): SipUaHandle { return new _FakeUa(); }
}

describe('DirectoryComponent', () => {

    let fixture: ComponentFixture<DirectoryComponent>;
    let comp:    DirectoryComponent;
    let backend: HttpTestingController;
    let auth:    AuthService;
    let sip:     SipService;
    let pubsub:  PubsubsvcService;

    const directoryUrl = UriMap.get('from_web_directory') as string;

    const rows: DirectoryEntry[] = [
        { flatNumber: 'A-204', displayName: 'Alice', sipUri: 'sip:A-204@pbx.s1', online: true  },
        { flatNumber: 'A-205', displayName: 'Bob',   sipUri: 'sip:A-205@pbx.s1', online: false },
        { flatNumber: 'A-206', displayName: 'Carol', sipUri: 'sip:A-206@pbx.s1', online: true  },
    ];

    beforeEach(async () => {
        localStorage.clear();
        await TestBed.configureTestingModule({
            declarations: [DirectoryComponent],
            imports: [
                ReactiveFormsModule, HttpClientTestingModule,
                ClarityModule, NoopAnimationsModule,
            ],
            providers: [
                { provide: SIP_UA_FACTORY, useValue: new _FakeFactory() },
            ],
        }).compileComponents();

        auth   = TestBed.inject(AuthService);
        sip    = TestBed.inject(SipService);
        pubsub = TestBed.inject(PubsubsvcService);

        // Seed an authenticated subscriber so the component picks up societyId.
        auth.setSession('tok', {
            societyId: 'soc-123', flatNumber: 'A-204', displayName: 'Alice',
            sipUser:   'A-204',   role: 'resident',
        });
        // Mimic a registered SIP state so canCall() reports true.
        pubsub.emit_callState({ kind: 'registered' });

        fixture = TestBed.createComponent(DirectoryComponent);
        comp    = fixture.componentInstance;
        backend = TestBed.inject(HttpTestingController);
        fixture.detectChanges();
    });

    afterEach(() => backend.verify());

    it('debounces the search input and queries with the entered prefix', fakeAsync(() => {
        comp.search.setValue('A');
        tick(100);
        backend.expectNone(directoryUrl);     // still inside debounce window

        comp.search.setValue('A-');
        tick(300);                            // > 250 ms debounce

        const req = backend.expectOne(r => r.url === directoryUrl
            && r.params.get('societyId')  === 'soc-123'
            && r.params.get('flatPrefix') === 'A-');
        req.flush(rows);

        // The signed-in user (A-204) is filtered out so we don't list self.
        expect(comp.results.map(r => r.flatNumber)).toEqual(['A-205', 'A-206']);
    }));

    it('clicking Call delegates to SipService.placeCall with the row flat', fakeAsync(() => {
        const spy = spyOn(sip, 'placeCall');
        comp.search.setValue('A');
        tick(300);
        backend.expectOne(r => r.url === directoryUrl).flush(rows);

        comp.onCall(rows[2]);                 // Carol — online
        expect(spy).toHaveBeenCalledWith('A-206');
    }));

    it('does not call when SIP is not registered', fakeAsync(() => {
        pubsub.emit_callState({ kind: 'idle' });
        fixture.detectChanges();
        const spy = spyOn(sip, 'placeCall');

        comp.search.setValue('A');
        tick(300);
        backend.expectOne(r => r.url === directoryUrl).flush(rows);

        comp.onCall(rows[2]);
        expect(spy).not.toHaveBeenCalled();
    }));

    it('surfaces a friendly message when the lookup fails', fakeAsync(() => {
        comp.search.setValue('A');
        tick(300);
        backend.expectOne(r => r.url === directoryUrl).flush('nope', {
            status: 500, statusText: 'Server Error',
        });
        expect(comp.errorMsg).toMatch(/server error|lookup failed/i);
        expect(comp.results).toEqual([]);
    }));
});
