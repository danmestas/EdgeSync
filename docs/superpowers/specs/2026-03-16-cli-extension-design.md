# CLI Extension Design: Stash, Undo, Annotate, Bisect, Branch

**Date:** 2026-03-16
**Branch:** feat/cli-extension
**Status:** Approved (rev 2 — post spec review)

## Overview

Five new feature groups for the EdgeSync CLI, extending the existing 28-command set. All features match Fossil's behavior, using the Fossil and libfossil source as reference implementations.

## Implementation Order

Features must be built in this order due to dependencies:

1. **Prerequisites** — refactor `repo co` to update checkout DB; extend `manifest.CheckinOpts` with Tags field; add `go-libfossil/tag/` package for control artifacts
2. **Undo** — must exist before stash (stash pop/apply calls `undoSave()`)
3. **Stash** — depends on undo
4. **Path** — standalone, no dependencies
5. **Annotate** — standalone, no dependencies (can parallel with 4)
6. **Bisect** — depends on path package and `repo co` refactor
7. **Branch** — depends on Tags in CheckinOpts and tag package

## 1. Stash

### Commands

| Command | Description |
|---------|-------------|
| `repo stash save` | Snapshot changed files, revert working dir |
| `repo stash pop` | Apply top stash + drop it |
| `repo stash apply` | Apply stash without dropping |
| `repo stash ls` | List stash entries |
| `repo stash show` | Show diff of stash contents |
| `repo stash drop` | Remove specific stash entry |
| `repo stash clear` | Remove all stash entries |

### Schema

Added to checkout DB on first use:

```sql
CREATE TABLE IF NOT EXISTS stash(
  stashid INTEGER PRIMARY KEY,
  hash TEXT,
  comment TEXT,
  ctime TIMESTAMP
);
CREATE TABLE IF NOT EXISTS stashfile(
  stashid INTEGER REFERENCES stash,
  isAdded BOOLEAN,
  isRemoved BOOLEAN,
  isExec BOOLEAN,
  isLink BOOLEAN,
  hash TEXT,
  origname TEXT,
  newname TEXT,
  delta BLOB,
  PRIMARY KEY(newname, stashid)
);
```

`vvar('stash-next')` tracks the next stash ID.

### Column Semantics

- `stash.hash` — UUID of the checkout version (manifest hash) at time of stash, NOT a hash of the stash content. Used by apply to know which baseline to apply deltas against.
- `stashfile.hash` — UUID of the file's baseline blob (NULL for newly added files).
- `stashfile.delta` — delta from baseline content to working-dir content (raw content for added files, empty for removed files).

### Behavior

- **save**: For each changed/added/removed file in vfile, compute delta against repo baseline content (using `pkg/delta`). Store in stashfile. Record checkout manifest hash in `stash.hash`. Revert working directory to clean state. Optional `-m` message.
- **pop**: Apply top stash (re-patch files from delta against baseline), then drop. Calls `undoSave()` before modifying files.
- **apply**: Same as pop without dropping. Calls `undoSave()` before modifying files.
- **ls**: List entries with id, date, comment.
- **show**: Expand deltas, diff against baseline, show unified diff.
- **drop**: Remove stash entry by ID.
- **clear**: Remove all entries.

### Package

- `go-libfossil/stash/` — stash save/apply/drop logic
- `cmd/edgesync/repo_stash.go` — CLI commands

### Reference

- `fossil/src/stash.c` — Fossil's stash implementation

## 2. Undo

### Commands

| Command | Description |
|---------|-------------|
| `repo undo` | Undo last destructive operation |
| `repo redo` | Redo the undone operation |

### Schema

Created in checkout DB before each undoable operation:

```sql
CREATE TABLE IF NOT EXISTS undo(
  pathname TEXT,
  content BLOB,
  existsflag BOOLEAN,
  isExec BOOLEAN,
  isLink BOOLEAN,
  redoflag BOOLEAN DEFAULT 0
);
CREATE TABLE IF NOT EXISTS undo_vfile AS SELECT * FROM vfile WHERE 0;
CREATE TABLE IF NOT EXISTS undo_vmerge AS SELECT * FROM vmerge WHERE 0;
```

