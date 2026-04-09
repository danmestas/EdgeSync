# Agent & Bridge Deployment

## Leaf Agent Architecture

The leaf agent is a Go daemon in its own module (`leaf/`) that opens a Fossil repo via go-libfossil, connects to NATS, and syncs artifacts on a poll loop. It acts as both sync client and server.

**Lifecycle:** `New(Config)` -> `Start()` -> `Stop()`. A single poll goroutine is the sole executor of `doSync()`. `SyncNow()` is a non-blocking signal (buffered channel, size 1) that coalesces if a sync is already running. This avoids concurrent SQLite access.

**Signals:** SIGUSR1 triggers `SyncNow()`. SIGINT/SIGTERM trigger graceful `Stop()` (cancel context, drain NATS, close repo).

| Config Field | Default | CLI Flag | Env Var |
|---|---|---|---|
| `RepoPath` | (required) | `--repo` | `LEAF_REPO` |
| `NATSRole` | `"peer"` | `--nats-role` | |
| `NATSUpstream` | `""` (embedded only) | `--nats` | `LEAF_NATS_URL` |
| `PollInterval` | `5s` | `--poll` | |
| `User` | `""` (anonymous) | `--user` | |
| `Password` | `""` | `--password` | |
| `Push` | `true` | `--push` / `--no-push` | |
| `Pull` | `true` | `--pull` / `--no-pull` | |
| `ServeHTTPAddr` | `""` (disabled) | `--serve-http` | |
| `ServeNATSEnabled` | `false` | `--serve-nats` | |
| `UV` | `false` | `--uv` | |
| `SubjectPrefix` | `"fossil"` | `--prefix` | |

**Embedded NATS:** Every agent runs an in-process NATS server. The agent's NATS client connects to localhost. If `NATSUpstream` is set, the embedded server joins external NATS as a leaf node. If iroh peers are configured, the embedded server connects to them via tunnels over QUIC.

**NATSRole** determines mesh topology:

| Role | Accepts leaf connections | Solicits leaf connections | Use case |
|---|---|---|---|
| `peer` (default) | Yes | Yes (lower EndpointId solicits) | Native P2P mesh |
| `hub` | Yes | No | Dedicated server, cluster node |
| `leaf` | No | Yes (always solicits outward) | WASM browser, lightweight client |

## Bridge Architecture

The bridge is a NATS-to-HTTP translator in its own module (`bridge/`). It subscribes to `<prefix>.<project-code>.sync`, decodes xfer messages, proxies them to a Fossil HTTP server via `sync.HTTPTransport`, and replies. One project per instance.

**Error handling:** Decode or HTTP failures produce an empty response (never leave the leaf hanging, never crash).

| Config Field | Default | CLI Flag | Env Var |
|---|---|---|---|
| `FossilURL` | (required) | `--fossil` | `BRIDGE_FOSSIL_URL` |
| `ProjectCode` | (required) | `--project` | `BRIDGE_PROJECT_CODE` |
| `NATSUrl` | `nats://localhost:4222` | `--nats` | `BRIDGE_NATS_URL` |
| `SubjectPrefix` | `"fossil"` | `--prefix` | `BRIDGE_PREFIX` |

## NATS Transport

`NATSTransport` implements `sync.Transport` over NATS request/reply. Subject scheme: `<prefix>.<project-code>.sync`. Exchange flow: encode xfer message -> zlib compress -> NATS request -> decode reply.

**Constraint:** Leaf and bridge must agree on the subject prefix for a given project.

**JetStream upgrade path:** Replace `Request` with JetStream publish/consume. The `Exchange` signature stays the same.

## Iroh P2P Transport

Third transport option alongside NATS and HTTP. Enables direct peer-to-peer sync without central infrastructure, using QUIC holepunching + relay fallback for NAT traversal.

**Architecture:** Rust sidecar binary (`iroh-sidecar`) communicates with the Go agent via HTTP on a Unix socket. The agent spawns the sidecar as a child process. This avoids CGo while leveraging iroh's Rust networking stack.

**Identity:** Ed25519 keypair. Each agent has a stable `EndpointId` derived from its key. Peers are configured by EndpointId, not address.

| Config Field | Default | CLI Flag |
|---|---|---|
| `IrohEnabled` | `false` | `--iroh` |
| `IrohPeers` | `[]` | `--iroh-peer` (repeatable) |
| `IrohKeyPath` | `~/.config/edgesync/iroh.key` | `--iroh-key` |

**Agent lifecycle:**
- `Start()`: spawn sidecar (`--socket`, `--key-path`, `--callback`, `--alpn /edgesync/xfer/1`), wait for `/status` 200, register each peer as a `syncTarget` with `IrohTransport`
- Poll loop: iterates all `syncTargets` uniformly (NATS, iroh, HTTP)
- Incoming: sidecar forwards peer requests to agent's callback listener → `repo.XferHandler()`
- `Stop()`: POST `/shutdown` to sidecar, wait 5s, SIGTERM, wait 2s, SIGKILL

**Socket path:** `/tmp/iroh-<sha256(repoPath)[:12]>.sock` — deterministic, collision-free for multiple agents.

**IrohTransport:** `RoundTrip(ctx, payload)` POSTs to `http://iroh-sidecar/exchange/<endpointID>` over Unix socket. Same `Transport` interface as NATS and HTTP — sync layer is unaware of the transport.

**Discovery:** Phase 1 is manual config only (`--iroh-peer`). DNS discovery and NATS-assisted peer exchange are future work.

## HTTP Serving

`serve_http.go` in `leaf/agent/` composes an HTTP mux with `/healthz` (200 OK, JSON status) and `sync.XferHandler()` for Fossil's `/xfer` endpoint. Operational endpoints live in the agent, not in go-libfossil (which is transport-agnostic).

