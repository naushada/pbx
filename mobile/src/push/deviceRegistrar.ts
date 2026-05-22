/**
 * DeviceRegistrar — TDD layer M3.a.
 *
 * Keeps the cloud's record of this device's push token current. The OS
 * hands the app a token at launch and may rotate it later;
 * `ensureRegistered()` posts it only when it has actually changed, so
 * repeated launches with the same token cost no round-trip.
 *
 * The register call is injected (production: `CloudClient.registerDevice`),
 * so the dedup logic is unit-testable without a network.
 */
import {DeviceRegistration} from '../api/cloudClient';

export type RegisterFn = (reg: DeviceRegistration) => Promise<void>;

export class DeviceRegistrar {
  private lastToken: string | null = null;

  constructor(
    private readonly register: RegisterFn,
    private readonly sipUsername: string,
    private readonly platform: 'ios' | 'android',
  ) {}

  /**
   * Register `token` with the cloud if it differs from the last one
   * successfully registered. A failed registration is not remembered,
   * so the next call retries.
   */
  async ensureRegistered(token: string): Promise<void> {
    if (token === '' || token === this.lastToken) {
      return;
    }
    await this.register({
      sipUsername: this.sipUsername,
      platform: this.platform,
      token,
    });
    this.lastToken = token;
  }
}
