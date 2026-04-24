# libfossil Core Library

Pure-Go reimplementation of Fossil's core operations. Standalone repo at `github.com/danmestas/libfossil` (public, v0.1.x). No CGo in the default build. Every operation must produce `.fossil` files that `fossil rebuild --verify` accepts.

## Repository & Module Structure

Extracted from EdgeSync monorepo (April 2026) via `git filter-repo` with history. Three sub-modules, lockstep versioned:

| Module | Purpose |
|---|---|
| `github.com/danmestas/libfossil` | Root — opaque handle API, CLI, tests |
| `github.com/danmestas/libfossil/db/driver/modernc` | Default SQLite driver |
| `github.com/danmestas/libfossil/db/driver/ncruces` | WASM-compatible SQLite driver |

Optional sub-module: `observer/otel/` — OTel observer implementation (separate go.mod, zero OTel deps in root).

Versioning: lockstep `v0.x` tags across all sub-modules. Tags: `v0.1.0`, `db/driver/modernc/v0.1.0`, `db/driver/ncruces/v0.1.0`.

## Encapsulation & Public API

All implementation packages live under `internal/` (blob, content, manifest, sync, xfer, etc.). The public API is an opaque `Repo` handle with methods — no direct DB/SQL access for consumers.

**Constructors:** `Open(path)`, `Create(path, user, rand)`, `Clone(ctx, url, path, opts)`

**Repo handle methods:** `Sync()`, `HandleSync()`, `HandleSyncWithOpts()`, `XferHandler()`, `Commit()`, `Timeline()`, `ListFiles()`, `Tag()`, `UVWrite/Read/List()`, `CreateUser()`, `Config()`

**Checkout handle:** `CreateCheckout()`, `OpenCheckout()` — working directory management

**Extension points:**
- `Transport` interface: `RoundTrip(ctx, []byte) ([]byte, error)` — pluggable sync transport
- `SyncObserver` / `CheckoutObserver` interfaces — lifecycle hooks for telemetry (nil = nop)
- Driver registration: `db.Register(DriverConfig)` via import side-effect

**CLI embedding:** `cli/` package provides 38 kong command structs (`cli.RepoCmd`). EdgeSync embeds this and adds only NATS/bridge/iroh commands on top.

**Standalone binary:** `cmd/fossil/` — Fossil-compatible CLI built from `cli.RepoCmd`.

## Package Architecture

All packages below are under `internal/` (not importable by consumers). Public packages: `db/`, `db/driver/*`, `simio/`, `testutil/`, `cli/`.

Key dependency chain:

```
hash, delta (no deps)
blob -> db, hash, delta
content -> db, blob, delta
deck (stdlib only, no DB)
xfer -> deck
manifest -> deck, db, blob, content, hash
repo -> db, blob, content, hash
sync -> repo, manifest, xfer, blob, content
```

No circular imports. `deck` and `xfer` are intentionally database-free. `internal/fsltype` breaks import cycles (root types re-exported).

## Context Decomposition

C libfossil uses a monolithic `fsl_cx` context. Go decomposes it:

| C concept | Go equivalent |
|-----------|---------------|
| `fsl_cx` (monolithic) | `repo.Repo` (top-level handle, owns DB) |
| `fsl_cx.dbMem` | `db.DB` (connection + statement cache) |
| `fsl_cx` error state | Standard Go error returns |
| `fsl_cx` transactions | `repo.Repo.WithTx(func(tx) error)` |

## Key Types

| Type | Definition | Notes |
|------|-----------|-------|
| `FslID` | `int64` | Widened from C's `int32` -- SQLite returns `int64` natively |
| `FslSize` | `int64` | Signed, not unsigned -- `-1` sentinel for phantom blobs (`PhantomSize`) |
| `db.Querier` | interface (`Exec`, `QueryRow`, `Query`) | Satisfied by both `*db.DB` and `*db.Tx` |
| `deck.Deck` | struct with fields A-Z | All 20 manifest card types; `ArtifactType` enum discriminates |
| `xfer.Card` | interface (`Type() CardType`) | 19 card types + `UnknownCard` for forward compat |
| `xfer.Message` | `struct { Cards []Card }` | Full xfer request/response with zlib encode/decode |

