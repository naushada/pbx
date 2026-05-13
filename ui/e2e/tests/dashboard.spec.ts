import { test, expect, signIn, mockApi, SAMPLE } from '../fixtures';

test.describe('dashboard', () => {

    test('shows subscriber info + idle SIP state + sign-out', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {});

        await page.goto('/main/dashboard');
        await expect(page).toHaveURL(/\/main\/dashboard$/);
        await expect(page.getByText('Welcome')).toBeVisible();

        // Info grid surfaces the cached subscriber.
        await expect(page.getByText(SAMPLE.societyId)).toBeVisible();
        await expect(page.getByText(SAMPLE.flatNumber).first()).toBeVisible();

        // SIP idle state — Connect enabled, Disconnect disabled.
        await expect(page.getByText(/Disconnected/i)).toBeVisible();
        await expect(page.getByRole('button', { name: 'Connect', exact: true })).toBeEnabled();
        await expect(page.getByRole('button', { name: 'Disconnect', exact: true })).toBeDisabled();

        // Conference button disabled until registered.
        await expect(page.getByRole('button', { name: 'Join conference' })).toBeDisabled();
    });

    test('sign-out clears session and routes back to /login', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {});

        await page.goto('/main/dashboard');
        await page.getByRole('link', { name: 'Sign out' }).click();
        await expect(page).toHaveURL(/\/login$/);
    });

    test('sidebar nav reaches every section', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {});

        await page.goto('/main/dashboard');
        await page.getByRole('link', { name: 'Directory' }).click();
        await expect(page).toHaveURL(/\/main\/directory$/);
        await page.getByRole('link', { name: 'History'   }).click();
        await expect(page).toHaveURL(/\/main\/history$/);
        await page.getByRole('link', { name: 'Settings'  }).click();
        await expect(page).toHaveURL(/\/main\/settings$/);
    });
});
