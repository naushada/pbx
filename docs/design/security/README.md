# Security design

Detailed security documentation for the onprem-pbx system. The high-level model lives in [`/DESIGN.md`](../../../DESIGN.md) §5 (auth), §8 (media), §9 (gate guard), §10 (checklist); this directory holds the operational specifics — what shipped, where the gaps are, and how to rotate things.

| Doc | What's in it |
|---|---|
| [`threat-model.md`](./threat-model.md) | STRIDE pass over the 12 surfaces (browser → cloud → agent → Asterisk → Mongo → coturn → push), with **defended** / **accepted** / **N/A** per cell. Includes the deferred-controls list — what `DESIGN.md` §10 calls for that the MVP doesn't yet enforce. |
| [`auth.md`](./auth.md) | Subscriber portal (bearer in localStorage), SIP digest (HA1 in pjsip), agent ↔ cloud (mTLS). **Honest gap**: the MVP ships Bearer-in-Authorization rather than the cookie+CSRF model in `DESIGN.md` §5; this doc explains why and what changes if we ever flip back. |
| [`media.md`](./media.md) | DTLS-SRTP. The 1:1 trapezoid (truly end-to-end) vs the conference shape (Asterisk has plaintext inside the mixer). What we explicitly don't do (no SDES, no ZRTP, no plaintext-RTP fallback). |
| [`controls.md`](./controls.md) | Ten enforced controls: admission cap, society scoping, push payload encryption, time-limited TURN creds, bridge isolation, mTLS, hand-off ordering invariant, 410-prune, busy auto-reject, source-of-truth scoping. Each entry says **what** / **where** / **what failure mode it defends against**. |
| [`secrets.md`](./secrets.md) | Inventory of every piece of sensitive material, rotation playbooks, and the dev-only placeholders we ship in the repo that **must** be overridden before any non-local deployment. |

If you're auditing the system, [`threat-model.md`](./threat-model.md) is the place to start. If you're deploying a society, [`secrets.md`](./secrets.md) has the override checklist.
