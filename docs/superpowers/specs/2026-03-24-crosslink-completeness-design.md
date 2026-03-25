# Crosslink Completeness Design Spec

**Date:** 2026-03-24
**Status:** Draft
**Related:** CDG-165 (clustering), Fossil manifest.c:2336-2875

## Problem

`manifest.Crosslink` currently handles checkin manifests and control artifacts but ignores wiki, ticket, technote, attachment, forum, and cluster artifacts. Synced repos contain these blobs but the metadata tables (`event`, `attachment`, `cherrypick`, `forumpost`, etc.) are not populated, making `fossil open` after EdgeSync clone/sync produce incomplete results.

Additionally, `sync.Sync()` does not auto-crosslink — callers must invoke `Crosslink` manually or via `PostSyncHook`. Phantom blobs that receive content mid-sync are never re-crosslinked.

## Goals

- Full parity with Fossil's `manifest_crosslink` for all 8 artifact types
- Two-pass architecture matching Fossil's deferred processing model
- Dephantomize hook matching Fossil's `after_dephantomize`
- Auto-crosslink after `sync.Sync()` convergence
- Backlink extraction explicitly deferred to a follow-on ticket

## Non-Goals

- Search indexing (`search_doc_touch`) — Fossil-specific full-text search
- TH1 hook scripts (`xfer_commit_code`, `xfer_ticket_code`)
- Time fudge adjustments for near-simultaneous checkins (cosmetic, can add later)
- Backlink extraction (`backlink_extract`, `backlink_wiki_refresh`)
- Delta compression of prior wiki/event versions (`content_deltify`) — space optimization, not correctness; follow-on ticket

## Design

### Two-Pass Architecture

Restructure `Crosslink` from single-pass to Fossil's two-pass model:

**Pass 1 — Crosslink artifacts into tables:**

Scan all uncrosslinked blobs (see Discovery Query section below). Parse each blob, dispatch by `deck.ArtifactType` to a handler function. Tag insertion and `tag_propagate_all` happen immediately per artifact in Pass 1, matching Fossil's `manifest_crosslink` where `tag_propagate_all(parentid)` is called at line 2472 within each artifact's processing. Collect deferred work:

- `pendingXlink []pendingItem` — wiki backlinks and ticket rebuilds to process after all artifacts are linked

**Pass 2 — Deferred processing:**

