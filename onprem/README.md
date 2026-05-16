# onprem-pbx-onprem-ui

Vaadin 24 admin UI for **onprem-pbx**. A thin Java / Spring Boot
front-end whose only backend is the C++ `pbx-cloud` REST API. No DB
access, no Asterisk talk — those live in the cloud and the on-prem
agent respectively.

Sibling of `xpmile/onprem/` (same shape, swap shipment for
society + subscriber).

## What it gives an operator

- **Login** — `(society code, flat number, password)` against the
  cloud's `POST /api/v1/subscriber/login`. Strict mode enforces
  bcrypt + `role=admin`.
- **Dashboard** — society at-a-glance: active subscriber count, online
  presence (placeholder until admin endpoint joins the presence cache),
  CDR count for the last 24 h.
- **Societies** — list every tenant; create new ones; the
  create-success dialog surfaces the generated `sipRealm` +
  `turnSharedSecret` (with Copy buttons) for the operator to paste
  into the on-prem `.env`.
- **Subscribers** — full per-row admin list with disable / enable /
  delete actions and a client-side flat-prefix filter.
- **Bulk import** — Download template → fill CSV → upload → result
  grid shows the one-shot plaintext credentials per row (with the
  "save NOW, passwords are not recoverable" banner) + a
  Download-results-as-CSV button.

## Run

The canonical entry point lives at the repo root — it wraps `mvn` in
a podman container so neither Java nor Maven is required on the host:

```sh
scripts/run-admin-ui.sh                           # → Heroku by default
scripts/run-admin-ui.sh --backend-url http://localhost:8080
PORT=9090 scripts/run-admin-ui.sh
```

Defaults: backend `https://pabx-5fbf3550f938.herokuapp.com`, port
`8081`. See [`scripts/run-admin-ui.sh --help`](../scripts/run-admin-ui.sh)
for every flag.

## First-time admin bootstrap

The login is gated on a real `subscribers` row with `role=admin`. No
such row exists on a fresh deploy — chicken-and-egg. The recipe (see
the README at the repo root for the full version):

1. Bring up `pbx-wsdbagent` so the cloud's `REMOTE_DB=1` lookups
   actually reach an on-prem Mongo:
   ```sh
   lima start
   ```
2. Seed the first society + admin row via the CLI:
   ```sh
   scripts/bootstrap-society.sh \
     --society-code SUNSET \
     --society-name 'Sunset Towers' \
     --admin-email admin@sunset.example \
     --admin-password 'changeme123' \
     --mongo-uri mongodb://localhost:27017/pabx
   ```
3. Flip the cloud to strict mode:
   ```sh
   heroku config:set PBX_AUTH_STRICT=1 --app pabx
   ```
4. Refresh `http://localhost:8081/login` and log in:

   | Form field      | What to type                                                |
   |-----------------|-------------------------------------------------------------|
   | `Society label` | the `--society-code` from step 2 (e.g. `SUNSET`)            |
   | `Flat number`   | `ADMIN` (bootstrap seeds the first admin with this flatNumber)|
   | `Password`      | the `--admin-password` from step 2                          |

## Direct `mvn` (no script)

If you've got Java 17 + Maven 3.9+ on the host and want to skip
podman:

```sh
cd onprem
mvn spring-boot:run \
  -Dspring-boot.run.arguments="--backend.url=https://pabx-5fbf3550f938.herokuapp.com"
```

## Layout

```
onprem/
├── pom.xml                               # Spring Boot 3.2.5 + Vaadin 24.3.11 BOM
├── README.md
└── src/main
    ├── java/com/onprempbx/onprem
    │   ├── Application.java              # @SpringBootApplication
    │   ├── config/BackendConfig.java     # RestTemplate + backend.url
    │   ├── model/
    │   │   ├── Society.java              # _id, code, name, address, sipRealm, turnSharedSecret
    │   │   ├── Subscriber.java           # full admin shape (PUT/DELETE keyed by sipUsername)
    │   │   ├── LoginCredentials.java     # societyCode, flatNumber, password
    │   │   └── BulkResult.java           # placeholder (rich result data is in BulkSubscriberImportView)
    │   ├── service/
    │   │   ├── AuthService.java          # login + VaadinSession bearer + isAdmin()
    │   │   ├── AuthException.java        # carries the HTTP status code
    │   │   ├── SocietyService.java       # list / get / create
    │   │   ├── SubscriberService.java    # list / setStatus / delete
    │   │   ├── BulkSubscriberParser.java # POST CSV + download template
    │   │   └── CdrService.java           # list (for the dashboard tile)
    │   └── ui/
    │       ├── LoginView.java            # @Route("login") @AnonymousAllowed
    │       ├── MainLayout.java           # sidenav + logout + beforeEnter auth guard
    │       ├── DashboardView.java        # @Route("dashboard")
    │       ├── society/
    │       │   ├── SocietiesView.java        # @Route("societies")
    │       │   └── CreateSocietyView.java    # @Route("societies/new")
    │       └── subscriber/
    │           ├── SubscriberListView.java       # @Route("subscribers")
    │           └── BulkSubscriberImportView.java # @Route("subscribers/import")
    └── resources/application.properties
```

## The bigger plan

The 13-item Phase 0/1/2 roadmap (cloud REST gaps → Vaadin scaffold →
admin views) is captured in the `project_vaadin_admin_ui` memory note.
All 13 items are merged on `main` as of 2026-05-16; what's documented
above is the live shape.
