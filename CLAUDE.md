# EdgeSync

Fossil sync engine — replace Fossil's HTTP sync with NATS messaging or direct peer-to-peer. Leaf agents act as both sync clients and servers.

See `MEMORY.md` for detailed project history, patterns, and decisions across sessions.

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

Five modules in a workspace:
- `.` (root) — hosts `cmd/edgesync/`, `sim/`, soak runner
- `go-libfossil/` — core library, all Fossil internals
- `leaf/` — leaf agent module
- `bridge/` — bridge module
- `dst/` — deterministic simulation tests

## Project Structure

### Entry Points
- `cmd/edgesync/` — Unified CLI binary (50 subcommands via kong)
- `leaf/cmd/leaf/` — Standalone leaf agent daemon
- `bridge/cmd/bridge/` — Standalone bridge daemon
- `sim/cmd/soak/` — Continuous soak test runner

### Core Library: go-libfossil/

| Package | Purpose | Key Types |
|---------|---------|-----------|
| `annotate/` | Line-level blame/annotate | `Annotate()` |
| `bisect/` | Binary search for regressions | `Session`, `Step()` |
| `blob/` | Blob compression (4-byte BE size prefix + zlib) | `Compress()`, `Decompress()`, `Load()` |
| `content/` | Artifact storage, delta chain expansion | `Store()`, `Expand()` |
| `db/` | SQLite adapter (3 drivers via build tags) | `Open()`, `OpenWith()`, `DB` |
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
- **Server**: `HandleSync()`, `HandleSyncWithOpts()`, `HandleFunc`, `HandleOpts`, `ServeHTTP()`
- **UV Cards**: `handler_uv.go` (server-side uvfile/uvigot/uvgimme dispatch), client UV in `client.go`
- **Shared**: `storeReceivedFile()`, `resolveFileContent()`, `BuggifyChecker`
- **Transports**: `HTTPTransport`, `MockTransport` (in sync/); `NATSTransport` (in leaf/agent/)

### Agent/Bridge
- `leaf/agent/` — `Config` (config.go), `New()`, `Start()`, `Stop()`, `SyncNow()`, `ServeNATS()`, `ServeP2P()` stub
  - Config fields: `ServeHTTPAddr` (":8080" to serve HTTP), `ServeNATSEnabled` (leaf-to-leaf)
- `bridge/bridge/` — `Config` (config.go), `New()`, `Start()`, `Stop()`

### Simulation Testing
- `dst/` — Deterministic single-threaded sim. `SimNetwork` (bridge mode), `PeerNetwork` (leaf-to-leaf). `MockFossil` delegates to `HandleSyncWithOpts`.
- `sim/` — Integration sim (real NATS + TCP fault proxy + real Fossil). Serve tests verify `fossil clone`/`fossil sync` against ServeHTTP.
- Both share `simio/` abstractions and `sync.BuggifyChecker` interface

## Key Conventions

- **SQLite drivers**: `go build` (modernc), `-tags ncruces`, `-tags mattn`. Or `EDGESYNC_SQLITE_DRIVER` env var.
- **Fossil blob format**: `[4-byte BE uncompressed size][zlib data]`. `Compress()` and `Decompress()` handle this.
- **xfer wire format**: Raw zlib (no prefix). `xfer.Decode` auto-detects: raw zlib → prefix+zlib → uncompressed.
- **SHA3 UUIDs**: 64-char = SHA3-256 (Fossil 2.0+), 40-char = SHA1 (legacy)
- **Auth**: Empty `User` = no login card = unauthenticated "nobody" sync
- **simio.CryptoRand{}**: Use for production callsites of `repo.Create` and `db.SeedConfig`
- **Pre-commit hook**: `make setup-hooks` installs ~8s test gate
- **HandleSync vs HandleSyncWithOpts**: Use `HandleSync` in production, `HandleSyncWithOpts` with `HandleOpts{Buggify}` for DST

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
