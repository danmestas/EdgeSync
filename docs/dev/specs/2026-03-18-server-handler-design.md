# ServerHandler Design Spec

## Goal

Add a server-side sync handler to go-libfossil that lets a leaf agent accept clone and sync requests from other leaves or stock Fossil clients, over swappable transports (HTTP, NATS, libp2p).

## Scope

**In scope (this spec):**
- Peer sync: two populated repos exchange artifacts bidirectionally
- Clone: a fresh repo clones from a serving leaf, paginated via `clone_seqno`
- Config sync: `reqconfig` → `config` cards for project-code, project-name, server-code
- HTTP listener: stock `fossil clone`/`fossil sync` can talk to our handler
- NATS listener: leaf-to-leaf sync without a bridge
- libp2p stub: reserved, not implemented

**Out of scope (documented for future):**
- Unversioned file cards (`uvfile`, `uvigot`, `uvgimme`) for forum/wiki/chat
- Cookie-keyed sessions for igot optimization
- In-memory TTL sessions for libp2p connection identity
- `private` card handling for private branches
- Full config sync (ticket settings, skins, backoffice)
- Login verification beyond basic accept/reject

## Architecture

```
                    ┌─────────────────────────────────┐
                    │        go-libfossil/sync/        │
                    │                                  │
                    │  HandleFunc signature:           │
                    │    func(ctx, *repo.Repo,         │
                    │         *xfer.Message)           │
                    │      → (*xfer.Message, error)    │
                    │                                  │
                    │  HandleSync() — implementation   │
                    │  ServeHTTP() — HTTP listener     │
                    └──────────┬──────────────────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
    ┌─────────▼──────┐ ┌──────▼───────┐ ┌──────▼────────┐
    │  ServeHTTP()   │ │ ServeNATS()  │ │ ServeP2P()    │
    │  sync package  │ │ leaf/agent   │ │ leaf/agent    │
    │                │ │              │ │ (stub)        │
    └────────────────┘ └──────────────┘ └───────────────┘

Leaf Agent
├── pollLoop()        ← existing client role (calls Sync())
├── ServeNATS()       ← server over NATS (leaf-to-leaf)
├── ServeHTTP()       ← server over HTTP (fossil clients connect directly)
└── ServeP2P()        ← stub for libp2p (future)
```

### Key Decisions

- **Handler callback pattern** — `HandleFunc` signature mirrors `http.HandlerFunc`. Transport listeners call it; sync logic is transport-agnostic.
- **Stateless per-round** — No session tracking. The client drives convergence via `Sync()` or `Clone()`. The handler answers each round honestly based on current repo state.
- **Clone atomicity owned by client** — The `Clone()` implementation (client-side, in progress) handles rollback/cleanup on partial clone. The handler just serves blobs.
- **Clone pagination** — Uses `clone_seqno` card. Client sends its last-seen rid, server responds with next batch. No server state needed.
- **Shared blob I/O** — Extract `handleFileCard`/`loadFileCard` from `client.go` into shared helpers. Both client session and server handler reuse them.
- **TigerStyle** — All library code follows TigerStyle discipline (assertions, preconditions, function size limits).

### Future Enhancements (documented, not implemented)

- **Cookie sessions** — Server generates cookie on first round, client echoes it back. Reduces redundant igot lists on subsequent rounds.
- **In-memory TTL sessions** — For libp2p where connection identity exists at the transport level. Sessions keyed by peer ID with timeout.
- **UV cards** — `uvfile`/`uvigot`/`uvgimme` for unversioned content: forum posts, wiki pages, chat messages.
- **Private cards** — `private` card for private branch artifact exchange.
- **Full config sync** — Ticket configuration, skins, and other backoffice settings via `config`/`reqconfig`.

## Handler: Card-by-Card Behavior

### Implemented Cards

| Incoming Card | Server Response | Notes |
|---------------|----------------|-------|
| `login` | Validate if auth configured, reject if invalid | Empty user config = allow anonymous |
| `pragma` | Handle `client-version`, ignore unknown | Unknown pragmas are not errors |
| `pull` | Validate project/server code, emit `igot` for all blobs | Queries blob table for non-phantom UUIDs |
| `push` | Validate project/server code, enable accepting files | Sets flag; files without preceding push rejected |
| `igot` | If we don't have that UUID, emit `gimme` | Mirror of client processResponse igot handling |
| `gimme` | Load blob, emit `file` card | Uses shared loadBlob helper |
| `file`/`cfile` | Store blob in repo | Uses shared storeBlob helper |
| `clone` | Emit batch of file cards from clone_seqno cursor | Paginated: `WHERE rid > ? ORDER BY rid LIMIT ?` |
| `clone_seqno` | Read client's cursor position | Drives clone pagination |
| `reqconfig` | Emit `config` cards for requested keys | project-code, project-name, server-code |

### Future Cards (not implemented)

| Card | Purpose | Phase |
|------|---------|-------|
| `uvfile`/`uvigot`/`uvgimme` | Unversioned file sync (forum, wiki, chat) | Forum/wiki |
| `cookie` | Session optimization | Performance |
| `config` (full) | Ticket/skin settings | Backoffice |
| `private` | Private branch artifacts | Private branches |

