import { Injectable } from '@angular/core';
import { BehaviorSubject } from 'rxjs';
import { Subscriber } from './app-globals';

// Pub-sub for cross-component state. Same `BehaviorSubject` pattern as
// xpmile/ui/src/common/pubsubsvc.service.ts. Topics are softphone-
// specific; more are added as feature modules land.

export type CallState =
    | { kind: 'idle' }
    | { kind: 'registering' }
    | { kind: 'registered' }
    | { kind: 'incoming';   fromFlat: string; callId: string }
    | { kind: 'outgoing';   toFlat:   string; callId: string }
    | { kind: 'in-call';    peerFlat: string; callId: string; startedAt: number }
    | { kind: 'failed';     reason:   string };

@Injectable({ providedIn: 'root' })
export class PubsubsvcService {

    private subscriberBs$ = new BehaviorSubject<Subscriber | undefined>(undefined);
    private callStateBs$  = new BehaviorSubject<CallState>({ kind: 'idle' });

    constructor() {}

    /** Currently logged-in subscriber. `undefined` => not authenticated. */
    public onSubscriber = this.subscriberBs$.asObservable();
    public emit_subscriber(s: Subscriber | undefined): void {
        this.subscriberBs$.next(s);
    }

    /** Current SIP UA / call state. */
    public onCallState = this.callStateBs$.asObservable();
    public emit_callState(state: CallState): void {
        this.callStateBs$.next(state);
    }
}
