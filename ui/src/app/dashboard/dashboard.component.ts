import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';

import { PubsubsvcService } from 'src/common/pubsubsvc.service';
import { Subscriber } from 'src/common/app-globals';

// Stub dashboard. Slice 2+ adds widgets for SIP registration state,
// recent calls, push permission, and TURN credential expiry.
@Component({
    selector: 'app-dashboard',
    templateUrl: './dashboard.component.html',
    styleUrls: ['./dashboard.component.scss'],
})
export class DashboardComponent implements OnInit, OnDestroy {

    subscriber?: Subscriber;
    private sub?: Subscription;

    constructor(private pubsub: PubsubsvcService) {}

    ngOnInit(): void {
        this.sub = this.pubsub.onSubscriber.subscribe(s => this.subscriber = s);
    }

    ngOnDestroy(): void { this.sub?.unsubscribe(); }
}
