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

# Dev server on :4200 (proxy + live-reload come in slice 1)
podman run --rm -p 4200:4200 -v "$PWD/ui:/ui" -w /ui docker.io/library/node:16-alpine \
  npx ng serve --host 0.0.0.0
```

## Notable pinning

- `@types/node` pinned to `^16.18.x` — TypeScript 4.7 (Angular 14's compiler) cannot consume the `^20` typings shipped on `latest`.
- `overrides` in `package.json` forces `lit-html@^2.3.0` + `lit-element@^3.0.0` for both `@clr/core` (which lists `lit-html@^1` as a peer) and `@cds/core` (which needs the `TemplateResult<1>` generic from lit-html 2). Same pattern xpmile resolves via its checked-in `package-lock.json`.
- `--legacy-peer-deps` is currently needed because `@clr/angular@13` lists Angular 13 as a peer while we run Angular 14. Identical to xpmile's setup.

## Slices ahead

| Slice | What | Status |
|---|---|---|
| 0 | Project scaffold + Clarity/CDS styles + common services + empty routes | ✅ Complete |
| 1 | Login flow (form + HttpInterceptor for bearer token + route guard) | ⏳ |
| 2 | `src/common/sip.service.ts` — SIP.js wrapper over `/sip-ws` + registration | ⏳ |
| 3 | Directory search + outgoing call (P2P) UI | ⏳ |
| 4 | Incoming call wakeup: VAPID push + Service Worker + ringtone | ⏳ |
| 5 | Conference (ConfBridge) + call history (CDR list) + settings | ⏳ |
| 6 | `docker/Dockerfile.ui` (multi-stage: node-build → nginx-serve) + integration with `docker-compose.heroku.yml` | ⏳ |
| 7 | Playwright E2E | ⏳ |

## Origin

The scaffold is a curated copy of `xpmile/ui/` — same Angular/Clarity/CDS stack, same `httpsvc`/`pubsubsvc` pattern, same Clarity-themed `styles.scss`. The feature surface (softphone) is wholly new; the xpmile shipment/inventory/tracking modules are not carried over.
