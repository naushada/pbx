import { test as base, Page } from '@playwright/test';

// Test-wide fixtures + REST mocks.
//
//   signedIn  — pre-seeds localStorage with a fake auth session before
//               the app boots, so tests can land on /main/* without
//               going through the login form.
//   mockApi   — installs page.route() handlers for the cloud REST
//               endpoints the UI touches.

export interface SampleSubscriber {
    societyId:   string;
    flatNumber:  string;
    displayName: string;
    sipUser:     string;
    role:        'admin' | 'resident';
}

export const SAMPLE: SampleSubscriber = {
    societyId:   'soc-1',
    flatNumber:  'A-204',
    displayName: 'Alice Resident',
    sipUser:     'A-204',
    role:        'resident',
};

export const TOKEN = 'e2e-tok-xyz';

/** Inject `pbxui:auth-token` and `pbxui:subscriber` into localStorage
 *  before the SPA boots. Combine with `await page.goto('/main/...')`. */
export async function signIn(page: Page, sub: SampleSubscriber = SAMPLE): Promise<void> {
    await page.addInitScript(({ token, subscriber }) => {
        localStorage.setItem('pbxui:auth-token', token);
        localStorage.setItem('pbxui:subscriber', JSON.stringify(subscriber));
    }, { token: TOKEN, subscriber: sub });
}

/** Intercept all /api/v1/* GETs/POSTs and reply with the provided
 *  fixtures. Anything not pre-mocked falls through to a 404 so missing
 *  mocks are loud rather than silent. */
export interface ApiMocks {
    login?:        { status: number; body?: unknown };
    directory?:    Array<{ flatNumber: string; displayName: string; sipUri: string; online: boolean }>;
    history?:      Array<Record<string, unknown>>;
    vapidKey?:     string;
    pushSubscribe?:{ status: number; body?: unknown };
    turnCreds?:    { urls: string[]; username: string; credential: string; ttlSec: number };
}

export async function mockApi(page: Page, mocks: ApiMocks): Promise<void> {
    await page.route('**/api/v1/**', async (route) => {
        const url = route.request().url();

        if (url.includes('/api/v1/subscriber/login')) {
            const m = mocks.login ?? { status: 200, body: { token: TOKEN, subscriber: SAMPLE } };
            return route.fulfill({ status: m.status, contentType: 'application/json',
                body: JSON.stringify(m.body ?? { token: TOKEN, subscriber: SAMPLE }) });
        }
        if (url.includes('/api/v1/subscriber')) {
            return route.fulfill({ status: 200, contentType: 'application/json',
                body: JSON.stringify(mocks.directory ?? []) });
        }
        if (url.includes('/api/v1/cdr')) {
            return route.fulfill({ status: 200, contentType: 'application/json',
                body: JSON.stringify(mocks.history ?? []) });
        }
        if (url.includes('/api/v1/push-vapid-key')) {
            return route.fulfill({ status: 200, contentType: 'application/json',
                body: JSON.stringify({ key: mocks.vapidKey ?? 'BTest' }) });
        }
        if (url.includes('/api/v1/push-subscribe')) {
            const m = mocks.pushSubscribe ?? { status: 200, body: { ok: true } };
            return route.fulfill({ status: m.status, contentType: 'application/json',
                body: JSON.stringify(m.body ?? { ok: true }) });
        }
        if (url.includes('/api/v1/turn-credentials')) {
            return route.fulfill({ status: 200, contentType: 'application/json',
                body: JSON.stringify(mocks.turnCreds ?? {
                    urls: ['turn:turn.local:3478'],
                    username: '0:A-204', credential: 'x', ttlSec: 3600,
                }) });
        }
        return route.fulfill({ status: 404, contentType: 'application/json',
            body: JSON.stringify({ error: 'mock-not-defined', url }) });
    });
}

// Re-export Playwright test as-is. (Future slices can wrap it with
// extra fixtures if needed.)
export const test = base;
export { expect } from '@playwright/test';
