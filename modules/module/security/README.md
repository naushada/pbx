# security — TLS helpers shared by webservice + wsdbproxy

> **Status:** ✅ Verbatim copy of the upstream shared-library module — do not modify locally (Layer 1, transitive — `wsdbproxy.hpp` includes `innertls.hpp`). 8 inherited `InnerTlsTest*` tests green.

## What lives here

- `innertls` — inner-TLS helpers used by the WebSocket-over-TLS path between cloud and pbx-agent (the wsdbagent tunnel).
- `wstransport.hpp` — header-only WS transport adapter (used by the wsdbproxy module).

Not directly invoked by our new code (`SipBridge`, `SipFrame`) — pulled in transitively because the wsdbproxy module includes its headers.

## Origin

Files are a byte-identical copy of the upstream shared-library `modules/module/security/`. Do not modify locally. Any future onprem-pbx-specific extension should be documented here when it lands.

## Tests

`InnerTlsTest*` — 8 tests, all green under our podman build.

## Dependencies

- OpenSSL (`-lssl -lcrypto`)
- ACE/ACE_SSL
