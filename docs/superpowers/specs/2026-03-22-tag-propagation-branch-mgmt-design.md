# Spec: Tag Propagation + Branch Management (CDG-151)

**Date:** 2026-03-22
**Branch:** feature/cdg-151-implement-branch-management-in-go-libfossil
**Worktree:** .worktrees/branch-mgmt
**Status:** Draft

## Problem

go-libfossil's `tag.AddTag` inserts a single `tagxref` row but does not propagate tags to descendant checkins. Fossil C's `tag_insert()` always calls `tag_propagate()` which walks the `plink` DAG and updates `tagxref` at every descendant. Without propagation:

- Branch names don't carry forward to child checkins
- `manifest.Crosslink` can't process control artifacts (tag/branch cards) after clone/sync
- `fossil open` requires manual `sym-trunk` workarounds in tests

## Fossil C Reference

### tag_propagate (tag.c:34-113)

Tree-walks the `plink` DAG from an originating artifact to all descendants:

- **Propagating (tagtype=2):** `REPLACE INTO tagxref` with `srcid=0` at each descendant. Skips if descendant already has a more recent tag for this tagid.
- **Cancel (tagtype=0):** `DELETE FROM tagxref WHERE tagid=? AND rid=?` at each descendant (deletes ALL tagxref entries for that tag, not just propagated ones). Continues walking to descendants that had a propagated tag — stops naturally when no more propagated descendants exist.
- Uses a priority queue ordered by mtime (oldest first).
- Only follows primary parent links (`plink.isprim=1`).
- Special handling: `TAG_BGCOLOR` also updates `event.bgcolor`; `TAG_BRANCH` triggers leaf recomputation.

### tag_insert (tag.c:157-242)

Main entry point. Inserts/replaces direct tagxref row, then calls `tag_propagate()`. Called during manifest crosslink when processing control artifacts.

### branch_new (branch.c:79-228)

Creates a new checkin manifest with branch tags:
- `T *branch * <BRANCHNAME>` — propagating tag marking branch identity
- `T *sym-<BRANCHNAME> *` — propagating tag for symbolic branch name
- `T -sym-<OLDNAME> *` — cancel tags for previous branch sym-* tags from parent ancestry
- `T *bgcolor * <COLOR>` — optional propagating color tag

### Branch listing (branch.c:242-267)

Queries `tagxref JOIN tag JOIN event WHERE tagname='branch' AND tagtype>0 GROUP BY value`.

### tagxref schema

```sql
CREATE TABLE tagxref(
  tagid INTEGER REFERENCES tag,
  tagtype INTEGER,        -- 0:cancel, 1:singleton, 2:propagated
  srcid INTEGER REFERENCES blob,   -- 0 if propagated, >0 if direct (control artifact RID)
  origid INTEGER REFERENCES blob,  -- artifact the tag originated from
  value TEXT,
  mtime TIMESTAMP,
  rid INTEGER REFERENCE blob,      -- artifact the tag is applied to
  UNIQUE(rid, tagid)
);
```

## Design

### Part 1: Tag Propagation in `tag/`

**Change `AddTag`:** After inserting the direct `tagxref` row (existing code), call `propagate()` when `TagType == TagPropagating` or `TagType == TagCancel`.

**New `propagate()` algorithm** (new file `tag/propagate.go`):

Matches Fossil's `tag_propagate` (tag.c:34-113). Seeds the queue with the target artifact itself (mtime=0), then processes uniformly:

1. Seed priority queue with `(targetRID, mtime=0.0)`
2. For each dequeued node, query its primary children: `SELECT cid, plink.mtime FROM plink WHERE pid=? AND isprim=1`
3. For each child, LEFT JOIN tagxref to check existing tag state:
   - `doit = coalesce(srcid=0 AND tagxref.mtime < :mtime, tagtype==2)` — for propagating, visit if no tag or tag is older; for cancel, visit only if child has a propagated tag that's older
4. If `doit`:
   - **Propagating (type 2):** `REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid) VALUES(?, 2, 0, ?, ?, ?, ?)`. Queue child.
   - **Cancel (type 0):** `DELETE FROM tagxref WHERE tagid=? AND rid=?` (deletes ALL entries for this tag at child, matching Fossil). Queue child.
5. Continue until queue is empty.

The cancel walk continues through descendants that had propagated tags, stopping naturally when no more propagated descendants exist (the `doit` condition filters them out).

**Priority queue:** Use `container/heap` with `(rid, mtime)` pairs, ordered by mtime ascending.

**Special tag handling:**
- Tag named `bgcolor`: also `UPDATE event SET bgcolor=? WHERE objid=?` on propagation
- Tag named `branch`: no special handling in go-libfossil (leaf table already maintained by Crosslink)

### Part 2: Crosslink Enhancement in `manifest/`

**Extend `Crosslink()` to process control artifacts:**

After processing checkin manifests (existing code), add a second pass:

1. Find blobs not yet processed: blobs with no `tagxref.srcid` entry matching their RID
2. Parse each with `deck.Parse()`. If `deck.Type == deck.Control`, process T-cards
3. For each T-card: resolve the target UUID to a RID, then call `tag.AddTag` with:
   - `TargetRID` = resolved target RID
   - `TagName` = T-card name (without prefix)
   - `TagType` = mapped from T-card type byte (`*`=propagating, `+`=singleton, `-`=cancel)
   - `Value` = T-card value
   - `srcRID` = the control artifact's RID (new field needed — see below)

