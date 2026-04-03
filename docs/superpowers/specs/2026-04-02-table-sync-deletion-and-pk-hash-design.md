# Table Sync: Row Deletion & PK Hash Fix

**Date**: 2026-04-02  
**Tickets**: CDG-168 (xdelete card), CDG-171 (PK hash coercion)  
**Branch**: `feature/table-sync-deletion`

## Problem

Two related issues in the remote schema sync feature:

1. **No deletion support** (CDG-168): Once a row is synced to extension tables (`x_<name>`), it persists forever across all peers. Tables are effectively append-only. This limits use cases like feature flags (can't remove a flag), peer registry (can't deregister a peer), etc.

2. **PK hash type coercion risk** (CDG-171): `PKHash()` uses `json.Marshal(pkValues)` to serialize primary key values before hashing. JSON round-trips coerce `int64` to `float64` via `json.Unmarshal` into `map[string]any`. For values >2^53, `json.Marshal` of `float64` loses precision, producing different hashes on sender vs receiver.

## Design Decisions

### Deletion: UV-style in-place tombstone

Following Fossil's established pattern for mutable synced data (UV files use `hash=NULL`, attachments use `src=NULL`), deleted rows remain in `x_<table>` with all non-PK, non-mtime value columns set to NULL.

**Convention**: A row where all value columns are NULL is a tombstone. This is a documented schema contract. Extension table schemas should have at least one value column that is meaningfully non-NULL for live rows.

**Why not a `_deleted` column**: Defensive engineering for a scenario (all-NULL as legitimate row state) that won't materialize in practice. Every current and likely future table has columns that are meaningfully non-NULL. Follow Fossil's precedent over speculative safety.

**Why not a separate `_sync_tombstones` table**: Fossil uses separate tracking tables only for admin/policy data (shun table). Mutable synced data uses in-place tombstones. Extension tables are mutable synced data.

### PK hash: Type-aware canonical normalization

Following Fossil's philosophy of canonical string representation at the storage boundary (ticket fields stored as TEXT regardless of semantic type), `PKHash` normalizes values to canonical strings based on declared column type before hashing. No JSON dependency in the hash path.

**Why not `json.Decoder.UseNumber()`**: Fixes the symptom (JSON coercion) but leaves `PKHash` fragile — still depends on `json.Marshal` rendering `int64` and `json.Number` identically. Type-aware normalization makes the hash self-contained and deterministic by construction.

## PK Hash Changes (CDG-171)

### New `PKHash` signature

```go
// Before:
func PKHash(pkValues map[string]any) string

// After:
func PKHash(pkCols []ColumnDef, pkValues map[string]any) string
```

### Canonical normalization rules

| Column type | Normalization | Example |
|-------------|--------------|---------|
| `"integer"` | `strconv.FormatInt(toInt64(v), 10)` | `42` → `"42"`, `float64(42)` → `"42"` |
| `"real"` | `strconv.FormatFloat(toFloat64(v), 'f', -1, 64)` | `3.14` → `"3.14"` |
| `"text"` | `v.(string)` | `"hello"` → `"hello"` |
| `"blob"` | `hex.EncodeToString(v.([]byte))` | `[]byte{0xDE,0xAD}` → `"dead"` |

### Hash computation

1. Sort PK column names lexicographically
2. For each column, normalize the value to canonical string
3. Concatenate as `name=normalizedValue` pairs joined by `\x00` (null byte separator)
4. SHA1 the result

### Callers to update

All callers already have access to `TableDef`:
- `client_tablesync.go`: `buildTableXIGotCards`, `extractPKColumns`
- `handler_tablesync.go`: `handleXRow`, `handleXIGot`, `sendXRow`
- `repo/tablesync.go`: `CatalogHash`

## xdelete Card (CDG-168)

### Wire protocol

New card type `CardXDelete = 24` in `card.go`.

```go
type XDeleteCard struct {
    Table   string // table name (without x_ prefix)
    PKHash  string // SHA1 of primary key values
    MTime   int64  // deletion timestamp
    PKData  []byte // JSON-encoded PK column values
}
```

