/**
 * FakeCallController — test double for the `CallController` seam.
 *
 * Records `placeCall` / `hangup` / `setMuted`, and lets a test push a
 * new call state to subscribers via `emit()`.
 */
import {Call, idleCall} from '../sip/callState';
import {CallController} from '../call/callController';

export class FakeCallController implements CallController {
  call: Call = idleCall;
  muted = false;

  placeCall = jest.fn();
  hangup = jest.fn();
  setMuted = jest.fn((muted: boolean) => {
    this.muted = muted;
  });

  private readonly listeners = new Set<(call: Call) => void>();

  getCall(): Call {
    return this.call;
  }

  isMuted(): boolean {
    return this.muted;
  }

  subscribe(listener: (call: Call) => void): () => void {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }

  /** Test driver — push a new call state to every subscriber. */
  emit(call: Call): void {
    this.call = call;
    this.listeners.forEach(listener => listener(call));
  }
}
