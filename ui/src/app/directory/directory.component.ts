import { Component, OnDestroy, OnInit } from '@angular/core';
import { FormControl } from '@angular/forms';
import { Subscription } from 'rxjs';
import { debounceTime, distinctUntilChanged, switchMap, tap, catchError } from 'rxjs/operators';
import { of } from 'rxjs';

import { HttpsvcService } from 'src/common/httpsvc.service';
import { PubsubsvcService, CallState } from 'src/common/pubsubsvc.service';
import { SipService } from 'src/common/sip.service';
import { AuthService } from 'src/common/auth.service';
import { DirectoryEntry } from 'src/common/app-globals';

@Component({
    selector: 'app-directory',
    templateUrl: './directory.component.html',
    styleUrls: ['./directory.component.scss'],
})
export class DirectoryComponent implements OnInit, OnDestroy {

    search = new FormControl<string>('', { nonNullable: true });
    results: DirectoryEntry[] = [];
    loading = false;
    errorMsg = '';
    callState: CallState = { kind: 'idle' };

    private subs: Subscription[] = [];

    constructor(
        private http: HttpsvcService,
        private auth: AuthService,
        private pubsub: PubsubsvcService,
        private sip: SipService,
    ) {}

    ngOnInit(): void {
        const societyId = this.auth.getSubscriber()?.societyId ?? '';

        this.subs.push(this.pubsub.onCallState.subscribe(s => this.callState = s));

        this.subs.push(
            this.search.valueChanges.pipe(
                debounceTime(250),
                distinctUntilChanged(),
                tap(() => { this.loading = true; this.errorMsg = ''; }),
                switchMap(prefix => this.http.searchDirectory(societyId, prefix).pipe(
                    catchError(err => {
                        this.errorMsg = err?.statusText
                            ? `Lookup failed: ${err.statusText}`
                            : 'Lookup failed';
                        return of([] as DirectoryEntry[]);
                    }),
                )),
            ).subscribe(rows => {
                this.results = (rows ?? []).filter(r => r.flatNumber !== this.auth.getSubscriber()?.flatNumber);
                this.loading = false;
            }),
        );
    }

    ngOnDestroy(): void { for (const s of this.subs) s.unsubscribe(); }

    canCall(): boolean { return this.callState.kind === 'registered'; }

    onCall(entry: DirectoryEntry): void {
        if (!this.canCall()) return;
        this.sip.placeCall(entry.flatNumber);
    }
}
