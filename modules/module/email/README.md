# email — SMTP client (FSM-driven)

> **Status:** ⏳ Empty. Copied from xpmile in **Layer 1**.

Sends transactional email from the cloud. In onprem-pbx the only sender is the CSV-import flow: after the admin uploads a resident list, the cloud generates `sipUsername` + `sipPassword` + portal password for each subscriber, then mails them their credentials.

## Components (xpmile naming)

- `EmailService` — SMTP client, configurable provider.
- `EmailServiceFSM` — finite-state machine driving the SMTP conversation (HELO/EHLO → AUTH → MAIL FROM → RCPT TO → DATA → QUIT). xpmile's tests cover the FSM transitions exhaustively.

## Onprem-pbx usage

The `MicroService::handle_subscriber_import` handler (Layer 1, in [`webservice/`](../webservice/README.md)) calls `EmailService` once per imported row. The plaintext password is included in the email body and **also** returned to the admin in a one-time-download CSV. After that download, the plaintext is unrecoverable (only the bcrypt portal hash and the MD5 `sipHa1` remain in Mongo).

PRD §11 calls out SMS as the recommended secondary channel; SMS is **not** in scope for v1.

## Origin

Verbatim copy from `xpmile/modules/module/email/`. The SMTP FSM is already well-tested in xpmile (the `EmailService*` suite, ~12 tests, will travel with the copy). No API divergence planned.

## Tests

`EmailService*` (xpmile origin) carries over. Must remain green.
