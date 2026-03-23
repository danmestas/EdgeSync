# Checkout Package Design — CDG-152

**Date:** 2026-03-23
**Branch:** `feature/cdg-152-implement-checkoutworking-directory-layer-in-go-libfossil`
**Status:** Design
**Linear:** [CDG-152](https://linear.app/craft-design-group/issue/CDG-152/implement-checkoutworking-directory-layer-in-go-libfossil)

## Summary

Clean-room port of libfossil's checkout API into `go-libfossil/checkout/`. Provides working directory management — extract files from a checkin, track changes, stage edits, commit back to the repo. All filesystem operations go through `simio.Env` (Storage, Clock, Rand), making the package platform-agnostic with no build tags.

## Context

go-libfossil is repo-only. There is no library-level ability to check out files to a working directory, detect changes, stage edits, or commit from a working tree. The CLI (`cmd/edgesync/`) has partial checkout logic baked into command handlers (`repo_open.go`, `repo_co.go`, `repo_status.go`, etc.), but this is not reusable by the leaf agent or browser WASM.

libfossil exposes 43 public checkout functions in `checkout.h`. The `stash/` and `undo/` packages already operate on the checkout DB (`.fslckout`) directly. The browser WASM playground (`spike/opfs-vfs`) has a spike-quality `Checkout` struct that materializes files to OPFS — this design replaces that with a proper library.

## Design Decisions

1. **Clean-room port from libfossil** — not an extraction from CLI. Go-idiomatic API mapping libfossil's 43 functions to ~30 methods on `*Checkout`.

2. **`simio.Env` for all I/O** — no build tags, no `//go:build !js`. Filesystem ops go through `env.Storage` (OSStorage for native, MemStorage for tests, OPFS adapter for browser). Time through `env.Clock`. Consistent with `repo.CreateWithEnv`, `sync.Clone`.

3. **Separate `checkout.Observer` interface** — lifecycle hooks for extract, scan, commit. OTel implementation in `leaf/telemetry/checkout_observer.go`. go-libfossil stays OTel-free.

4. **`Commit()` delegates to `manifest.Checkin()`** — checkout owns the staging queue and working-directory-aware orchestration. `manifest.Checkin()` owns manifest artifact creation. Non-materialized flows (browser editing a single file without full checkout) call `manifest.Checkin()` directly.

5. **Fossil-compatible schema** — vfile, vmerge, vvar tables match Fossil's layout exactly so checkout DBs are interoperable.

6. **Extend `simio.Storage` with `ReadDir`** — `ScanChanges` needs to walk the checkout directory to detect extra/missing files. `simio.Storage` currently lacks directory listing. Add `ReadDir(path string) ([]fs.DirEntry, error)` to the interface, implement in `OSStorage` (delegates to `os.ReadDir`) and `MemStorage` (synthesizes entries from the in-memory map).

7. **Checkout DB uses `*sql.DB` (stdlib)** — the checkout DB is a separate SQLite file from the repo (`.fslckout`), opened directly via `database/sql`. It does not need the `db.DB` wrapper or multi-driver abstraction — the driver is already registered by the importing binary. This matches how `stash/` and `undo/` take `*sql.DB` today.

8. **Both `Open` and `Create` take `*repo.Repo`** — the caller owns the repo lifecycle. `Checkout.Close()` closes only the checkout DB, not the repo. This avoids ownership ambiguity and matches Go convention (caller opens, caller closes).

9. **TigerStyle assertions** — all public methods use precondition panics for nil/invalid arguments, consistent with the rest of go-libfossil (`repo.go`, `manifest.go`, `stash.go`).

## Package Structure

```
go-libfossil/checkout/
├── checkout.go       # Open, Create, Extract, Close, version info, fingerprint
├── update.go         # Update with merge, CalcUpdateVersion
├── vfile.go          # Load, unload, changes scan, pathname ops
├── manage.go         # Add/remove files from SCM tracking (manage/unmanage)
├── checkin.go        # Enqueue, dequeue, commit (delegates to manifest.Checkin)
├── revert.go         # Revert changes
├── rename.go         # Rename tracked files
├── status.go         # VisitChanges, HasChanges
├── schema.go         # Table DDL, EnsureTables
├── observer.go       # Observer interface + nopObserver
├── types.go          # Enums, option structs, ChangeEntry
```

## Core Type

```go
type Checkout struct {
    db   *sql.DB       // checkout DB (.fslckout / _FOSSIL_)
    repo *repo.Repo    // linked repository
    env  *simio.Env    // Storage, Clock, Rand
    obs  Observer      // nil → nopObserver
    dir  string        // checkout root directory
}
```

All methods are on `*Checkout`. No free functions that take raw `*sql.DB` (unlike stash/undo today). The struct is the single entry point.

## API Surface

### Opening & Lifecycle

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `Open(r, dir, OpenOpts)` | `fsl_ckout_open_dir` + `fsl_repo_open_ckout` | Open existing checkout |
| `Create(r, dir, CreateOpts)` | `fsl_repo_open_ckout` | Create new checkout from repo |
| `Close()` | — | Close checkout and repo DBs |
| `Dir()` | — | Return checkout root directory |
| `Repo()` | — | Return linked repo |
| `Version()` | `fsl_ckout_version_info` | Current checkout RID + UUID |
| `ValidateFingerprint()` | `fsl_ckout_fingerprint_check` | Verify checkout matches repo |

```go
func Open(r *repo.Repo, dir string, opts OpenOpts) (*Checkout, error)
func Create(r *repo.Repo, dir string, opts CreateOpts) (*Checkout, error)
func (c *Checkout) Close() error
func (c *Checkout) Dir() string
func (c *Checkout) Repo() *repo.Repo
func (c *Checkout) Version() (rid libfossil.FslID, uuid string, err error)
func (c *Checkout) ValidateFingerprint() error
```

### Extract & Update

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `Extract(rid, ExtractOpts)` | `fsl_repo_ckout` | Write files to disk from checkin |
| `Update(UpdateOpts)` | `fsl_ckout_update` | Update to new version, merge local edits |
| `CalcUpdateVersion()` | `fsl_ckout_calc_update_version` | Find tip of current branch |

```go
func (c *Checkout) Extract(rid libfossil.FslID, opts ExtractOpts) error
func (c *Checkout) Update(opts UpdateOpts) error
func (c *Checkout) CalcUpdateVersion() (libfossil.FslID, error)
```

### Vfile Operations

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `LoadVFile(rid, clear)` | `fsl_vfile_load` | Populate vfile from manifest |
| `UnloadVFile(rid)` | `fsl_vfile_unload` | Clear vfile for version |
| `ScanChanges(flags)` | `fsl_vfile_changes_scan` | Detect changed/missing files |

```go
func (c *Checkout) LoadVFile(rid libfossil.FslID, clear bool) (missing uint32, err error)
func (c *Checkout) UnloadVFile(rid libfossil.FslID) error
func (c *Checkout) ScanChanges(flags ScanFlags) error
```

### File Management

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `Manage(ManageOpts)` | `fsl_ckout_manage` | Add files to tracking |
| `Unmanage(UnmanageOpts)` | `fsl_ckout_unmanage` | Remove files from tracking |

```go
func (c *Checkout) Manage(opts ManageOpts) (*ManageCounts, error)
func (c *Checkout) Unmanage(opts UnmanageOpts) error
```

### Status & Changes

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `HasChanges()` | `fsl_ckout_has_changes` | Quick check (DB only) |
| `VisitChanges(vid, scan, fn)` | `fsl_ckout_changes_visit` | Walk all changes with visitor |

```go
func (c *Checkout) HasChanges() (bool, error)
func (c *Checkout) VisitChanges(vid libfossil.FslID, scan bool, fn ChangeVisitor) error
```

### Staging & Commit

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `Enqueue(EnqueueOpts)` | `fsl_checkin_enqueue` | Stage files for commit |
| `Dequeue(DequeueOpts)` | `fsl_checkin_dequeue` | Unstage files |
| `IsEnqueued(name)` | `fsl_checkin_is_enqueued` | Check if file staged |
| `DiscardQueue()` | `fsl_checkin_discard` | Clear staging queue |
| `Commit(CommitOpts)` | `fsl_checkin_commit` | Create checkin from staged files |

```go
func (c *Checkout) Enqueue(opts EnqueueOpts) error
func (c *Checkout) Dequeue(opts DequeueOpts) error
func (c *Checkout) IsEnqueued(name string) (bool, error)
func (c *Checkout) DiscardQueue() error
func (c *Checkout) Commit(opts CommitOpts) (rid libfossil.FslID, uuid string, err error)
```

`Commit()` reads staged files from vfile + disk via Storage, builds `manifest.CheckinOpts`, and delegates to `manifest.Checkin()`. Non-materialized flows (e.g., browser editing a single file without full checkout) call `manifest.Checkin()` directly — no checkout required.

### Revert, Rename, Manifest

| Method | libfossil equivalent | Purpose |
|--------|---------------------|---------|
| `Revert(RevertOpts)` | `fsl_ckout_revert` | Revert file changes |
| `Rename(RenameOpts)` | `fsl_ckout_rename` | Rename tracked file |
| `RevertRename(name, doFsMove)` | `fsl_ckout_rename_revert` | Undo pending rename |
| `WriteManifest(flags)` | `fsl_ckout_manifest_write` | Write manifest files to checkout |
| `FileContent(name)` | `fsl_ckout_file_content` | Read file from checkout |

```go
func (c *Checkout) Revert(opts RevertOpts) error
func (c *Checkout) Rename(opts RenameOpts) error
func (c *Checkout) RevertRename(name string, doFsMove bool) (bool, error)
func (c *Checkout) WriteManifest(flags ManifestFlags) error
func (c *Checkout) FileContent(name string) ([]byte, error)
```

### Utilities

| Function | libfossil equivalent | Purpose |
|----------|---------------------|---------|
| `CheckFilename(name)` | `fsl_ckout_filename_check` | Canonicalize + validate path |
| `IsRootedIn(absPath)` | `fsl_is_rooted_in_ckout` | Check path within checkout |
| `FindCheckoutDB(storage, dir, searchParents)` | `fsl_ckout_db_search` | Locate .fslckout |
| `PreferredDBName()` | `fsl_preferred_ckout_db_name` | Platform DB name |
| `DBNames()` | `fsl_ckout_dbnames` | All valid DB names |

```go
func (c *Checkout) CheckFilename(name string) (string, error)
func (c *Checkout) IsRootedIn(absPath string) bool
func FindCheckoutDB(storage simio.Storage, dir string, searchParents bool) (string, error)
func PreferredDBName() string
func DBNames() []string
```

## Types & Enums

### Enums

```go
// VFileChange — vfile.chnged column values (matches Fossil's vfile states)
type VFileChange int
const (
    VFileNone        VFileChange = 0  // unchanged
    VFileMod         VFileChange = 1  // modified
    VFileMergeMod    VFileChange = 2  // modified via merge
    VFileMergeAdd    VFileChange = 3  // added via merge
    VFileIntMod      VFileChange = 4  // modified via integrate
    VFileIntAdd      VFileChange = 5  // added via integrate
    VFileIsExec      VFileChange = 6  // became executable
    VFileBecameLink  VFileChange = 7  // became symlink
    VFileNotExec     VFileChange = 8  // lost executable
    VFileNotLink     VFileChange = 9  // lost symlink
)

// FileChange — checkout-level change types (maps to fsl_ckout_change_e)
type FileChange int
const (
    ChangeNone    FileChange = iota
    ChangeAdded
    ChangeRemoved
    ChangeMissing
    ChangeRenamed
    ChangeModified
)

// UpdateChange — file states during extract/update (maps to fsl_ckup_fchange_e)
type UpdateChange int
const (
    UpdateNone              UpdateChange = iota
    UpdateAdded
    UpdateAddPropagated
    UpdateRemoved
    UpdateRmPropagated
    UpdateUpdated
    UpdateUpdatedBinary
    UpdateMerged
    UpdateConflictMerged
    UpdateConflictAdded
    UpdateConflictUnmanaged
    UpdateConflictRm
    UpdateConflictSymlink
    UpdateConflictBinary
    UpdateRenamed
    UpdateEdited
)

// RevertChange — revert operation types (maps to fsl_ckout_revert_e)
type RevertChange int
const (
    RevertNone        RevertChange = iota
    RevertUnmanage
    RevertRemove
    RevertRename
    RevertPermissions
    RevertContents
)

// ScanFlags — controls ScanChanges behavior
type ScanFlags uint32
const (
    ScanHash       ScanFlags = 1 << iota  // hash file content (not just mtime)
    ScanENotFile                           // mark non-regular files
    ScanSetMTime                           // update mtime in vfile
    ScanKeepOthers                         // keep other version entries
)

// ManifestFlags — controls WriteManifest behavior
type ManifestFlags int
const (
    ManifestMain ManifestFlags = 1 << iota  // write manifest file
    ManifestUUID                             // write manifest.uuid file
)
```

### Option Structs

```go
type OpenOpts struct {
    Env           *simio.Env  // nil → RealEnv
    Observer      Observer    // nil → nopObserver
    SearchParents bool        // search parent dirs for .fslckout
}

type CreateOpts struct {
    Env      *simio.Env  // nil → RealEnv
    Observer Observer    // nil → nopObserver
}

type ExtractOpts struct {
    Callback func(name string, change UpdateChange) error  // per-file notification
    SetMTime bool
    DryRun   bool
    Force    bool  // overwrite locally modified files
}

type UpdateOpts struct {
    TargetRID libfossil.FslID  // 0 → auto-calculate via CalcUpdateVersion
    Callback  func(name string, change UpdateChange) error
    SetMTime  bool
    DryRun    bool
}

type ManageOpts struct {
    Paths    []string
    Callback func(name string, added bool) error
}

type ManageCounts struct {
    Added, Updated, Skipped int
}

type UnmanageOpts struct {
    Paths    []string
    VFileIDs []libfossil.FslID  // alternative: pass IDs directly
    Callback func(name string) error
}

type EnqueueOpts struct {
    Paths    []string
    Callback func(name string) error
}

type DequeueOpts struct {
    Paths []string  // empty → dequeue all
}

type CommitOpts struct {
    Message     string
    User        string
    Branch      string     // empty → current branch
    Tags        []string   // additional T-cards
    Delta       bool
    Time        time.Time  // zero → env.Clock.Now()
}

type RevertOpts struct {
    Paths    []string  // empty → revert all
    Callback func(name string, change RevertChange) error
}

type RenameOpts struct {
    From, To string
    DoFsMove bool  // also move on filesystem via Storage
    Callback func(from, to string) error
}

type ChangeEntry struct {
    Name     string
    Change   FileChange
    VFileID  libfossil.FslID
    IsExec   bool
    IsLink   bool
    OrigName string  // non-empty if renamed
}

type ChangeVisitor func(entry ChangeEntry) error
```

## Observer Interface

Follows the exact pattern from `sync/observer.go` — lifecycle hooks, zero-cost nop default, OTel implementation in `leaf/telemetry/`.

```go
type Observer interface {
    ExtractStarted(ctx context.Context, e ExtractStart) context.Context
    ExtractFileCompleted(ctx context.Context, name string, change UpdateChange)
    ExtractCompleted(ctx context.Context, e ExtractEnd)
    ScanStarted(ctx context.Context) context.Context
    ScanCompleted(ctx context.Context, e ScanEnd)
    CommitStarted(ctx context.Context, e CommitStart) context.Context
    CommitCompleted(ctx context.Context, e CommitEnd)
    Error(ctx context.Context, err error)
}
```

### Event Structs

```go
type ExtractStart struct {
    Operation string         // "extract" or "update"
    TargetRID libfossil.FslID
}

type ExtractEnd struct {
    Operation    string
    TargetRID    libfossil.FslID
    FilesWritten int
    FilesRemoved int
    Conflicts    int
    Err          error
}

type ScanEnd struct {
    FilesScanned int
    FilesChanged int
    FilesMissing int
    FilesExtra   int
}

type CommitStart struct {
    FilesEnqueued int
    Branch        string
    User          string
}

type CommitEnd struct {
    RID         libfossil.FslID
    UUID        string
    FilesCommit int
    Err         error
}
```

### nopObserver

```go
type nopObserver struct{}

func (nopObserver) ExtractStarted(ctx context.Context, _ ExtractStart) context.Context { return ctx }
// ... all methods empty (~2ns per call)

func resolveObserver(o Observer) Observer {
    if o == nil { return nopObserver{} }
    return o
}
```

### OTel Implementation

In `leaf/telemetry/checkout_observer.go`:

| Hook | Span/Metric |
|------|-------------|
| `ExtractStarted` | span `checkout.extract`, attr `checkout.target_rid` |
| `ExtractFileCompleted` | span event `checkout.file` with name + change type |
| `ExtractCompleted` | close span, metrics: `checkout.files_written`, `checkout.files_removed`, `checkout.conflicts` |
| `ScanStarted/Completed` | span `checkout.scan`, metrics: file counts |
| `CommitStarted/Completed` | span `checkout.commit`, attrs: `checkout.rid`, `checkout.uuid`, `checkout.branch` |
| `Error` | `span.AddEvent("checkout.error")`, `span.SetStatus(codes.Error)` |

## Schema

Checkout DB (`.fslckout` or `_FOSSIL_`) — Fossil-compatible tables:

```sql
CREATE TABLE IF NOT EXISTS vfile(
  id       INTEGER PRIMARY KEY,
  vid      INTEGER NOT NULL,
  chnged   INTEGER DEFAULT 0,
  deleted  INTEGER DEFAULT 0,
  isexe    INTEGER DEFAULT 0,
  islink   INTEGER DEFAULT 0,
  rid      INTEGER DEFAULT 0,
  mrid     INTEGER DEFAULT 0,
  mtime    INTEGER DEFAULT 0,
  pathname TEXT NOT NULL,
  origname TEXT,
  mhash    TEXT,
  UNIQUE(pathname, vid)
);

CREATE TABLE IF NOT EXISTS vmerge(
  id    INTEGER REFERENCES vfile,
  merge INTEGER,
  mhash TEXT
);

CREATE TABLE IF NOT EXISTS vvar(
  name  TEXT PRIMARY KEY,
  value TEXT
);
```

Key vvar entries: `repository` (repo path), `checkout` (current RID), `checkout-hash` (current UUID), `undo_available` (0/1/2), `undo_checkout` (saved RID).

## Dependency Graph

```
checkout/
  ├── repo/       (open repo, DB access)
  ├── manifest/   (ListFiles, Checkin, Crosslink, Log)
  ├── content/    (Expand delta chains)
  ├── blob/       (Exists, Store, Load)
  ├── hash/       (SHA1, SHA3, ContentHash)
  ├── delta/      (Create, Apply)
  ├── merge/      (ThreeWayText — for Update)
  ├── simio/      (Env, Storage, Clock, Rand)
  ├── deck/       (Parse manifests for vfile load)
  └── tag/        (ApplyTag — for commit T-cards)
```

`stash/` and `undo/` continue to take raw `*sql.DB` for the checkout DB. No circular dependency. A future cleanup could have them take `*Checkout`, but that is out of scope.

CLI commands (`cmd/edgesync/repo_*.go`) become thin wrappers: flag parsing + output formatting, delegating to checkout methods.

## Testing Strategy

### Unit tests (`checkout/*_test.go`)

Use `testutil.NewTestRepo()` for repo setup, `simio.MemStorage` for filesystem operations. No real disk I/O.

| Area | Test cases |
|------|------------|
| Schema | `EnsureTables` creates tables, idempotent on re-call |
| Open/Create | Round-trip: create, close, reopen, verify vvar state |
| Extract | Materialize files, verify via MemStorage reads, verify vfile rows |
| LoadVFile | Populate from manifest, verify row count/pathnames/RIDs |
| ScanChanges | Modify file in MemStorage, scan, verify `vfile.chnged` updated |
| Manage/Unmanage | Add new file → vfile row; remove → deleted |
| Enqueue/Dequeue/Commit | Stage, commit, verify manifest blob + parent linkage |
| Revert | Modify + revert → vfile reset, disk restored |
| Rename | Rename → verify vfile.pathname + origname |
| Update | Two divergent checkins, update, verify 3-way merge applied |
| VisitChanges | Known state (modified/added/deleted), verify visitor entries |
| Observer | Recording observer verifies hooks fire at correct lifecycle points |

### DST integration (`dst/`)

- Checkout after clone: clone via HandleSync, extract, verify content
- Commit + sync convergence: edit on leaf A, sync to leaf B
- Concurrent checkout + sync: local edit while sync delivers blobs

### Sim integration (`sim/`)

- End-to-end: create repo, checkin, open checkout, extract, modify, commit, sync, verify convergence

## CLI Impact

Existing CLI commands become thin wrappers:

| CLI Command | Calls |
|-------------|-------|
| `repo open` | `checkout.Create()` |
| `repo co` | `c.Extract()` |
| `repo status` | `c.VisitChanges()` |
| `repo add` | `c.Manage()` |
| `repo rm` | `c.Unmanage()` |
| `repo rename` | `c.Rename()` |
| `repo revert` | `c.Revert()` |
| `repo ci` | `c.Enqueue()` + `c.Commit()` |
| `repo diff` | `c.VisitChanges()` + diff formatting |

CLI retains: flag parsing, output formatting, `openRepo()` helper, `currentUser()`.

## Prerequisites

**Extend `simio.Storage` interface** — add `ReadDir(path string) ([]fs.DirEntry, error)` before starting checkout implementation. Required by `ScanChanges` to walk the checkout directory and detect extra/missing files.

- `OSStorage.ReadDir` → delegates to `os.ReadDir`
- `MemStorage.ReadDir` → synthesizes `fs.DirEntry` from the in-memory map, deduplicating directory prefixes

This is a small, self-contained change to `simio/storage.go` that unblocks the checkout package.

## Out of Scope

- Refactoring `stash/` and `undo/` to take `*Checkout` (future cleanup)
- Ignore-glob filtering (can be added to `ManageOpts` later)
- Full merge conflict UI (Update detects conflicts, CLI presents them)
- Migrating existing CLI tests to library tests (follow-up)
