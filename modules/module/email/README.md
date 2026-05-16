# email — SMTP client (FSM-driven)

> **Status:** ✅ Verbatim copy of the upstream shared-library module — do not modify locally (Layer 1, transitive — `webservice.cpp` includes `emailservice.hpp`). Inherited `EmailService*` tests green.

Sends transactional email from the cloud. In onprem-pbx the only sender is the CSV-import flow: after the admin uploads a resident list, the cloud generates `sipUsername` + `sipPassword` + portal password for each subscriber, then mails them their credentials.

## Components

- `EmailService` — SMTP client, configurable provider.
- `EmailServiceFSM` — finite-state machine driving the SMTP conversation (HELO/EHLO → AUTH → MAIL FROM → RCPT TO → DATA → QUIT). The inherited tests cover the FSM transitions exhaustively.

## Onprem-pbx usage

The `MicroService::handle_subscriber_import` handler (Layer 1, in [`webservice/`](../webservice/README.md)) calls `EmailService` once per imported row. The plaintext password is included in the email body and **also** returned to the admin in a one-time-download CSV. After that download, the plaintext is unrecoverable (only the bcrypt portal hash and the MD5 `sipHa1` remain in Mongo).

PRD §11 calls out SMS as the recommended secondary channel; SMS is **not** in scope for v1.

## Origin

Verbatim copy of the upstream shared-library `modules/module/email/` — do not modify locally. The SMTP FSM is already well-tested upstream (the `EmailService*` suite, ~12 tests, travels with the copy). No API divergence planned.

## Tests

`EmailService*` (inherited from the shared library) carries over. Must remain green.