**New field in TagOpts:** `SrcRID libfossil.FslID` — the control artifact that introduced this tag. Currently `AddTag` always sets `srcid = controlRid` (the artifact it creates). For Crosslink, the control artifact already exists — Crosslink needs to pass its RID rather than creating a new one.

**Alternative approach:** Split tag insertion into two functions:
- `AddTag()` — creates a control artifact AND inserts tagxref (existing behavior, for new tags)
- `ApplyTag()` — inserts tagxref + propagates WITHOUT creating a control artifact (for Crosslink processing existing artifacts)

This avoids bloating `TagOpts` and keeps concerns separate.

### Part 3: Branch Package

**New package: `go-libfossil/branch/`**

**`Create(r *repo.Repo, opts CreateOpts) (libfossil.FslID, string, error)`**

`CreateOpts`:
```go
type CreateOpts struct {
    Name     string          // Branch name (required)
    Parent   libfossil.FslID // Parent checkin RID (required)
    User     string          // Author
    Time     time.Time       // Timestamp (default: now)
    Color    string          // Optional bgcolor
    Private  bool            // Private branch
}
```

Steps:
1. Get parent's file list via `manifest.ListFiles(r, parentRID)`
2. Query existing `sym-*` tags on parent: `SELECT tagname FROM tagxref JOIN tag USING(tagid) WHERE rid=? AND tagtype>0 AND tagname GLOB 'sym-*'`
3. Build T-cards: `*branch <name>` (propagating), `*sym-<name>` (propagating), `-sym-<old>` (cancel) for each old sym tag, optional `*bgcolor <color>` (propagating)
4. Call `manifest.Checkin(r, CheckinOpts{Files: parentFiles, Parent: parentRID, Tags: tCards, ...})` — this reuses existing manifest construction, blob storage (auto-marks unclustered+unsent), and mlink/plink/event insertion
5. Run `manifest.Crosslink` on the repo (processes the new checkin's inline T-cards → triggers tag propagation)
6. Return the new checkin's RID and UUID

Note: `manifest.Checkin` already supports custom T-cards via `CheckinOpts.Tags`. The branch checkin reuses the parent's files — `Checkin` re-stores them via `blob.Store` which is idempotent (returns existing RID if blob exists).

**`List(r *repo.Repo) ([]Branch, error)`**

```go
type Branch struct {
    Name       string
    LastMTime  float64  // Julian day of last checkin
    IsClosed   bool
    CheckinCount int
    LatestUUID string
}
```

Query:
```sql
SELECT tagxref.value, max(event.mtime), count(*),
       (SELECT 1 FROM tagxref tx2 JOIN tag t2 ON tx2.tagid=t2.tagid
        WHERE t2.tagname='closed' AND tx2.rid=tagxref.rid AND tx2.tagtype>0) AS isclosed,
       (SELECT uuid FROM blob WHERE rid=tagxref.rid) AS latest
FROM tagxref JOIN tag ON tagxref.tagid=tag.tagid
JOIN event ON event.objid=tagxref.rid
WHERE tag.tagname='branch' AND tagxref.tagtype>0
GROUP BY tagxref.value
```

**`Close(r *repo.Repo, name string, user string) error`**

1. Find the latest leaf checkin on the branch (query tagxref for branch name, join leaf table)
2. Add singleton `closed` tag via `tag.AddTag` to that checkin

## Files Changed

| File | Change |
|------|--------|
| Modify: `go-libfossil/tag/tag.go` | Add `ApplyTag()`, call `propagate()` from `AddTag` and `ApplyTag` for propagating/cancel types |
| Create: `go-libfossil/tag/propagate.go` | `propagate()` — plink DAG tree walk with priority queue |
| Modify: `go-libfossil/tag/tag_test.go` | Tests for propagation chains and cancellation |
| Modify: `go-libfossil/manifest/crosslink.go` | Process control artifacts — call `tag.ApplyTag` per T-card |
| Modify: `go-libfossil/manifest/crosslink_test.go` | Test control artifact processing |
| Create: `go-libfossil/branch/branch.go` | `Create`, `List`, `Close` |
| Create: `go-libfossil/branch/branch_test.go` | Tests for all three + fossil rebuild |
| Modify: `sim/equivalence_test.go` | Remove manual `sym-trunk` / `tag.AddTag` workarounds |

## Testing

### Tag Propagation
- Create chain A→B→C, add propagating tag to A, verify tagxref at B and C with `srcid=0`
- Add cancel tag at B, verify C's propagated entry is deleted
- Add propagating tag to A with value "v1", then more recent tag at B with value "v2", verify C has "v2" (newer wins)
- Propagate `bgcolor` tag, verify `event.bgcolor` updated at descendants

### Crosslink Control Artifacts
- Create a repo with checkins + control artifacts (tag cards), run Crosslink, verify tagxref populated
- Clone a repo, run Crosslink, verify `sym-trunk` and `branch` tags exist without manual workarounds

### Branch
- `Create`: create branch from checkin, verify `branch` and `sym-*` tagxref entries, old `sym-*` cancelled, `fossil rebuild` passes
- `List`: create 2 branches, list them, verify names and metadata
- `Close`: close a branch, verify `closed` tag, list shows it as closed
- Round-trip: create branch, sync to another repo, Crosslink, verify branch visible in List

### Equivalence Test Cleanup
- Remove all manual `tag.AddTag(sym-trunk)` calls from `sim/equivalence_test.go`
- Verify all equivalence tests still pass (Crosslink now handles it)

## Non-Goals

- Branch merging (that's a merge operation, not a branch operation)
- Branch permissions/access control (needs auth package first — CDG-155)
- Private branches (requires private blob infrastructure)
- Branch color UI rendering
