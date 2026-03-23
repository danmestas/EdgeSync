# Spec: blob.Store Auto-Mark Unclustered & Single-Pass Handler

**Date:** 2026-03-22
**Branch:** fix/checkin-mark-file-blobs-unsent (PR #19)
**Status:** Draft

## Problem

PR #19 fixes a real bug (file blobs not pushed after checkin) but deviates from Fossil C in two ways:

1. `blob.Store` does not auto-mark `unclustered`, forcing every caller to remember. Fossil's `content_put_ex` (content.c:633) marks every new blob as unclustered automatically.
2. The handler reorders card processing (file/cfile first, then igot), but Fossil's `page_xfer` (xfer.c:1349) processes cards in wire order with a single pass.

## Fossil C Reference

### content_put_ex (content.c:629-634)

Every new blob inserted via `content_put_ex` is automatically marked unclustered:

```c
/* Add the element to the unclustered table if has never been
** previously seen. */
if( markAsUnclustered ){
    db_multi_exec("INSERT OR IGNORE INTO unclustered VALUES(%d)", rid);
}
```

`markAsUnclustered` is set to 1 only for brand-new blobs (no existing row with that hash). It stays 0 for already-existing blobs (including phantom-to-real dephantomization) and is cleared for private blobs. This confirms `blob.Store`'s early-return-on-Exists behavior is correct -- only genuinely new inserts get marked.

### checkin_cmd (checkin.c:3124-3133)

File blobs: `content_put` handles unclustered; checkin calls `db_add_unsent` explicitly:

```c
nrid = content_put(&content);        /* auto-marks unclustered */
...
db_add_unsent(nrid);                 /* caller marks unsent */
```

Manifest blob (checkin.c:3219-3223): same pattern:

```c
nvid = content_put(&manifest);       /* auto-marks unclustered */
db_add_unsent(nvid);                 /* caller marks unsent */
```

### page_xfer (xfer.c:1349+)

Single `while(blob_line(...))` loop. Cards processed in wire order: file, cfile, gimme, igot, etc. No reordering.

### Two distinct concerns

- **unclustered** = internal bookkeeping ("not yet assigned to a cluster"). Marked by `content_put_ex` for every new blob.
- **unsent** = sync concern ("needs to be pushed to remotes"). Marked explicitly by callers (`db_add_unsent`) for locally-created artifacts only.

## Design

### Change 1: blob.Store auto-marks unclustered

**File:** `go-libfossil/blob/blob.go`

`blob.Store` inserts `INSERT OR IGNORE INTO unclustered(rid) VALUES(?)` after a successful blob insert. This matches `content_put_ex`. The caller (`db.Querier`) must support `Exec`, which it already does (`*db.DB` and `*db.Tx` both satisfy this).

No change for `blob.Exists` returning early (blob already exists) -- only new inserts get marked, matching Fossil where `markAsUnclustered` is only true for new blobs.

`blob.StoreDelta` gets the same treatment -- it also calls `content_put_ex` under the hood in Fossil.

`blob.StorePhantom` already has its own unclustered logic and does not use `blob.Store`, so no change needed.

### Change 2: Remove redundant unclustered inserts from callers

Once `blob.Store` handles unclustered, remove the manual `INSERT INTO unclustered` from:

- **`manifest.Checkin`** (manifest.go:78) -- remove unclustered insert for file blobs (keep unsent)
- **`manifest.markLeafAndEvent`** (manifest.go:226) -- remove unclustered insert for manifest blob (keep unsent)
- **`tag.AddTag`** (tag.go:109) -- remove unclustered insert for control artifact (keep unsent)
- **`storeReceivedFile`** (client.go:501) -- remove unclustered re-insert on the exists-early-return path only (blob was already marked unclustered on first insertion). Keep the unclustered insert on the new-blob raw SQL path (line 519) since this path bypasses `blob.Store`.

### Change 3: Revert handler to single-pass wire-order processing

**File:** `go-libfossil/sync/handler.go`

Revert the three-pass card processing (file first, then remaining) back to a single `for` loop over `req.Cards`, processing each card via `handleDataCard` in wire order. This matches `page_xfer`.

The file-before-igot reordering was introduced to prevent "spurious gimmes" when an igot arrives before its file blob in the same request. In Fossil, this is a non-issue because igot cards (from `unclustered` -- pre-existing blobs the client announces) and file cards (from `pendingSend` -- blobs the server previously requested) reference disjoint blob sets within a single request. A blob won't appear as both an igot and a file card in the same round.

Note: the client actually sends igots before file cards (client.go:57-70, step 4 before step 5), but this doesn't matter because the server's igot handler checks blob existence in the server's own database, not in the current request. The server sends gimme only if it doesn't already have the blob locally.

### storeReceivedFile detail

`storeReceivedFile` (client.go:499-521) bypasses `blob.Store` -- it does raw SQL with a pre-verified UUID. This is necessary because `blob.Store` hardcodes `hash.SHA1(content)` (blob.go:25), but received files may have SHA3-256 UUIDs (64-char). Calling `blob.Store` would recompute a SHA1 hash, storing blobs with the wrong UUID.

Two paths in `storeReceivedFile`:

1. **Blob exists** (line 500-502): Currently does `INSERT OR IGNORE INTO unclustered`. This is a no-op since the blob was already marked unclustered on first insertion. **Remove it.**

2. **New blob** (line 504-519): Does its own compress+insert+unclustered via raw SQL. This path must keep its own unclustered INSERT since it bypasses `blob.Store`. **No change.**

## Files Changed

| File | Change |
|------|--------|
| `go-libfossil/blob/blob.go` | `Store` and `StoreDelta` auto-mark unclustered |
| `go-libfossil/manifest/manifest.go` | Remove unclustered inserts from Checkin + markLeafAndEvent (keep unsent) |
| `go-libfossil/tag/tag.go` | Remove unclustered insert from AddTag (keep unsent) |
| `go-libfossil/sync/client.go` | Remove exists-path unclustered re-insert in `storeReceivedFile` (keep new-blob path) |
| `go-libfossil/sync/handler.go` | Revert to single-pass wire-order card processing |
| `sim/seed.go` | Remove redundant unclustered insert (now handled by `blob.Store`) |
| `dst/mock_fossil.go` | Remove redundant unclustered insert |
| `dst/e2e_test.go` | Remove redundant unclustered inserts (2 sites) |
| `dst/scenario_test.go` | Remove redundant unclustered insert |

## Testing

- All existing unit tests pass (blob.Store callers get unclustered for free)
- `storeReceivedFile` tests continue to work (same behavior, cleaner internals)
- Handler tests pass with single-pass processing
- DST + sim tests verify end-to-end sync still converges
- Pre-commit hook covers all of the above

## Non-Goals

- Changing `blob.StorePhantom` (does NOT currently mark unclustered -- a known deviation from Fossil C's `content_new` which does. Deferred because phantoms are created during sync/clone where unclustered is managed by the sync protocol.)
- Adding `unsent` to `blob.Store` (unsent is a caller concern, matching Fossil)
- Changing client card ordering (clients already send files before igots)
