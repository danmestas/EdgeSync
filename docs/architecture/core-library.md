# go-libfossil Core Library

Pure-Go reimplementation of Fossil's core operations. Lives at `go-libfossil/` as its own Go module. No CGo in the default build. Every operation must produce `.fossil` files that `fossil rebuild --verify` accepts.

## Package Architecture

See the package table in `CLAUDE.md` for the full listing. Key dependency chain:

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

No circular imports. `deck` and `xfer` are intentionally database-free.

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

Three drivers, selected via import (registration API pattern):

| Driver | Module | CGo | WASM | Import |
|--------|--------|-----|------|--------|
| modernc (default) | `db/driver/modernc/` | No | No | `_ ".../db/driver/modernc"` |
| ncruces | `db/driver/ncruces/` | No | Yes | `_ ".../db/driver/ncruces"` |
| mattn | `db/driver/mattn/` | Yes | No | `_ ".../db/driver/mattn"` |

Each driver is a separate Go sub-module with its own `go.mod`. The `db` package has zero driver dependencies -- drivers self-register via `db.Register(DriverConfig)` in `init()`. Consumers import exactly one driver; no build tags needed in production code.

Default pragmas applied to every connection: `journal_mode=WAL`, `busy_timeout=5000`, `foreign_keys=ON`. DSN syntax varies per driver; each driver's `BuildDSN` handles it.

```bash
go test ./go-libfossil/...                           # modernc
go test -tags test_ncruces ./go-libfossil/...        # ncruces
CGO_ENABLED=1 go test -tags test_mattn ./go-libfossil/...  # mattn
```

## TigerStyle Conventions

Applied across all 20 go-libfossil packages. Zero new features -- hardening only.

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

## Constraints

- Performance: compute ops within 3x of C libfossil, I/O ops within 5x
- No operation regresses beyond 10% from its own prior Go baseline
- `fossil rebuild --verify` must pass on every Go-created repo
- Race detector clean (`go test -race ./...`)
- Timestamps stored as Julian day numbers (float64)
- SHA3-256 UUIDs (64-char) for Fossil 2.0+, SHA1 (40-char) for legacy
