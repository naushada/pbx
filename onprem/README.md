# onprem-pbx-onprem-ui

Vaadin 24 admin UI for **onprem-pbx**. It's a thin Java/Spring Boot
front-end whose only backend is the C++ `pbx-cloud` REST API. No DB
access, no Asterisk talk — those live in the cloud and the on-prem
agent respectively.

Sibling of `xpmile/onprem/` (same shape, swap shipment for
society + subscriber).

## What it gives an operator

- Login with `(society code, flat number, password)` against the
  cloud's `POST /api/v1/subscriber/login`.
- Dashboard, society list, subscriber list, bulk import (subsequent
  PRs — see plan).

## Prerequisites

- Java 17
- Maven 3.9+
- A reachable `pbx-cloud` instance (defaults to
  `http://localhost:8080`).

## Run

```sh
cd onprem
mvn spring-boot:run
```

Then open `http://localhost:8081/login`.

## Point at a different backend

Override `backend.url` from the CLI or environment:

```sh
mvn spring-boot:run -Dspring-boot.run.arguments=--backend.url=https://my-cloud.example.com
# or
BACKEND_URL=https://my-cloud.example.com mvn spring-boot:run
```

## Layout

```
onprem/
├── pom.xml
├── README.md
└── src/main
    ├── java/com/onprempbx/onprem
    │   ├── Application.java           # @SpringBootApplication
    │   ├── config/BackendConfig.java  # RestTemplate + backend.url
    │   ├── model/                     # Society, Subscriber, LoginCredentials, BulkResult
    │   ├── service/                   # AuthService, SocietyService, SubscriberService, BulkSubscriberParser
    │   └── ui/                        # LoginView, MainLayout, DashboardView (placeholder)
    └── resources/application.properties
```

## The bigger plan

The 13-item Phase 0/1/2 roadmap (cloud REST gaps → Vaadin scaffold →
admin views) lives in the `project_vaadin_admin_ui` memory note. This
PR delivers Phase 1 items #6-9 (scaffold + models + services + login
shell). Phase 2 (Dashboard, Societies, Subscribers, Bulk Import) ships
in subsequent PRs.
