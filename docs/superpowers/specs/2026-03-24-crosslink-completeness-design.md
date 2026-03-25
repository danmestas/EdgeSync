# Crosslink Completeness Design Spec

**Date:** 2026-03-24
**Status:** Draft
**Related:** CDG-165 (clustering), Fossil manifest.c:2336-2800

## Problem

`manifest.Crosslink` currently handles checkin manifests and control artifacts but ignores wiki, ticket, technote, attachment, forum, and cluster artifacts. Synced repos contain these blobs but the metadata tables (`event`, `attachment`, `cherrypick`, etc.) are not populated, making `fossil open` after EdgeSync clone/sync produce incomplete results.

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

## Design

### Two-Pass Architecture

Restructure `Crosslink` from single-pass to Fossil's two-pass model:

**Pass 1 — Crosslink artifacts into tables:**

Scan all blobs not yet in the `event` table. Parse each blob, dispatch by `deck.ArtifactType` to a handler function. During this pass, collect deferred work:

- `pendingXlink []pendingItem` — wiki backlinks and ticket rebuilds to process after all artifacts are linked
- `parentIDs []FslID` — parent IDs for deferred `tag_propagate_all`

Tag processing for Checkin, Control, and Event types happens in Pass 1 (matching Fossil lines 2445-2473), but `tag_propagate_all` is deferred to Pass 2.

**Pass 2 — Deferred processing:**

- Process `pendingXlink` entries (wiki backlink refresh, ticket entry rebuild)
- Run `tag.PropagateAll` for collected parent IDs

This matches Fossil's `manifest_crosslink_begin` / `manifest_crosslink` / `manifest_crosslink_end` pattern.

### Per-Type Handlers

All handlers are unexported functions in `crosslink.go`.

#### `crosslinkCheckin` (existing, extended)

Current behavior preserved: event/plink/leaf/mlink + inline T-cards.

Extensions:
- **Cherrypick:** Populate `cherrypick(parentid, childid, isExclude)` from `deck.Q` cards. Matches Fossil lines 2380-2390.
- **baseid in plink:** Store baseline manifest RID in `plink.baseid` for delta manifests (B-card). Currently passes NULL.
- **parentid collection:** Return primary parent ID for deferred `tag_propagate_all`.

#### `crosslinkWiki` (new)

Matches Fossil lines 2479-2516.

1. Insert plink for wiki version chain via `addFWTPlink` helper (shared with forum/event)
2. Create `wiki-<title>` singleton tag with content length as value
3. Determine comment prefix: `-` (empty/delete), `+` (new), `:` (edit with prior version)
4. Insert event row: `type='w', mtime, objid=rid, user, comment='<prefix><title>'`
5. Queue `pendingItem{Type: 'w', ID: title}` for deferred backlink refresh

#### `crosslinkTicket` (new)

Matches Fossil lines 2596-2631.

1. Create `tkt-<uuid>` singleton tag
2. Queue `pendingItem{Type: 't', ID: ticketUUID}` for deferred ticket rebuild
3. Update attachment event comments targeting this ticket UUID

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

Matches Fossil lines 2475-2478 (shared with wiki/event).

