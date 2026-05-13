import { TestBed } from '@angular/core/testing';

import { RingtoneService } from './ringtone.service';

// Minimal fake of the Web Audio types Ringtone touches. Just enough
// to verify the start/stop wiring without rendering audio.
class FakeOscillator {
    public started = false;
    public stopped = false;
    public connectedTo: any;
    public frequency = { value: 0 };
    connect(node: any): void { this.connectedTo = node; }
    disconnect():     void { this.connectedTo = undefined; }
    start():          void { this.started = true; }
    stop():           void { this.stopped = true; }
}

class FakeGain {
    public connectedTo: any;
    public gain = {
        value: 0,
        setTargetAtTime: jasmine.createSpy('setTargetAtTime'),
    };
    public context: any;
    connect(node: any): void { this.connectedTo = node; }
    disconnect():     void { this.connectedTo = undefined; }
}

class FakeAudioContext {
    public state: AudioContextState = 'running';
    public destination = {};
    public currentTime = 0;
    public oscillators: FakeOscillator[] = [];
    public gain: FakeGain;
    constructor() {
        this.gain = new FakeGain();
        this.gain.context = this;
    }
    createOscillator(): any { const o = new FakeOscillator(); this.oscillators.push(o); return o; }
    createGain(): any        { return this.gain; }
    async resume(): Promise<void> { this.state = 'running'; }
}

describe('RingtoneService', () => {

    let svc: RingtoneService;
    let ctx: FakeAudioContext;

    beforeEach(() => {
        TestBed.configureTestingModule({});
        svc = TestBed.inject(RingtoneService);
        ctx = new FakeAudioContext();
        svc.setAudioContextFactory(() => ctx as unknown as AudioContext);
    });

    afterEach(() => svc.stop());

    it('start() spins up two oscillators connected to the destination', async () => {
        await svc.start();
        expect(svc.isActive()).toBeTrue();
        expect(ctx.oscillators.length).toBe(2);
        expect(ctx.oscillators[0].started).toBeTrue();
        expect(ctx.oscillators[1].started).toBeTrue();
        expect(ctx.gain.connectedTo).toBe(ctx.destination);
    });

    it('start() is idempotent — a second call does not rebuild graph', async () => {
        await svc.start();
        const firstCount = ctx.oscillators.length;
        await svc.start();
        expect(ctx.oscillators.length).toBe(firstCount);
    });

    it('stop() tears the oscillators down', async () => {
        await svc.start();
        svc.stop();
        expect(svc.isActive()).toBeFalse();
        for (const o of ctx.oscillators) expect(o.stopped).toBeTrue();
    });

    it('stop() before start() is a no-op', () => {
        expect(() => svc.stop()).not.toThrow();
        expect(svc.isActive()).toBeFalse();
    });

    it('start() ramps gain via setTargetAtTime (not a hard 0→V edge)', async () => {
        await svc.start();
        expect(ctx.gain.gain.setTargetAtTime).toHaveBeenCalled();
        const args = ctx.gain.gain.setTargetAtTime.calls.mostRecent().args;
        expect(args[0]).toBeGreaterThan(0); // first beep targets a positive volume
    });
});