Stock `fossil clone` and `fossil sync` work against the leaf's HTTP endpoint.

## Docker Deployment

Stack lives in `deploy/`. Two containers: NATS + leaf agent.

**Dockerfile:** Multi-stage build. Build stage: `golang:1.25-alpine` with CGO (modernc SQLite). Run stage: `alpine:3.20` (~20-30MB). Uses `GOWORK=off`, copies only `go-libfossil/` and `leaf/`.

**Compose services:**

| Service | Ports | Notes |
|---|---|---|
| `nats` | `100.78.32.45:4222` (Tailscale only) | JetStream enabled, no auth (Tailscale boundary) |
| `leaf` | `0.0.0.0:9000` (public HTTP) | Health check via `/healthz`, auto-restart |

**Volumes:** `deploy/data/*.fossil` (host mount), `nats-data` (named volume).

**Endpoints:**

| Endpoint | URL | Access |
|---|---|---|
| Public HTTPS | `https://sync.craftdesign.group` | Cloudflare Tunnel |
| Tailscale HTTP | `http://100.78.32.45:9000` | Tailnet only |
| Tailscale NATS | `nats://100.78.32.45:4222` | Tailnet only |

**Excluded from v1:** TLS termination (use reverse proxy), NATS auth, multi-repo, non-root user, OTel in container, backup cron.

## WASM Targets

Two targets, same agent API (`New` -> `Start` -> `Stop`):

| Target | GOOS | SQLite | Network | HTTP Serve | Entry Point |
|---|---|---|---|---|---|
| WASI | `wasip1` | ncruces (as-is) | Host networking | Full | `leaf/cmd/leaf/` |
| Browser | `js` | ncruces + OPFS VFS | NATS via WebSocket | No-op (can't listen) | `leaf/cmd/wasm/` |

**Key decisions:**

- `simio.Storage` interface added (following Clock/Rand pattern): `OSStorage` (prod/WASI), `MemStorage` (tests), `OPFSStorage` (browser, `//go:build js`).
- Browser NATS uses `nats.SetCustomDialer()` with a `syscall/js` WebSocket dialer.
- Build-tagged no-ops: `serve_http_js.go`, `signals_js.go` replace platform features in browser.
- Browser exposes JS globals (`edgesync.newAgent`, `.start`, `.stop`, `.syncNow`, `.clone`) and runs in a Web Worker.
- `stash/` and `undo/` excluded from WASM (`//go:build !js`) -- checkout features, not needed for sync.

## Observability

### Observer Pattern

`sync.Observer` interface in go-libfossil -- zero OTel dependency. Lifecycle hooks injected via `SyncOpts.Observer` and `CloneOpts.Observer`. Nil observer uses `nopObserver` (zero-cost, ~2ns indirect call).

```go
type Observer interface {
    Started(ctx context.Context, info SessionStart) context.Context
    RoundStarted(ctx context.Context, round int) context.Context
    RoundCompleted(ctx context.Context, round int, stats RoundStats)
    Completed(ctx context.Context, info SessionEnd, err error)
    Error(ctx context.Context, err error)
    HandleStarted(ctx context.Context, info HandleStart) context.Context
    HandleCompleted(ctx context.Context, info HandleEnd)
}
```

**Design constraint:** No per-card child spans. Card-level detail aggregated into `RoundStats` (files, gimmes, igots, bytes). `HandleStarted`/`HandleCompleted` cover server-side request lifecycle. Bridge uses Observer for tracing without an OTel dependency.

### OTel Integration

All OTel code lives in `leaf/telemetry/` (build-tagged `!wasip1 && !js`). WASM gets no-op stubs. go-libfossil and bridge modules have zero OTel dependencies.

**Setup:** `telemetry.Setup()` initializes TracerProvider + MeterProvider with OTLP exporters. Falls back to standard `OTEL_*` env vars. No endpoint = no-op (zero overhead).

**Shutdown order:** `agent.Stop()` first (drains in-flight sync), then `telemetry.Shutdown()` (flushes spans/metrics).

| Metric | Type |
|---|---|
| `sync.sessions.total` | Counter |
| `sync.errors.total` | Counter |
| `sync.duration.seconds` | Histogram |
| `sync.rounds` | Histogram |
| `sync.files.sent` | Histogram |
| `sync.files.received` | Histogram |
| `sync.uv.files.sent` | Histogram |
| `sync.uv.files.received` | Histogram |

**slog integration:** `log.Printf` replaced with `slog.InfoContext`/`slog.ErrorContext` across agent, bridge, and serve_http. Context carries trace/span IDs via otelslog bridge.

**Backend swappable via env vars only** (Honeycomb, Grafana Cloud, Jaeger, Axiom). OTel secrets managed by Doppler -- no `.env` files.

| CLI Flag | Env Var |
|---|---|
| `--otel-endpoint` | `OTEL_EXPORTER_OTLP_ENDPOINT` |
| `--otel-headers` | `OTEL_EXPORTER_OTLP_HEADERS` |
| `--otel-service-name` | `OTEL_SERVICE_NAME` |

## Module Layout

Four modules in EdgeSync's `go.work`:

| Module | OTel Deps | NATS Deps | Purpose |
|---|---|---|---|
| `.` (root) | No | No | CLI, sim tests, soak runner |
| `leaf/` | Yes | Yes | Agent daemon, telemetry |
| `bridge/` | No | Yes | NATS-HTTP translator |
| `dst/` | No | No | Deterministic simulation |

go-libfossil is an external dependency at `github.com/danmestas/go-libfossil` (standalone repo). For local development, copy `go.work.example` to `go.work` and clone go-libfossil at `../go-libfossil`.
