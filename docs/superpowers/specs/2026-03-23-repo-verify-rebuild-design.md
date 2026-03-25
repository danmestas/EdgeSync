# Repo Verify & Rebuild

**Date**: 2026-03-23
**Ticket**: CDG-132
**Branch**: TBD (from main, in worktree)

## Problem

go-libfossil has no way to detect or recover from repo corruption without the `fossil` binary. The existing `repo.Verify()` only checks blob hash integrity, stops at the first error, and cannot repair anything. Derived tables (event, mlink, plink, tagxref, filename, leaf) have no verification or reconstruction capability. Autonomous leaf nodes need self-diagnosis without human intervention.

## Solution

A new `go-libfossil/verify/` package with two operations:

- **`Verify`** — read-only scan of the entire repo, collecting all issues into a structured report
- **`Rebuild`** — drop-and-recompute all derived tables in a single transaction, with blob verification included

This is a full `fossil rebuild` replacement in Go.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Package location | `go-libfossil/verify/` | Keeps repo package focused on open/close/create. Verify+rebuild is cross-cutting (manifests, deltas, tags, events). |
| Rebuild strategy | Drop-and-recompute in transaction | Simple, correct, crash-safe. Matches Fossil's approach. Incremental diff-repair adds complexity for marginal speed on a rare operation. |
| Verify behavior | Report-all (never stops early) | Autonomous nodes need full visibility. One bad blob shouldn't hide ten other problems. |
| Auto-heal | No auto-rebuild | Fossil philosophy: prevention at ingestion, manual repair when needed. Ingestion-time hash checks are the sync-time safety net. |
| Periodic check | Agent can run Verify on interval | Detect corruption at rest (disk bit rot). Log issues but don't auto-rebuild. |
| Existing repo.Verify | Deprecate, delegate | Keep backward compat by delegating to verify.Verify() and returning first error. |
| Tag propagation order | Two-pass: structure first, tags second | Tags propagate along plink graph — all plinks must exist before propagation. Matches how `manifest.Checkin` and `crosslink` work (tags applied after tx commit). |
| Delta manifest handling | Expand to full file set | Delta manifests (with B-card) have abbreviated F-cards. Must merge with baseline to get the full file set before inserting mlink rows, same as `manifest.ListFiles` does. |
| Transaction size | Single transaction | Rare operation, correctness over performance. Future: add `RebuildOpts{BatchSize}` if large repos need it. |

## Types

```go
package verify

// IssueKind categorizes a problem found during verification.
type IssueKind int

const (
    IssueHashMismatch    IssueKind = iota // blob content doesn't match UUID
    IssueBlobCorrupt                       // blob can't be loaded/decompressed
    IssueDeltaDangling                     // delta.srcid references nonexistent blob
    IssuePhantomOrphan                     // phantom entry with no blob row
    IssueEventMissing                      // checkin blob has no event row
    IssueEventMismatch                     // event row doesn't match manifest content
    IssueMlinkMissing                      // file mapping missing for checkin
    IssuePlinkMissing                      // parent-child link missing
    IssueTagxrefMissing                    // tag mapping missing or stale
    IssueFilenameMissing                   // filename not in filename table
    IssueLeafIncorrect                     // leaf table doesn't match computed leaf set
    IssueMissingReference                  // parent/file UUID in manifest doesn't resolve to a blob
)

// Issue describes a single problem found during verification.
type Issue struct {
    Kind    IssueKind       // category of problem
    RID     libfossil.FslID // affected blob RID (0 if not blob-specific)
    UUID    string          // affected blob UUID (empty if not applicable)
    Table   string          // affected table name (e.g., "event", "delta")
    Message string          // human-readable description
}

// Report summarizes the results of a Verify or Rebuild.
type Report struct {
    Issues        []Issue
    BlobsChecked  int
    BlobsOK       int
    BlobsFailed   int
    BlobsSkipped  int      // blobs that couldn't be processed (corrupt/unparseable)
    MissingRefs   int      // parent/file UUIDs that didn't resolve during rebuild
    TablesRebuilt []string // table names dropped and reconstructed (non-empty only for Rebuild)
    Duration      time.Duration
}

// OK returns true if no issues were found.
func (r *Report) OK() bool { return len(r.Issues) == 0 }
```

