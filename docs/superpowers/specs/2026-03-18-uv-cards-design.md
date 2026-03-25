# Unversioned File Sync (UV Cards) Design

Date: 2026-03-18
Status: Approved
Scope: Full bidirectional UV sync, Fossil 2.28 wire-compatible

## Goal

Implement `uvfile`, `uvigot`, and `uvgimme` card handling in the sync engine so EdgeSync leaves can sync unversioned files (forum posts, wiki pages, attachments) bidirectionally with Fossil servers and with each other. Must pass wire-level interop tests against real Fossil 2.28.

## Prior Art

The xfer codec already encodes/decodes all three UV card types (Phase C). This spec covers everything above the codec: schema, storage, conflict resolution, handler dispatch, client session, DST, and sim integration.

Reference implementation: `fossil/src/xfer.c` and `fossil/src/unversioned.c` (local checkout).

## 1. Schema & Storage

### Package: `go-libfossil/uv/`

New package for all unversioned file operations.

### Table (exact Fossil match)

```sql
CREATE TABLE IF NOT EXISTS unversioned(
  uvid INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT UNIQUE,
  rcvid INTEGER,
  mtime DATETIME,        -- seconds since 1970
  hash TEXT,             -- NULL = deletion marker
  sz INTEGER,            -- uncompressed size
  encoding INT,          -- 0=plaintext, 1=zlib
  content BLOB           -- NULL if deleted or oversized
);
```

### Functions

- `EnsureSchema(db)` — creates table if not exists
- `Write(db, name, content, mtime)` — hash content (SHA1 or SHA3 per repo config), compress with zlib, store compressed if size <= 80% of original, else store plaintext. REPLACE INTO.
- `Delete(db, name, mtime)` — tombstone: hash=NULL, sz=0, encoding=0, content=NULL
- `Read(db, name) -> (content, mtime, hash, error)` — decompress on read if encoding=1
- `List(db) -> []Entry` — all rows including tombstones
- `Status(localMtime, localHash, remoteMtime, remoteHash) -> int` — pure function, no DB
- `ContentHash(db) -> string` — SHA1 of sorted catalog, cached as config `uv-hash`
- `InvalidateHash(db)` — delete config `uv-hash`

## 2. Status Function

Pure function. Exact port of Fossil's `unversioned_status()`.

```
func Status(localMtime int64, localHash string, remoteMtime int64, remoteHash string) int
```

Conventions:
- `localHash = ""` means no local row -> return 0
- `"-"` means deletion marker (in either position)

Algorithm:

```
if localHash == "":  return 0

mtimeCmp = sign(localMtime - remoteMtime)
hashCmp  = strcmp(localHash, remoteHash)

if hashCmp == 0:
    return 3 + mtimeCmp     // 2, 3, or 4

if mtimeCmp < 0 || (mtimeCmp == 0 && hashCmp < 0):
    return 1                // pull

return 5                    // push
```

Return codes:

| Code | Meaning | Action |
|------|---------|--------|
| 0 | Not present locally | Pull |
| 1 | Different hash, remote newer (or same mtime, local hash < remote hash) | Pull |
| 2 | Same hash, remote mtime older | Pull mtime only |
| 3 | Identical | No action |
| 4 | Same hash, remote mtime newer | Push mtime only |
| 5 | Different hash, local newer (or same mtime, local hash > remote hash) | Push |

## 3. Catalog Hash

Exact port of Fossil's `unversioned_content_hash()`.

Algorithm:
1. Query: `SELECT name, datetime(mtime,'unixepoch'), hash FROM unversioned WHERE hash IS NOT NULL ORDER BY name`
2. For each row: feed `"name datetime hash\n"` into running SHA1
3. Cache result in config table as `uv-hash`
4. Empty table -> SHA1 of empty string = `da39a3ee5e6b4b0d3255bfef95601890afd80709`

Cache invalidation: any Write/Delete/accept-uvfile calls `InvalidateHash()`.

The `datetime(mtime,'unixepoch')` format is `YYYY-MM-DD HH:MM:SS` — must match Fossil exactly.

