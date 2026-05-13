import { Injectable } from '@angular/core';

// Web Audio-based ringtone. Two-tone burst (480/620 Hz, classic
// US/EU phone) for 2 s, 4 s silence, repeat. Avoids shipping a
// binary asset and gives us full control over volume + duty cycle.
//
// Caveats:
//   - AudioContext starts in 'suspended' state on most browsers until
//     a user gesture. start() calls resume() but cannot recover if the
//     user hasn't interacted with the page yet — the ring will be silent
//     until they do. This matches Slack/Discord behaviour.
//   - Tests stub AudioContext via a constructor injection seam so
//     karma doesn't try to play audio in headless chrome.

const TONE_A_HZ      = 480;
const TONE_B_HZ      = 620;
const RING_ON_MS     = 2000;
const RING_OFF_MS    = 4000;
const VOLUME         = 0.15;

export type AudioContextFactory = () => AudioContext;

@Injectable({ providedIn: 'root' })
export class RingtoneService {

    private ctx?: AudioContext;
    private oscA?: OscillatorNode;
    private oscB?: OscillatorNode;
    private gain?: GainNode;
    private cycleTimer?: ReturnType<typeof setInterval>;
    private active = false;

    // Default factory uses the platform AudioContext; tests inject a fake.
    private factory: AudioContextFactory = () => {
        const Ctor = (window as any).AudioContext ?? (window as any).webkitAudioContext;
        if (!Ctor) throw new Error('Web Audio API not supported');
        return new Ctor();
    };

    /** Test-only seam. */
    setAudioContextFactory(f: AudioContextFactory): void { this.factory = f; }

    isActive(): boolean { return this.active; }

    async start(): Promise<void> {
        if (this.active) return;
        this.active = true;

        if (!this.ctx) this.ctx = this.factory();
        if (this.ctx.state === 'suspended') {
            try { await this.ctx.resume(); } catch { /* user hasn't gestured yet */ }
        }

        this.gain = this.ctx.createGain();
        this.gain.gain.value = 0;        // off until first beep
        this.gain.connect(this.ctx.destination);

        this.oscA = this.ctx.createOscillator();
        this.oscA.frequency.value = TONE_A_HZ;
        this.oscA.connect(this.gain);

        this.oscB = this.ctx.createOscillator();
        this.oscB.frequency.value = TONE_B_HZ;
        this.oscB.connect(this.gain);

        this.oscA.start();
        this.oscB.start();

        // Kick off the on/off cycle (on-first).
        this.beep(true);
        this.cycleTimer = setInterval(() => this.cycle(), RING_ON_MS + RING_OFF_MS);
    }

    stop(): void {
        if (!this.active) return;
        this.active = false;
        if (this.cycleTimer) { clearInterval(this.cycleTimer); this.cycleTimer = undefined; }

        try { this.oscA?.stop(); } catch { /* idem */ }
        try { this.oscB?.stop(); } catch { /* idem */ }
        this.oscA?.disconnect();
        this.oscB?.disconnect();
        this.gain?.disconnect();
        this.oscA = this.oscB = this.gain = undefined;
    }

    private cycle(): void {
        this.beep(true);
        setTimeout(() => this.beep(false), RING_ON_MS);
    }

    private beep(on: boolean): void {
        if (!this.gain) return;
        // Use setTargetAtTime to soften the gate; instant 0→V causes clicks.
        const target = on ? VOLUME : 0;
        this.gain.gain.setTargetAtTime(target, this.gain.context.currentTime, 0.005);
    }
}
