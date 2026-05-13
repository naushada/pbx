import { test, expect, signIn, mockApi } from '../fixtures';

test.describe('settings', () => {

    test('renders push toggle + device pickers; pickers default to System default', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {});

        await page.goto('/main/settings');
        await expect(page).toHaveURL(/\/main\/settings$/);

        await expect(page.getByText(/Incoming-call notifications/i)).toBeVisible();
        await expect(page.getByText(/Audio devices/i)).toBeVisible();

        // Initial push state: disabled (no SW registration yet, but the
        // headless browser does report Notification API support, so the
        // pill is "Disabled — incoming calls will only ring while…").
        await expect(page.getByText(/Disabled|Blocked|doesn.t support/i)).toBeVisible();

        // Both selects render with a System default option.
        await expect(page.locator('#mic-pick option', { hasText: 'System default' })).toHaveCount(1);
        await expect(page.locator('#speaker-pick option', { hasText: 'System default' })).toHaveCount(1);
    });
});
