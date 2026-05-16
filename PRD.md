# onprem-pbx — Product Requirements Document

**Owner:** Naushad
**Status:** Draft v1
**Last updated:** 2026-05-14
**Companion docs:** [DESIGN.md](./DESIGN.md), [TDD-PLAN.md](./TDD-PLAN.md)

---

## 1. Problem

Residential societies (gated apartment complexes) want their residents and staff to talk to each other without using personal mobile numbers. Today they fall back to WhatsApp groups, intercom hardware that's expensive and breaks, or shouting from the balcony.

A society-scoped VoIP PBX would let any resident reach any other resident (or the main-gate guard) by flat number, from a phone or laptop, without revealing personal numbers and without depending on the public telco network for inside-society calls.

## 2. Goals

1. **Dial-by-flat** — any resident can reach any other resident in the same society by typing the flat number.
2. **Privacy** — flat numbers (not phone numbers) are the addressable identity. Voice never leaves the society's network for in-society calls.
3. **One-touch gate intercom** — every flat can reach the main-gate guard with a single digit (`0`), and the guard can dial any flat.
4. **Self-contained on-prem footprint** — one box behind the society's NAT runs the PBX. No public-IP requirement beyond a single UDP port-forward.
5. **Cloud-managed control plane** — admins manage subscribers, see CDRs, and ship updates without visiting the society.

## 3. Non-goals (v1)

- No calls to the public telephone network (no SIP trunk, no PSTN gateway).
- No call recording or transcript capture.
- No voicemail, IVR, or auto-attendant menus.
- No federation between societies (Society A residents cannot dial Society B).
- No native iOS/Android app — browser softphone only.
- No video calls.
- No emergency services dialling (911/112 equivalent). Explicitly out of scope; pilot deployments must communicate this to residents.

## 4. Personas

| Persona | Role | Primary value |
|---|---|---|
| **Resident** (Asha, age 34, IT manager) | Lives in flat `A-204` with spouse and child. Wants to reach the gate, the maintenance office, and neighbours. | Browser softphone on laptop; opens during work-from-home hours. |
| **Tenant** (Rohan, age 27, on a 1-year lease) | Same flat type; shares the flat number with the landlord. | Same softphone; only the period of tenancy. |
| **Security guard** (Vijay, gate kiosk) | 24×7 rotation at the main gate. Receives parcel-delivery, visitor, and missed-pet calls. | Always-on kiosk browser; auto-answers some flats, manually accepts others. |
| **Society admin / secretary** (Mr. Kapoor, retired) | Manages resident roster; not technical. | Uses a portal to bulk-import residents from a spreadsheet at the start of each year. |
| **System installer** (3rd party technician) | Sets up the on-prem box on first install; revisits rarely. | Plugs in the box, runs one shell script, opens one firewall port. |

## 5. Use cases (user stories)

### 5.1 Resident-to-resident call (primary use case)

> As a resident, I want to dial my neighbour's flat number so we can talk without me knowing their mobile number.

**Acceptance:**
- Asha logs into the portal, sees the dial-pad and a contact search.
- She types `B-204`; the contact list shows the names registered on `B-204`.
- She clicks call; within 3 seconds Bob's browser rings (or Bob's phone shows an OS notification if his tab was closed).
- Bob answers; both hear each other within 1 second of pickup.
- Either hangs up; the call appears in both call histories within 5 seconds.

### 5.2 Forked ringing on a multi-resident flat

> As a resident on a flat shared with my spouse and parent, I want every device registered on the flat to ring when someone dials our flat number.

**Acceptance:**
- Flat `B-204` has 3 subscribers. Any incoming INVITE for `B-204` rings all 3.
- The first to answer takes the call; the other two see "Answered elsewhere".
- The call history shows who actually answered.

### 5.3 Main-gate intercom

> As a resident, I want to dial `0` to reach the gate guard. As a guard, I want to dial a flat to announce a visitor.

**Acceptance:**
- Dialling `0` from any flat rings every guard subscriber (typically 1–2 kiosks).
- Guards see caller's flat number on the screen.
- The guard can search by flat number and dial outbound to any resident.
- All guard-initiated calls appear in the audit log.

### 5.4 Conference within a flat

> As a resident, I want to start a group call with everyone on my flat (e.g. to coordinate dinner plans).

**Acceptance:**
- Resident selects "Group call" → all subscribers on the same flat receive an INVITE.
- Joiners are added to a conference bridge; everyone can hear everyone.
- Mid-call participants can leave/rejoin.
- Call history records `type=conference` with all participant ids.

### 5.5 Incoming call while tab is closed

> As a resident who closed the portal tab, I want my laptop to notify me when someone calls so I don't miss them.

**Acceptance:**
- Web Push notification appears on the OS (Chrome/Firefox/Edge desktop; Chrome on Android).
- Clicking the notification reopens the portal in "incoming call" state within Asterisk's ring timeout (30 s).
- If un-answered the call shows as "Missed" on next login.

### 5.6 In-call controls

