# EdgeSync

Fossil NATS Sync — replace Fossil's HTTP sync with NATS messaging on leaf nodes.

## Architecture

- **Leaf Agent** (Go): daemon that reads/writes Fossil SQLite repo DB directly, publishes/subscribes artifacts via NATS JetStream
- **Bridge** (Go): translates between NATS messages and Fossil's HTTP /xfer card protocol. Master Fossil server is unmodified.

## Read-Only Directories

`fossil/` and `libfossil/` are upstream reference checkouts. NEVER create, edit, write, or delete any file inside these directories. They are read-only reference material for porting.

## Key Decisions

- Go for both components
- Pure Go SQLite via `modernc.org/sqlite` (no CGo)
- NATS + JetStream for messaging (`nats.go`)
- Fossil repo format is untouched — `fossil ui` works on any synced repo
- Master Fossil server is vanilla — bridge speaks HTTP /xfer to it

## Build & Run

```bash
go build ./cmd/leaf
go build ./cmd/bridge
go test ./...
```

## Project Structure

- `cmd/leaf/` — leaf agent entry point
- `cmd/bridge/` — bridge entry point
- `pkg/delta/` — Fossil delta codec (ported from C)
- `pkg/repo/` — Fossil SQLite repo DB operations
- `pkg/card/` — Xfer card protocol parser/encoder
- `pkg/msync/` — NATS sync messaging layer
- `pkg/hash/` — SHA1/SHA3 content addressing

## Reference Source

C reference implementations live in the repo checkouts:
- `fossil/src/delta.c` — delta algorithm (~800 lines, priority port target)
- `fossil/src/xfer.c` — sync protocol
- `libfossil/checkout/src/delta.c` — libfossil's delta port
- `libfossil/checkout/src/content.c` — artifact storage/retrieval
- `libfossil/checkout/src/xfer.c` — network sync (partial)
- `libfossil/checkout/src/deck.c` — manifest parsing
- `libfossil/checkout/src/db.c` — SQLite wrapper

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

## Phase Plan

1. Delta codec
2. Repo DB layer
3. Card protocol
4. Leaf agent
5. Bridge
6. Checkin support
