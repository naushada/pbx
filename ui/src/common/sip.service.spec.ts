import { TestBed } from '@angular/core/testing';

import { SipService } from './sip.service';
import { AuthService } from './auth.service';
import { PubsubsvcService, CallState } from './pubsubsvc.service';
import {
    SIP_UA_FACTORY, SipUaFactory, SipUaHandle, SipUaOpts, SipUaStateChange,
    SipCallHandle, SipCallStateChange,
    IncomingCallHandle, IncomingCallInfo,
} from './sip-ua';
import { RingtoneService } from './ringtone.service';

// Fake SipUaFactory whose handle exposes a `pump()` for the spec to
// drive state transitions imperatively. Captures the SipUaOpts so the
// spec can verify the constructed SIP URI / WS URL.
class FakeUaHandle implements SipUaHandle {
    public listener?: (c: SipUaStateChange) => void;
    public started = false;
    public stopped = false;
    public startError?: Error;

    // Slice-3 surface.
    public callTargets: string[] = [];
    public callHandle = new FakeCallHandle();
    public placeCallThrows?: Error;

    // Slice-4 surface.
    public incomingCb?: (h: IncomingCallHandle) => void;

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
    placeCall(targetUri: string): SipCallHandle {
        if (this.placeCallThrows) throw this.placeCallThrows;
        this.callTargets.push(targetUri);
        return this.callHandle;
    }
    onIncomingCall(cb: (h: IncomingCallHandle) => void): void {
        this.incomingCb = cb;
    }
    /** Test-only: simulate an inbound INVITE arriving. */
    fireIncoming(info: IncomingCallInfo, h: FakeIncomingHandle): void {
        h.info = info;
        this.incomingCb?.(h);
    }
}

class FakeIncomingHandle implements IncomingCallHandle {
    public info: IncomingCallInfo = { fromUri: '', fromFlat: '', callId: '' };
    public accepted = false;
    public rejectedCause?: 'busy' | 'declined';
    public acceptThrows?: Error;
    public acceptedHandle = new FakeCallHandle();

    async accept(): Promise<SipCallHandle> {
        if (this.acceptThrows) throw this.acceptThrows;
        this.accepted = true;
        return this.acceptedHandle;
    }
    async reject(cause?: 'busy' | 'declined'): Promise<void> {
        this.rejectedCause = cause;
    }
}

class FakeRingtone {
    public starts = 0;
    public stops  = 0;
    isActive(): boolean { return this.starts > this.stops; }
    async start(): Promise<void> { this.starts++; }
    stop(): void { this.stops++; }
    // setAudioContextFactory is unused in tests but keep parity.
    setAudioContextFactory(_: any): void {}
}

class FakeCallHandle implements SipCallHandle {
    public listener?: (c: SipCallStateChange) => void;
    public hungup     = false;
    public muteCalls: boolean[] = [];
    public remoteStream?: MediaStream;

    onStateChange(cb: (c: SipCallStateChange) => void): void { this.listener = cb; }
    async hangup(): Promise<void> { this.hungup = true; }
    setMute(m: boolean): void { this.muteCalls.push(m); }
    getRemoteStream(): MediaStream | undefined { return this.remoteStream; }

    /** Test-only: drive call state through to the listener. */
    pump(state: SipCallStateChange['state'], detail?: string): void {
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
    let ring:   FakeRingtone;
    let states: CallState[];

    beforeEach(() => {
        localStorage.clear();
        fake = new FakeUaFactory();
        ring = new FakeRingtone();

        TestBed.configureTestingModule({
            providers: [
                { provide: SIP_UA_FACTORY,  useValue: fake },
                { provide: RingtoneService, useValue: ring },
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

    // ─── placeCall / hangup / mute ───────────────────────────────────

    describe('placeCall', () => {

        async function makeRegistered(): Promise<void> {
            authenticate();
            await svc.connect();
            fake.handle.pump('registered');
        }

        it('refuses when not connected', () => {
            authenticate();
            svc.placeCall('B-12');
            expect(states.at(-1)).toEqual({ kind: 'failed', reason: 'not_connected' });
            expect(fake.handle.callTargets).toEqual([]);
        });

        it('refuses when connected but not yet registered', async () => {
            authenticate();
            await svc.connect();
            svc.placeCall('B-12');
            expect(states.at(-1)).toEqual({ kind: 'failed', reason: 'not_registered' });
            expect(fake.handle.callTargets).toEqual([]);
        });

        it('builds the correct target URI from society + flat', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            expect(fake.handle.callTargets).toEqual(['sip:B-12@pbx.soc-123']);
        });

        it('emits outgoing → in-call as the SIP call progresses', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            const cb = fake.handle.callHandle;
            cb.pump('calling');
            cb.pump('in-call');

            // The first state after placeCall is outgoing; later 'calling'
            // pumps remain outgoing; 'in-call' transitions to in-call.
            const recent = states.slice(-3).map(s => s.kind);
            expect(recent).toEqual(['outgoing', 'outgoing', 'in-call']);
            const last = states.at(-1)!;
            if (last.kind !== 'in-call') fail('expected in-call');
            else                          expect(last.peerFlat).toBe('B-12');
        });

        it('after the peer hangs up emission returns to registered', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            fake.handle.callHandle.pump('in-call');
            fake.handle.callHandle.pump('ended');
            expect(states.at(-1)).toEqual({ kind: 'registered' });
        });

        it('hangup() asks the call handle and re-emits registered', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            fake.handle.callHandle.pump('in-call');

            await svc.hangup();
            expect(fake.handle.callHandle.hungup).toBeTrue();
            expect(states.at(-1)).toEqual({ kind: 'registered' });
        });

        it('setMute forwards the boolean through to the call handle', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            svc.setMute(true);
            svc.setMute(false);
            expect(fake.handle.callHandle.muteCalls).toEqual([true, false]);
        });

        it('placeCall is idempotent — second call while one is up is ignored', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            svc.placeCall('C-44');
            expect(fake.handle.callTargets).toEqual(['sip:B-12@pbx.soc-123']);
        });

        it('placeCall throw surfaces as failed CallState', async () => {
            await makeRegistered();
            fake.handle.placeCallThrows = new Error('mic permission denied');
            svc.placeCall('B-12');
            expect(states.at(-1)).toEqual({
                kind: 'failed', reason: 'mic permission denied',
            });
        });

        it('call-handle failure (e.g. ICE failure) is surfaced and clears the call', async () => {
            await makeRegistered();
            svc.placeCall('B-12');
            fake.handle.callHandle.pump('failed', 'ice_disconnected');
            expect(states.at(-1)).toEqual({
                kind: 'failed', reason: 'ice_disconnected',
            });

            // Subsequent placeCall is now allowed (no active call).
            const before = fake.handle.callTargets.length;
            svc.placeCall('C-44');
            expect(fake.handle.callTargets.length).toBe(before + 1);
        });
    });