## API

```go
// Verify performs a read-only scan of the entire repo, collecting all issues.
// Never stops early — reports every problem found.
//
// Panics if r is nil (TigerStyle precondition).
func Verify(r *repo.Repo) (*Report, error)

// Rebuild drops all derived tables and recomputes them from raw blobs
// in a single transaction. If anything fails, the transaction rolls back
// and the repo is untouched. Includes blob verification (Phase 1).
//
// Panics if r is nil (TigerStyle precondition).
func Rebuild(r *repo.Repo) (*Report, error)
```

## Verify Phases

All phases run to completion — issues are collected, never cause early exit.

### Phase 1: Blob Integrity

Walk every blob with `size >= 0` (non-phantoms):

1. `content.Expand(db, rid)` — tests delta chain traversal + decompression
2. If Expand errors → append `IssueBlobCorrupt`
3. Hash expanded content: SHA3 if UUID is 64 chars, SHA1 if 40 chars
4. Compare against stored UUID → append `IssueHashMismatch` if different
5. Increment `BlobsChecked`, `BlobsOK` or `BlobsFailed`

### Phase 2: Delta Chain Integrity

Walk every row in `delta` table:

1. For each `(rid, srcid)`: verify `rid` exists in `blob` table
2. Verify `srcid` exists in `blob` table
3. If either missing → append `IssueDeltaDangling`

### Phase 3: Phantom Integrity

Walk every row in `phantom` table:

1. Verify each `rid` exists in `blob` table
2. If missing → append `IssuePhantomOrphan`

### Phase 4: Derived Table Consistency

**Checkin manifests** — for each non-phantom blob, expand and parse. If `deck.Type == Checkin`:

1. Verify `event` row exists with matching mtime, user, comment → `IssueEventMissing` or `IssueEventMismatch`
2. Expand full file set (merge B-card baseline if delta manifest) via same logic as `manifest.ListFiles`
3. For each file: verify `filename` row exists → `IssueFilenameMissing`
4. For each file: verify `mlink` row exists for this checkin → `IssueMlinkMissing`
5. For each P-card: verify `plink` row exists → `IssuePlinkMissing`

**Control artifacts** — for each blob where `deck.Type == Control`:

6. For each T-card: verify corresponding `tagxref` row exists → `IssueTagxrefMissing`

**Forward tagxref check:**

7. Walk `tagxref` table: verify each entry references a valid blob (rid exists) and valid tag (tagid exists in tag table) → `IssueTagxrefMissing`

**Leaf set:**

8. Recompute leaf set: a checkin is a leaf if no plink row has `pid = this_rid`
9. Compare computed leaf set against actual `leaf` table → `IssueLeafIncorrect`

## Rebuild Flow

Entire operation runs in a single SQLite transaction. Rollback on any error.

### Step 1: Verify Blobs

Same as Verify Phase 1. Collect issues into the report but continue — rebuild derived tables from whatever blobs are valid. Corrupt blobs are skipped during reconstruction. Increment `BlobsSkipped` for each unprocessable blob.

### Step 2: Drop Derived Tables

```sql
DELETE FROM event;
DELETE FROM mlink;
DELETE FROM plink;
DELETE FROM tagxref;
DELETE FROM filename;
DELETE FROM leaf;
DELETE FROM unclustered;
DELETE FROM unsent;
```

Note: `backlink`, `attachment`, and `cherrypick` tables are not rebuilt. These are populated by wiki/forum/ticket artifact types which go-libfossil does not yet produce or consume. Documented in Out of Scope.

### Step 3: Walk All Blobs — Structure Pass

