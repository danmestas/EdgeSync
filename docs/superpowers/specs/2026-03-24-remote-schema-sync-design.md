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

Four new card types added to the xfer protocol. All use the same encoding conventions as existing cards: arguments are space-separated on a single line, with Fossil-encoding (`\s` for space, `\n` for newline, `\\` for backslash) applied to values that may contain special characters.

### Encoding conventions

**JSON payloads** (`schema` and `xrow` cards): Follow the `config` card pattern — the card header line includes a `SIZE` field, followed by `\n` and then `SIZE` bytes of raw JSON. This avoids needing to Fossil-encode the entire JSON blob.

**Primary key hash** (`pk_hash`): SHA1 of the JSON-serialized primary key values, with keys sorted lexicographically. For single-column PKs: `SHA1(json.Marshal({"peer_id":"leaf-01"}))`. For composite PKs: `SHA1(json.Marshal({"col1":"a","col2":"b"}))` with keys sorted.

**Catalog hash**: SHA1 of all rows' `pk_hash + " " + strconv.FormatInt(mtime, 10) + "\n"` concatenated, sorted by `pk_hash`. Matches UV's `ContentHash()` pattern.

### `schema` card

```
schema <table_name> <version> <hash> <mtime> <size>\n<json_def>
```

`json_def` is raw JSON (not Fossil-encoded), with length specified by `size`:

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
xrow <table_name> <pk_hash> <mtime> <size>\n<json_payload>
```

Full row data as raw JSON (length specified by `size`), following the `config` card pattern. Analogous to `uvfile`.

### Short-circuit optimization

Per-table catalog hash to skip exchange when tables are already in sync:

```
pragma xtable-hash <table_name> <catalog_hash>
```

If both sides match, skip `xigot`/`xgimme` for that table entirely. Catalog hash computation is defined in the "Encoding conventions" section above. Dispatched via `handlePragmaXTableHash(tableName, hash)` in `handler_tablesync.go`, called from `handleControlCard()` when `pragma.Name == "xtable-hash"`.

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
2. **Data cards** — `file`, `cfile`, `igot`, `gimme`, `xigot`, `xrow`, `xgimme`, UV cards (`uvigot`, `uvfile`, `uvgimme`)

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

Identity for enforcement comes from the `LoginCard.User` field (`h.loginUser`), which is already tracked by the handler. In leaf-to-leaf sync, both peers run `HandleSync` on their end of the connection — enforcement happens on the **receiving** side (the peer processing the incoming `xrow` card).

For `owner-write`, an `_owner TEXT` column is automatically injected into the extension table (alongside `mtime`). It is set to `loginUser` on first insert and immutable thereafter.

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
        if local != nil && local.Owner != "" && local.Owner != h.loginUser {
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
3. **After successful sync convergence** — update own peer registry row (`last_sync`, `repo_hash`) via `PostSyncHook` (only on success, not per-round).

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

V1 supports **append-only** schema evolution (ADD COLUMN only). Type changes and column removal require manual migration (drop table + re-introduce).

When a table needs new columns (v1 → v2):

1. User runs `edgesync schema alter peer_registry --add-column region:text`
2. Local `_sync_schema` version incremented, column added
3. Schema card with new version auto-propagates on next sync — receiving peers apply `ALTER TABLE` automatically when incoming version > local version
4. Receivers run `ALTER TABLE x_<name> ADD COLUMN ...`
5. Existing rows unaffected — new column is NULL until populated
6. Schema downgrades are not supported — once a peer receives v2, it stays at v2

## Security Considerations

- **Namespace protection**: `x_` prefix enforced. Schema cards targeting Fossil core tables rejected.
- **Schema introduction**: Only via explicit CLI action locally. During sync, peers accept schema cards from any authenticated peer (v1 trust model). A `_sync_schema.origin` column tracks `local` vs `remote` for future policy enforcement (e.g. admin-only schema introduction).
- **Conflict enforcement**: Server-side in `HandleSync` (real security). Client-side advisory only (UX). In leaf-to-leaf topology, both peers enforce on their receiving side.
- **SQL injection**: All table and column names validated by `validateTableName(name string) error` in `repo/tablesync.go` against `^[a-z_][a-z0-9_]*$` allowlist before any DDL or DML. JSON payloads always parameterized via `?` placeholders, never interpolated into SQL.
- **Scaling**: Linear `xigot` exchange is practical up to ~1000 rows per table. Beyond that, consider Bloom filter optimization (`pragma xtable-bloom`) as future work.

## Open Questions

- Should `xigot` exchange use a Bloom filter or Merkle summary at very high row counts (10k+)?
- How does this interact with browser WASM leaf (OPFS storage)? Likely transparent — same SQLite, same sync.
- Should schema removal propagate (drop table on remote peers) or only affect local?
- Rate limiting: max rows per table? Max tables per repo?
