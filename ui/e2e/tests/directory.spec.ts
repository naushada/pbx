import { test, expect, signIn, mockApi } from '../fixtures';

test.describe('directory', () => {

    test('typing fires search, results render, click-to-call disabled while idle', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {
            directory: [
                { flatNumber: 'A-205', displayName: 'Bob',   sipUri: 'sip:A-205@pbx.soc-1', online: true  },
                { flatNumber: 'A-206', displayName: 'Carol', sipUri: 'sip:A-206@pbx.soc-1', online: false },
            ],
        });

        await page.goto('/main/directory');
        await expect(page).toHaveURL(/\/main\/directory$/);

        await page.getByPlaceholder('e.g. A-204').fill('A');

        // Wait for the row to appear (covers the 250ms debounce + render).
        const bobRow = page.getByRole('row', { name: /A-205/ });
        await expect(bobRow).toBeVisible();
        await expect(page.getByRole('row', { name: /A-206/ })).toBeVisible();

        // Self-row is filtered out.
        await expect(page.getByRole('row', { name: /A-204/ })).toHaveCount(0);

        // Call buttons are disabled because the SIP UA isn't registered
        // in headless (no /sip-ws backend running).
        await expect(bobRow.getByRole('button', { name: 'Call' })).toBeDisabled();
    });

    test('empty results show the empty state', async ({ page }) => {
        await signIn(page);
        await mockApi(page, { directory: [] });

        await page.goto('/main/directory');
        await page.getByPlaceholder('e.g. A-204').fill('zzzz');
        await expect(page.getByText(/No matches/i)).toBeVisible();
    });
});