- Process `pendingXlink` entries (wiki backlink refresh, ticket entry rebuild)
- TAG_PARENT reparenting if applicable (Fossil's `manifest_crosslink_end` lines 2047-2057)

This matches Fossil's `manifest_crosslink_begin` / `manifest_crosslink` / `manifest_crosslink_end` pattern. Note: `tag_propagate_all` is NOT deferred — it runs in Pass 1 immediately after tag insertion for each artifact. Only wiki backlinks and ticket rebuilds are truly deferred.

### Discovery Query

The current query (`NOT EXISTS (SELECT 1 FROM event e WHERE e.objid = b.rid)`) will miss clusters (which don't get event rows) and cause redundant reprocessing. Replace with a multi-table check:

```sql
SELECT b.rid, b.uuid FROM blob b
WHERE b.size >= 0
  AND NOT EXISTS (SELECT 1 FROM event e WHERE e.objid = b.rid)
  AND NOT EXISTS (SELECT 1 FROM tagxref tx WHERE tx.srcid = b.rid)
  AND NOT EXISTS (SELECT 1 FROM forumpost fp WHERE fp.fpid = b.rid)
  AND NOT EXISTS (SELECT 1 FROM attachment a WHERE a.attachid = b.rid)
```

This covers all artifact types: checkins/wiki/ticket/event → event table; control → tagxref.srcid; cluster → tagxref (cluster tag); forum → forumpost; attachment → attachment table. The query is only used for rebuild/catchup; during normal sync, dephantomize handles incremental crosslinking.

### Per-Type Handlers

All handlers are unexported functions in `crosslink.go`.

#### `crosslinkCheckin` (existing, extended)

Current behavior preserved: event/plink/leaf/mlink + inline T-cards.

Extensions:
- **Cherrypick:** Populate `cherrypick(parentid, childid, isExclude)` from `deck.Q` cards. Matches Fossil lines 2380-2390.
- **baseid in plink:** Store baseline manifest RID in `plink.baseid` for delta manifests (B-card). Currently passes NULL.
- **tag_propagate_all:** Call `tag.PropagateAll` on primary parent ID immediately after tag insertion (matching Fossil line 2472).

#### `crosslinkWiki` (new)

Matches Fossil lines 2475-2516.

1. Insert plink for wiki version chain via `addFWTPlink` helper (which calls `tag.PropagateAll` immediately)
2. Create `wiki-<title>` singleton tag with content length as value
3. Determine comment prefix: `-` (empty/delete), `+` (new), `:` (edit with prior version)
4. Insert event row: `type='w', mtime, objid=rid, user, comment='<prefix><title>'`
5. Queue `pendingItem{Type: 'w', ID: title}` for deferred backlink refresh

#### `crosslinkTicket` (new)

Matches Fossil lines 2596-2631.

1. Create `tkt-<uuid>` singleton tag
2. Queue `pendingItem{Type: 't', ID: ticketUUID}` for deferred ticket rebuild
3. Update attachment event comments targeting this ticket UUID

**Ticket rebuild (deferred):** In Pass 2, for each pending ticket UUID, reconstruct the ticket's current state from all ticket-change artifacts (type J) for that UUID, ordered by mtime. This populates/updates a `ticket` table row. Note: the `ticket` table uses a dynamic schema defined by Fossil's ticketing configuration. For initial implementation, ticket rebuild inserts a minimal event record; full ticket table reconstruction is deferred to a follow-on ticket if needed.

#### `crosslinkEvent` (new)

Matches Fossil lines 2517-2595.

1. Insert plink for technote version chain via `addFWTPlink`
2. Create `event-<eventid>` singleton tag with content length as value
3. Handle subsequent-version visibility: if a later version already exists, skip event row insertion (only latest version gets the event entry)
4. If no subsequent version: insert event row `type='e'` with `bgcolor` from tagxref
5. If prior version exists with no subsequent: delete stale event rows for this event tag
6. Update attachment event comments targeting this event UUID

#### `crosslinkAttachment` (new)

Matches Fossil lines 2632-2704.

1. Insert into `attachment(attachid, mtime, src, target, filename, comment, user)`
2. Update `isLatest` flag: `SET isLatest = (mtime == (SELECT max(mtime) FROM attachment WHERE target=? AND filename=?))`
3. Detect target type via tag lookup:
   - Hash-like target + `tkt-<target>` tag exists → type='t'
   - Hash-like target + `event-<target>` tag exists → type='e'
   - Otherwise → type='w' (wiki, default)
4. Generate descriptive comment: "Add attachment [src] to <type> [target]" or "Delete attachment filename from <type> [target]"
5. Insert event row with detected type

#### `crosslinkCluster` (new)

Matches Fossil lines 2431-2443.

1. Apply `cluster` singleton tag (tagid=7, pre-seeded in schema)
2. For each M-card UUID in the cluster: resolve to rid, delete from `unclustered`
3. Ties into CDG-165 for cluster generation

#### `crosslinkForum` (new)

Matches Fossil lines 2810-2875.

1. Ensure `forumpost` table exists (schema addition required — see below)
2. Insert plink for forum post version chain via `addFWTPlink`
3. Resolve thread references: `froot` from `deck.G` (or self if thread starter), `fprev` from `deck.P[0]`, `firt` from `deck.I`
4. Insert into `forumpost(fpid, froot, fprev, firt, fmtime)`
5. If thread starter (`firt==0`):
   - Determine comment type: "Edit" (if fprev) or "Post" (if new)
   - Insert event row `type='f'` with comment `"<type>: <title>"` using `deck.H` as title
   - If this is the most recent edit, update all event comments for the same thread root with the new title
6. If reply (`firt!=0`):
   - Get title from thread root's event comment
   - Determine comment type: "Delete reply" (empty body), "Edit reply" (fprev), or "Reply" (new)
   - Insert event row `type='f'` with comment `"<type>: <title>"`

**Deck field mappings for forum:** `deck.G` = thread root UUID (zThreadRoot), `deck.H` = thread title (zThreadTitle), `deck.I` = in-reply-to UUID (zInReplyTo). These are already parsed by `deck.Parse`.

#### `crosslinkControl` (existing, extended)

Current T-card and tag propagation behavior preserved.

Extension: Create event row with `type='g'` and a generated comment describing the tag operations, matching Fossil lines 2705-2808. The comment summarizes branch moves, tag additions/cancellations, bgcolor changes, etc.

### Shared Helper: `addFWTPlink`

Matches Fossil's `manifest_add_fwt_plink` (lines 2287-2307).

Used by wiki, forum, and event types. For each parent UUID in the deck:
1. Resolve to rid
2. Insert `plink(pid, cid, isprim, mtime, baseid=NULL)`
3. If primary parent exists, call `tag.PropagateAll(parentID)` immediately (not deferred)

### New Function: `tag.PropagateAll`

New exported function in `go-libfossil/tag/propagate.go` matching Fossil's `tag_propagate_all` (tag.c:118).

Queries all propagating tags (`tagtype=2`) on a given rid and calls `propagate()` for each. Used by `crosslinkCheckin` (after tag insertion) and `addFWTPlink` (after plink insertion for wiki/forum/event).

### Dephantomize Hook

Matches Fossil's `after_dephantomize` in content.c:389-456.

#### `AfterDephantomize(q db.Querier, rid FslID)`

Called when a phantom blob receives real content.

1. Load blob content, parse and crosslink it (call per-artifact handler)
2. Check `orphan` table for delta manifests whose baseline was this phantom — crosslink those
3. Check `delta` table for blobs derived from this rid that haven't been crosslinked (`NOT EXISTS (SELECT 1 FROM mlink WHERE mid=delta.rid)`) — recursively dephantomize them
4. Clean up: `DELETE FROM orphan WHERE baseline=rid`

#### Orphan Tracking

When `crosslinkCheckin` encounters a delta manifest (B-card) whose baseline blob is a phantom, insert into `orphan(rid, baseline)` instead of failing silently. When the baseline arrives and dephantomizes, the orphan gets crosslinked automatically.

#### `SetDephantomizeHook(hook func(db.Querier, FslID))`

Per-session hook instead of a package-level toggle. This avoids data races when multiple goroutines run concurrent sync sessions (e.g., leaf agent syncing with multiple peers).

- The sync session sets `manifest.AfterDephantomize` as the hook at session start
- Clone sets nil (no per-blob crosslinking; bulk `Crosslink` at end)
- The hook is stored on the session, not as a global variable

#### Integration with `storeReceivedFile`

**Important:** `storeReceivedFile` in `sync/client.go:505-527` bypasses `blob.Store` — it inserts directly into the `blob` table via raw SQL. The `OnDephantomize` callback on `blob.Store` would never fire during sync.

Solution: Add dephantomize check directly into `storeReceivedFile`. After inserting/updating a blob:
1. Check if the blob was previously a phantom (`size=-1` before the INSERT, or the INSERT updated an existing phantom row)
2. If dephantomize hook is set on the session, invoke it with the rid

Additionally, add the `OnDephantomize` callback to `blob.Store`/`blob.StoreDelta` for non-sync callers (e.g., `repo.Create`, direct blob insertion).

### Auto-Crosslink in Sync

`sync.Sync()` automatically crosslinks after convergence:

1. Set dephantomize hook on the session at start of sync loop (incremental crosslinking as blobs arrive)
2. After all rounds complete and convergence is reached, call `manifest.Crosslink(s.repo)` as a catch-all
3. Populate `SyncResult.ArtifactsLinked` from `Crosslink` return value (renamed from `CheckinsLinked`)
4. Clear dephantomize hook before returning

**Clone behavior unchanged:** `Clone` already calls `Crosslink` at the end. No dephantomize hook during clone.

### Schema Addition

Add `forumpost` table to `go-libfossil/db/schema.go`:

```sql
CREATE TABLE forumpost(
  fpid INTEGER PRIMARY KEY,
  froot INT,
  fprev INT,
  firt INT,
  fmtime REAL
);
CREATE INDEX forumpost_froot ON forumpost(froot);
```

This matches Fossil's `schema_forum()` output.

## File Changes

| File | Change |
|------|--------|
| `go-libfossil/manifest/crosslink.go` | Restructure to two-pass. Add `crosslinkWiki`, `crosslinkTicket`, `crosslinkEvent`, `crosslinkAttachment`, `crosslinkCluster`, `crosslinkForum`, `addFWTPlink`. Extend `crosslinkCheckin` with cherrypick + baseid. Extend `crosslinkControl` with event row (type='g'). Updated discovery query. Add deferred processing pass. |
| `go-libfossil/manifest/dephantomize.go` | New. `AfterDephantomize()`, `SetDephantomizeHook()`, orphan tracking. |
| `go-libfossil/manifest/crosslink_test.go` | Extend with per-type tests + two-pass ordering test. |
| `go-libfossil/manifest/dephantomize_test.go` | New. Phantom fill, orphan, delta chain tests. |
| `go-libfossil/tag/propagate.go` | Add exported `PropagateAll(q db.Querier, rid FslID)` function. |
| `go-libfossil/blob/blob.go` | `Store`/`StoreDelta`: detect phantom-to-real transition, invoke `OnDephantomize` callback. |
| `go-libfossil/sync/client.go` | Call `manifest.Crosslink` after sync convergence. Set/clear dephantomize hook. Add dephantomize check to `storeReceivedFile`. Rename `CheckinsLinked` to `ArtifactsLinked`. |
| `go-libfossil/sync/stubs.go` | Rename `CheckinsLinked` to `ArtifactsLinked` in `SyncResult`. |
| `go-libfossil/db/schema.go` | Add `forumpost` table and index. |
| `dst/crosslink_test.go` | New DST test for crosslink completeness across multi-leaf sync. |
| `sim/equivalence_test.go` | Extend to verify wiki/ticket/forum metadata equivalence. |

## Testing Strategy

### Unit Tests (`manifest/crosslink_test.go`, `manifest/dephantomize_test.go`)

- `TestCrosslinkWiki` — wiki manifest → event type='w', `wiki-*` tag, plink chain
- `TestCrosslinkTicket` — ticket manifest → `tkt-*` tag, pending xlink processed
- `TestCrosslinkEvent` — technote manifest → event type='e', `event-*` tag, subsequent-version visibility
- `TestCrosslinkAttachment` — attachment manifest → `attachment` table, `isLatest` flag, generated comment, type detection
- `TestCrosslinkCluster` — cluster manifest with M-cards → `cluster` tag, members removed from `unclustered`
- `TestCrosslinkForum` — forum post manifest → `forumpost` table, event type='f', thread title propagation, reply handling
- `TestCrosslinkCherrypick` — checkin with Q-cards → `cherrypick` table populated
- `TestCrosslinkControl` — control artifact → event type='g' with generated comment
- `TestCrosslinkTwoPass` — mixed artifacts → deferred wiki/ticket processing runs after all artifacts linked
- `TestDiscoveryQueryIdempotent` — run `Crosslink` twice, verify no duplicate processing
- `TestDephantomizeCrosslinks` — store phantom, fill it, verify crosslink triggered
- `TestDephantomizeOrphan` — delta manifest with phantom baseline, fill baseline, verify orphan crosslinked
- `TestDephantomizeDeltaChain` — chain of deltas from phantom, fill root, verify all crosslinked recursively

### DST Tests (`dst/`)

- `TestCrosslinkCompletenessAfterSync` — multi-leaf sync with wiki/ticket/checkin artifacts, verify all metadata tables populated on every leaf
- Extend `TestTagPropagationAcrossSync` to verify tag propagation runs immediately (not deferred)

### Sim Equivalence Tests (`sim/`)

- Extend equivalence tests to verify wiki/ticket/forum event rows match between `fossil` and EdgeSync after clone+crosslink
