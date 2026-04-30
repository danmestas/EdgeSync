# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

EdgeSync uses multi-module versioning: the root module and the `leaf` and
`bridge` submodules each carry their own tags (e.g. `v0.0.6`, `leaf/v0.0.3`,
`bridge/v0.0.2`). This file tracks the root module unless otherwise noted.

## [Unreleased]

## [0.0.7] - 2026-04-30

### Changed

- Bumped `libfossil` dependency to v0.4.5. Pulls in fixes for hub xfer
  handler panic on cancelled partial transfers ([libfossil#14], v0.4.4)
  and `sync.Clone` convergence against a hub being concurrently written
  to ([libfossil#17], v0.4.5). Wire-format compatible; no EdgeSync source
  changes.

[libfossil#14]: https://github.com/danmestas/libfossil/issues/14
[libfossil#17]: https://github.com/danmestas/libfossil/issues/17

## [0.0.6] - 2026-04-26

Latest pre-1.0 release. EdgeSync is alpha — see the README "Status" section
for what's stable vs. in-flight.

### Changed

- Bumped `libfossil` dependency to v0.4.3.

## [0.0.1] - 2026-04-23

Initial open-source release of `EdgeSync`, a peer-to-peer replication and
sync substrate built on `libfossil` for durable state and embedded NATS
for real-time coordination. Provides the `leaf` daemon used by `bones`
and other downstream consumers.

### Added

- `leaf` daemon with embedded NATS server.
- `bridge` component for cross-cluster sync.
- `iroh-sidecar` for libp2p-based replication.
- Notify system with shared `natshdr.Carrier` for header propagation.
- WASI and browser WASM build targets.
- Deterministic simulation harness (`sim/`) for replay-based testing.
- Module rename to `github.com/danmestas/EdgeSync`.
