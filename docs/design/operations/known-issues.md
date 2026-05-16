# Known issues — operational

A living registry of in-flight issues that the architecture/design docs
should NOT pretend are solved. Each entry: what's broken, what we
*think* is happening, the workaround (if any), and the next debug step.

When something here gets resolved, move the summary into the relevant
design doc (or a PR description) and delete the entry.

---

## SIP REGISTER returns 408 after agent's `societyId` changes mid-session

**Symptom.** Browser sip.js logs `User agent client request timed out. Generating internal 408 Request Timeout` → `Failed to register, status code 408`. The cloud's HTTP log shows `GET /sip-ws?token=…` but no follow-up forward log. Asterisk shows zero REGISTER activity. Agent shows no `[AsteriskStream sid=N] connected` line for the session.

**Hypothesis.** Stale cloud-side tunnel binding. The cloud's `CloudTunnelEndpoint` is a **single-slot** structure (see [[agent-heroku-3s-disconnect]] memory for the prior bite). If the Lima agent's `AGENT_SOCIETY_ID` changes after a tunnel was already registered with the old value (e.g. by editing `.env` and recreating the agent container mid-session), the cloud's lookup for the new `societyId` finds no tunnel and silently drops every frame, including REGISTER. sip.js times out after ~32 s.

**Workaround.** Tear the on-prem stack down with `./scripts/lima.sh stop` (deletes the VM and its containers), then `./scripts/lima.sh start` to bring it back up. The agent connects fresh and announces the right `societyId` from the first `AGENT_HELLO`, so the cloud's tunnel slot is bound correctly from the start.

**Next debug step.** The cloud is *silent* about tunnel lookup at `/sip-ws`. Add `LM_INFO` logging in `modules/module/pbx/src/microservice_pbx.cpp` around the tunnel-resolution path so a missed-routing failure is observable instead of being a 32-second mystery.

**Related.** PRs #76 (`pbx.local` URI host), #77 (no SIP digest auth in provisioner), #78 (no SIP digest auth in static pjsip.conf) — all merged 2026-05-17. The 408 surfaced *after* these unblocked the prior 401-loop, so the 408 is now the lowest-water-mark blocking end-to-end push validation.

---

## `SubscriberWatcher::bootstrap()` runs silently — zero ARI activity

**Symptom.** Agent starts, logs `[pbx-agent] starting: ... society=<id>` and `[pbx-agent] reactor ready; entering event loop`, but emits no `[ARI*]` PUT/DELETE traffic and Asterisk's `pjsip show endpoint <known-user>` shows no freshly-provisioned shape (so e.g. PR #77's drop-the-auth-field change can't be observed in-cluster despite the binary being rebuilt).

**Hypothesis.** Three candidates, none confirmed:

- `MongodbClient::get_documents` swallows an error and returns empty (the agent's local Mongo is the cloud Mongo over the wsdbagent tunnel — a transient handshake delay during agent startup could make the first DB call return nothing).
- Bootstrap fires before `wsdbagent` finishes inner-TLS, so the call is hitting an unhealthy DB connection.
- A `MONGO_URI` mismatch — the agent's compose env points at `pbx-mongo:27017` (local container) but data lives in the cloud-mediated Mongo, or vice versa. Worth tracing.

**Workaround.** None that's been confirmed; you can sometimes get the bootstrap to fire by `lima stop` + `lima start` (full fresh stack) but that's also the workaround for the 408 above, so they may share a root cause.

**Next debug step.** Add `LM_INFO` logs in `pbx-agent/src/main/subscriber_watcher.cpp::run_full_scan` (around the `get_documents` call + after the iteration loop) so we can tell whether the query returned 0 rows or the call threw silently. Then add an `LM_INFO` in `pjsip_provisioner.cpp::provision` (just one line, "provisioning <user>") so we can tell at least one provision was attempted.

**Related.** PR #77 (`pjsip-no-digest-auth`) was meant to be validated in-session on 2026-05-17 but couldn't be — the fixed binary was demonstrably running but the bootstrap stayed silent. The architectural correctness of #77 stands; only the in-session observation was blocked.

---

## Static sample endpoints in pjsip.conf require an Asterisk **restart**, not a `pjsip reload`, to clear stale auth associations

**Symptom.** After editing `docker/asterisk/pjsip.conf` to remove an `auth = …-auth` line from a static endpoint, `asterisk -rx 'pjsip reload'` succeeds but `pjsip show endpoint <user>` still shows the old `auth :` value. REGISTER continues to be challenged unanswerably.

**Workaround.** `podman restart pbx-asterisk` (or equivalent — full process restart, not module reload). After restart, the new pjsip.conf is parsed cleanly and the auth association is gone.

**Why.** `pjsip reload` merges deltas, not removals — fields that were present in the prior parse but absent in the new one are not automatically dropped from in-memory sorcery objects. This is an Asterisk behaviour, not something the project can fix on its side; the workaround is to document it (here, and in the PR #78 commit message) so operators don't waste time wondering why their edit didn't take.

**Related.** Surfaced 2026-05-17 while applying PR #78 locally. Captured in the PR description so future doc-readers see the same caveat.

---

## ARI `DELETE /ari/asterisk/config/dynamic/{class}/{type}/{id}` returns 403 from inside the asterisk container

**Symptom.** `curl -u asterisk:asterisk -X DELETE http://localhost:8088/ari/asterisk/config/dynamic/res_pjsip/endpoint/<id>` returns HTTP 403 despite `ari.conf` having `read_only = no` and `allowed_origins = *`. The `pbx-agent` process makes the same call shape successfully (the architecture depends on this), so there's some permission nuance specific to ad-hoc curl-from-shell.

**Workaround.** Don't try to ad-hoc-mutate dynamic-config from inside the container. Either restart Asterisk (wipes sorcery memory for the `memory` wizard) or let the agent's own re-provision pass apply the change.

**Next debug step.** Compare what the agent's `AriRestClient` sends vs what the bare curl sends — header set, content type, body shape on DELETE. Possibly a missing `Content-Type: application/json` or `Content-Length: 0` that the curl call omits.

**Related.** Surfaced 2026-05-17 while trying to nuke a stale `bob-auth` association manually — eventually solved by editing pjsip.conf (which is the static-side fix, PR #78) and restarting Asterisk.

---

## Format for future entries

```markdown
## <one-line symptom>

**Symptom.** What the operator/dev observes. Include exact error
strings — they're how someone re-finds this entry by grep.

**Hypothesis.** What we *think* is happening, with caveats. Not a
diagnosis — a starting point.

**Workaround.** What actually unblocks today.

**Next debug step.** The single most useful thing to try next.

**Related.** PRs, memories, or other issues this connects to.
```
