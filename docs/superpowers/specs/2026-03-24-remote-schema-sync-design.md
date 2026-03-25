# Remote Schema Sync — Design Spec

**Date**: 2026-03-24
**Status**: Draft
**Consumer**: Peer Registry (first), ReBAC / feature flags / config (future)

## Overview

A generic table sync primitive for EdgeSync that allows arbitrary SQLite tables to be defined, deployed, and kept in sync across all peers via the xfer protocol. Schemas are introduced explicitly via CLI, propagate through the sync network as schema cards, and peer tables stay synchronized incrementally using `xigot`/`xgimme`/`xrow` card exchange — the same pattern UV sync already uses for unversioned files.

The peer registry is the first consumer: each leaf agent advertises its identity, capabilities, and state in a synced `x_peer_registry` table.

## Motivation

UV sync already implements incremental per-row sync with mtime-wins conflict resolution — but only for one table (`unversioned`). This design extracts that pattern into a reusable primitive, enabling any structured data to sync alongside repo content with zero additional transport code.

## Data Model

### `_sync_schema` table (repo DB)

Stores registered synced table definitions. Mutable (not content-addressed). Syncs between peers using schema cards with mtime-wins resolution.

```sql
CREATE TABLE _sync_schema (
    table_name TEXT PRIMARY KEY,
    version    INTEGER NOT NULL DEFAULT 1,
    columns    TEXT NOT NULL,    -- JSON array of column definitions
    conflict   TEXT NOT NULL,    -- conflict resolution strategy
    mtime      INTEGER NOT NULL, -- for schema-level mtime-wins sync
    hash       TEXT NOT NULL     -- SHA1 of canonical schema definition
);
```

### Extension tables

All synced tables are prefixed with `x_` to protect Fossil core tables (`blob`, `delta`, `event`, `unversioned`, etc.). An `mtime` column is injected automatically by the primitive.

Example — peer registry:

```sql
CREATE TABLE x_peer_registry (
    peer_id      TEXT PRIMARY KEY,
    last_sync    INTEGER,
    repo_hash    TEXT,
    version      TEXT,
    platform     TEXT,
    capabilities TEXT,
    nats_subject TEXT,
    addr         TEXT,
    mtime        INTEGER NOT NULL  -- injected by primitive
);
```

### Conflict Strategies

Declared per-table in `_sync_schema.conflict`:

| Strategy | Behavior | Use Case |
|----------|----------|----------|
| `self-write` | Peer can only write rows where PK matches its identity. Enforced at sync. | Peer registry |
| `mtime-wins` | Any peer can write any row. Last mtime wins. | Config, flags |
| `owner-write` | Row has an `_owner` field. Only that owner can update it. Ownership assigned on first write. | Shared resources |

## Wire Protocol

Four new card types added to the xfer protocol.

### `schema` card

```
schema <table_name> <version> <hash> <mtime> <json_def>
```

`json_def` is Fossil-encoded JSON:

```json
{
  "columns": [
    {"name": "peer_id", "type": "text", "pk": true},
    {"name": "last_sync", "type": "int"},
    {"name": "repo_hash", "type": "text"},
    {"name": "version", "type": "text"},
    {"name": "platform", "type": "text"},
    {"name": "capabilities", "type": "text"},
    {"name": "nats_subject", "type": "text"},
    {"name": "addr", "type": "text"}
  ],
  "conflict": "self-write"
}
```

Processed in the **control card phase** (alongside `login`, `push`, `pull`). Receiver runs `CREATE TABLE IF NOT EXISTS x_<name>` and registers in `_sync_schema`.

### `xigot` card

```
xigot <table_name> <pk_hash> <mtime>
```

"I have this row at this version." Sent for every row in every registered synced table. Analogous to `uvigot`.

### `xgimme` card

```
xgimme <table_name> <pk_hash>
```

"Send me this row." Emitted when the receiver has a stale or missing row. Analogous to `uvgimme`.

### `xrow` card