## File Layout

### New files

| File | Contents |
|------|----------|
| `sync/blob_io.go` | Shared blob store/load/list helpers extracted from client.go |
| `sync/handler.go` | `HandleFunc` type, `HandleSync()` implementation |
| `sync/serve_http.go` | `ServeHTTP()` — HTTP /xfer listener |
| `sync/handler_test.go` | Unit tests for HandleSync card handling |
| `sync/serve_http_test.go` | Integration test with real `fossil clone`/`fossil sync` |
| `leaf/agent/serve_nats.go` | `ServeNATS()` — NATS request/reply listener |
| `leaf/agent/serve_p2p.go` | `ServeP2P()` — stub, panics with "not implemented" |

### Modified files

| File | Change |
|------|--------|
| `sync/client.go` | Extract blob I/O into blob_io.go, session methods become thin wrappers with buggify/budget |
| `leaf/agent/config.go` | Add `ServeHTTP string`, `ServeNATS bool` fields |
| `leaf/agent/agent.go` | Start listeners alongside pollLoop in `Start()` |

## Shared Blob I/O Helpers (`sync/blob_io.go`)

Extracted from existing `client.go` methods:

```go
// storeBlob validates, compresses, and stores a received blob.
// Handles both full content and delta-compressed payloads.
// Used by both client session (processResponse) and server handler.
func storeBlob(db *db.DB, uuid, deltaSrc string, payload []byte) error

// loadBlob loads a blob by UUID and returns it as a FileCard.
// Expands delta chains via content.Expand.
func loadBlob(db *db.DB, uuid string) (*xfer.FileCard, int, error)

// listBlobUUIDs returns all non-phantom blob UUIDs in the repo.
// Used for igot lists in both pull responses and push announcements.
func listBlobUUIDs(db *db.DB) ([]string, error)

// listBlobsFromRID returns a paginated batch of blobs for clone.
// Returns file cards, the last rid in the batch, and whether more remain.
func listBlobsFromRID(db *db.DB, afterRID, limit int) ([]xfer.FileCard, int, bool, error)
```

## Transport Listeners

### ServeHTTP (`sync/serve_http.go`)

```go
// ServeHTTP starts an HTTP server that accepts Fossil xfer requests.
// Blocks until ctx is cancelled. Stock fossil clone/sync can connect.
func ServeHTTP(ctx context.Context, addr string, r *repo.Repo, h HandleFunc) error
```

- Accepts POST with `Content-Type: application/x-fossil`
- Decodes request with `xfer.Decode`, calls handler, encodes response with `msg.Encode()`
- Inverse of existing `HTTPTransport.Exchange()`

### ServeNATS (`leaf/agent/serve_nats.go`)

```go
// ServeNATS subscribes to the project sync subject and dispatches
// incoming requests to the handler. Blocks until ctx is cancelled.
func ServeNATS(ctx context.Context, nc *nats.Conn, subject string, r *repo.Repo, h sync.HandleFunc) error
```

- Subscribes to `<prefix>.<project-code>.sync`
- Receives request, calls handler, publishes response on reply subject
- Inverse of existing `NATSTransport`

### ServeP2P (`leaf/agent/serve_p2p.go`)

Stub only. Panics with "not implemented — planned for libp2p phase."

## Leaf Agent Integration

The agent's `Config` gains server fields:

```go
ServeHTTP  string  // listen address, e.g. ":8080". Empty = disabled.
ServeNATS  bool    // serve on NATS sync subject. Default false.
```

`Start()` launches listeners alongside the existing poll loop:

```go
func (a *Agent) Start() error {
    // existing: start poll loop (client role)
    go a.pollLoop(ctx)

    // new: start server listeners
    if a.config.ServeHTTP != "" {
        go sync.ServeHTTP(ctx, a.config.ServeHTTP, a.repo, sync.HandleSync)
    }
    if a.config.ServeNATS {
        go ServeNATS(ctx, a.conn, subject, a.repo, sync.HandleSync)
    }
    return nil
}
```

`Stop()` cancels the context, which shuts down all listeners and the poll loop.

## Testing Strategy

### Unit tests (`sync/handler_test.go`)

Build xfer.Message requests by hand, pass to HandleSync with a temp repo, assert response cards:
- pull → igot list matches repo contents
- push + file → blob stored correctly
- igot for unknown UUID → gimme response
- gimme for known UUID → file card with correct content
- clone + clone_seqno pagination → correct batches
- reqconfig → config values returned
- file with bad hash → error card
- file without preceding push → rejected

### Integration test (`sync/serve_http_test.go`)

Start ServeHTTP on localhost, use real `fossil clone` to clone from it, verify:
- Cloned repo opens in `fossil ui`
- All blobs present and valid
- `fossil sync` against the handler converges with no errors

### DST integration (follow-on)

Add a mode where two agents sync through HandleSync directly — one agent's Exchange() calls the other's HandleSync(). Tests leaf-to-leaf under fault injection without real transport.

Sim harness extension: leaf → NATS → leaf (both serving) instead of leaf → NATS → bridge → Fossil HTTP.