### Behavior

- **Before destructive operations** (`co`, `revert`, `add`, `rm`, `rename`, `stash pop/apply`): call `undoSave()` which:
  1. Drops and recreates undo tables
  2. Snapshots affected files' content into `undo` table
  3. Copies `vfile` -> `undo_vfile`, `vmerge` -> `undo_vmerge`
  4. Sets `undo_available=1` and `undo_checkout=<current vid>` in vvar

- **`repo undo`**: Requires `undo_available=1` in vvar. Swap vfile <-> undo_vfile, vmerge <-> undo_vmerge. Restore file content from `undo` table (entries where `redoflag=0`). Flip all `redoflag` values. Set `undo_available=2` (redo now available). Swap `vvar.checkout` <-> `vvar.undo_checkout`.

- **`repo redo`**: Requires `undo_available=2` in vvar. Same swap mechanism, using entries where `redoflag=1`. Set `undo_available=1` (undo available again). Swap checkout vvars.

- **State machine**: `undo_available` values: `0` = nothing available, `1` = undo available, `2` = redo available. After undo sets to 2, after redo sets to 1, after any new undoable operation resets to 1.

- **Single level only**: Each new undoable operation replaces previous undo state (matches Fossil).

- **Output per file**: `UNDO <path>`, `REDO <path>`, `NEW <path>`, `DELETE <path>`

### Integration Points

Existing commands that call `undoSave()`:
- `repo_revert.go`
- `repo_add.go`
- `repo_rm.go`
- `repo_rename.go`
- `repo_co.go`
- New: `repo_stash.go` (pop/apply)

### Package

- `go-libfossil/undo/` — save/restore/redo logic
- `cmd/edgesync/repo_undo.go` — CLI commands

### Reference

- `fossil/src/undo.c` — Fossil's undo implementation

## 3. Annotate/Blame

### Commands

| Command | Description |
|---------|-------------|
| `repo annotate` | Show line-by-line history attribution |
| `repo blame` | Alias for annotate |

### Flags

- `--file FILE` — pathname of file to annotate (required). CLI resolves pathname to file blob UUID via the manifest's F-cards for the starting version.
- `--version VERSION` — starting version (default: tip)
- `--limit N` — max versions to walk (0 = unlimited)
- `--origin VERSION` — stop at this commit, attribute remaining lines to it

### Algorithm

1. Load file content at starting version. Resolve `--file` pathname to blob UUID via the version's manifest F-cards. Split content into lines.
2. Each line gets an `iVers` marker tracking which version introduced it.
3. Walk parent chain via `plink` + `mlink` to find file's rid in each ancestor. Use `mlink.fnid` + `filename.name` to track the file across renames.
4. At each ancestor, diff ancestor's content against current annotated version using Myers.
5. Lines unchanged in ancestor get `iVers` pushed back to that version.
6. Stop when: all lines attributed, `--limit` reached, `--origin` reached, or no more ancestors.
7. Output per line: `UUID_PREFIX USER DATE | content`

### Key Types

```go
type Line struct {
    Text    string
    Version VersionInfo
}

type VersionInfo struct {
    UUID string
    User string
    Date time.Time
}

type Options struct {
    FilePath  string      // pathname — resolved to UUID internally
    StartRID  fsl.FslID
    Limit     int         // 0 = unlimited
    OriginRID fsl.FslID   // 0 = none
}
```

### Package

- `go-libfossil/annotate/` — annotation engine
- `cmd/edgesync/repo_annotate.go` — CLI commands

### Reference

- `libfossil/checkout/src/annotate.c` — libfossil's annotate implementation (472 lines)

## 4. Bisect

### Commands

