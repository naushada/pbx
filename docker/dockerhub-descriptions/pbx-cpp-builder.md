# pbx-cpp-builder — INTERNAL CI base image

> ⚠️ **This image is NOT meant for runtime use.** It exists only as a
> `FROM` base for the other `onprem-pbx-*` images during build. If you
> were looking for the on-prem PBX agent runtime, see
> [`onprem-pbx-agent`](https://hub.docker.com/r/naushada/onprem-pbx-agent)
> and [`onprem-pbx-wsdbagent`](https://hub.docker.com/r/naushada/onprem-pbx-wsdbagent).

## What's inside

The pre-baked C++ toolchain layer the onprem-pbx project compiles
against:

- **Ubuntu 20.04** base (matches the runtime stage glibc of every
  dependent Dockerfile).
- **ACE/TAO 7.0.0** — the ADAPTIVE Communication Environment, the
  networking + reactor framework used throughout pbx-agent and
  pbx-cloud.
- **mongo-c-driver 1.19.1** + **mongo-cxx-driver v3.6** — Mongo
  client libs (`BSONCXX_POLY_USE_MNMLSTC=1`).
- **googletest 1.12.1** — link target for the `offtarget` test
  binary.
- Standard build tools: `cmake`, `build-essential`, `libboost-all-dev`,
  `libssl-dev`, `zlib1g-dev`, `wget`, `git`, `pkg-config`.

## How it's used

Every dependent Dockerfile starts with:

```Dockerfile
ARG BUILDER_IMAGE=localhost/pbx-cpp-builder:bootstrap
FROM ${BUILDER_IMAGE} AS cpp-builder
```

CI overrides `BUILDER_IMAGE` to `docker.io/naushada/pbx-cpp-builder:bootstrap`
(this image) so each downstream build skips the ~30 min cpp-toolchain
compile and just builds the per-service binary on top.

## Tags

- `bootstrap` — rolling tag, always the latest main-branch build.
- `<sha>` — immutable per-commit tag (40-char Git SHA).

Source: https://github.com/naushada/pbx — `docker/Dockerfile.bootstrap`