1. Insert plink for forum post version chain via `addFWTPlink`
2. No event table entry (Fossil doesn't create one for forum posts in crosslink)

#### `crosslinkControl` (existing, unchanged)

Already processes external T-cards and applies tags with propagation.

### Shared Helper: `addFWTPlink`

Matches Fossil's `manifest_add_fwt_plink` (lines 2287-2307).

Used by wiki, forum, and event types. For each parent UUID in the deck:
1. Resolve to rid
2. Insert `plink(pid, cid, isprim, mtime, baseid=NULL)`
3. Collect primary parent ID for deferred `tag_propagate_all`

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

#### `EnableDephantomize(enabled bool)`

Package-level toggle matching Fossil's `content_enable_dephantomize`.

- **Disabled during Clone:** Clone runs full `Crosslink` at the end, no need for per-blob processing
- **Enabled during Sync:** Incremental crosslinking as blobs arrive

#### Integration with `blob.Store`

`blob.Store` and `blob.StoreDelta` are in `go-libfossil/blob/`. Dephantomize logic is in `go-libfossil/manifest/`. To avoid circular imports:

- `blob.Store` accepts an optional `OnDephantomize func(db.Querier, FslID)` callback
- When storing content into a previously-phantom blob (`size=-1` → real content), invoke the callback
- `sync` package wires `manifest.AfterDephantomize` as the callback when dephantomize is enabled
- When nil (default, or during clone), no callback fires

### Auto-Crosslink in Sync

`sync.Sync()` automatically crosslinks after convergence:

1. Enable dephantomize at start of sync loop (incremental crosslinking as blobs arrive)
2. After all rounds complete and convergence is reached, call `manifest.Crosslink(s.repo)` as a catch-all
3. Populate `SyncResult.CheckinsLinked` from `Crosslink` return value
4. Disable dephantomize before returning

**Clone behavior unchanged:** `Clone` already calls `Crosslink` at the end. Dephantomize stays disabled during clone.

## File Changes

| File | Change |
|------|--------|
| `go-libfossil/manifest/crosslink.go` | Restructure to two-pass. Add `crosslinkWiki`, `crosslinkTicket`, `crosslinkEvent`, `crosslinkAttachment`, `crosslinkCluster`, `crosslinkForum`, `addFWTPlink`. Extend `crosslinkCheckin` with cherrypick + baseid. Add deferred processing pass. |
| `go-libfossil/manifest/dephantomize.go` | New. `AfterDephantomize()`, `EnableDephantomize()`, orphan tracking. |
| `go-libfossil/manifest/crosslink_test.go` | Extend with per-type tests + two-pass ordering test. |
| `go-libfossil/manifest/dephantomize_test.go` | New. Phantom fill, orphan, delta chain tests. |
| `go-libfossil/blob/blob.go` | `Store`/`StoreDelta`: detect phantom-to-real transition, invoke `OnDephantomize` callback. |
| `go-libfossil/sync/client.go` | Call `manifest.Crosslink` after sync convergence. Enable/disable dephantomize around sync loop. |
| `dst/crosslink_test.go` | New DST test for crosslink completeness across multi-leaf sync. |
| `sim/equivalence_test.go` | Extend to verify wiki/ticket metadata equivalence. |

## Testing Strategy

### Unit Tests (`manifest/crosslink_test.go`, `manifest/dephantomize_test.go`)

- `TestCrosslinkWiki` — wiki manifest → event type='w', `wiki-*` tag, plink chain
- `TestCrosslinkTicket` — ticket manifest → `tkt-*` tag, pending xlink processed
- `TestCrosslinkEvent` — technote manifest → event type='e', `event-*` tag, subsequent-version visibility
- `TestCrosslinkAttachment` — attachment manifest → `attachment` table, `isLatest` flag, generated comment, type detection
- `TestCrosslinkCluster` — cluster manifest with M-cards → `cluster` tag, members removed from `unclustered`
- `TestCrosslinkForum` — forum post manifest → plink chain, no event row
- `TestCrosslinkCherrypick` — checkin with Q-cards → `cherrypick` table populated
- `TestCrosslinkTwoPass` — mixed artifacts → tags applied in correct order with deferred propagation
- `TestDephantomizeCrosslinks` — store phantom, fill it, verify crosslink triggered
- `TestDephantomizeOrphan` — delta manifest with phantom baseline, fill baseline, verify orphan crosslinked
- `TestDephantomizeDeltaChain` — chain of deltas from phantom, fill root, verify all crosslinked recursively

### DST Tests (`dst/`)

- `TestCrosslinkCompletenessAfterSync` — multi-leaf sync with wiki/ticket/checkin artifacts, verify all metadata tables populated on every leaf
- Extend `TestTagPropagationAcrossSync` to verify two-pass ordering

### Sim Equivalence Tests (`sim/`)

- Extend equivalence tests to verify wiki/ticket event rows match between `fossil` and EdgeSync after clone+crosslink
