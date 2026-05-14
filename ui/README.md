# ui — softphone web app

> **Status:** ✅ Slice 0 (Angular project scaffold) complete. `ng build` green. Login + SIP + WebRTC come in subsequent slices.

Angular 14 + Clarity Design System (mirrors the xpmile `ui/` shape) + [SIP.js](https://sipjs.com/) for SIP-over-WebSocket signalling. The cloud serves this bundle from `/` and proxies REST under `/api/v1/...`; SIP signalling goes over the `/sip-ws` WebSocket upgrade and media flows directly browser↔browser via DTLS-SRTP.

## Repo layout

Same shape as `xpmile/ui/`:

```
ui/
├── angular.json, package.json, tsconfig*.json, karma.conf.js
├── .browserslistrc, .editorconfig, .gitignore
└── src/
    ├── index.html, main.ts, polyfills.ts, test.ts, styles.scss, favicon.ico
    ├── environments/
    │   ├── environment.ts          # dev (sourceMap, no minify)
    │   └── environment.prod.ts     # selected by `--configuration production`
    ├── common/                     # cross-cutting services
    │   ├── app-globals.ts          # Subscriber / DirectoryEntry / CallRecord / TurnCredentials types + cloud UriMap
    │   ├── httpsvc.service.ts      # REST wrapper (one method per cloud endpoint)
    │   ├── pubsubsvc.service.ts    # BehaviorSubject pub-sub: onSubscriber, onCallState
    │   └── types.d.ts              # ambient module declarations (none yet)
    └── app/
        ├── app.module.ts, app-routing.module.ts, app.component.{ts,html,scss}
        ├── login/                  # placeholder — slice 1
        ├── main/                   # placeholder shell — Clarity header + outlet
        └── dashboard/              # placeholder — slice 2
```

## Build & run

All operations run inside a `node:16-alpine` podman container — same toolchain policy as the rest of the repo.

```sh
# From the repo root.

# One-time install (clean install pinned by package-lock.json).
podman run --rm -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npm install --legacy-peer-deps --no-audit --no-fund

# Production build → ui/dist/pbxui/
podman run --rm -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npx ng build --configuration production

# Dev build (sourcemaps, no minification) → faster, ~9 MB initial chunk
podman run --rm -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npx ng build --configuration development

# Dev server on :4200
podman run --rm -p 4200:4200 -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npx ng serve --host 0.0.0.0

# Run unit tests headless (Chromium installed on-demand inside a
# debian-slim container — karma + jasmine drive the spec files).
podman run --rm -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-bullseye-slim sh -c "
  apt-get update -qq && apt-get install -y -qq \
    chromium ca-certificates fonts-liberation libnss3 libatk-bridge2.0-0 \
    libxkbcommon0 libgbm1 libasound2 >/dev/null
  CHROME_BIN=/usr/bin/chromium \
  npx ng test --watch=false --browsers=ChromeHeadlessCI
"

# Run Playwright E2E (uses the production bundle served by http-server).
# The official Playwright image ships Node + Chromium; image tag must
# match the @playwright/test version pinned in package.json (1.40.1).
podman run --rm -v "$PWD/ui:/work" -w /work mcr.microsoft.com/playwright:v1.40.1-jammy sh -c "
  npm install --legacy-peer-deps --no-audit --no-fund
  npx ng build --configuration development
  npx playwright test
"
```

## Notable pinning

- `@types/node` pinned to `^16.18.x` — TypeScript 4.7 (Angular 14's compiler) cannot consume the `^20` typings shipped on `latest`.
- `overrides` in `package.json` forces `lit-html@^2.3.0` + `lit-element@^3.0.0` for both `@clr/core` (which lists `lit-html@^1` as a peer) and `@cds/core` (which needs the `TemplateResult<1>` generic from lit-html 2). Same pattern xpmile resolves via its checked-in `package-lock.json`.
- `--legacy-peer-deps` is currently needed because `@clr/angular@13` lists Angular 13 as a peer while we run Angular 14. Identical to xpmile's setup.

## Slices ahead

| Slice | What | Status |
|---|---|---|
| 0 | Project scaffold + Clarity/CDS styles + common services + empty routes | ✅ Complete |
| 1 | Login flow — `AuthService` (localStorage session, rehydrate on load) + `AuthInterceptor` (Bearer + 401 → /login) + `AuthGuard` on /main + real `LoginComponent` (Clarity form, error states) + logout in header/dashboard. **15/15 specs green.** | ✅ Complete |
| 2 | `SipService` — high-level state machine (idle / registering / registered / failed) emitting through `PubsubsvcService.onCallState`. Sits on a seam: `SipUaFactory` interface + `SipJsUaFactory` production wrapper around SIP.js's `UserAgent` + `Registerer`; tests inject a fake. Dashboard shows live status with Connect/Disconnect actions. **22/22 specs green (7 new).** | ✅ Complete |
| 3 | Outbound call surface — seam extended with `SipUaHandle.placeCall` + `SipCallHandle` (hangup/setMute/getRemoteStream); production wraps sip.js `Inviter` + `Session`. `SipService.placeCall` builds `sip:<flat>@pbx.<society>`, drives the `outgoing → in-call → registered` lifecycle, and exposes the remote audio stream to a shell-level `CallPanelComponent` overlay. Sidebar nav (Dashboard / Directory); `DirectoryComponent` does debounced `searchDirectory` calls and click-to-call gated on `registered`. **36/36 specs green (14 new — 10 SipService + 4 Directory).** | ✅ Complete |
| 4 | Inbound call surface — seam extended with `IncomingCallHandle` (accept/reject) + `SipUaHandle.onIncomingCall(cb)`; production wraps sip.js `Invitation` via `ua.delegate.onInvite`. `SipService` accepts/rejects, auto-busies a second arrival, and starts/stops a `RingtoneService` (Web Audio 480/620 Hz two-tone burst, 2 s on / 4 s off). `CallPanelComponent` shows pulsing Accept/Reject buttons. `PushService` + `src/sw.js` handle VAPID Web Push wakeup (`Notification.permission` gating, `pushManager.subscribe`, `POST /api/v1/push-subscribe`, SW `'push'` + `'notificationclick'`). Dashboard has Enable/Disable push toggle. **53/53 specs green (17 new — 6 SipService incoming + 5 Ringtone + 6 Push).** | ✅ Complete |
| 5 | Conference + history + settings — `SipService.joinConference()` dials `sip:conf@pbx.<society>` (Asterisk ConfBridge, one room per society); dashboard gets a one-click Join button. `HistoryComponent` at `/main/history` fetches `GET /api/v1/cdr` and renders newest-first rows with peer flat, direction arrow, mm:ss duration, colour-coded outcome. `SettingsComponent` at `/main/settings` owns the push toggle (moved from dashboard) + mic/speaker pickers backed by a new `DeviceService` (localStorage-persisted). Sidebar adds History + Settings. **61/61 specs green (8 new — 5 history + 3 conference).** | ✅ Complete |
| 6 | Production deployment — UI bundles into `docker/Dockerfile.cloud` (3-stage: cpp-builder + ui-builder + ubuntu:focal runtime). Served at `/webui/` by the C++ webservice (`<base href="/webui/">` so all asset URLs resolve). The login page uses plain HTML + namespaced `login-*` classes to avoid collisions with Clarity's global CSS (which was breaking `<header>` and `.label` rendering). `docker/Dockerfile.ui` + nginx config retained for optional standalone-UI deployments behind a reverse proxy. Live at https://pabx-5fbf3550f938.herokuapp.com/webui/. | ✅ Complete |
| 7 | Playwright E2E — `e2e/` runs against the production-shape Angular bundle served by `http-server` (SPA fallback). `playwright.config.ts` autostarts the server via `webServer` and uses `page.route()` to mock the cloud REST surface so no real backend is needed. **12/12 specs pass in 5 s** across login, dashboard, directory, history, settings. Caught one prod bug along the way (root-redirect ignored deep links — fixed in `app.component.ts`). | ✅ Complete |

## Origin

The scaffold is a curated copy of `xpmile/ui/` — same Angular/Clarity/CDS stack, same `httpsvc`/`pubsubsvc` pattern, same Clarity-themed `styles.scss`. The feature surface (softphone) is wholly new; the xpmile shipment/inventory/tracking modules are not carried over.