Always SHA1, even in SHA3-256 repos. Fossil's `unversioned_content_hash()` hardcodes `sha1sum_finish()`. The catalog hash format string is `"%s %s %s\n"` (space-separated, newline-terminated).

## 4. Handler UV Dispatch (Server-Side)

Extend `sync/handler.go`. The `pragma uv-hash` card is received during the control-card pass but triggers data generation (sending `uvigot` catalog). Dispatch it from `handleControlCard` into a new `handlePragmaUVHash` method.

Add `uvCatalogSent bool` to handler state to prevent duplicate catalog sends per exchange.

### pragma uv-hash HASH

1. If `uvCatalogSent` -> skip (already sent this exchange)
2. `uv.EnsureSchema(db)`
3. Compute local `uv.ContentHash(db)`
4. If match -> no response
5. If differ -> send `pragma uv-push-ok` (or `uv-pull-only` without write permission), then `uvigot` for every row (tombstones use hash="-", sz=0). Set `uvCatalogSent=true`.

Note: permission checking for UV write is deferred, consistent with the existing handler's approach where all logins are accepted.

### uvigot NAME MTIME HASH SIZE (from client)

1. `uv.Status()` comparing client's file against local
2. Status 0, 1 -> send `uvgimme NAME`
3. Status 2 -> update local mtime, invalidate hash
4. Status 3 -> no action
5. Status 4, 5 -> send `uvfile` for local version

### uvgimme NAME (from client)

1. Validate filename (no path separators, no special characters)
2. Look up in `unversioned` table
3. Send `uvfile` with content
4. If response buffer >= mxSend -> send `uvigot` instead (retry next cycle)

### uvfile NAME MTIME HASH SIZE FLAGS (from client)

1. Validate hash matches content (if content present)
2. Double-check `uv.Status()` — reject if status >= 3
3. Deletion (hash="-") -> apply tombstone
4. Status 2 -> update mtime only
5. Otherwise -> `uv.Write()`, compress at 80% threshold
6. Invalidate hash cache

### uvfile FLAGS bitmap

| Value | Meaning | Content payload? |
|-------|---------|-----------------|
| 0 | Normal file with content | Yes |
| 1 (0x0001) | Deletion marker (hash="-", sz=0) | No |
| 4 (0x0004) | Content omitted (mtime-only or size limit) | No |

Combined mask: `flags & 0x0005 != 0` means no content follows.

### BUGGIFY sites

- Drop a `uvfile` card before storing (lost write)
- Corrupt `uvigot` size field (wire corruption, caught by validation)

## 5. Client UV Sync (Session-Side)

### New session state

```go
uvHashSent    bool              // sent pragma uv-hash this sync?
uvPushOK      bool              // server sent pragma uv-push-ok
uvPullOnly    bool              // server sent pragma uv-pull-only
uvToSend      map[string]bool   // true=full content, false=mtime-only
```

### SyncOpts extension

```go
type SyncOpts struct {
    // ... existing ...
    UV bool  // sync unversioned files
}
```

### Initialization

When `UV: true`, pre-populate `uvToSend` with all local non-tombstone UV files (full content). This matches Fossil's `uv_tosend` temp table approach. Incoming `uvigot` cards then remove or downgrade entries. Files that exist only on the client (server never sent `uvigot` for them) remain in `uvToSend` and get pushed — this is how client-originated UV files propagate.

### Building request

1. First round: if `UV` and not `uvHashSent`, emit `pragma uv-hash HASH`. Set `uvHashSent=true`.
2. Subsequent rounds (only if `uvPushOK`): for each `uvToSend` entry, send `uvfile` (flags=0 for content, flags=4 for mtime-only). Respect mxSend.
3. Track `nUvGimmeSent`.

### Processing response

- `pragma uv-push-ok` -> `uvPushOK=true`
- `pragma uv-pull-only` -> `uvPullOnly=true`
- `uvigot NAME MTIME HASH SIZE`:
  - Status 0, 1: hash != "-" -> send `uvgimme`, delete local row if exists (no-op for status 0). hash == "-" and status 1 -> apply tombstone.
  - Status 2: update local mtime
  - Status 3: remove from `uvToSend`
  - Status 4: mark `uvToSend[name]=false` (mtime-only)
  - Status 5: mark `uvToSend[name]=true` (full content, if `uvPushOK`)
  - Status >= 4 and `uvPullOnly` -> warn, remove from `uvToSend`
