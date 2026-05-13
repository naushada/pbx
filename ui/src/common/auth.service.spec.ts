import { TestBed } from '@angular/core/testing';

import { AuthService } from './auth.service';
import { PubsubsvcService } from './pubsubsvc.service';
import { Subscriber } from './app-globals';

const SAMPLE: Subscriber = {
    societyId:   'soc-123',
    flatNumber:  'A-204',
    displayName: 'Alice Resident',
    sipUser:     'A-204',
    role:        'resident',
};

describe('AuthService', () => {

    let svc:    AuthService;
    let pubsub: PubsubsvcService;

    beforeEach(() => {
        localStorage.clear();
        TestBed.configureTestingModule({});
        pubsub = TestBed.inject(PubsubsvcService);
        svc    = TestBed.inject(AuthService);
    });

    it('starts unauthenticated when nothing is in localStorage', () => {
        expect(svc.isAuthenticated()).toBeFalse();
        expect(svc.getToken()).toBeUndefined();
        expect(svc.getSubscriber()).toBeUndefined();
    });

    it('setSession persists token + subscriber and emits to pubsub', (done) => {
        const seen: (Subscriber | undefined)[] = [];
        pubsub.onSubscriber.subscribe(s => {
            seen.push(s);
            if (seen.length === 2) {
                // [0] = undefined initial, [1] = SAMPLE after setSession
                expect(seen[0]).toBeUndefined();
                expect(seen[1]).toEqual(SAMPLE);
                done();
            }
        });

        svc.setSession('tok-xyz', SAMPLE);
        expect(svc.isAuthenticated()).toBeTrue();
        expect(svc.getToken()).toBe('tok-xyz');
        expect(svc.getSubscriber()).toEqual(SAMPLE);
        expect(localStorage.getItem('pbxui:auth-token')).toBe('tok-xyz');
    });

    it('clearSession drops persisted state and emits undefined', () => {
        svc.setSession('tok-xyz', SAMPLE);
        expect(svc.isAuthenticated()).toBeTrue();

        svc.clearSession();
        expect(svc.isAuthenticated()).toBeFalse();
        expect(svc.getToken()).toBeUndefined();
        expect(svc.getSubscriber()).toBeUndefined();
        expect(localStorage.getItem('pbxui:auth-token')).toBeNull();
        expect(localStorage.getItem('pbxui:subscriber')).toBeNull();
    });

    it('rehydrates from localStorage on construction', () => {
        localStorage.setItem('pbxui:auth-token', 'persisted-tok');
        localStorage.setItem('pbxui:subscriber', JSON.stringify(SAMPLE));

        TestBed.resetTestingModule();
        TestBed.configureTestingModule({});
        const fresh = TestBed.inject(AuthService);

        expect(fresh.isAuthenticated()).toBeTrue();
        expect(fresh.getToken()).toBe('persisted-tok');
        expect(fresh.getSubscriber()).toEqual(SAMPLE);
    });

    it('clears state when localStorage contains garbage subscriber JSON', () => {
        localStorage.setItem('pbxui:auth-token', 'tok');
        localStorage.setItem('pbxui:subscriber', '{not-json');

        TestBed.resetTestingModule();
        TestBed.configureTestingModule({});
        const fresh = TestBed.inject(AuthService);

        expect(fresh.isAuthenticated()).toBeFalse();
        expect(localStorage.getItem('pbxui:auth-token')).toBeNull();
    });
});
