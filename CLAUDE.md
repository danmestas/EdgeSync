# EdgeSync

Fossil sync engine — replace Fossil's HTTP sync with NATS messaging or direct peer-to-peer. Leaf agents act as both sync clients and servers.

See `MEMORY.md` for detailed project history, patterns, and decisions across sessions.

See `docs/architecture/` for condensed architectural decision records (ARDs):
- `core-library.md` — libfossil package architecture, blob format, xfer wire format, SQLite drivers
- `sync-protocol.md` — xfer card protocol, client/server flow, clone, UV, config sync, private artifacts
- `agent-deployment.md` — leaf agent, bridge, Docker, WASM targets, observability
- `checkout-merge.md` — checkout/checkin, merge strategies, fork prevention, autosync, ci-lock
- `repo-operations.md` — CLI, tags, FTS, verify/rebuild, auth, shun/purge
- `testing-strategy.md` — test tiers, DST, sim, interop, BUGGIFY
- `notify-messaging.md` — bidirectional messaging: data model, dual delivery, CLI, planned phases

## Architecture

- **Leaf Agent** (Go): daemon that reads/writes Fossil SQLite repo DB directly. Syncs via NATS (client) and serves via HTTP or NATS (server). Can replace both client and server roles.
- **Bridge** (Go): translates between NATS messages and Fossil's HTTP /xfer card protocol. Optional — only needed to talk to an unmodified Fossil server.

## Local-Only Directories

`fossil/` and `libfossil/` are upstream C reference checkouts, gitignored (local-only). They exist on disk for porting reference but are NOT tracked in git.

## Build & Test

```bash
make build              # Build edgesync, leaf, bridge binaries into bin/
make test               # Run CI-level tests + sim serve tests (~15s)
make setup-hooks        # Install pre-commit hook (~8s before each commit)
go build -buildvcs=false ./cmd/edgesync/   # Dual VCS needs -buildvcs=false
```

**CI:** GitHub Actions (`.github/workflows/ci.yml`) runs on push to main and PRs. Steps: `go vet`, unit tests (leaf + bridge in parallel), CLI build, CLI tests. Uses `GOWORK=off`. All Go deps are public — no private-module auth needed.

## Go Modules (go.work)

Four modules in a workspace:
- `.` (root) — hosts `cmd/edgesync/`, `sim/`, soak runner
- `leaf/` — leaf agent module
- `bridge/` — bridge module
- `dst/` — deterministic simulation tests
- External dependency: `github.com/danmestas/libfossil` v0.1.x (public, standalone repo)

For local libfossil development, copy `go.work.example` to `go.work` and clone libfossil at `../libfossil`.

**Important:** libfossil hides all implementation packages under `internal/`. EdgeSync imports only the root package handle API (`libfossil.Repo`, `libfossil.Transport`, etc.), plus `db/`, `simio/`, and `testutil/` which remain public.

## Project Structure