- `uvfile NAME MTIME HASH SIZE FLAGS`:
  - Validate hash if content present
  - Double-check `uv.Status()`, reject if >= 3
  - Store/tombstone/mtime-only as appropriate
  - Invalidate hash, increment `nUvFileRcvd`
- `uvgimme NAME`:
  - Send `uvfile` with full content from local table

### Loop continuation

```
nUvGimmeSent > 0 && (nUvFileRcvd > 0 || nCycle < 3)
```

Matches Fossil's continuation logic.

## 6. DST Scenarios & Invariants

### New invariants (`dst/invariants.go`)

- `CheckUVIntegrity(repo)` — for each row: if hash != NULL, verify hash matches SHA1/SHA3 of decompressed content, verify sz matches decompressed size
- `CheckUVConvergence(repos...)` — all repos have identical `uv.ContentHash()`. Spot-check: same names, mtimes, hashes.

### Scenarios (`dst/scenario_test.go`)

1. **TestUVCleanSync** — 3 UV files on upstream, 2 empty leaves. Sync. Assert convergence.
2. **TestUVBidirectional** — Leaf A has `wiki/page1`, upstream has `wiki/page2`. Sync. Assert both everywhere.
3. **TestUVConflictMtimeWins** — Same file, different content, upstream newer mtime. Assert upstream wins.
4. **TestUVConflictHashTiebreaker** — Same file, same mtime, different content. Assert lexically larger hash wins consistently (smaller hash side pulls from larger).
5. **TestUVDeletion** — Upstream tombstones a file. Leaf has it. Sync. Assert tombstone propagates.
6. **TestUVDeletionRevival** — Deleted upstream (mtime=100), re-created on leaf (mtime=200). Assert leaf wins.
7. **TestUVPartitionHeal** — Partition, independent UV mutations, heal. Assert convergence.
8. **TestUVMtimeOnlyUpdate** — Same content, different mtime. Assert mtime converges without content retransmit (flags=4).
9. **TestUVContentOmitted** — Large file exceeds mxSend. Assert content-omitted triggers retry, eventually converges.
10. **TestUVCatalogHashSkip** — Identical sides. Assert no uvigot sent (short-circuit).

### PeerNetwork

Pass `UV: true` in SyncOpts. No structural changes — `HandleSyncWithOpts` dispatches UV cards once handler is extended.

### BUGGIFY

Existing `BuggifyChecker` interface — new handler BUGGIFY sites fire under DST seeds.

## 7. Sim Integration (Real Fossil)

Tests in `sim/`, against Fossil 2.28.

1. **TestSimUVSyncPull** — `fossil uv add` on server, EdgeSync leaf syncs, assert `unversioned` table matches.
2. **TestSimUVSyncPush** — Leaf has UV files, syncs to Fossil server, `fossil uv list` verifies arrival.
3. **TestSimUVSyncBidirectional** — Different files on each side, sync, assert convergence.
4. **TestSimUVCatalogHashCompat** — Known UV files via `fossil uv add`, compare `uv.ContentHash()` against `fossil uv hash` output.
5. **TestSimUVDeletion** — `fossil uv rm`, sync, assert tombstone. Reverse: leaf deletes, sync back, Fossil sees deletion.
6. **TestSimUVRoundTrip** — Fossil creates -> EdgeSync pulls -> modifies -> pushes back -> Fossil verifies.

Fossil CLI commands: `fossil uv add/rm/list/hash`, `fossil sync --uv`.

## Implementation Order (TDD)

Red-green cycle, bottom-up with DST driving integration:

1. Unit tests for `uv.Status()` — 12+ cases — RED
2. Implement `Status()` — GREEN
3. Unit tests for `uv.ContentHash()` — RED -> GREEN
4. Schema + Write/Delete/Read helpers — test-first
5. DST scenario TestUVCleanSync — RED
6. Handler UV dispatch + client UV builders — GREEN
7. Remaining DST scenarios — RED -> GREEN incrementally
8. Sim integration tests against real Fossil — RED -> GREEN