For each non-phantom blob in `blob` table:

1. Attempt `content.Expand()` — skip on error (append `IssueBlobCorrupt`, increment `BlobsSkipped`)
2. Attempt `deck.Parse()` — skip if not a valid manifest (not all blobs are manifests)
3. If `deck.Type == Checkin`:
   a. Insert `event` row: `(type='ci', mtime, objid=rid, user=deck.U, comment=deck.C)`
   b. Expand full file set (merge B-card baseline if `deck.B != ""`, same as `manifest.ListFiles`)
   c. For each file in full set:
      - Ensure `filename` row (INSERT OR IGNORE)
      - Resolve file blob RID via `blob.Exists(uuid)` — if missing, increment `MissingRefs`, append `IssueMissingReference`, continue
      - Insert `mlink` row (mid=manifest_rid, fid=file_rid, fnid, pmid, pid)
   d. For each P-card:
      - Resolve parent RID via `blob.Exists(uuid)` — if missing, increment `MissingRefs`, append `IssueMissingReference`, continue
      - Insert `plink` row (pid=parent_rid, cid=manifest_rid, isprim, mtime)
4. Record all inline T-cards and control artifact T-cards for Step 4 (do NOT apply yet — plink graph must be complete first)

### Step 4: Tag Pass

After all event/plink/mlink rows are inserted, process tags. This must happen after Step 3 because tag propagation walks the plink graph.

For each checkin manifest's inline T-cards (where UUID == "*"):
1. Ensure `tag` row exists for tag name
2. Call `tag.ApplyTag` with TargetRID=checkin_rid, SrcRID=checkin_rid

For each control artifact (deck.Type == Control):
1. For each T-card (UUID != "*"):
   a. Resolve target RID via `blob.Exists(uuid)`
   b. If missing, increment `MissingRefs`, continue
   c. Call `tag.ApplyTag` with TargetRID=resolved_rid, SrcRID=control_rid

### Step 5: Compute Leaves

```sql
INSERT INTO leaf(rid)
SELECT e.objid FROM event e
WHERE e.type = 'ci'
AND NOT EXISTS (SELECT 1 FROM plink WHERE pid = e.objid);
```

### Step 6: Rebuild Sync Bookkeeping

```sql
INSERT INTO unclustered(rid)
SELECT rid FROM blob WHERE size >= 0;

INSERT INTO unsent(rid)
SELECT rid FROM blob WHERE size >= 0;
```

### Step 7: Commit Transaction

If any step errors, the transaction rolls back. The report includes all issues from Step 1 plus `TablesRebuilt` listing every table name that was reconstructed (e.g., `["event", "mlink", "plink", "tagxref", "filename", "leaf", "unclustered", "unsent"]`).

## Integration Points

### CLI

- **`edgesync repo verify`** — rewire to call `verify.Verify()`, print structured report (issue count by kind, blob stats, duration)
- **`edgesync repo rebuild`** — new command, calls `verify.Rebuild()`, print report

### Leaf Agent

- Periodic health check: `verify.Verify()` on configurable interval (e.g., daily)
- Log issues via observer/slog. Do NOT auto-rebuild — that's a manual/explicit decision.
- No PostSyncHook integration — verify is too expensive for every sync. Ingestion-time hash checks in `storeReceivedFile` are the sync-time safety net.

### Existing repo.Verify()

Deprecate but keep for backward compatibility:

```go
// Deprecated: Use verify.Verify() for comprehensive scanning.
// This method delegates to verify.Verify() and returns the first issue as an error.
func (r *Repo) Verify() error {
    report, err := verify.Verify(r)
    if err != nil { return err }
    if len(report.Issues) > 0 {
        return fmt.Errorf("repo.Verify: %s", report.Issues[0].Message)
    }
    return nil
}
```

### DST Invariants

Existing DST checks (`CheckBlobIntegrity`, `CheckDeltaChains`, `CheckNoOrphanPhantoms`, `CheckTagxrefIntegrity`) stay as-is — they're fast, targeted checks for simulation. `verify.Verify()` is the comprehensive version for production use.

