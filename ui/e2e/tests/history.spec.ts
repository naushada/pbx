import { test, expect, signIn, mockApi } from '../fixtures';

test.describe('history', () => {

    test('renders rows newest-first with peer + duration + outcome', async ({ page }) => {
        await signIn(page);
        await mockApi(page, {
            history: [
                {
                    callId: 'c1', societyId: 'soc-1', fromFlat: 'A-204', toFlat: 'B-12',
                    direction: 'outbound', type: 'p2p',
                    startedAt: '2026-05-13T10:00:00Z', endedAt: '2026-05-13T10:02:00Z',
                    durationSec: 120, hangupCause: 'normal',
                },
                {
                    callId: 'c2', societyId: 'soc-1', fromFlat: 'C-99', toFlat: 'A-204',
                    direction: 'inbound',  type: 'p2p',
                    startedAt: '2026-05-15T09:00:00Z', endedAt: '2026-05-15T09:00:15Z',
                    durationSec: 0, hangupCause: 'busy',
                },
            ],
        });

        await page.goto('/main/history');
        const rows = page.locator('tbody tr');
        await expect(rows).toHaveCount(2);

        // Newest first: c2 (May 15) above c1 (May 13).
        await expect(rows.nth(0)).toContainText('C-99');
        await expect(rows.nth(1)).toContainText('B-12');

        await expect(rows.nth(0).getByText('busy')).toBeVisible();
        await expect(rows.nth(1).getByText('normal')).toBeVisible();
    });

    test('empty state when there are no calls yet', async ({ page }) => {
        await signIn(page);
        await mockApi(page, { history: [] });
        await page.goto('/main/history');
        await expect(page.getByText(/No calls yet/i)).toBeVisible();
    });
});
