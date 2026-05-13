# onprem-pbx

VoIP PBX for residential societies. Heroku-hosted control plane (C++ / ACE) + on-prem `pbx-agent` (C++ / ACE + Asterisk + coturn + MongoDB) + Angular web softphone (SIP.js + WebRTC). Sibling project to [xpmile](../xpmile).

See:

- [PRD.md](./PRD.md) — product requirements, personas, success metrics.
- [DESIGN.md](./DESIGN.md) — architecture, components, data model, call flows, media security.
- [TDD-PLAN.md](./TDD-PLAN.md) — test layers and implementation order.

## Build & run

All container operations use **podman** (not docker). Same toolchain as xpmile.

```sh
# Full cloud-side build (C++ + Angular) — inside cpp-builder stage
podman-compose -f docker-compose.heroku.yml build

# On-prem agent stack: pbx-agent + Asterisk + coturn + MongoDB
podman-compose -f docker-compose.agent.yml up --build -d

# Tests (offtarget GTest binary)
podman-compose -f docker-compose.yml run --rm app ./offtarget --gtest_filter='*'
```

Wrapper scripts (`run.sh`, `run-agent.sh`, `deploy-heroku.sh`) will be added per the xpmile reuse map in [DESIGN.md §12](./DESIGN.md#12-reuse-map-dry-against-xpmile).

## Repo layout

```
modules/module/
  http/         # extracted MessageParser base + Http subclass (copied from xpmile)
  sip/          # Sip subclass + compact-header alias table (new)
  webservice/   # ACE WebServer / WebConnection / MicroService (copied from xpmile, extended)
  mongodb/      # MongodbClient pool (copied from xpmile)
  wsdbproxy/    # cloud-side Mongo-over-WSS proxy (copied from xpmile)
  email/        # CSV-import credential emails (copied from xpmile)
  pbx/          # SipBridge multiplexer, sip_frame wire format, PushSender (new)

pbx-agent/      # On-prem daemon (ACE_SSL_SOCK_Connector dial-out, SipFrameDemux, AriClient)
                # Mirrors xpmile/onprem/ layout.

ui/             # Angular softphone (Clarity + SIP.js + Service Worker)

docker/         # Dockerfiles (cpp-builder multi-stage, agent, ui)
docs/           # Long-form design docs and security analysis
test/           # Top-level integration tests
scripts/        # Build/deploy helpers
certs/          # Local-dev cert material (gitignored except templates)
```

## Implementation order

See [TDD-PLAN.md → Order of work](./TDD-PLAN.md#order-of-work-drives-the-implementation). TL;DR: copy xpmile skeleton, extract `MessageParser`, add `SipParser`, build `SipFrame` primitives, then layer up to a full Asterisk-backed call.
