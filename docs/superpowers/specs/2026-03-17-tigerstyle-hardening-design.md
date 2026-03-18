# TigerStyle Hardening — go-libfossil

**Date:** 2026-03-17
**Branch:** feat/tigerstyle
**Scope:** All 20 go-libfossil packages (~13.4K lines)
**Goal:** Apply TigerStyle coding discipline — assertions, function splitting, error handling, bounds checks, naming — with zero new features.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Assertion aggressiveness | Full TigerStyle (every function) | Sync engine — silent corruption is worst-case |
| Postconditions | Named returns + defer on critical path; test-only elsewhere | Strongest guarantees where data integrity matters |
| Function splitting | Same-file private helpers | Co-location, Go convention |
| Delta naming | Keep C-aligned names + mapping comments | Cross-reference with `fossil/src/delta.c` |
| Non-ported naming | Full TigerStyle rename | No reference constraint |
| Stubs | Panic with intent | Crash loudly if called, impossible to silently swallow |

## 1. Assertions

### Preconditions (all public and private functions)

Every function gets precondition panics for programmer errors. These are NOT operating errors — a nil repo is a broken call site, not bad user input.

**Panic message format:** `"pkg.Function: description"`

```go
func Store(q Querier, data []byte, uuid string) (rid FslID, err error) {
    if q == nil { panic("blob.Store: q must not be nil") }
    if len(data) == 0 { panic("blob.Store: data must not be empty") }
    if uuid == "" { panic("blob.Store: uuid must not be empty") }
    // ...
}
```

### Postconditions (critical-path functions only)

Named returns + defer pattern for data integrity functions:

```go
func Store(q Querier, data []byte, uuid string) (rid FslID, err error) {
    if q == nil { panic("blob.Store: q must not be nil") }
    defer func() {
        if err == nil && rid <= 0 { panic("blob.Store: rid must be positive on success") }
    }()
    // ...
}
```

**Critical-path functions receiving postconditions:**

- `delta.Apply()`, `delta.Create()`
- `blob.Store()`, `blob.StoreDelta()`, `blob.StorePhantom()`, `blob.Load()`
- `blob.Compress()`, `blob.Decompress()`
- `content.Expand()`, `content.walkDeltaChain()`
- `manifest.Checkin()`, `manifest.GetManifest()`
- `sync.Sync()`, `sync.processResponse()`

### Assertion inventory by package

| Package | Functions needing preconditions | Functions needing postconditions |
|---------|-------------------------------|--------------------------------|
| delta | 9 | 2 (Apply, Create) |
| blob | 5 | 4 (Store, StoreDelta, StorePhantom, Load) |
| compress | 2 | 2 (Compress, Decompress) |
| hash | 4 | 0 |
| content | 4 | 2 (Expand, walkDeltaChain) |
| db | 3 (OpenWith, WithTx, SqlDB) | 0 |
| repo | 3 (Create, Open, Verify) | 0 |
| manifest | 4 | 2 (Checkin, GetManifest) |
| deck | 5 (Parse, Marshal, parseCard, encode, decode) | 0 |
| xfer | 4 (DecodeCard, EncodeCard, readPayload, parseLine) | 0 |
| sync | 6 (Sync, buildRequest, processResponse, etc.) | 2 (Sync, processResponse) |
| merge | 8 (Merge, merge3, FindCommonAncestor, etc.) | 0 |
| undo | 5 (Save, Undo, Redo, swapState, labelUpper) | 0 |
| stash | 7 (Save, Apply, Pop, List, Drop, Clear, EnsureTables) | 0 |
| bisect | 6 (NewSession, MarkGood, MarkBad, Skip, Next, Reset) | 0 |
| annotate | 4 (Annotate, loadFileAt, versionInfoFor, primaryParent) | 0 |
| path | 3 (Shortest, neighbors, reconstruct) | 0 |
| tag | 2 (AddTag, ensureTag) | 0 |
| simio | 3 (AdvanceTo, After, Buggify) | 0 |

**Total: ~87 preconditions + ~14 postconditions**

## 2. Function Splitting

10 functions exceed the 70-line hard limit. All split into same-file private helpers.

### stash.Save() — 160 lines → 3 functions

```
Save(ckout, repoDB, dir, comment)
  ├── snapshotVFile(tx, dir) → []fileRecord
  └── computeAndStoreDeltas(tx, repoDB, stashID, files)
```

### deck.Marshal() — 143 lines → 2 functions

```
Marshal(d)
  └── marshalCards(buf, d) — handles B/C/D/F/J/K/L/N/P/Q/R/T/U/W cards
```

### undo.swapState() — 141 lines → 4 functions

```
swapState(ckout, dir, label)
  ├── swapVFile(tx, label)
  ├── swapDiskFiles(tx, dir, label)
  └── swapVMerge(tx, label)
```

### manifest.Checkin() — 135 lines → 4 functions

```
Checkin(r, opts)
  ├── buildCheckinDeck(tx, opts, parentRid, parentUUID) → *deck.Deck
  ├── insertCheckinBlob(tx, deckBytes, uuid) → rid
  └── markLeafAndEvent(tx, rid, uuid, opts)
```

### deck.parseCard() — 107 lines → dispatcher + helpers

```
parseCard(d, cardType, line)
  ├── parseBCard(d, line)
  ├── parseFCard(d, line)
  ├── parseTCard(d, line)
  └── ... (one per card type)
```