```
xrow <table_name> <pk_hash> <mtime> <json_payload>
```

Full row data as a JSON object. Analogous to `uvfile`.

### Short-circuit optimization

Per-table catalog hash to skip exchange when tables are already in sync:

```
pragma xtable-hash <table_name> <catalog_hash>
```

If both sides match, skip `xigot`/`xgimme` for that table entirely. Catalog hash is computed as SHA1 of all `(pk_hash, mtime)` pairs sorted by pk_hash.

### Sync round flow

```
Client                              Server
  |                                   |
  |-- schema (if introducing) ------->|  control phase
  |-- pragma xtable-hash pr <hash> -->|
  |-- xigot pr pk1 1711300000 ------->|  data phase
  |-- xigot pr pk2 1711300100 ------->|
  |                                   |
  |<--- xgimme pr pk2 ----------------|
  |<--- xigot pr pk3 1711300200 ------|
  |                                   |
  |-- xrow pr pk2 1711300100 {...} -->|  next round
  |-- xgimme pr pk3 ----------------->|
  |                                   |
  |<--- xrow pr pk3 1711300200 {...} -|  convergence
```

## Processing Pipeline

### Handler phases

1. **Control cards** — `login`, `push`, `pull`, `clone`, `pragma`, `schema`
2. **Data cards** — `file`, `cfile`, `igot`, `gimme`, UV cards, `xigot`, `xgimme`, `xrow`

Schema cards in the control phase guarantee tables exist before data arrives.

### Handler state

```go
type handler struct {
    // ... existing fields ...

    syncedTables map[string]*syncedTableMeta  // loaded from _sync_schema on init
    xrowsSent    int                           // for observer
    xrowsRecvd   int                           // for observer
}

type syncedTableMeta struct {
    Name     string
    Version  int
    Columns  []ColumnDef
    Conflict string
    Hash     string  // catalog hash of all rows
}
```

### Card handling

- **`schema`** — upsert into `_sync_schema`, `CREATE TABLE IF NOT EXISTS x_<name>`, register in `syncedTables`
- **`pragma xtable-hash`** — compare against local catalog hash. If match, skip that table's `xigot` emission.
- **`xigot`** — compare `(pk_hash, mtime)` against local row. If local is missing or older, emit `xgimme`. If local is newer, emit `xrow`.
- **`xgimme`** — look up row by `pk_hash`, emit `xrow`.
- **`xrow`** — validate conflict strategy, then upsert row.

### Conflict enforcement at `xrow` receipt

```go
func (h *handler) handleXRow(table *syncedTableMeta, card *XRowCard) error {
    switch table.Conflict {
    case "self-write":
        if card.PKHash != hashPK(h.loginUser) {
            return fmt.Errorf("self-write violation: %s cannot write row %s",
                h.loginUser, card.PKHash)
        }
    case "mtime-wins":
        local, _ := h.lookupRow(table.Name, card.PKHash)
        if local != nil && local.Mtime >= card.Mtime {
            return nil // local is newer, discard
        }
    case "owner-write":
        local, _ := h.lookupRow(table.Name, card.PKHash)
        if local != nil && local.Owner != "" && local.Owner != h.senderID {
            return fmt.Errorf("owner-write violation: row owned by %s", local.Owner)
        }
    }
    return h.upsertRow(table.Name, card)
}
```

## Client Side

### Leaf agent integration

1. **On startup** — load `_sync_schema` from local repo, populate own `x_peer_registry` row.
2. **Each sync round** — include `schema`, `pragma xtable-hash`, and `xigot` cards alongside existing cards.
3. **After sync** — update own peer registry row (`last_sync`, `repo_hash`).

### Advisory policy checks

```go
type TableSyncPolicy interface {
    CanWrite(table string, pkHash string, senderID string) error
}
```

Client checks locally before building `xrow` cards. Not security — UX only. Prevents wasted round trips for rows the server will reject.

### CLI commands