> As a resident in a call, I want to mute myself, place the call on hold, and send DTMF digits (for any future IVR).

**Acceptance:**
- Mute is local-only (no SIP re-INVITE; instant).
- Hold sends a SIP re-INVITE with `a=sendonly`; the other side hears music-on-hold (or silence in v1).
- DTMF is sent via RFC 4733 telephone-events.

### 5.7 Society onboarding (bulk import)

> As the society admin, I want to upload a CSV of residents and have credentials emailed to each one.

**Acceptance:**
- Admin uploads a CSV with columns: `flat_number, name, email, phone, role`.
- System creates flats and subscribers, generates static SIP usernames and passwords, hashes the passwords, and emails plaintext credentials to each resident.
- Admin downloads a one-time CSV with all plaintext credentials for archival; after that download the plaintexts are unrecoverable.
- Re-uploading the same CSV is idempotent (no duplicates, no re-email).

### 5.8 Capacity-limited busy

> As a system, when 5 calls are already in progress, I should reject a 6th rather than degrade audio.

**Acceptance:**
- 6th concurrent INVITE returns `503 Service Unavailable` to the caller.
- Caller sees a friendly "All lines busy, try again" message.
- The 6th attempt is recorded in CDR with `hangupCause = capacity_busy`.

## 6. Functional requirements

| ID | Requirement | Priority |
|---|---|---|
| FR-01 | Subscriber registers a web softphone using static SIP digest credentials | P0 |
| FR-02 | Dial-by-flat with search & autocomplete in the portal | P0 |
| FR-03 | 1:1 audio call between two subscribers in the same society | P0 |
| FR-04 | Forked ringing for multi-subscriber flats | P0 |
| FR-05 | Main-gate extension `0` reaches all guard subscribers | P0 |
| FR-06 | Conference within a flat (Asterisk ConfBridge) | P0 |
| FR-07 | Hold, mute, DTMF in-call controls | P0 |
| FR-08 | Persisted CDR for every call (success, fail, missed, busy) | P0 |
| FR-09 | Bulk CSV import of subscribers; email delivery of credentials | P0 |
| FR-10 | Web Push (VAPID) notification of incoming call when tab closed | P0 |
| FR-11 | Admission control: max 5 concurrent calls per society | P0 |
| FR-12 | Admin portal: society/flat/subscriber CRUD, role assignment | P0 |
| FR-13 | Audit log for admin actions and guard-initiated calls | P1 |
| FR-14 | Per-subscriber preference: auto-answer (for guard kiosks) | P1 |
| FR-15 | Per-society config: ring timeout, max concurrent calls | P1 |
| FR-16 | Operator dashboard: live call list, registration health, tunnel up/down | P1 |
| FR-17 | Password reset by admin (regenerates SIP password) | P1 |

## 7. Non-functional requirements

| ID | Requirement | Target |
|---|---|---|
| NFR-01 | Call setup latency (INVITE to ringing) | p50 ≤ 1.5 s, p95 ≤ 3 s |
| NFR-02 | One-way audio latency on intra-LAN 1:1 (P2P media) | p50 ≤ 80 ms |
| NFR-03 | Audio quality (MOS, Opus 16 kHz, LAN) | p50 ≥ 4.0 |
| NFR-04 | Concurrent registered subscribers per society | ≥ 1000 |
| NFR-05 | Concurrent active calls per society | 5 (hard cap, configurable up to 20) |
| NFR-06 | Cloud tunnel uptime per pbx-agent | ≥ 99.5 % monthly |
| NFR-07 | Push notification delivery success | ≥ 95 % within 5 s of INVITE |
| NFR-08 | On-prem box hardware floor | 4-core / 8 GB / 64 GB SSD |
| NFR-09 | New society time-to-install | ≤ 90 minutes by a single technician |
| NFR-10 | Recovery from pbx-agent restart | ≤ 60 s before registrations re-establish |

## 8. Constraints and assumptions

- **Heroku platform** — control plane runs on Heroku web dynos. Only HTTP/WebSocket on `$PORT`. No UDP. No inbound to the on-prem box through Heroku. (See DESIGN.md §1.)
- **Implementation stack** — C++ with ACE_* APIs, reusing the project's shared-library `WebServer`/`WebConnection`/`MicroService` patterns. UI in Angular. (See DESIGN.md §11 reuse map.)
- **Society network** — society must open one public UDP port and DNAT it to coturn on the on-prem box, for off-LAN residents and conference media.
- **MongoDB** — on-prem MongoDB reached from the cloud via the `wsdbagent` tunnel.
- **mTLS CA** — per-society X.509 CA generated at install time; the leaf cert + key ship to the on-prem box.
- **Residents have modern browsers** — Chrome 90+, Safari 15+, Firefox 90+, Edge 90+. No IE.
- **Society has reliable broadband** — outbound TLS to Heroku from the on-prem box must stay up.
- **One society = one pbx-agent deployment** — no multi-tenant on-prem boxes in v1.

