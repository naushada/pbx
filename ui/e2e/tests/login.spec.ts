import { test, expect, mockApi, signIn, SAMPLE } from '../fixtures';

test.describe('login flow', () => {

    test('fresh visit lands on /login', async ({ page }) => {
        await mockApi(page, {});
        await page.goto('/');
        await expect(page).toHaveURL(/\/login$/);
        await expect(page.getByText('Society Softphone')).toBeVisible();
    });

    test('signing in with valid credentials routes to /main/dashboard', async ({ page }) => {
        await mockApi(page, {});
        await page.goto('/login');

        await page.getByPlaceholder('e.g. greenwoods-a').fill('greenwoods-a');
        await page.getByPlaceholder('e.g. A-204').fill('A-204');
        await page.getByLabel('Password').fill('correct horse');
        await page.getByRole('button', { name: 'SIGN IN' }).click();

        await expect(page).toHaveURL(/\/main\/dashboard$/);
        // Header should show the active subscriber.
        await expect(page.locator('.header .who')).toContainText(SAMPLE.flatNumber);
    });

    test('invalid credentials surface an error and stay on /login', async ({ page }) => {
        await mockApi(page, { login: { status: 401, body: 'bad' } });
        await page.goto('/login');

        await page.getByPlaceholder('e.g. greenwoods-a').fill('greenwoods-a');
        await page.getByPlaceholder('e.g. A-204').fill('A-204');
        await page.getByLabel('Password').fill('wrong');
        await page.getByRole('button', { name: 'SIGN IN' }).click();

        await expect(page.getByRole('alert')).toContainText(/Invalid/i);
        await expect(page).toHaveURL(/\/login$/);
    });

    test('cached session boots straight to dashboard', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {});
        await page.goto('/');
        await expect(page).toHaveURL(/\/main\/dashboard$/);
    });
});