Wire format: `xdelete TABLE PK_HASH MTIME SIZE\n<SIZE bytes of JSON PK data>\n` — same payload pattern as xrow, but the payload only contains PK column values (not all columns). The PK data is needed so receivers that have never seen the row can insert a tombstone, preventing them from requesting it via xgimme in future rounds.

### Tombstone storage

`DeleteXRow(d *db.DB, table string, def TableDef, pkHash string, mtime int64) error`:
- If row exists with lower mtime: UPDATE all non-PK, non-mtime value columns to NULL, set mtime to deletion timestamp
- If row exists with higher mtime: no-op (local is newer)
- If row doesn't exist: INSERT tombstone using PK values from `PKData`, all value columns NULL, mtime set. This prevents the peer from requesting the row via xgimme in future sync rounds.

### Conflict resolution

Extends existing `resolveXRowConflict()`:

| Incoming | Local state | Local mtime vs incoming | Result |
|----------|------------|------------------------|--------|
| xdelete | live row | incoming newer | apply tombstone |
| xdelete | live row | incoming older | ignore |
| xdelete | tombstone | incoming newer | update mtime |
| xdelete | tombstone | incoming older | ignore |
| xdelete | missing | — | insert tombstone (using PKData) |
| xrow | tombstone | incoming newer | resurrect (overwrite NULLs) |
| xrow | tombstone | incoming older | ignore |
| mtime tie | — | equal | incoming wins (existing behavior) |

### Catalog hash

Exclude tombstone rows from catalog hash computation. A tombstone is detected by checking that all value columns (non-PK, non-mtime, non-`_owner`) are NULL.

### Sync flow

**Client-side**:
- `buildTableXIGotCards()`: emits xigot for all rows including tombstones — remote needs to know about deletions
- New `buildTableXDeleteCards()`: emits xdelete for locally tombstoned rows queued in `xTableToSend`
- Receiving xrow for a tombstoned row resurrects it if mtime is newer (handled by existing conflict resolution)

**Server-side**:
- New `handleXDelete()` in `handleDataCard()` dispatch: looks up local row, applies mtime-wins, calls `DeleteXRow()`
- `handleXIGot()`: when server has tombstone and client's mtime is older, server emits xdelete (not xrow)
- `emitXIGotsForTable()`: tombstones naturally included since they remain in the table

**Convergence**: Same round-based convergence as today. Deletions propagate because tombstone rows participate in xigot exchange, and xdelete cards flow when a peer discovers the other hasn't deleted yet.

## Testing

### CDG-171 — PK hash tests

- **Unit** (`repo/tablesync_test.go`):
  - `PKHash` produces identical hashes for int64 vs float64 inputs of same logical value
  - Large integers (>2^53) that would diverge under old `json.Marshal` approach
  - Composite PK with mixed types (text + integer)

### CDG-168 — xdelete tests

- **xfer round-trip** (`xfer/`):
  - Encode/decode `XDeleteCard`, verify fidelity
  - Fuzz test: `parseXDelete` with garbage input (new network-facing parser)

- **Repo layer** (`repo/tablesync_test.go`):
  - `DeleteXRow` tombstones correctly (value cols NULL, mtime updated)
  - `DeleteXRow` with lower mtime than existing row is rejected
  - Resurrection: `UpsertXRow` overwrites tombstone when mtime is newer
  - Catalog hash excludes tombstones

- **Handler** (`sync/handler_tablesync_test.go`):
  - xdelete card processed correctly (mtime-wins)
  - Server emits xdelete when it has tombstone and client's xigot is older
  - Conflict: xdelete vs xrow with competing mtimes

- **DST** (`dst/tablesync_test.go`):
  - Multi-peer: peer A deletes row, peers B and C converge to tombstone
  - Delete-then-resurrect: peer A deletes, peer B updates with newer mtime, all converge to live row
  - Integer PK convergence: peers with large integer PKs reach identical catalog hashes

## Non-goals

- Tombstone garbage collection / TTL expiry
- Schema evolution (DROP COLUMN, RENAME)
- Bloom filter / Merkle optimization for large tables
- `_deleted` column or separate tombstone table