### delta.Create() — 96 lines → 3 functions

```
Create(source, target)
  ├── buildHashTable(source) → table, nHash
  └── emitMatches(source, target, table, nHash) → output
```

### merge.merge3() — 93 lines → 3 functions

```
merge3(base, local, remote)
  ├── classifyHunks(baseDiff, localDiff, remoteDiff) → []hunk
  └── emitMergedLines(hunks) → (result, conflicts)
```

### sync.processResponse() — 91 lines → 2 functions

```
processResponse(msg)
  └── handleResponseCard(card) → error
```

### annotate.Annotate() — 82 lines → 2 functions

```
Annotate(r, opts)
  └── walkParentChain(r, startRid, lines, limit) → []Line
```

### delta.Apply() — 76 lines → 2 functions

```
Apply(source, delta)
  └── applyCopyInsert(reader, source, output) → error
```

## 3. Error Handling

### SQL exec errors (6 locations)

All `tx.Exec()` calls must check and propagate errors:

| Location | Current | Fix |
|----------|---------|-----|
| `manifest/manifest.go:151-154` | 4x `tx.Exec()` errors ignored | Check each, return on failure |
| `tag/tag.go:103-104` | 2x `tx.Exec()` errors ignored | Check each, return on failure |
| `sync/session.go:274` | `Exec("DELETE FROM unsent")` ignored | Check and return |
| `bisect/bisect.go:197` | `s.db.Exec()` ignored in Reset() | Check and return |

### File I/O errors (4 locations)

| Location | Current | Fix |
|----------|---------|-----|
| `repo/repo.go:30,36,42` | `os.Remove(path)` x3 ignored | Log via `fmt.Errorf` wrapping (cleanup best-effort, but don't silently drop) |
| `stash/stash.go:205` | `os.Remove()` ignored | Return error |

### Binary encoding (1 location)

| Location | Current | Fix |
|----------|---------|-----|
| `blob/compress.go:19` | `binary.Write()` ignored | Check and return error |

### Scan-in-loops (3 locations)

| Location | Current | Fix |
|----------|---------|-----|
| `manifest/log.go:65` | `rows.Scan()` ignored | Check, skip row on error |
| `merge/ancestor.go:78` | `rows.Scan()` ignored | Check, skip row on error |
| `merge/detect.go:24` | `rows.Scan()` ignored | Check, skip row on error |

### Other (4 locations)

| Location | Current | Fix |
|----------|---------|-----|
| `merge/resolve.go:61` | `filepath.Match()` error ignored | Check and propagate |
| `stash/stash.go:370` | `RowsAffected()` error discarded | Check error |
| `db/db.go:103` | `Rollback()` error ignored | Log rollback failure (can't return — already in error path) |
| `deck/parse.go:40` | `reader.Read()` error ignored | Check error |

### Stub replacement (1 location)

| Location | Current | Fix |
|----------|---------|-----|
| `sync/stubs.go:28-29` | `return fmt.Errorf("not yet implemented")` | `panic("sync.Clone: not implemented")` |

## 4. Bounds & Naming

### Bounds assertions (5 locations)

| Location | Fix |
|----------|-----|
| `deck/parse.go:16` | Assert `len(data) >= 35` before `data[:len(data)-35]` |
| `xfer/decode.go:82` | Assert `len(fields) > 0` before `fields[0]` |
| `delta/delta.go:101` | Assert `offset+cnt` fits in `int` before cast |
| `delta/delta.go:108` | Assert `r.pos+int(cnt)` doesn't overflow |
| `sync/client.go:316` | Assert UUID is valid hex, not just length check |

### Naming (ported code)

Add C-reference mapping comment at top of `delta.Create()` and `delta.Apply()`:

```go
// Variable naming follows fossil/src/delta.c for cross-reference:
//   nHash = NHASH (rolling hash window size)
//   ei    = (entry index into hash table)
//   ml    = (match length)
//   tPos  = iSrc (target position)
//   sOff  = iSrc (source offset)
```

### Naming (non-ported code)

| Location | Current | Rename |
|----------|---------|--------|
| `content/content.go` | `srcid` | `sourceID` |
| `manifest/log.go` | `mtimeRaw` | `mtimeScanned` |

### simclock fix

`simclock.AdvanceTo()` line 71: replace silent `return` with panic:

```go
if t.Before(c.now) { panic("simclock.AdvanceTo: cannot move time backwards") }
```

## 5. Testing Strategy

### Baseline preservation

All 20 packages must remain green. Run full suite after each package is modified.

### New assertion-trigger tests

For each package receiving precondition panics, add tests verifying panics fire:

```go
func TestStoreNilQuerier(t *testing.T) {
    defer func() {
        if r := recover(); r == nil {
            t.Fatal("expected panic on nil querier")
        }
    }()
    blob.Store(nil, []byte("data"), "abc123")
}
```

Estimated ~20-30 new test functions across all packages.

### DST sweep

Run `make dst-full` after all changes to verify no regressions in sync behavior.

### Sim integration

Run sim test to confirm end-to-end sync still converges.

## 6. What We're NOT Doing

- No new features
- No refactoring beyond TigerStyle requirements
- No file reorganization (same-file helpers only)
- No changes to existing test logic (only adding new assertion tests)
- No touching `fossil/` or `libfossil/` (read-only)
- No performance optimization
