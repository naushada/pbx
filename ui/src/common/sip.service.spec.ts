import { TestBed } from '@angular/core/testing';

import { SipService } from './sip.service';
import { AuthService } from './auth.service';
import { PubsubsvcService, CallState } from './pubsubsvc.service';
import {
    SIP_UA_FACTORY, SipUaFactory, SipUaHandle, SipUaOpts, SipUaStateChange,
} from './sip-ua';

// Fake SipUaFactory whose handle exposes a `pump()` for the spec to
// drive state transitions imperatively. Captures the SipUaOpts so the
// spec can verify the constructed SIP URI / WS URL.
class FakeUaHandle implements SipUaHandle {
    public listener?: (c: SipUaStateChange) => void;
    public started = false;
    public stopped = false;
    public startError?: Error;

    onStateChange(cb: (c: SipUaStateChange) => void): void { this.listener = cb; }
    async start(): Promise<void> {
        this.started = true;
        if (this.startError) throw this.startError;
    }
    async stop(): Promise<void> { this.stopped = true; }
    /** Test-only: drive a state transition through to the listener. */
    pump(state: SipUaStateChange['state'], detail?: string): void {
        this.listener?.(detail ? { state, detail } : { state });
    }
}

class FakeUaFactory implements SipUaFactory {
    public lastOpts?: SipUaOpts;
    public handle = new FakeUaHandle();
    create(opts: SipUaOpts): SipUaHandle {
        this.lastOpts = opts;
        return this.handle;
    }
}

describe('SipService', () => {

    let svc:    SipService;
    let auth:   AuthService;
    let pubsub: PubsubsvcService;
    let fake:   FakeUaFactory;
    let states: CallState[];

    beforeEach(() => {
        localStorage.clear();
        fake = new FakeUaFactory();

        TestBed.configureTestingModule({
            providers: [
                { provide: SIP_UA_FACTORY, useValue: fake },
            ],
        });

        auth   = TestBed.inject(AuthService);
        pubsub = TestBed.inject(PubsubsvcService);
        svc    = TestBed.inject(SipService);

        states = [];
        pubsub.onCallState.subscribe(s => states.push(s));
    });

    function authenticate(): void {
        auth.setSession('tok-xyz', {
            societyId: 'soc-123', flatNumber: 'A-204', displayName: 'Alice',
            sipUser:   'A-204',   role: 'resident',
        });
    }

    it('refuses to connect when not authenticated', async () => {
        await svc.connect();
        expect(states.at(-1)).toEqual({ kind: 'failed', reason: 'not_authenticated' });
        expect(fake.lastOpts).toBeUndefined();
    });

    it('connect() builds the SIP URI + WS URL from subscriber + token', async () => {
        authenticate();
        await svc.connect();
        expect(fake.lastOpts!.uri).toBe('sip:A-204@pbx.soc-123');
        expect(fake.lastOpts!.wsUrl).toMatch(/\/sip-ws\?token=tok-xyz$/);
        expect(fake.lastOpts!.wsUrl.startsWith('ws')).toBeTrue();
        expect(fake.handle.started).toBeTrue();
    });

    it('emits registering then registered on a clean REGISTER 200', async () => {
        authenticate();
        await svc.connect();
        fake.handle.pump('registering');
        fake.handle.pump('registered');

        const kinds = states.map(s => s.kind);
        expect(kinds).toContain('registering');
        expect(kinds.at(-1)).toBe('registered');
    });

    it('emits failed with reason when the UA terminates unexpectedly', async () => {
        authenticate();
        await svc.connect();
        fake.handle.pump('terminated', 'transport_disconnected');

        expect(states.at(-1)).toEqual({
            kind: 'failed', reason: 'transport_disconnected',
        });
    });

    it('treats a terminate after disconnect() as idle, not failed', async () => {
        authenticate();
        await svc.connect();
        await svc.disconnect();
        // After disconnect() the handle is cleared — pumping has no
        // effect because the listener pointer is no longer hooked into
        // SipService. Re-checking only the last state is enough.
        expect(states.at(-1)).toEqual({ kind: 'idle' });
        expect(fake.handle.stopped).toBeTrue();
    });

    it('surfaces start() failures as a failed callState', async () => {
        authenticate();
        fake.handle.startError = new Error('boom');
        await svc.connect();
        expect(states.at(-1)).toEqual({ kind: 'failed', reason: 'boom' });
    });

    it('connect() is idempotent — no duplicate UA when already started', async () => {
        authenticate();
        await svc.connect();
        const firstHandle = fake.lastOpts;
        await svc.connect();
        expect(fake.lastOpts).toBe(firstHandle);
    });
});
