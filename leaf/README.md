# leaf

The EdgeSync leaf agent: a Go daemon that opens a Fossil repo via libfossil, runs an embedded NATS server, and syncs artifacts on a poll loop. Each agent acts as both sync client and server, with three transport options: NATS, HTTP, and iroh (QUIC peer-to-peer).

Module path: `github.com/danmestas/EdgeSync/leaf`

## What it does

A single agent process owns one Fossil repo, polls peers/upstreams for changes, and serves incoming sync requests. The poll goroutine is the sole executor of `doSync()`, so SQLite stays single-writer. `SyncNow()` is a non-blocking signal (size-1 buffered channel) that coalesces with in-flight syncs.

Three transports compose uniformly via the `sync.Transport` interface (the layer above is unaware of which one is in use):

- **NATS** — request/reply on `<prefix>.<project-code>.sync`, optionally over an embedded NATS leafnode
- **HTTP** — `/xfer` Fossil-compatible endpoint plus `/healthz`; stock `fossil clone`/`fossil sync` work against it
- **iroh** — Rust sidecar over Unix socket; QUIC holepunching with relay fallback for NAT traversal

## Public API surface

- `agent.Config` — fields documented in [`docs/architecture/agent-deployment.md`](../docs/architecture/agent-deployment.md)
- `agent.New(Config)`, `(*Agent).Start()`, `Stop()`, `SyncNow()`
- `notify.Service` — bidirectional messaging on a separate `notify.fossil` repo (`Send`, `Watch`, pair-URL helpers)
- `telemetry.Setup`, `telemetry.NewOTelObserver` — optional OTel; nil endpoint means zero-cost no-op observer

CLI: `cmd/leaf/main.go` (Kong); WASM browser entry: `cmd/wasm/main.go` (exposes `edgesync.newAgent` etc. as JS globals).

## Build & test

```bash
cd leaf && go build -buildvcs=false -o ../bin/leaf ./cmd/leaf  # or `make leaf` from repo root
cd leaf && go test ./... -short -count=1                       # unit tests
make test-iroh                                                  # iroh integration (builds Rust sidecar)
```

WASM targets are built from the repo root: `make wasm-wasi`, `make wasm-browser`. See [`docs/architecture/wasm-targets.md`](../docs/architecture/wasm-targets.md).

## Where it fits

The leaf agent is the user-facing daemon in EdgeSync. It replaces both Fossil's HTTP sync client and server. The optional [`bridge`](../bridge) module exists only for talking to an unmodified Fossil HTTP server. Deterministic and integration tests live in [`sim/`](../sim) at the repo root.

See [`docs/architecture/agent-deployment.md`](../docs/architecture/agent-deployment.md) for full configuration, NATS-mesh roles, and observability details.
