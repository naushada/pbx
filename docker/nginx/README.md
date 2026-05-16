# `docker/nginx/`

nginx config template + entrypoint script consumed by
[`docker/Dockerfile.ui`](../Dockerfile.ui)'s runtime stage. The output
is the standalone **`pbx-ui`** image — Angular SPA served from disk +
reverse-proxy to the `pbx-cloud` REST + SIP-over-WS endpoints.

## What's in this directory

| File | Role |
|------|------|
| `nginx.conf.template` | The server-block. Two variables (`$PORT`, `$BACKEND_ORIGIN`) are substituted at container start. Defines static-asset caching for `/`, a runtime-resolver-backed `proxy_pass` for `/api/`, and a WS-upgrade-aware `proxy_pass` for `/sip-ws` (and `/agent`, `/ws/db` — same shape). |
| `entrypoint.sh` | Drop-in `/docker-entrypoint.d/40-render-template.sh`. Runs `envsubst` against `nginx.conf.template`, writes to `/etc/nginx/conf.d/default.conf`, then nginx's stock entrypoint takes over and execs `nginx -g 'daemon off;'`. |

Neither file is independently runnable — they only make sense inside
the `pbx-ui` image built by `Dockerfile.ui`.

## Where the resulting `pbx-ui` image runs

The image exists for **two** deployment shapes; the production Heroku
deploy uses **neither** today.

### 1. Local production-like smoke test (current primary use)

`docker-compose.heroku.yml` brings up `pbx-cloud` + `pbx-ui` side-by-side
on the developer host so the browser can hit `http://localhost:8080`
and exercise the same REST + WS path it would in production:

```sh
HEROKU_APP_CLOUD=onprem-pbx HEROKU_APP_UI=onprem-pbx-ui \
  podman-compose -f docker-compose.heroku.yml up --build
```

`pbx-ui` listens on `:8080`, reverse-proxies `/api/*` and `/sip-ws` to
`http://pbx-cloud:8080` over the compose bridge. The browser sees one
origin; the cloud sees plain HTTP from a same-network peer.

### 2. Standalone Heroku UI app (`deploy-ui`)

The image is also tagged + pushable as a separate Heroku app:

```sh
HEROKU_APP_UI=onprem ./deploy-heroku.sh deploy-ui
```

Used when you want the SPA hosted under its own domain
(`onprem.herokuapp.com` → reverse-proxies to `pabx.herokuapp.com`).
Currently not part of the production deploy — see "where it doesn't
run" below.

### Where it **doesn't** run today

- **Production cloud (`pabx.herokuapp.com`).** `Dockerfile.cloud`'s
  stage 2 runs `ng build` and copies the Angular bundle straight into
  the `pbx-cloud` runtime image at `/opt/webgui/webui/`. The cloud's
  `WebServer` serves the SPA via the static-file handler inherited
  from xpmile. The browser hits `pabx.herokuapp.com` for both REST
  and `/webui/*` — no second app, no separate nginx.
- **On-prem agent stack** (`docker-compose.agent.yml`). No nginx
  service. The agent stack is mongo + asterisk + coturn + pbx-agent
  + pbx-wsdbagent; no web layer. Browsers always talk to the cloud,
  not the on-prem host.

## Runtime contract

Two env vars, both required (entrypoint fails fast if missing):

| Var | Meaning | Typical value |
|-----|---------|---------------|
| `PORT` | Listen port. | `8080` locally; Heroku auto-assigns when used as a standalone app. |
| `BACKEND_ORIGIN` | Full URL of the `pbx-cloud` backend. No trailing slash. | `http://pbx-cloud:8080` (compose bridge) or `https://pabx-5fbf3550f938.herokuapp.com` (Heroku standalone deploy). |

## Why a custom envsubst step instead of nginx's built-in `NGINX_ENVSUBST_*`

The nginx:alpine image has built-in template rendering via
`NGINX_ENVSUBST_TEMPLATE_DIR` / `_SUFFIX` / `_FILTER` — but those need
nginx ≥ 1.19 *and* every variable you reference has to be in
`NGINX_ENVSUBST_FILTER`'s allow-list, which is awkward to keep in sync
with the template. Our `entrypoint.sh` runs `envsubst` with an
explicit two-variable spec, so the substitution surface is obvious
and the upstream-image contract stays minimal.

## See also

- [`docker/Dockerfile.ui`](../Dockerfile.ui) — the multi-stage build that
  produces `pbx-ui` (node:16 builder + nginx:alpine runtime).
- [`docker-compose.heroku.yml`](../../docker-compose.heroku.yml) —
  side-by-side bring-up with `pbx-cloud`.
- [`deploy-heroku.sh`](../../deploy-heroku.sh) — the `deploy-ui`,
  `push-ui`, `release-ui` commands that push the image to Heroku's
  registry.
- [`ui/README.md`](../../ui/README.md) — Angular workspace, build
  config, e2e harness.
