import { ComponentFixture, TestBed } from '@angular/core/testing';
import { HttpClientTestingModule, HttpTestingController } from '@angular/common/http/testing';
import { CommonModule } from '@angular/common';

import { HistoryComponent } from './history.component';
import { AuthService } from 'src/common/auth.service';
import { CallRecord, UriMap } from 'src/common/app-globals';

const cdrUrl = UriMap.get('from_web_cdr') as string;

const rows: CallRecord[] = [
    {
        callId: 'c1', societyId: 's1', fromFlat: 'A-204', toFlat: 'B-12',
        direction: 'outbound', type: 'p2p',
        startedAt: '2026-05-13T10:00:00Z', endedAt: '2026-05-13T10:02:00Z',
        durationSec: 120, hangupCause: 'normal',
    },
    {
        callId: 'c2', societyId: 's1', fromFlat: 'C-99', toFlat: 'A-204',
        direction: 'inbound',  type: 'p2p',
        startedAt: '2026-05-14T09:00:00Z', endedAt: '2026-05-14T09:00:15Z',
        durationSec: 0, hangupCause: 'busy',
    },
    {
        callId: 'c3', societyId: 's1', fromFlat: 'A-204', toFlat: 'Conference',
        direction: 'outbound', type: 'conference', conferenceBridge: 'br-1',
        startedAt: '2026-05-15T14:00:00Z', endedAt: '2026-05-15T14:30:00Z',
        durationSec: 1800, hangupCause: 'normal',
    },
];

describe('HistoryComponent', () => {

    let fixture: ComponentFixture<HistoryComponent>;
    let comp:    HistoryComponent;
    let backend: HttpTestingController;
    let auth:    AuthService;

    beforeEach(async () => {
        localStorage.clear();
        await TestBed.configureTestingModule({
            declarations: [HistoryComponent],
            imports: [HttpClientTestingModule, CommonModule],
        }).compileComponents();

        auth    = TestBed.inject(AuthService);
        backend = TestBed.inject(HttpTestingController);

        auth.setSession('tok', {
            societyId: 's1', flatNumber: 'A-204', displayName: 'Alice',
            sipUser:   'A-204', role: 'resident',
        });

        fixture = TestBed.createComponent(HistoryComponent);
        comp    = fixture.componentInstance;
    });

    afterEach(() => backend.verify());

    it('fetches CDR for the signed-in flat on init and sorts newest first', () => {
        fixture.detectChanges();
        const req = backend.expectOne(r => r.url === cdrUrl
            && r.params.get('flat') === 'A-204');
        req.flush(rows);

        expect(comp.rows.map(r => r.callId)).toEqual(['c3', 'c2', 'c1']);
        expect(comp.loading).toBeFalse();
        expect(comp.errorMsg).toBe('');
    });

    it('peerOf returns the *other* party regardless of direction', () => {
        expect(comp.peerOf(rows[0])).toBe('B-12');    // outbound → toFlat
        expect(comp.peerOf(rows[1])).toBe('C-99');    // inbound  → fromFlat
    });

    it('durationLabel formats seconds as mm:ss; 0 renders as em dash', () => {
        expect(comp.durationLabel(rows[0])).toBe('02:00');
        expect(comp.durationLabel(rows[1])).toBe('—');
        expect(comp.durationLabel(rows[2])).toBe('30:00');
    });

    it('refresh() re-issues the request', () => {
        fixture.detectChanges();
        backend.expectOne(r => r.url === cdrUrl).flush([]);

        comp.refresh();
        backend.expectOne(r => r.url === cdrUrl).flush([rows[0]]);
        expect(comp.rows.length).toBe(1);
    });

    it('surfaces a friendly message on HTTP failure', () => {
        fixture.detectChanges();
        backend.expectOne(r => r.url === cdrUrl).flush('nope', {
            status: 500, statusText: 'Server Error',
        });
        expect(comp.errorMsg).toMatch(/server error|couldn.t load/i);
        expect(comp.rows).toEqual([]);
    });
});