## File Structure

| File | Purpose |
|------|---------|
| `verify.go` | `Verify()` entry point, phase orchestration, `Report`/`Issue`/`IssueKind` types |
| `check_blobs.go` | Phase 1: blob integrity (expand, hash, compare) |
| `check_structure.go` | Phases 2-3: delta chains, phantom integrity |
| `check_derived.go` | Phase 4: event, mlink, plink, tagxref, filename, leaf consistency |
| `rebuild.go` | `Rebuild()` entry point, transaction wrapper, drop+reconstruct |
| `rebuild_manifest.go` | Step 3: manifest walking, event/mlink/plink/filename insertion |
| `rebuild_tags.go` | Step 4: inline T-card and control artifact tag application |
| `rebuild_leaves.go` | Steps 5-6: leaf computation, sync bookkeeping |
| `verify_test.go` | All tests |

## Testing Strategy

- **Blob corruption detection**: Create repo, corrupt a blob via direct SQL (`UPDATE blob SET content = X'00' WHERE rid = ?`), verify detects `IssueHashMismatch`
- **Delta chain detection**: Delete a blob that's a delta source, verify detects `IssueDeltaDangling`
- **Phantom orphan detection**: Insert phantom row with no blob, verify detects `IssuePhantomOrphan`
- **Derived table detection**: Delete event/mlink/plink rows, verify detects `IssueEventMissing`/`IssueMlinkMissing`/`IssuePlinkMissing`
- **Control artifact verification**: Create repo with branch tags (control artifacts), delete tagxref rows, verify detects `IssueTagxrefMissing`
- **Missing reference reporting**: Create checkin referencing phantom file UUID, rebuild, verify `IssueMissingReference` reported and `MissingRefs` incremented
- **Rebuild correctness**: Create repo with checkins, delete all derived tables, rebuild, verify all tables match original state
- **Rebuild with branches and tags**: Create repo with branches (control artifacts with propagating tags), delete derived tables, rebuild, verify tag propagation is correct
- **Rebuild idempotency**: Rebuild twice, verify same report (zero issues on second run)
- **Transaction rollback**: Simulate error mid-rebuild (e.g., close DB during rebuild), verify repo is untouched
- **BUGGIFY resilience**: Rebuild with `content.Expand` byte-flip active, verify it completes (corrupted blobs skipped, valid blobs rebuilt)
- **Delta manifest rebuild**: Create repo with delta manifests (B-card), delete derived tables, rebuild, verify mlink has full file set (not just delta F-cards)
- **Multi-checkin**: Repo with branches, merges, tags — verify rebuild reconstructs the full graph
- **Report-all**: Introduce multiple different corruptions, verify all are reported (not just the first)

## Dependencies

- `go-libfossil/repo` — open repo, access DB
- `go-libfossil/content` — expand blobs (delta chain resolution)
- `go-libfossil/blob` — check existence, load
- `go-libfossil/deck` — parse manifests
- `go-libfossil/hash` — SHA1/SHA3 computation
- `go-libfossil/tag` — tag application and propagation for rebuild
- `go-libfossil/db` — Querier, Tx interfaces
- `go-libfossil/manifest` — ListFiles for delta manifest expansion (or replicate the B-card merge logic)

No new external dependencies.

## Out of Scope

- Auto-rebuild (agent autonomously decides to rebuild)
- Remote re-request of corrupt blobs (sync naturally resolves phantoms)
- Incremental/differential repair
- Schema migration (Fossil version upgrades)
- Delta re-clustering/compression optimization
- FTS index rebuild (handled by `search.RebuildIndex`)
- `backlink`, `attachment`, `cherrypick` table reconstruction (wiki/forum/ticket artifact types not yet implemented in go-libfossil)
- Batch commit mode for large repos (future `RebuildOpts{BatchSize}`)
