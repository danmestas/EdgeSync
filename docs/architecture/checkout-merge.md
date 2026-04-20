# Checkout, Merge & Working Tree

## Checkout Architecture

The `libfossil/checkout/` package is a clean-room port of libfossil's 43 public checkout functions into ~30 methods on `*Checkout`. All filesystem I/O goes through `simio.Env` (Storage, Clock, Rand) -- no build tags, platform-agnostic, WASM-ready.

```go
type Checkout struct {
    db   *sql.DB       // checkout DB (.fslckout / _FOSSIL_)
    repo *repo.Repo    // caller-owned, Close() does not close repo
    env  *simio.Env    // OSStorage | MemStorage | OPFS adapter
    obs  Observer      // nil -> nopObserver (zero-cost)
    dir  string        // checkout root directory
}
```

Key accessors: `Version()` returns current RID + UUID. `Dir()` returns root path. `Repo()` returns linked repo. `ValidateFingerprint()` verifies checkout/repo consistency.

**Schema** -- Fossil-compatible `vfile`, `vmerge`, `vvar` tables in `.fslckout`. Checkout DB uses raw `*sql.DB` (no `db.DB` wrapper). `stash/` and `undo/` continue to take `*sql.DB` directly -- no circular dependency.

**`simio.Storage` extension** -- `ReadDir(path string) ([]fs.DirEntry, error)` added for `ScanChanges` directory walking.

**Observer** -- separate `checkout.Observer` interface (extract/scan/commit lifecycle hooks). OTel implementation in `leaf/telemetry/checkout_observer.go`. libfossil stays OTel-free.

## Checkout Operations

| Operation | Method | Notes |
|-----------|--------|-------|
| Open existing | `Open(r, dir, OpenOpts)` | SearchParents option for .fslckout discovery |
| Create new | `Create(r, dir, CreateOpts)` | From repo, caller owns repo lifecycle |
| Extract files | `Extract(rid, ExtractOpts)` | Materialize checkin to disk via Storage |
| Update | `Update(UpdateOpts)` | Update to tip, 3-way merge local edits |
| Scan changes | `ScanChanges(flags)` | Detect changed/missing/extra files |
| Track/untrack | `Manage()` / `Unmanage()` | Add/remove from vfile |
| Stage/unstage | `Enqueue()` / `Dequeue()` | Staging queue for commit |
| Revert | `Revert(RevertOpts)` | Restore files from repo baseline |
| Rename | `Rename(RenameOpts)` | Update vfile + optional fs move |

CLI commands become thin wrappers: flag parsing + output formatting, delegating to checkout methods.

## Checkin Flow

`Commit(CommitOpts)` orchestrates: scan changes, run `PreCommitCheck` hook (if set), build file list from vfile + disk via Storage, delegate to `manifest.Checkin()` for manifest artifact creation.

Non-materialized flows (browser single-file edit) call `manifest.Checkin()` directly -- no checkout required.

`CommitOpts` includes: Message, User, Branch, Tags, Delta, Time, `PreCommitCheck func() error`.

## Merge Strategies

All strategies implement the `Strategy` interface:

```go
type Strategy interface {
    Name() string
    Merge(base, local, remote []byte) (*Result, error)
}
```

| Strategy | Behavior | Conflicts? |
|----------|----------|------------|
| `ThreeWayText` | Myers diff, line-level 3-way merge | Yes -- conflict markers + sidecars |
| `LastWriterWins` | Pick newer by `event.mtime` | Never |
| `Binary` | Always conflict, user picks manually | Always |
| `ConflictFork` | Preserve all versions in `conflict` table (offline-first) | Deferred |

**Strategy resolution** (priority order): CLI `--strategy` flag, `.edgesync-merge` file (glob patterns, first-match), config table `merge-strategy` key (default: `three-way`).

**Conflict storage** -- strategy-dependent:
- Standard strategies: Fossil-native `vfile.chnged=5`, sidecar files (`.LOCAL`, `.BASELINE`, `.MERGE`), conflict markers. Visible to `fossil changes`.
- ConflictFork: `conflict` table in repo DB (only created when strategy is active). Rows deleted on resolution.

**Fork detection**: `DetectForks()` queries the `leaf` table for multiple leaves, finds common ancestors via BFS on `plink`. SQL-only -- no file content read.

## Fork Prevention & Autosync

### WouldFork Detection (`checkout/fork.go`)

