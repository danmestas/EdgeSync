# EdgeSync

Fossil NATS Sync — replace Fossil's HTTP sync with NATS messaging on leaf nodes.

## Architecture

- **Leaf Agent** (Go): daemon that reads/writes Fossil SQLite repo DB directly, publishes/subscribes artifacts via NATS JetStream
- **Bridge** (Go): translates between NATS messages and Fossil's HTTP /xfer card protocol. Master Fossil server is unmodified.

## Read-Only Directories

`fossil/` and `libfossil/` are upstream reference checkouts. NEVER create, edit, write, or delete any file inside these directories. They are read-only reference material for porting.

## Build & Test

```bash
make build              # Build edgesync, leaf, bridge binaries into bin/
make test               # Run CI-level tests (~10s)
make setup-hooks        # Install pre-commit hook (~5s of tests before each commit)
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
- `cmd/edgesync/` — Unified CLI binary (49 subcommands via kong)
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
| `sync/` | Xfer sync session (push/pull rounds) | `Session`, `SyncOpts`, `BuggifyChecker` |
| `tag/` | Tag read/write on artifacts | `Add()`, `List()` |
| `testutil/` | Shared test helpers | `TempRepo()` |
| `undo/` | Undo/redo state tracking | `Save()`, `Undo()`, `Redo()` |
| `xfer/` | Xfer card protocol encoder/decoder | `Encode()`, `Decode()`, `Message` |

### Agent/Bridge
- `leaf/agent/` — Agent logic: `Config` (in config.go), `New()`, `Start()`, `Stop()`, `SyncNow()`
- `bridge/bridge/` — Bridge logic: `Config` (in config.go), `New()`, `Start()`, `Stop()`

### Simulation Testing
- `dst/` — Deterministic single-threaded sim (seeded PRNG, `SimNetwork`, `MockFossil`, event queue)
- `sim/` — Integration sim (real NATS + TCP fault proxy + real Fossil server). Part of root module.
- Both share `simio/` abstractions and `sync.BuggifyChecker` interface

## Key Conventions

- **SQLite drivers**: Build tags select driver — `go build` (modernc), `-tags ncruces`, `-tags mattn`. Or `EDGESYNC_SQLITE_DRIVER` env var at runtime.
- **Fossil blob format**: `[4-byte BE uncompressed size][zlib data]`. Both `Compress()` and `Decompress()` handle this prefix.
- **SHA3 UUIDs**: 64-char = SHA3-256 (Fossil 2.0+), 40-char = SHA1 (legacy)
- **Auth**: Empty `User` in config = no login card = unauthenticated "nobody" sync
- **simio.CryptoRand{}**: Use for production callsites of `repo.Create` and `db.SeedConfig`
- **Pre-commit hook**: `make setup-hooks` installs ~5s test gate before each commit

## Fossil Sync Protocol

- Transport: HTTP POST to `/xfer`, `application/x-fossil`, zlib-compressed
- Wire format: newline-separated cards (`command arg1 arg2`)
- Core cards: login, push, pull, file, cfile, igot, gimme, cookie, clone
- Stateless request/response rounds that repeat until convergence

## Fossil Repo Schema (core tables)

- `blob` — content-addressed blobs (rid, uuid, size, content)
- `delta` — delta relationships (rid -> srcid)
- `event` — checkin manifests
- `mlink` — file mappings per checkin

## Reference Source

C reference implementations live in the repo checkouts:
- `fossil/src/delta.c` — delta algorithm
- `fossil/src/xfer.c` — sync protocol
- `libfossil/checkout/src/delta.c` — libfossil's delta port
- `libfossil/checkout/src/content.c` — artifact storage/retrieval
- `libfossil/checkout/src/xfer.c` — network sync (partial)
- `libfossil/checkout/src/deck.c` — manifest parsing
- `libfossil/checkout/src/db.c` — SQLite wrapper
