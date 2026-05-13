import { Component, OnDestroy, OnInit } from '@angular/core';
import { Router } from '@angular/router';
import { Subscription } from 'rxjs';

import { AuthService } from 'src/common/auth.service';
import { PubsubsvcService } from 'src/common/pubsubsvc.service';
import { Subscriber } from 'src/common/app-globals';

// Authenticated shell. Header shows the active subscriber + a Sign-out
// button; the <router-outlet> carries the per-feature content. AuthGuard
// guarantees subscriber is set by the time we arrive here, but we still
// subscribe to PubsubsvcService.onSubscriber so the header updates on
// rehydrate / cross-tab session changes.

@Component({
    selector: 'app-main',
    templateUrl: './main.component.html',
    styleUrls: ['./main.component.scss'],
})
export class MainComponent implements OnInit, OnDestroy {

    subscriber?: Subscriber;
    private sub?: Subscription;

    constructor(
        private auth: AuthService,
        private router: Router,
        private pubsub: PubsubsvcService,
    ) {}

    ngOnInit(): void {
        this.sub = this.pubsub.onSubscriber.subscribe(s => this.subscriber = s);
    }

    ngOnDestroy(): void { this.sub?.unsubscribe(); }

    onSignOut(): void {
        this.auth.clearSession();
        this.router.navigateByUrl('/login');
    }
}