```go
func (c *Checkout) WouldFork() (bool, error)    // true if another leaf on same branch
func BranchLeaves(r *repo.Repo, branch string) ([]libfossil.FslID, error)
```

`WouldFork` queries `leaf` + `tagxref` for same-branch leaves excluding current checkout RID. Trunk identified by `branch` tag value, not `sym-trunk`.

### PreCommitCheck Hook

Optional `func() error` on `CommitOpts`. Called after `ScanChanges`, before `manifest.Checkin()`. The agent injects `WouldFork()` here; other consumers can use it for custom validation.

### ci-lock Protocol

Wire format: `pragma ci-lock PARENT-UUID CLIENT-ID` / `pragma ci-lock-fail HOLDING-USER LOCK-TIMESTAMP`. Handled as pragma cards in existing dispatch (like `uv-hash`).

Server storage: repo `config` table, key `edgesync-ci-lock-<PARENT-UUID>`, JSON value with clientid/login/mtime. Stale locks expire by timeout (default 60s) or when parent is no longer a leaf.

Client: `SyncOpts.CkinLock` requests lock; `SyncResult.CkinLockFail` reports conflict.

### Autosync Workflow (`leaf/agent/autosync.go`)

| Mode | Behavior |
|------|----------|
| `AutosyncOff` | Direct `co.Commit()`, no sync |
| `AutosyncOn` | Pull + lock + fork-check + commit + push |
| `AutosyncPullOnly` | Pull + lock + fork-check + commit (no push) |

Sequence: pre-pull with ci-lock request, check lock result, inject `WouldFork` as PreCommitCheck, commit, post-push (if on), post-fork warning.

**Error semantics:**

| Step | On failure |
|------|------------|
| Pre-pull | Abort, no commit |
| Lock held | Abort with `ErrCkinLockHeld` |
| Would fork | Abort with `ErrWouldFork` |
| Commit | Abort, no push |
| Post-sync | Warn only (commit succeeded) |
| Post-fork | Warn only (commit succeeded) |

**Escape hatches:** `--allow-fork` bypasses fork + lock checks. `--override-lock` implies `--allow-fork`. `--branch` skips both (new branch cannot fork existing one).

**ClientID:** UUID v4 via `simio.Rand`, persisted in repo config table as `client-id`.

## Stash, Undo, Annotate, Bisect

### Stash (`libfossil/stash/`)

Schema: `stash` + `stashfile` tables in checkout DB. `stash.hash` = checkout manifest UUID at save time. `stashfile.delta` = delta from baseline to working content. Save computes deltas via `delta/` package, reverts working dir. Pop/apply call `undoSave()` first.

### Undo (`libfossil/undo/`)

Single-level undo/redo via `undo`, `undo_vfile`, `undo_vmerge` tables. State machine: `undo_available` in vvar (0=none, 1=undo, 2=redo). Swap mechanism: vfile <-> undo_vfile, vmerge <-> undo_vmerge, restore file content. Called before: checkout, revert, add, rm, rename, stash pop/apply.

### Annotate (`libfossil/annotate/`)

Walk parent chain via `plink` + `mlink`, diff each ancestor with Myers, push `iVers` markers back. Track renames via `mlink.fnid` + `filename.name`. Options: `--limit`, `--origin`, `--version`.

### Bisect (`libfossil/bisect/`)

State in vvar: `bisect-good`, `bisect-bad`, `bisect-log`. BFS path-finding over `plink` (unweighted, matches Fossil's `path_shortest()`). Auto-next on good/bad/skip. Converges when good and bad are adjacent.

### Branch (`libfossil/tag/`)

Branches are propagating `sym-<name>` tags. Creation = checkin with `sym-<name>` (propagating) + `branch=<name>` (singleton) via `CheckinOpts.Tags`. Closing = control artifact cancelling the sym tag + adding `closed`.

## Key Constraints

- **libfossil is transport-agnostic** -- no transport imports, no operational endpoints. Fork detection and PreCommitCheck are primitives; the agent owns the workflow.
- **Fossil-compatible schema** -- vfile, vmerge, vvar, stash, undo tables match Fossil's layout for interoperability.
- **simio.Env everywhere** -- all I/O through Storage/Clock/Rand. No build tags in checkout package.
- **TigerStyle assertions** -- precondition panics for nil/invalid arguments on all public methods.
- **Observer pattern** -- lifecycle hooks in libfossil, OTel implementation in leaf/telemetry.