## 9. Success metrics

| Metric | Target after first pilot society (3 months) |
|---|---|
| Subscribers registered (out of imported) | ≥ 80 % |
| Daily active users / total residents | ≥ 25 % (weekday median) |
| Successful call rate (calls connected / calls attempted) | ≥ 95 % |
| In-call dropped (premature hang-up) rate | ≤ 2 % |
| Missed-call notifications delivered | ≥ 95 % within 5 s |
| Gate-intercom calls per day | ≥ 1 per 20 flats (proxy for "is it actually replacing the intercom") |
| Admin onboarding time per society | ≤ 90 minutes |
| Support tickets opened per society per month | ≤ 3 (after first month stabilises) |

## 10. Release scope and phasing

### Phase 0 — Foundations (engineering only, no user-facing release)

- Repo skeleton seeded from the shared-library modules.
- Layer 0 of TDD plan green: `MessageParserBase*`, `Http*` (regression), `SipParser*`, `SipFrame*`.
- CI green.

### Phase 1 — Internal alpha

- All P0 functional requirements implemented.
- Single staging society, all developers as subscribers.
- Layer 1–4 of TDD plan green.
- Daily dogfood for 2 weeks; >= 95 % call success on the staging society.

### Phase 2 — Pilot society

- First real society installed, ~200 flats imported.
- Layer 5 (browser E2E) + Layer 6 (smoke) green.
- 6-week pilot with on-call support; success metrics measured.
- Decision gate: green-light Phase 3 if pilot metrics in §9 met.

### Phase 3 — General availability

- P1 requirements landed: audit log, auto-answer for guards, operator dashboard, admin password reset.
- Installer documentation + 1-pager for residents.
- Pricing and contract templates (out of this PRD).

## 11. Risks and open questions

| Risk | Mitigation |
|---|---|
| Symmetric NAT on mobile data defeats P2P; TURN over UDP needs a public port the society may resist opening | Default coturn UDP port to 3478; document the firewall ask in installer guide; fall back to "no calls off-LAN" with a clear UX banner if the port is closed |
| iOS Safari Web Push has known limits (PWA-only, no background) | v1: document desktop browsers as primary; offer the missed-call history as the iOS fallback. Mobile native deferred. |
| Email delivery of credentials is unreliable / hits spam | Provide an admin "regenerate + show inline" flow; recommend SMS as out-of-band secondary channel |
| Asterisk version compatibility / chan_pjsip config drift | Pin Asterisk LTS version; vendor a `pjsip.conf` template in the agent image; integration tests run against the same pinned version |
| pbx-agent ↔ Heroku tunnel flap during peak hours kills all calls | WebSocket-level keep-alive ping (25 s) stops Heroku from H15-dropping idle tunnels in the first place; `CloudConnector` auto-reconnect with backoff recovers from real drops; in-flight RTP is P2P so already-connected calls survive a signaling drop |
| Society admin uploads a CSV with PII to the cloud | Cloud only sees hashed passwords + names + flat numbers; no payment data; document the data we store in a privacy notice |
| `directmedia=yes` interacts badly with browser ICE on some networks | Layer 4 SIP scenarios explicitly assert the re-INVITE direction and final media path; pilot will surface real-network edge cases |
| 5-concurrent-call cap is wrong for a big society | Configurable per society (NFR-05); raise after benchmarking on pilot |

**Open questions** (to resolve before Phase 2):

1. Do we need a "do not disturb" / quiet-hours feature for residents? (Not in P0; might be raised by pilot.)
2. Does the society want to brand the portal (logo, colours)? Affects UI work.
3. SMS fallback for credential delivery — which provider / cost model?
4. Backup/restore policy for MongoDB on the on-prem box (snapshot frequency, retention).
5. Should the operator dashboard be exposed to society admins, or only to us (the operator of the system)?

## 12. Dependencies

- **Heroku** — cloud platform.
- **Asterisk LTS** — bundled in the agent image.
- **coturn** — bundled in the agent image.
- **MongoDB** — bundled in the agent image.
- **SIP.js** — npm dependency for the Angular softphone.
- **VAPID push** — keys generated per cloud deployment; stored in Heroku config vars.

## 13. Glossary

- **Society** — a residential gated community; the unit of multi-tenancy.
- **Flat** — an apartment within a society; addressable by a number like `A-204`.
- **Subscriber** — a person with login credentials; many subscribers can belong to one flat.
- **pbx-agent** — the on-prem service that runs Asterisk, coturn, MongoDB, and the tunnel client.
- **Tunnel** — the persistent mTLS WSS connection from pbx-agent up to the Heroku cloud.
- **CDR** — Call Detail Record; one row per call attempt.
- **Forking** — a single INVITE rings all registered endpoints of a flat.
- **P2P media** — RTP/SRTP flows directly between browsers, not through Asterisk.
- **ConfBridge** — Asterisk's conference mixer; media flows through pbx-agent for conferences.
- **VAPID** — Voluntary Application Server Identification for Web Push.
