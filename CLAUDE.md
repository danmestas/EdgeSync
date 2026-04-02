# EdgeSync

Fossil sync engine — replace Fossil's HTTP sync with NATS messaging or direct peer-to-peer. Leaf agents act as both sync clients and servers.

See `MEMORY.md` for detailed project history, patterns, and decisions across sessions.

See `docs/architecture/` for condensed architectural decision records (ARDs):
- `core-library.md` — go-libfossil package architecture, blob format, xfer wire format, SQLite drivers
- `sync-protocol.md` — xfer card protocol, client/server flow, clone, UV, config sync, private artifacts
- `agent-deployment.md` — leaf agent, bridge, Docker, WASM targets, observability
- `checkout-merge.md` — checkout/checkin, merge strategies, fork prevention, autosync, ci-lock
- `repo-operations.md` — CLI, tags, FTS, verify/rebuild, auth, shun/purge
- `testing-strategy.md` — test tiers, DST, sim, interop, BUGGIFY

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

## Go Modules (go.work)

Four modules in a workspace:
- `.` (root) — hosts `cmd/edgesync/`, `sim/`, soak runner
- `leaf/` — leaf agent module
- `bridge/` — bridge module
- `dst/` — deterministic simulation tests
- External dependency: `github.com/danmestas/go-libfossil` v0.x (standalone repo)

For local go-libfossil development, copy `go.work.example` to `go.work` and clone go-libfossil at `../go-libfossil`.

## Project Structure

### Entry Points
- `cmd/edgesync/` — Unified CLI binary (50 subcommands via kong)
- `leaf/cmd/leaf/` — Standalone leaf agent daemon
- `bridge/cmd/bridge/` — Standalone bridge daemon
- `sim/cmd/soak/` — Continuous soak test runner

### Core Library: go-libfossil (external: github.com/danmestas/go-libfossil)

| Package | Purpose | Key Types |
|---------|---------|-----------|
| `annotate/` | Line-level blame/annotate | `Annotate()` |
| `bisect/` | Binary search for regressions | `Session`, `Step()` |
| `blob/` | Blob compression (4-byte BE size prefix + zlib) | `Compress()`, `Decompress()`, `Load()` |
| `content/` | Artifact storage, delta chain expansion | `Store()`, `Expand()` |
| `db/` | SQLite adapter (2 drivers: modernc, ncruces) | `Open()`, `OpenWith()`, `DB` |
| `deck/` | Manifest/control-artifact parsing | `Parse()`, `Deck` |
| `delta/` | Fossil delta codec (port of delta.c) | `Create()`, `Apply()` |
| `hash/` | SHA1/SHA3-256 content addressing | `SHA1()`, `SHA3()` |
| `manifest/` | Checkin, file listing, timeline | `Checkin()`, `ListFiles()`, `Log()` |
| `merge/` | 3-way merge with swappable strategies | `Strategy`, `ThreeWayText`, `FindCommonAncestor()` |
| `path/` | Checkout path resolution | `Resolve()` |
| `repo/` | Fossil repo DB operations | `Create()`, `Open()`, `Verify()` |
| `simio/` | Simulation I/O (Clock, Rand, Env) | `SimClock`, `RealEnv()`, `CryptoRand{}` |
| `stash/` | Working-tree stash | `Save()`, `Pop()`, `List()` |
| `sync/` | Sync engine — client + server | `Sync()`, `Clone()`, `HandleSync()`, `ServeHTTP()` |
| `tag/` | Tag read/write on artifacts | `Add()`, `List()` |
| `testutil/` | Shared test helpers | `NewTestRepo()` |
| `undo/` | Undo/redo state tracking | `Save()`, `Undo()`, `Redo()` |
| `uv/` | Unversioned file sync (wiki, forum, attachments) | `Status()`, `Write()`, `Read()`, `ContentHash()` |
| `xfer/` | Xfer card protocol encoder/decoder | `Encode()`, `Decode()`, `Message` |

### sync/ Package (key types)
- **Client**: `Sync()`, `Clone()`, `SyncOpts{UV: true}`, `CloneOpts`, `Transport` interface
- **Server**: `HandleSync()`, `HandleSyncWithOpts()`, `HandleFunc`, `HandleOpts`, `ServeHTTP()`, `XferHandler()`
- **UV Cards**: `handler_uv.go` (server-side uvfile/uvigot/uvgimme dispatch), client UV in `client.go`
- **Shared**: `storeReceivedFile()`, `resolveFileContent()`, `BuggifyChecker`
- **Transports**: `HTTPTransport`, `MockTransport` (in sync/); `NATSTransport` (in leaf/agent/)

### Agent/Bridge
- `leaf/agent/` — `Config` (config.go), `New()`, `Start()`, `Stop()`, `SyncNow()`
  - `serve_http.go` — composes mux with `/healthz` + `sync.XferHandler()` (operational endpoints live here, not in go-libfossil)
  - `serve_nats.go` — NATS request/reply listener for leaf-to-leaf sync
  - `serve_p2p.go` — libp2p stub
  - Config fields: `ServeHTTPAddr` (":8080" to serve HTTP), `ServeNATSEnabled` (leaf-to-leaf)
  - CLI flags: `--repo`, `--nats`, `--poll`, `--serve-http`, `--serve-nats`, `--uv`, `--push`, `--pull`, `--user`, `--password`
- `bridge/bridge/` — `Config` (config.go), `New()`, `Start()`, `Stop()`

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

`sync.Observer` interface in `go-libfossil/sync/observer.go` — lifecycle hooks for session, round, error, and server-side handle events. `leaf/telemetry/observer.go` implements it with OTel spans and metrics. `go-libfossil/` has **no OTel dependency** — all instrumentation flows through the Observer interface.

### Honeycomb Dashboard

- Dataset: `edgesync-sim` in `test` environment
- Board: "EdgeSync Sim — Operational Overview"

## Deployment (Hetzner VPS)

Docker Compose stack in `deploy/`. Two containers: NATS + leaf agent.

```bash
# On VPS (91.99.202.69):
cd ~/EdgeSync/deploy && sudo docker compose up -d --build

# Update:
cd ~/EdgeSync && git pull && cd deploy && sudo docker compose up -d --build
```

| Endpoint | URL | Access |
|----------|-----|--------|
| Public HTTPS | `https://sync.craftdesign.group` | Cloudflare Tunnel |
| Tailscale HTTP | `http://100.78.32.45:9000` | Tailnet only |
| Tailscale NATS | `nats://100.78.32.45:4222` | Tailnet only |

- `deploy/Dockerfile` — multi-stage build, uses `GOWORK=off` (copies leaf/, depends on published go-libfossil)
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
- **HandleSync vs HandleSyncWithOpts**: Use `HandleSync` in production, `HandleSyncWithOpts` with `HandleOpts{Buggify}` for DST
- **go-libfossil is transport-agnostic**: No operational endpoints (healthz, metrics) in go-libfossil. Use `XferHandler()` to compose custom muxes. Operational concerns live in `leaf/agent/serve_http.go`.

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
