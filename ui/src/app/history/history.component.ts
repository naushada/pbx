import { Component, OnInit } from '@angular/core';

import { HttpsvcService } from 'src/common/httpsvc.service';
import { AuthService } from 'src/common/auth.service';
import { CallRecord } from 'src/common/app-globals';

// Fetches CDR rows from the cloud (GET /api/v1/cdr?flat=...) and
// renders them newest-first. Sort happens client-side because the
// cloud's response order is implementation-defined.
@Component({
    selector: 'app-history',
    templateUrl: './history.component.html',
    styleUrls: ['./history.component.scss'],
})
export class HistoryComponent implements OnInit {

    rows: CallRecord[] = [];
    loading = false;
    errorMsg = '';

    constructor(private http: HttpsvcService, private auth: AuthService) {}

    ngOnInit(): void { this.refresh(); }

    refresh(): void {
        const flat = this.auth.getSubscriber()?.flatNumber;
        if (!flat) return;
        this.loading = true; this.errorMsg = '';
        this.http.getCallHistory(flat).subscribe({
            next: rows => {
                this.rows = (rows ?? []).slice().sort(
                    (a, b) => b.startedAt.localeCompare(a.startedAt),
                );
                this.loading = false;
            },
            error: err => {
                this.errorMsg = err?.statusText
                    ? `Couldn't load history: ${err.statusText}`
                    : 'Couldn\'t load history';
                this.loading = false;
            },
        });
    }

    peerOf(row: CallRecord): string {
        return row.direction === 'inbound' ? row.fromFlat : row.toFlat;
    }

    durationLabel(row: CallRecord): string {
        if (!row.durationSec) return '—';
        const mm = Math.floor(row.durationSec / 60).toString().padStart(2, '0');
        const ss = (row.durationSec % 60).toString().padStart(2, '0');
        return `${mm}:${ss}`;
    }

    causeClass(row: CallRecord): string {
        switch (row.hangupCause) {
            case 'normal':   return 'cause cause-ok';
            case 'busy':     return 'cause cause-busy';
            case 'noanswer': return 'cause cause-busy';
            case 'rejected': return 'cause cause-err';
            case 'failed':   return 'cause cause-err';
        }
    }
}
