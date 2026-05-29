/**
 * IncomingCallAlerter — pulses the device while a call is ringing.
 *
 * The web softphone has a Web-Audio ringtone (480/620 Hz two-tone)
 * via `RingtoneService` in ui/src/common/ringtone.service.ts. RN
 * doesn't ship Web Audio, and a bundled audio asset would add weight
 * + licensing scope, so we start with the built-in `Vibration` API.
 * Audio ring will land alongside real CallKit / PushKit when that
 * slice ships — at that point the OS provides the system ring on
 * iOS and the in-app alerter only matters when the app is already
 * foregrounded with CallKit disabled.
 *
 * The interface is a thin seam so tests can mock `start`/`stop`
 * without spinning up Jest's RN Vibration polyfill.
 */
import {Vibration} from 'react-native';

export interface IncomingCallAlerter {
  start(): void;
  stop(): void;
}

// 1 s pulse, 2 s gap — mirrors the cadence of the web softphone's
// ringtone (`RingtoneService` uses 2 s on / 4 s off but RN's
// Vibration battery cost is non-trivial, so we use a tighter pulse).
const VIBRATION_PATTERN: number[] = [0, 1000, 2000];

/** Production wrapper around RN's Vibration API. */
export const vibrationAlerter: IncomingCallAlerter = {
  start: () => {
    // Second arg `true` makes the pattern repeat until cancelled.
    Vibration.vibrate(VIBRATION_PATTERN, true);
  },
  stop: () => {
    Vibration.cancel();
  },
};
