/**
 * The process-wide CloudClient, wired to the real cloud over `fetch`.
 *
 * Screens import this as their default dependency; tests inject their
 * own CloudClient (scripted or FakeCloud-backed) instead.
 */
import {CloudClient} from './cloudClient';
import {FetchTransport} from './http';
import {CLOUD_BASE_URL} from '../config';

export const defaultClient = new CloudClient(new FetchTransport(CLOUD_BASE_URL));
