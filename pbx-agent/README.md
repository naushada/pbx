# pbx-agent — on-prem daemon

> **Status:** ⏳ Empty. Lands in **Layer 2**.

C++/ACE daemon that runs on the society's on-prem host alongside Asterisk, coturn, and MongoDB. Structurally similar to xpmile's `wsdbagent` (which is the analog directory in xpmile, located at `xpmile/onprem/` — same role, different name).

## Components (Layer 2)

| Class             | Source-of-truth file                   | Role |
|-------------------|----------------------------------------|------|
| `CloudConnector`  | `src/main/cloud_connector.{hpp,cpp}`   | `ACE_SSL_SOCK_Connector` dial-out to Heroku `/agent`. Maintains the persistent mTLS tunnel, reconnect with exponential backoff (1 s → 30 s cap). Pattern lifted from xpmile's `wsdbagent/agent.{hpp,cpp}`. |
| `SipFrameDemux`   | `src/main/sip_frame_demux.{hpp,cpp}`   | Reads frames off the cloud tunnel; opens (or reuses) a local TCP socket to Asterisk's `ws://127.0.0.1:8088/ws` for each unique `stream-id`; pipes bytes in both directions. |
| `AriClient`       | `src/main/ari_client.{hpp,cpp}`        | HTTP REST + WebSocket-events client for Asterisk ARI. Subscribes to `BridgeCreated`/`BridgeDestroyed` for admission control (counts bridges, not channels — see [`DESIGN.md §6.5`](../../DESIGN.md#65-admission-control-5-call-cap)), and `ChannelDestroyed` for CDR finalization. |
| `MongoSink`       | (uses [`mongodb/`](../modules/module/mongodb/README.md)) | Persists CDR rows and replicates subscriber records pushed from the cloud over `OPEN` frames. |

Third-party processes co-located on the same host (not built or shipped by this directory):

- **Asterisk** with `chan_pjsip`, WS transport on `127.0.0.1:8088`, `directmedia=yes` for 1:1, `ConfBridge` for conferences. DTLS-SRTP per [`DESIGN.md §8`](../../DESIGN.md#8-media-security-dtls-srtp).
- **coturn** with `use-auth-secret`. Society opens one public UDP port and DNATs to it.
- **MongoDB**.

## Origin

`CloudConnector` repurposes the xpmile pattern at `xpmile/modules/module/wsdbagent/`. The dial-out + WSS upgrade + reconnect loop is identical; only the per-frame handler changes (BSON DB-call payload → [`sip_frame`](../modules/module/pbx/README.md) multiplex).

## Build & run (when populated)

The agent ships as a separate container alongside Asterisk + coturn + MongoDB via `docker-compose.agent.yml`. See `xpmile/docker-compose.agent.yml` as the template.

## Tests (Layer 2)

`CloudConnector*`, `SipFrameDemux*`, `AriClient*` — see [`TDD-PLAN.md → Layer 2`](../TDD-PLAN.md).