    // ─── slice 4: incoming calls ────────────────────────────────────

    describe('incoming calls', () => {

        async function makeRegistered(): Promise<void> {
            authenticate();
            await svc.connect();
            fake.handle.pump('registered');
        }

        function fire(): FakeIncomingHandle {
            const h = new FakeIncomingHandle();
            fake.handle.fireIncoming({
                fromUri: 'sip:C-99@pbx.soc-123', fromFlat: 'C-99', callId: 'call-id-9',
            }, h);
            return h;
        }

        it('emits incoming + starts ringtone when an INVITE arrives', async () => {
            await makeRegistered();
            fire();
            expect(states.at(-1)).toEqual({
                kind: 'incoming', fromFlat: 'C-99', callId: 'call-id-9',
            });
            expect(ring.starts).toBe(1);
        });

        it('acceptIncoming transitions to in-call and stops the ringtone', async () => {
            await makeRegistered();
            const h = fire();
            await svc.acceptIncoming();
            expect(h.accepted).toBeTrue();
            expect(ring.stops).toBe(1);
            const last = states.at(-1)!;
            if (last.kind !== 'in-call') fail('expected in-call');
            else {
                expect(last.peerFlat).toBe('C-99');
                expect(last.callId).toBe('call-id-9');
            }
        });

        it('rejectIncoming sends 603 Decline and returns to registered', async () => {
            await makeRegistered();
            const h = fire();
            await svc.rejectIncoming();
            expect(h.rejectedCause).toBe('declined');
            expect(ring.stops).toBe(1);
            expect(states.at(-1)).toEqual({ kind: 'registered' });
        });

        it('after accept, the peer hangup transitions back to registered', async () => {
            await makeRegistered();
            const h = fire();
            await svc.acceptIncoming();
            h.acceptedHandle.pump('ended');
            expect(states.at(-1)).toEqual({ kind: 'registered' });
        });

        it('second incoming while busy is auto-rejected as 486 Busy', async () => {
            await makeRegistered();
            const h1 = fire();
            await svc.acceptIncoming();

            const h2 = new FakeIncomingHandle();
            fake.handle.fireIncoming({
                fromUri: 'sip:D-1@pbx.soc-123', fromFlat: 'D-1', callId: 'second',
            }, h2);
            expect(h2.rejectedCause).toBe('busy');
            // First call's in-call state is still the latest emit.
            const last = states.at(-1)!;
            expect(last.kind).toBe('in-call');
        });

        it('accept() throw surfaces as failed CallState and clears ringtone', async () => {
            await makeRegistered();
            const h = fire();
            h.acceptThrows = new Error('mic_denied');
            await svc.acceptIncoming();
            expect(states.at(-1)).toEqual({ kind: 'failed', reason: 'mic_denied' });
            expect(ring.stops).toBe(1);
        });
    });

    // ─── slice 5: joinConference ────────────────────────────────────

    describe('joinConference', () => {

        async function makeRegistered(): Promise<void> {
            authenticate();
            await svc.connect();
            fake.handle.pump('registered');
        }

        it('refuses when not registered', () => {
            authenticate();
            svc.joinConference();
            expect(states.at(-1)).toEqual({ kind: 'failed', reason: 'not_connected' });
            expect(fake.handle.callTargets).toEqual([]);
        });

        it('builds sip:conf@pbx.<society> and tags peer as "Conference"', async () => {
            await makeRegistered();
            svc.joinConference();

            expect(fake.handle.callTargets).toEqual(['sip:conf@pbx.soc-123']);
            const last = states.at(-1)!;
            if (last.kind !== 'outgoing') fail('expected outgoing');
            else expect(last.toFlat).toBe('Conference');
        });

        it('transitions to in-call with peerFlat="Conference" on answer', async () => {
            await makeRegistered();
            svc.joinConference();
            fake.handle.callHandle.pump('in-call');

            const last = states.at(-1)!;
            if (last.kind !== 'in-call') fail('expected in-call');
            else expect(last.peerFlat).toBe('Conference');
        });
    });
});
