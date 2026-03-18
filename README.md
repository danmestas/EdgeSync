# EdgeSync

Replace Fossil's HTTP sync on leaf repos with NATS messaging. The master Fossil server stays unmodified — a bridge translates between NATS and HTTP `/xfer`.

## Architecture

```
┌──────────┐    NATS     ┌──────────┐   HTTP /xfer   ┌──────────────┐
│   Leaf   │◄───────────►│  Bridge  │◄──────────────►│ Fossil Server│
│  Agent   │  JetStream  │          │   (standard)   │  (unmodified)│
└──────────┘             └──────────┘                └──────────────┘
     │                                                      │
     ▼                                                      ▼
  SQLite repo                                         SQLite repo
  (direct R/W)                                        (fossil ui works)
```

**Leaf Agent** — Daemon on machines with Fossil repos. Reads/writes the SQLite repo DB directly. Publishes artifacts to NATS on commit, subscribes for incoming artifacts. JetStream consumer for offline catch-up.

**Bridge** — Speaks NATS on one side, HTTP `/xfer` on the other. To the master Fossil server it looks like a normal Fossil client doing push/pull.

## Quick Start

```bash
# Build everything
make build

# Run tests (what CI runs)
make test

# Install pre-commit hook (~5s of tests before each commit)
make setup-hooks
```

## Project Layout

```
cmd/edgesync/          Unified CLI binary (49 subcommands)

go-libfossil/          Core library — Go port of Fossil internals
  annotate/            Line-level blame/annotate
  bisect/              Binary search for regressions
  blob/                Blob compression (Fossil's 4-byte prefix + zlib format)
  content/             Artifact storage and expansion (delta chains)
  db/                  SQLite adapter (3 drivers via build tags)
  deck/                Manifest/control-artifact parsing
  delta/               Fossil delta codec (ported from delta.c)
  hash/                SHA1/SHA3-256 content addressing
  manifest/            Checkin, file listing, timeline
  merge/               3-way merge with swappable strategies
  path/                Checkout path resolution
  repo/                Fossil repo DB operations (create, open, verify)
  simio/               Simulation I/O abstractions (Clock, Rand, Env)
  stash/               Working-tree stash save/restore
  sync/                Xfer sync session (push/pull rounds)
  tag/                 Tag read/write on artifacts
  testutil/            Shared test helpers
  undo/                Undo/redo state tracking
  xfer/                Xfer card protocol encoder/decoder

leaf/                  Leaf agent module
  agent/               Agent logic (config, NATS connection, sync loop)
  cmd/leaf/            Standalone leaf daemon binary

bridge/                Bridge module
  bridge/              Bridge logic (NATS <-> HTTP /xfer translation)
  cmd/bridge/          Standalone bridge daemon binary

dst/                   Deterministic simulation testing (single-threaded, seeded)
sim/                   Integration simulation testing (real NATS + Fossil + fault proxy)
  cmd/soak/            Continuous soak test runner

docs/                  Documentation
  dev/specs/           Design specifications (historical)
  dev/plans/           Implementation plans (historical)

fossil/                Fossil SCM source checkout (read-only reference)
libfossil/             Libfossil source checkout (read-only reference)
```

## Go Modules

This project uses a Go workspace (`go.work`) with multiple modules:

| Module | Path | Purpose |
|--------|------|---------|
| `github.com/dmestas/edgesync` | `.` | Root: CLI, sim/, soak runner |
| `github.com/dmestas/edgesync/go-libfossil` | `go-libfossil/` | Core library |
| `github.com/dmestas/edgesync/leaf` | `leaf/` | Leaf agent |
| `github.com/dmestas/edgesync/bridge` | `bridge/` | Bridge |
| `github.com/dmestas/edgesync/dst` | `dst/` | Deterministic sim tests |

## SQLite Drivers

The SQLite driver is configurable via build tags:

```bash
go build ./...                            # modernc (default, pure Go)
go build -tags ncruces ./...              # ncruces (WASM-based)
CGO_ENABLED=1 go build -tags mattn ./...  # mattn (CGo, best performance)
```

Or set `EDGESYNC_SQLITE_DRIVER` at runtime.

## Testing

```bash
make test              # CI tests (~10s)
make dst               # DST: 8 seeds, normal (~2s)
make dst-full          # DST: 16 seeds x 3 levels (~40s)
make sim               # Integration sim: 1 seed (requires fossil)
make sim-full          # Integration sim: 16 seeds x 3 severities
make drivers           # Test all 3 SQLite drivers
```

## NATS Subject Namespace

```
fossil.<project-code>.igot    "I have this artifact" (UUID)
fossil.<project-code>.gimme   "I need this artifact" (UUID)
fossil.<project-code>.file    Artifact payload delivery
fossil.<project-code>.delta   Delta-compressed artifact
fossil.<project-code>.meta    Config, tickets, wiki changes
fossil.<project-code>.events  Commit notifications
```

## Dependencies

- `modernc.org/sqlite` — Pure Go SQLite (default driver)
- `github.com/nats-io/nats.go` — NATS + JetStream client
- `github.com/alecthomas/kong` — CLI framework
- `github.com/hexops/gotextdiff` — Unified diff output

## Reference

- [Fossil sync protocol](https://fossil-scm.org/home/doc/tip/www/sync.wiki)
- [Fossil delta format](https://fossil-scm.org/home/doc/tip/www/delta_format.wiki)
- [Fossil password authentication](https://fossil-scm.org/home/doc/tip/www/password.wiki)
