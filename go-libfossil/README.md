# go-libfossil

Pure Go library for reading, writing, and syncing Fossil repositories.

## Quick Start

```go
import (
    "github.com/dmestas/edgesync/go-libfossil/repo"
    "github.com/dmestas/edgesync/go-libfossil/sync"
    "github.com/dmestas/edgesync/go-libfossil/simio"
    _ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
)

// Create a new repository.
r, err := repo.Create("my.fossil", "admin", simio.CryptoRand{})

// Open an existing repository.
r, err := repo.Open("my.fossil")
defer r.Close()

// Sync with a remote Fossil server.
result, err := sync.Sync(ctx, r, &sync.HTTPTransport{URL: "http://host/repo"}, sync.SyncOpts{
    Push: true,
    Pull: true,
})

// Clone a remote repository.
r, result, err := sync.Clone(ctx, "clone.fossil", &sync.HTTPTransport{URL: "http://host/repo"}, sync.CloneOpts{
    User:     "admin",
    Password: "secret",
})
```

## Architecture

```
sync/       ← Client sync loop, server handler, Transport interface, Observer
repo/       ← Open/Create/Verify .fossil files
content/    ← Delta-chain expansion (walks blob → delta → root)
blob/       ← Content-addressed blob storage (4-byte prefix + zlib)
xfer/       ← Wire codec for Fossil's xfer card protocol (20 card types)
delta/      ← Fossil delta encoder/decoder (port of delta.c)
merge/      ← Three-way merge with pluggable strategies
deck/       ← Manifest/control-artifact parser
hash/       ← SHA1 and SHA3-256 content addressing
uv/         ← Unversioned file sync (wiki, forum, attachments)
simio/      ← Clock, Rand, Storage interfaces for deterministic testing
db/         ← SQLite layer with pluggable drivers (modernc, ncruces, mattn)
manifest/   ← Checkin creation and crosslinking
tag/        ← Tag read/write on artifacts
testutil/   ← Shared test helpers (fossil CLI wrapper)
```

## SQLite Drivers

Select a driver by importing its package (or set `EDGESYNC_SQLITE_DRIVER`):

| Driver | Import | Notes |
|--------|--------|-------|
| modernc (default) | `db/driver/modernc` | Pure Go, no cgo |
| ncruces | `db/driver/ncruces` | WASM-based, works on all platforms |
| mattn | `db/driver/mattn` | cgo, uses system libsqlite3 |

## Observability

go-libfossil has **zero OpenTelemetry dependencies**. Observability is injected via the `sync.Observer` interface — 7 lifecycle callbacks for session start/end, round metrics, errors, and server-side handling. Pass nil for zero-cost no-ops.

The `leaf/telemetry` package (outside this library) implements Observer with full OTel traces, metrics, and structured logs.

## Deterministic Testing

All I/O flows through `simio.Env` (Clock, Rand, Storage). Pass `simio.SimEnv(seed)` to get deterministic time, seeded randomness, and in-memory storage for reproducible tests.

## Build

```bash
go build -buildvcs=false ./...   # -buildvcs=false required (dual VCS: git + fossil)
go test ./...
```
