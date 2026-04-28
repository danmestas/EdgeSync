# Contributing to EdgeSync

Thanks for your interest in `EdgeSync`, a peer-to-peer replication and sync
substrate built on `libfossil` (durable state) and NATS (real-time
coordination). This document covers everything you need to get a working
checkout, run the tests, and submit changes.

## Development setup

Requires Go 1.26 or newer.

```
git clone https://github.com/danmestas/EdgeSync
cd EdgeSync
make setup     # installs hooks, builds binaries, runs tests
```

`make setup` is the one-shot bootstrap. The repository has a multi-module
structure (root + `leaf` + `bridge` + `iroh-sidecar`); `setup` builds all
of them. A `go.work.example` file is checked in — copy to `go.work` to
resolve all submodules locally.

## Running tests

```
make test         # full suite across all modules
make vet          # go vet
make sim          # deterministic simulation harness
make sim-full     # extended simulation runs
```

`make test` is the canonical pre-PR gate — the same checks run in CI.

## Building variants

```
make build        # all four binaries (edgesync, leaf, bridge, iroh-sidecar)
make wasm         # WASI + browser WASM targets
make wasm-wasi    # WASI only
make wasm-browser # browser only
```

## Code layout

- `cmd/`, `cli/` — CLI entry points (`edgesync`).
- `leaf/` — leaf daemon Go submodule (separate `go.mod`, separately tagged).
- `bridge/` — cross-cluster sync submodule.
- `iroh-sidecar/` — libp2p sidecar for replication.
- `deploy/` — deployment manifests and tooling.
- `scripts/` — operational and test-helper scripts.
- `sim/` — deterministic simulation harness.
- `testdata/` — fixture data for tests.

## Submitting changes

1. Open a feature branch off `main`. Direct commits to `main` are not accepted.
2. Run `make test` locally before pushing.
3. Open a PR; CI will re-run the full suite.
4. Submodules (`leaf`, `bridge`) are tagged independently — coordinate
   release versions with the root module if a change spans both.

## Reporting issues

Open an issue at https://github.com/danmestas/EdgeSync/issues with
reproduction steps, expected vs. actual behavior, and which submodule
(`edgesync`, `leaf`, `bridge`, `iroh-sidecar`) is affected.