```bash
edgesync schema add peer_registry \
    --columns "peer_id:text:pk,last_sync:int,repo_hash:text,version:text,..." \
    --conflict self-write

edgesync schema list                # show all registered synced tables
edgesync schema show peer_registry  # show columns, version, conflict strategy
edgesync schema remove peer_registry
```

### Peer registry auto-seed

```go
func (a *Agent) seedPeerRegistry() {
    a.repo.UpsertXRow("peer_registry", map[string]any{
        "peer_id":      a.config.PeerID,
        "last_sync":    time.Now().Unix(),
        "version":      buildVersion,
        "platform":     runtime.GOOS + "/" + runtime.GOARCH,
        "capabilities": strings.Join(a.capabilities(), ","),
        "nats_subject": a.config.NATSSubject,
        "addr":         a.config.ServeHTTPAddr,
    })
}
```

## Package Layout

| Location | Purpose |
|----------|---------|
| `go-libfossil/sync/tablesync.go` | Core primitive: `syncedTableMeta`, catalog hash, row lookup/upsert |
| `go-libfossil/sync/handler_tablesync.go` | Handler-side card processing (`schema`, `xigot`, `xgimme`, `xrow`) |
| `go-libfossil/sync/client_tablesync.go` | Client-side card building (emit `xigot`, handle responses) |
| `go-libfossil/xfer/card_tablesync.go` | Card types: `SchemaCard`, `XIgotCard`, `XGimmeCard`, `XRowCard` |
| `go-libfossil/xfer/encode.go` | Encode additions for new cards |
| `go-libfossil/xfer/decode.go` | Decode additions for new cards |
| `go-libfossil/repo/tablesync.go` | DB operations: create extension table, upsert/query rows, schema CRUD |
| `leaf/agent/peer_registry.go` | Auto-seed, post-sync update |
| `cmd/edgesync/schema.go` | CLI subcommands |

## Observer Integration

Two new methods on the `Observer` interface:

```go
type Observer interface {
    // ... existing methods ...
    TableSyncStarted(table string, localRows int)
    TableSyncCompleted(table string, sent int, received int)
}
```

`OTelObserver` emits spans and metrics per table. `nopObserver` stays zero-cost.

## Testing

### DST coverage

Deterministic simulation tests in `dst/`:

- Schema propagation across peers
- Row convergence under network partitions
- Self-write enforcement rejection
- mtime-wins conflict resolution
- Catalog hash short-circuit (no exchange when in sync)
- BUGGIFY sites: corrupt `xrow` payload, drop `xigot` cards, stale mtime

### Integration tests

Sim tests in `sim/` with real NATS:

- End-to-end schema deployment via CLI → sync → table appears on remote
- Peer registry populated after agent startup and first sync
- Multi-peer convergence of registry data

## Schema Evolution

When a table needs new columns (v1 → v2):

1. User runs `edgesync schema alter peer_registry --add-column region:text`
2. Local `_sync_schema` version incremented, column added
3. Schema card with new version propagates on next sync
4. Receivers compare version: if incoming > local, run `ALTER TABLE x_<name> ADD COLUMN ...`
5. Existing rows unaffected — new column is NULL until populated

## Security Considerations

- **Namespace protection**: `x_` prefix enforced. Schema cards targeting Fossil core tables rejected.
- **Schema introduction**: Only via explicit CLI action. Peers accept schema cards from any authenticated peer (future: restrict to admin role).
- **Conflict enforcement**: Server-side in `HandleSync`. Client-side advisory only.
- **SQL injection**: Table and column names validated against `[a-z_][a-z0-9_]*` allowlist. JSON payloads parameterized, never interpolated.

## Open Questions

- Should `xigot` exchange use a Bloom filter or Merkle summary at very high row counts (10k+)?
- How does this interact with browser WASM leaf (OPFS storage)? Likely transparent — same SQLite, same sync.
- Should schema removal propagate (drop table on remote peers) or only affect local?
- Rate limiting: max rows per table? Max tables per repo?