### Entry Points
- `cmd/edgesync/` — Unified CLI binary (embeds libfossil's `cli.RepoCmd` + EdgeSync-specific commands)
- `leaf/cmd/leaf/` — Standalone leaf agent daemon
- `bridge/cmd/bridge/` — Standalone bridge daemon
- `sim/cmd/soak/` — Continuous soak test runner

### Core Library: libfossil (external: github.com/danmestas/libfossil v0.1.x)

libfossil exposes an opaque `Repo` handle — all operations are methods on it. Implementation packages (blob, content, manifest, sync, xfer, etc.) are under `internal/` and not importable.

**Public API surface:**

| Symbol | Purpose |
|--------|---------|
| `libfossil.Create()`, `Open()`, `Clone()` | Repo constructors |
| `r.Commit()`, `r.Timeline()`, `r.ListFiles()` | Checkout operations |
| `r.Sync()`, `r.HandleSync()`, `r.XferHandler()` | Client + server sync |
| `r.CreateCheckout()`, `r.OpenCheckout()` | Working directory management |
| `r.Tag()`, `r.UVWrite/Read/List()` | Tags, unversioned files |
| `r.CreateUser()`, `r.ListUsers()`, `r.Config()` | Admin |
| `libfossil.Transport` | Interface — `RoundTrip(ctx, []byte) ([]byte, error)` |
| `libfossil.SyncObserver`, `CheckoutObserver` | Telemetry hooks |
| `libfossil.NewHTTPTransport()` | Built-in HTTP transport |
| `libfossil.NopSyncObserver()`, `StdoutSyncObserver()` | Built-in observers |

**Public packages (still importable):**
- `db/` — SQLite adapter, `db/driver/modernc`, `db/driver/ncruces`
- `simio/` — Clock, Rand, Env for deterministic testing
- `testutil/` — Test helpers
- `cli/` — Embeddable kong CLI commands (38 Fossil-compatible subcommands)
- `observer/otel/` — Optional OTel observer (separate go.mod)

**EdgeSync-specific commands (not in libfossil):**
- `sync start` / `sync now` — NATS agent daemon
- `bridge serve` — NATS-to-HTTP bridge
- `doctor` — Environment health check

### Agent/Bridge
- `leaf/agent/` — `Config` (config.go), `New()`, `Start()`, `Stop()`, `SyncNow()`
  - `nats_mesh.go` — `NATSMesh` module: embedded NATS server + iroh tunnel establishment
  - `serve_http.go` — composes mux with `/healthz` + `r.XferHandler()` (operational endpoints live here, not in libfossil)
  - `serve_nats.go` — NATS request/reply listener for leaf-to-leaf sync
  - Config fields: `NATSRole` (peer/hub/leaf), `NATSUpstream` (optional external NATS), `ServeHTTPAddr`, `ServeNATSEnabled`, `IrohEnabled`, `IrohPeers`, `IrohKeyPath`
  - CLI flags: `--repo`, `--nats`, `--nats-client-port`, `--nats-role`, `--poll`, `--serve-http`, `--serve-nats`, `--uv`, `--push`, `--pull`, `--user`, `--password`, `--iroh`, `--iroh-peer`, `--iroh-key`
- `bridge/bridge/` — `Config` (config.go), `New()`, `Start()`, `Stop()`

### Notify Messaging
- `leaf/agent/notify/` — Bidirectional messaging for human-in-the-loop AI
  - `message.go` — Message, Action, Priority types; `NewMessage()`, `NewReply()`
  - `store.go` — Free functions on `*libfossil.Repo`: `InitNotifyRepo`, `CommitMessage`, `ReadMessage`, `ListThreads`, `ReadThread`
  - `pubsub.go` — `Publish()` free function, `Subscriber` type (dedup, wildcard)
  - `notify.go` — `Service` (Send, Watch, FormatWatchLine)
- `cmd/edgesync/notify.go` — 7 CLI commands: init, send, ask, watch, threads, log, status
- NATS subjects: `notify.<project>.<thread-short>` (separate from sync subjects)
- Storage: dedicated `notify.fossil` repo, managed by libfossil (no `fossil` binary)

### Simulation Testing
- `dst/` — Deterministic single-threaded sim. `SimNetwork` (bridge mode), `PeerNetwork` (leaf-to-leaf). `MockFossil` delegates to `HandleSyncWithOpts`.
- `sim/` — Integration sim (real NATS + TCP fault proxy + real Fossil). Serve tests verify `fossil clone`/`fossil sync` against ServeHTTP.
- Both share `simio/` abstractions and `sync.BuggifyChecker` interface

## Observability (OpenTelemetry)

The leaf agent and sim tests emit traces, metrics, and logs via OpenTelemetry. Telemetry is **optional** — everything builds and runs without it. When `OTEL_EXPORTER_OTLP_ENDPOINT` is unset, the OTel observer is nil and the nopObserver (zero-cost) is used.

### Secrets via Doppler

OTel export secrets (Honeycomb API key) are managed by [Doppler](https://doppler.com). No `.env` files in the repo.

```bash
# First time per device:
brew install doppler
doppler login
doppler setup          # links this directory to edgesync/dev

# Run with telemetry:
doppler run -- go test ./sim/ -run TestSimulation -v
doppler run -- make sim-full

# Run WITHOUT telemetry (no Doppler needed):
go test ./sim/ -run TestSimulation -v     # works fine, just no OTel export
make test                                  # CI tests never need Doppler
```

### Observer Pattern

`libfossil.SyncObserver` and `libfossil.CheckoutObserver` interfaces — lifecycle hooks for sync sessions, rounds, errors, server-side handling, and checkout operations. `leaf/telemetry/observer.go` implements them with OTel spans and metrics. libfossil's root package has **no OTel dependency** — the optional `observer/otel/` sub-module provides an OTel implementation with its own go.mod.

### Honeycomb Dashboard

- Dataset: `edgesync-sim` in `test` environment
- Board: "EdgeSync Sim — Operational Overview"

## Deployment (Hetzner VPS)

Docker Compose stack in `deploy/`. Two containers: NATS + leaf agent.

```bash
# On VPS (203.0.113.1):
cd ~/EdgeSync/deploy && sudo docker compose up -d --build

# Update:
cd ~/EdgeSync && git pull && cd deploy && sudo docker compose up -d --build
```

| Endpoint | URL | Access |
|----------|-----|--------|
| Public HTTPS | `https://sync.example.com` | Cloudflare Tunnel |
| Tailscale HTTP | `http://100.64.0.1:9000` | Tailnet only |
| Tailscale NATS | `nats://100.64.0.1:4222` | Tailnet only |

- `deploy/Dockerfile` — multi-stage build, uses `GOWORK=off` (copies leaf/, depends on published libfossil)
- `deploy/docker-compose.yml` — NATS on Tailscale IP, leaf on port 9000 (8080/8090 occupied by Coolify/Caddy)
- Cloudflare Tunnel config: `~/.cloudflared/config.yml` on VPS, service `cloudflared-fossil`
- Repo files: `deploy/data/*.fossil` (host volume mount)

## Key Conventions

- **SQLite drivers**: `go build` (modernc), `-tags ncruces`. Or `EDGESYNC_SQLITE_DRIVER` env var.
- **Fossil blob format**: `[4-byte BE uncompressed size][zlib data]`. `Compress()` and `Decompress()` handle this.
- **xfer wire format**: Raw zlib (no prefix). `xfer.Decode` auto-detects: raw zlib → prefix+zlib → uncompressed.
- **SHA3 UUIDs**: 64-char = SHA3-256 (Fossil 2.0+), 40-char = SHA1 (legacy)
- **Auth**: Empty `User` = no login card = unauthenticated "nobody" sync
- **simio.CryptoRand{}**: Use for production callsites of `repo.Create` and `db.SeedConfig`
- **Pre-commit hook**: `make setup-hooks` installs ~8s test gate
- **HandleSync vs HandleSyncWithOpts**: Use `r.HandleSync()` in production, `r.HandleSyncWithOpts(ctx, req, HandleOpts{Buggify})` for DST
- **libfossil is transport-agnostic**: No operational endpoints (healthz, metrics) in libfossil. Use `XferHandler()` to compose custom muxes. Operational concerns live in `leaf/agent/serve_http.go`.

## Fossil Sync Protocol

- Transport: HTTP POST to `/xfer`, `application/x-fossil`, zlib-compressed
- Wire format: newline-separated cards (`command arg1 arg2`)
- Core cards: login, push, pull, file, cfile, igot, gimme, cookie, clone, clone_seqno, reqconfig, config
- UV cards: uvfile (NAME MTIME HASH SIZE FLAGS), uvigot (NAME MTIME HASH SIZE), uvgimme (NAME), pragma uv-hash
- Stateless request/response rounds that repeat until convergence
- Push/pull cards accept 1 arg (server-code only) when syncing with known remote
- UV sync: `SyncOpts{UV: true}`, mtime-wins conflict resolution, `pragma uv-hash` short-circuits when catalogs match

## Fossil Repo Schema (core tables)

- `blob` — content-addressed blobs (rid, uuid, size, content)
- `delta` — delta relationships (rid -> srcid)
- `event` — checkin manifests
- `mlink` — file mappings per checkin
- `unversioned` — mutable UV files (name, mtime, hash, sz, encoding, content)

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus. Tools (`query`, `context`, `impact`, `rename`, `detect_changes`, `cypher`) are available via MCP. A PreToolUse hook on `git commit` automatically runs blast-radius analysis on staged changes — no manual steps needed.
<!-- gitnexus:end -->