| Command | Description |
|---------|-------------|
| `repo bisect good [VERSION]` | Mark version as good (default: current checkout) |
| `repo bisect bad [VERSION]` | Mark version as bad (default: current checkout) |
| `repo bisect next` | Check out the midpoint version |
| `repo bisect skip [VERSION]` | Skip current version |
| `repo bisect reset` | Clear all bisect state |
| `repo bisect ls` | Show bisect path with labels |
| `repo bisect status` | Show current bisect state |

### State

Stored in `vvar` in checkout DB:

- `bisect-good` — RID of known-good commit
- `bisect-bad` — RID of known-bad commit
- `bisect-log` — space-separated decision log (positive RID = good, negative = bad, `s<RID>` = skip)

### Algorithm

1. User marks a commit as `good` and another as `bad`.
2. `next` finds shortest path between good and bad in `plink` DAG using Dijkstra, respecting skip-set. Picks midpoint. Checks out that version.
3. User tests, marks current as `good`, `bad`, or `skip`.
4. Repeat until good and bad are adjacent — report first bad commit.
5. `ls` shows path with labels: GOOD, BAD, CURRENT, NEXT, SKIP.
6. `status` shows bounds and steps remaining.
7. `reset` clears all bisect vvar entries.

### Path-Finding

```go
// go-libfossil/path/
type PathNode struct {
    RID   fsl.FslID
    From  *PathNode
    Depth int
}

func Shortest(db *sql.DB, from, to fsl.FslID, directOnly bool, skip map[fsl.FslID]bool) ([]PathNode, error)
```