## Blob Format

Fossil stores blobs with a 4-byte big-endian uncompressed size prefix followed by zlib-compressed data:

```
[4-byte BE uint32: uncompressed size][zlib data]
```

`blob.Compress()` and `blob.Decompress()` handle this format. `blob.Store` auto-marks new blobs as `unclustered` (matching C's `content_put_ex`). `unsent` is a caller concern.

**Verify-before-commit:** `Store()` and `StoreDelta()` re-read, decompress (and for deltas, re-apply), and re-hash after INSERT — unconditionally, matching Fossil's `content_put_pk()`. Catches compression or delta bugs before they persist. BUGGIFY sites in `Decompress` (2% truncation) and `Expand` (1% byte-flip) exercise this in DST.

## Xfer Wire Format

HTTP POST to `/xfer`, Content-Type `application/x-fossil`, body is zlib-compressed (RFC 1950). Newline-separated cards.

- **file/cfile** payloads: header line + `\n` + SIZE bytes, NO trailing newline
- **config** payloads: header line + `\n` + SIZE bytes + trailing `\n`
- **cfile**: zlib within zlib (card-level compression inside message-level compression)
- Unknown card types produce `UnknownCard` (not an error)

## Manifest Format

Cards emitted in strict ASCII order: `A B C D E F G H I J K L M N P Q R T U W Z`. Z-card (MD5 of everything before it) always last. Manifest UUID = SHA1/SHA3 of complete bytes including Z-card.

- **Baseline manifest**: no B-card, F-cards list all files
- **Delta manifest**: B-card points to baseline, F-cards list only changes (no UUID = deleted)
- **R-card**: MD5 hash over sorted `filename + size + content` of resolved file set
- **D-card format**: `YYYY-MM-DDTHH:MM:SS.SSS` (always 3-digit millis)
- **Fossil encoding**: `\s` = space, `\n` = newline, `\\` = backslash (applies to C, F-name, L, U, H, J-name fields)

## SQLite Driver Selection

Two drivers, selected via import (registration API pattern):

| Driver | Module | CGo | WASM | Import |
|--------|--------|-----|------|--------|
| modernc (default) | `db/driver/modernc/` | No | No | `_ ".../db/driver/modernc"` |
| ncruces | `db/driver/ncruces/` | No | Yes | `_ ".../db/driver/ncruces"` |

Each driver is a separate Go sub-module with its own `go.mod`. The `db` package has zero driver dependencies — drivers self-register via `db.Register(DriverConfig)` in `init()`. Consumers import exactly one driver; no build tags needed in production code. Test files use `internal/testdriver` which selects the driver via build tags (`test_ncruces`).

Default pragmas applied to every connection: `journal_mode=WAL`, `busy_timeout=5000`, `foreign_keys=ON`. DSN syntax varies per driver; each driver's `BuildDSN` handles it.

```bash
GOWORK=off go test ./...                              # modernc (default)
GOWORK=off go test -tags test_ncruces ./...           # ncruces
```

## TigerStyle Conventions

Applied across all 20 libfossil packages. Zero new features -- hardening only.

### Assertions

- **Preconditions**: every public/private function panics on programmer errors (nil args, empty required params). Format: `"pkg.Function: description"`.
- **Postconditions**: named returns + `defer` on critical-path functions only (delta, blob, content, manifest, sync). Example: `if err == nil && rid <= 0 { panic(...) }`.
- ~87 preconditions, ~18 postconditions total.

### Function Length

Hard limit: 70 lines. 10 functions were split into same-file private helpers (largest: `stash.Save` 160 -> 3 functions, `deck.Marshal` 143 -> 2 functions).

### Error Handling

- All `tx.Exec()` return values checked (6 locations fixed)
- All `rows.Scan()` in loops checked (4 locations)
- `binary.Write()` checked in `blob/compress.go`
- Stubs panic with intent (`panic("sync.Clone: not implemented")`) instead of returning errors

### Naming

- Ported code (delta): keeps C-aligned names with mapping comments for cross-reference
- Non-ported code: full TigerStyle rename (e.g., `srcid` -> `sourceID`)

## Diff Engine

`diff/` package — Myers O(nd) algorithm, line-based, pure `[]byte` in / `string` out. No repo dependencies.

- `Unified(a, b []byte, opts Options) string` — standard unified diff output (`@@ -a,b +c,d @@`)
- `Stat(a, b []byte) DiffStat` — insertions, deletions, binary detection
- `Options{ContextLines, SrcName, DstName}` — 0 context = no context lines
- Binary detection via null byte scan; returns empty diff for binary inputs
- `\r\n` normalized to `\n` before comparison
- Side-by-side deferred until a consumer needs it (architecture supports adding formatters)

## File History (finfo)

`manifest.FileHistory()` — walks the `mlink` table to return a file's change history across checkins, ordered by date descending.

- **Query**: `mlink JOIN event JOIN blob` by `fnid`, with `COALESCE` for nullable `pid`/`pfnid` columns
- **Action classification**: `fid=0` → deleted, `pid=0` → added, `pfnid != fnid` → renamed, `fid=pid` → unchanged (filtered out), else → modified
- **Unchanged filtering**: `insertMlinks` (in `Checkin`) creates rows for all files in a commit, not just changes. `FileHistory` skips entries where `fid == pid` so consumers only see actual modifications.
- **`FileAt(r, checkinRID, path)`**: convenience function resolving a file's blob RID at a specific checkin via `mlink`. Enables diff-between-versions (`content.Expand` both sides, pipe through `diff.Unified`).
- **Rename tracking**: `pfnid` column is in the schema but not yet populated by `Checkin` or `crosslink`. When wired, `FileHistory` will automatically surface renames via the existing `pfnid != fnid` check.

## Content Cache

`content.Cache` — concurrency-safe LRU cache for `content.Expand` results, keyed by `FslID`. Eliminates redundant delta-chain walks (SQLite queries + zlib decompress + delta apply per chain link).

### Design Decisions

- **Memory-bounded, not entry-bounded**: Capped by total bytes of cached content. A few large blobs won't blow the budget since Fossil blobs range from tiny manifests (~200 bytes) to multi-MB files.
- **LRU eviction**: Sync access patterns have strong temporal locality — the same manifests and file blobs are expanded repeatedly across sync rounds and crosslink passes.
- **Nil-safe receiver**: `(*Cache)(nil).Expand(q, rid)` falls through to raw `content.Expand`. This lets all integration points use `cache.Expand()` unconditionally — nil means no caching, no branching needed at callsites.
- **Copy semantics**: Both cache storage and cache returns are copies. Callers can't corrupt cached data.
- **Opt-in via `SyncOpts.ContentCache` / `HandleOpts.ContentCache`**: Existing code passes nil (zero-value) and gets identical uncached behavior. No forced migration.
- **DST stays uncached**: Simulation tests use nil cache to maximize fault surface — BUGGIFY's 1% byte-flip in `Expand` would become persistent if cached, changing the failure mode.
- **Oversized blobs**: A single blob larger than `maxSize` is inserted then immediately evicted. The data is still returned correctly to the caller.

### Integration Points

| Layer | Where | How |
|-------|-------|-----|
| `content/cache.go` | Core type | `NewCache(maxBytes)`, `Expand`, `Invalidate`, `Clear`, `Stats` |
| `sync/session.go` | `SyncOpts.ContentCache` | Client sync uses cache for `loadFileCard` (push) and `resolveFileContent` (delta base) |
| `sync/handler.go` | `HandleOpts.ContentCache` | Server handler uses cache for gimme responses and clone batch sending |
| `leaf/agent/` | `Config.ContentCacheSize` | Agent creates 32 MiB cache by default (negative disables). Shared across sync rounds for agent lifetime. |

### Performance

Cache hits: ~550ns / 1 alloc (copy only). Uncached 5-deep delta chain: ~65µs / 224 allocs. **~120x speedup on hits.**

## Constraints

- Performance: compute ops within 3x of C libfossil, I/O ops within 5x
- No operation regresses beyond 10% from its own prior Go baseline
- `fossil rebuild --verify` must pass on every Go-created repo
- Race detector clean (`go test -race ./...`)
- Timestamps stored as Julian day numbers (float64)
- SHA3-256 UUIDs (64-char) for Fossil 2.0+, SHA1 (40-char) for legacy
