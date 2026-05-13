import { defineConfig, devices } from '@playwright/test';

// Playwright runs against the production-shape Angular bundle served
// statically (faster + closer to what users see than `ng serve`). The
// caller is expected to have already run `ng build --configuration
// development` so `dist/pbxui/` exists; the README has a one-liner.
//
// http-server is shipped as a devDep so we don't need nginx in the
// test container; SPA fallback uses the proxy-style routing
// (`-P 'http://localhost:4200?'`) which rewrites unknown paths to /.

export default defineConfig({
    testDir: './e2e',
    testMatch: /.*\.spec\.ts$/,
    timeout: 30_000,
    expect: { timeout: 5_000 },
    fullyParallel: true,
    retries: 0,
    workers: 1,
    reporter: process.env['CI'] ? 'line' : 'list',

    webServer: {
        // Serve dist/pbxui (production bundle) on :4200 with SPA fallback.
        command: 'npx http-server dist/pbxui -p 4200 -s -c-1 --proxy http://localhost:4200?',
        url: 'http://localhost:4200',
        reuseExistingServer: !process.env['CI'],
        timeout: 60_000,
        stdout: 'ignore',
        stderr: 'pipe',
    },

    use: {
        baseURL: 'http://localhost:4200',
        trace: 'on-first-retry',
        screenshot: 'only-on-failure',
        // Avoid the autoplay-gating that audio elements would face in
        // headless; nothing in the e2e suite plays real audio.
        launchOptions: {
            args: ['--autoplay-policy=no-user-gesture-required'],
        },
    },

    projects: [
        { name: 'chromium', use: { ...devices['Desktop Chrome'] } },
    ],
});