- `directOnly` — follow only `isprim=1` links (no merge parents)
- BFS over `plink` table (unweighted edges — BFS is correct and matches Fossil's `path_shortest()`)
- Returns ordered path from `from` to `to`

### Checkout Integration

`bisect good/bad/skip` automatically runs `bisect next` (Fossil's `auto-next` behavior), checking out the midpoint version. This updates both the working directory files AND the checkout DB (`vfile` table rebuilt from midpoint manifest, `vvar.checkout` updated to midpoint RID). Requires the `repo co` refactor (see Prerequisites).

### Package

- `go-libfossil/path/` — BFS path-finding over plink
- `go-libfossil/bisect/` — bisect state management
- `cmd/edgesync/repo_bisect.go` — CLI commands

### Reference

- `fossil/src/bisect.c` — Fossil's bisect implementation
- `fossil/src/path.c` — `path_shortest()` implementation

## 5. Branch Operations

### Commands

| Command | Description |
|---------|-------------|
| `repo branch ls` | List branches |
| `repo branch new NAME` | Create new branch |
| `repo branch close NAME` | Close a branch |

### How Fossil Branches Work

- A branch is a propagating tag `sym-<branchname>` on a checkin.
- `tagxref` tracks tag assignments: `tagtype=2` = propagating (active), `tagtype=0` = cancelled.
- Branch creation = new checkin with `sym-<name>` propagating tag + `branch=<name>` singleton tag.
- Branch closing = cancel `sym-<name>` tag (tagtype=0) + add `closed` tag.

### Behavior

- **ls**: Query `tagxref` + `tag` for `sym-*` tags with `tagtype=2`. Show branch name, tip UUID, date, user. Flags: `--closed` (closed only), `--all` (both).
- **new NAME**: Read ALL files from parent manifest (full carry-forward), create checkin with `sym-<name>` propagating tag and `branch=<name>` singleton tag via the new `Tags` field in `CheckinOpts`. Optional `--from VERSION` and `-m` message.
- **close NAME**: Create a control artifact using `go-libfossil/tag/` package. The control artifact contains: T-card with `-sym-<name>` (cancel propagating tag), T-card with `+closed`, D-card (timestamp), U-card (user), Z-card (checksum). Store as blob, crosslink by updating `tagxref` (set `tagtype=0` for the branch tag, add `closed` tag).

### Prerequisites

- `manifest.CheckinOpts` must have a `Tags []deck.TagCard` field so `branch new` can inject custom tags into the checkin manifest.
- `manifest.Checkin()` must carry forward all parent files when the caller doesn't pass explicit files (or a new helper reads parent files).
- `go-libfossil/tag/` package must exist for creating control artifacts and updating `tagxref`.

### Package

- `go-libfossil/tag/` — control artifact creation and tagxref management
- `cmd/edgesync/repo_branch.go` — CLI commands

### Reference

- `fossil/src/branch.c` — Fossil's branch implementation

## Testing Strategy

### Unit Tests (table-driven, isolated)

| Package | Tests |
|---------|-------|
| `go-libfossil/stash/` | Save/apply/pop/drop with added, removed, edited, renamed files. Delta round-trip verification. |
| `go-libfossil/undo/` | Save/restore/redo cycle. Vfile swap correctness. File content restoration. Single-level replacement. |
| `go-libfossil/annotate/` | Line attribution against known histories. Multi-version walks. Limit/origin cutoffs. Single-commit files. Unchanged files across versions. |
| `go-libfossil/path/` | BFS on constructed plink graphs. Linear chains. Diamond merges. Skip-set exclusions. Disconnected nodes. |
| `go-libfossil/bisect/` | State management. Log parsing. Midpoint selection. Full bisect sessions to convergence. |
| `go-libfossil/tag/` | Control artifact creation. Tagxref updates. Cancel/add propagating tags. |

### Integration Tests (real repos, end-to-end)

| Feature | Flow |
|---------|------|
| Stash | commit -> edit -> stash save -> verify clean -> stash pop -> verify restored |
| Undo | add files -> undo -> verify reverted -> redo -> verify re-applied |
| Annotate | multi-commit file history -> annotate -> verify line attributions |
| Bisect | linear history of N commits -> bisect good/bad -> verify convergence |
| Branch | create -> list -> verify appears -> close -> verify closed |

### Test Helpers

Shared test utilities for creating temp repos with known commit histories.

### Coverage

Every public function, every subcommand, every error path (bad input, missing checkout, empty repo).

## File Summary

### New Files

| File | Purpose |
|------|---------|
| `go-libfossil/stash/stash.go` | Stash save/apply/drop logic |
| `go-libfossil/stash/stash_test.go` | Stash unit tests |
| `go-libfossil/undo/undo.go` | Undo save/restore/redo logic |
| `go-libfossil/undo/undo_test.go` | Undo unit tests |
| `go-libfossil/annotate/annotate.go` | Annotation engine |
| `go-libfossil/annotate/annotate_test.go` | Annotate unit tests |
| `go-libfossil/path/path.go` | BFS path-finding over plink |
| `go-libfossil/path/path_test.go` | Path unit tests |
| `go-libfossil/bisect/bisect.go` | Bisect state management |
| `go-libfossil/bisect/bisect_test.go` | Bisect unit tests |
| `go-libfossil/tag/tag.go` | Control artifact creation, tagxref management |
| `go-libfossil/tag/tag_test.go` | Tag/control artifact unit tests |
| `cmd/edgesync/repo_stash.go` | Stash CLI commands |
| `cmd/edgesync/repo_undo.go` | Undo/redo CLI commands |
| `cmd/edgesync/repo_annotate.go` | Annotate/blame CLI commands |
| `cmd/edgesync/repo_bisect.go` | Bisect CLI commands |
| `cmd/edgesync/repo_branch.go` | Branch CLI commands |

### Modified Files

| File | Change |
|------|--------|
| `cmd/edgesync/cli.go` | Add Stash, Undo, Redo, Annotate, Blame, Bisect, Branch to RepoCmd |
| `cmd/edgesync/repo_revert.go` | Call `undoSave()` before revert |
| `cmd/edgesync/repo_add.go` | Call `undoSave()` before add |
| `cmd/edgesync/repo_rm.go` | Call `undoSave()` before rm |
| `cmd/edgesync/repo_rename.go` | Call `undoSave()` before rename |
| `cmd/edgesync/repo_co.go` | Refactor to update checkout DB (vfile/vvar) + call `undoSave()` before checkout |
| `go-libfossil/manifest/manifest.go` | Add `Tags []deck.TagCard` to `CheckinOpts`; support full parent file carry-forward |
