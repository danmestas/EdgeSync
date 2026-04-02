# Autosync & Fork Prevention Design

**Date:** 2026-04-02  
**Tickets:** EDG-1 (Autosync-on-commit), EDG-2 (Prevent "would fork" commits)  
**Status:** Draft

## Problem

Commits in go-libfossil succeed unconditionally — no sync, no fork check. A user can commit against a stale parent, creating an unintentional fork. Fossil prevents this with autosync (pull before commit, push after) and a check-in lock protocol that guards against races between concurrent committers.

## Design Principles

- **go-libfossil stays transport-agnostic.** It provides fork-detection primitives and a pre-commit hook point. It does not import any transport.
- **Leaf agent owns the workflow.** The pull → lock-check → fork-check → commit → push sequence lives in the agent, which knows its transport.
- **Match Fossil's behavior.** No auto-merge on fork — abort and tell the user to `update`. Provide `--allow-fork`, `--branch`, and `--override-lock` escape hatches.

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  leaf agent                      │
│                                                  │
│  autosync.Commit()                               │
│  ┌─────────────────────────────────────────────┐ │
│  │ 1. Pre-pull + ci-lock   (sync.Sync)         │ │
│  │ 2. Check lock result    (SyncResult)        │ │
│  │ 3. Commit               (checkout.Commit)   │ │
│  │    └─ PreCommitCheck → WouldFork()          │ │
│  │ 4. Post-sync + re-lock  (sync.Sync)         │ │
│  │ 5. Post-fork warning    (WouldFork)         │ │
│  └─────────────────────────────────────────────┘ │
└──────────┬──────────────────────┬────────────────┘
           │                      │
    ┌──────▼──────┐       ┌───────▼───────┐
    │ go-libfossil│       │  Transport    │
    │  checkout/  │       │  (HTTP/NATS/  │
    │  sync/      │       │   libp2p)     │
    │  xfer/      │       └───────────────┘
    └─────────────┘
```

## Part 1: go-libfossil Primitives

### 1.1 Fork Detection — `checkout/fork.go` (new file)

```go
package checkout

// WouldFork reports whether committing on the current branch would
// create a fork. Returns true when another leaf exists on the same
// branch that is not the current checkout version.
//
// If the checkout has no branch tag (trunk), checks trunk leaves.
// Returns false if checkout is the sole leaf on its branch.
func (c *Checkout) WouldFork() (bool, error)

// BranchLeaves returns the leaf RIDs for a named branch.
// A branch with >1 leaf is already forked.
// An empty branch name queries trunk.
func BranchLeaves(r *repo.Repo, branch string) ([]libfossil.FslID, error)
```

**`WouldFork()` implementation:**

1. Get current checkout RID via `c.Version()`
2. Determine current branch: query `tagxref` for propagating `branch` tag value on current RID (i.e., `tagname='branch'` with `tagtype=2`); default to `"trunk"` if no branch tag exists
3. Query same-branch leaves:
   ```sql
   SELECT l.rid FROM leaf l
   JOIN tagxref tx ON tx.rid = l.rid
   JOIN tag t ON t.tagid = tx.tagid
   WHERE t.tagname = 'branch'
     AND tx.value = ?
     AND tx.tagtype > 0
     AND l.rid != ?
   ```
   (Parameters: branch name, current checkout RID)
4. If any rows returned → would fork

Note: trunk checkins may not have an explicit `sym-trunk` tag. The `branch` tag with `value='trunk'` is the reliable way to identify branch membership, consistent with `branch.List()`.

**`BranchLeaves()` implementation:**

Same query without the `l.rid != ?` filter. Returns all leaf RIDs on the branch. Useful for diagnostics (showing which commits are the fork tips).

### 1.2 Pre-Commit Hook — `checkout/checkin.go`

Add optional callback to `CommitOpts`:

```go
type CommitOpts struct {
    Message        string
    User           string
    Branch         string
    Tags           []string
    Delta          bool
    Time           time.Time
    PreCommitCheck func() error // nil = no check
}
```

Note: `Commit()` currently has signature `func (c *Checkout) Commit(opts CommitOpts) (libfossil.FslID, string, error)` with no `context.Context` parameter. `PreCommitCheck` takes no arguments to match this — the agent closure captures everything it needs.

**Integration point in `Commit()`:**

After `ScanChanges()` completes but before `manifest.Checkin()` is called:

```go
func (c *Checkout) Commit(opts CommitOpts) (libfossil.FslID, string, error) {
    // ... existing: get parent, scan changes, collect vfile entries ...

    if opts.PreCommitCheck != nil {
        if err := opts.PreCommitCheck(); err != nil {
            return 0, "", fmt.Errorf("pre-commit check: %w", err)
        }
    }

    // ... existing: build commit files, manifest.Checkin, finalize ...
}
```

This is a general-purpose hook. The agent uses it for fork checking; other consumers can use it for custom validation.

### 1.3 ci-lock Protocol — `xfer/card.go`, `sync/handler.go`, `sync/client.go`

#### 1.3.1 Wire Format (xfer package)

Two new pragma cards:

```
pragma ci-lock PARENT-UUID CLIENT-ID
pragma ci-lock-fail HOLDING-USER LOCK-TIMESTAMP
```

These are handled as `PragmaCard` instances in the existing pragma dispatch — no new `CardType` enum values needed. The `ci-lock` and `ci-lock-fail` pragma names are parsed in the handler/client pragma switch, consistent with how `uv-hash` is handled today. Fields are positional arguments on the pragma line.

#### 1.3.2 Server-Side Lock Management (sync package)

**Storage:** Repo `config` table, key format `edgesync-ci-lock-<PARENT-UUID>`, value is JSON `{"clientid":"...","login":"...","mtime":unix_seconds}`. The `edgesync-` prefix avoids collision with any future upstream Fossil config keys.

**New file: `sync/ckin_lock.go`**

```go
package sync

const DefaultCkinLockTimeout = 60 * time.Second

// processCkinLock handles an incoming ci-lock pragma.
// Returns a CkinLockFailCard if another client holds the lock, or nil.
func processCkinLock(tx *db.Tx, req xfer.CkinLockCard, requestingUser string, timeout time.Duration) *xfer.CkinLockFailCard

// expireStaleLocks removes ci-lock entries older than timeout
// or whose parent is no longer a leaf.
func expireStaleLocks(tx *db.Tx, timeout time.Duration)

// isLeaf checks whether rid is in the leaf table (has no children).
func isLeaf(tx *db.Tx, rid libfossil.FslID) bool
```

**`processCkinLock` logic:**

1. Call `expireStaleLocks(tx, timeout)` — delete locks where:
   - `mtime + timeout < now`, OR
   - parent UUID's RID is no longer in the `leaf` table
2. Check for existing lock: `SELECT value FROM config WHERE name = 'edgesync-ci-lock-' || parentUUID`
3. If lock exists AND `clientid != req.ClientID`:
   - Return `&CkinLockFailCard{User: lock.login, Time: lock.mtime}`
4. Otherwise, upsert lock:
   ```sql
   REPLACE INTO config(name, value, mtime)
   VALUES('edgesync-ci-lock-'||?, json_object('clientid',?,'login',?,'mtime',?), ?)
   ```
5. Return nil (lock acquired)

**Integration in `HandleSync`:**

In the pragma dispatch section of `handler.go`, add a case for `ci-lock` pragmas. Call `processCkinLock` and, if it returns a fail card, append `pragma ci-lock-fail` to the response message.

#### 1.3.3 Client-Side Lock Request (sync package)

**New fields on `SyncOpts`:**

```go
type SyncOpts struct {
    // ... existing fields ...
    CkinLock *CkinLockReq // nil = no lock requested
}

// CkinLockReq requests a server-side check-in lock.
type CkinLockReq struct {
    ParentUUID string // UUID of the parent commit to lock
    ClientID   string // unique identifier for this agent instance
}
```

**New fields on `SyncResult`:**

```go
type SyncResult struct {
    // ... existing fields ...
    CkinLockFail *CkinLockFail // nil = no conflict (or no lock requested)
}

// CkinLockFail reports that another client holds the check-in lock.
type CkinLockFail struct {
    HeldBy string    // login name of the lock holder
    Since  time.Time // when the lock was acquired
}
```

**Client changes:**

- `buildRequest()`: if `opts.CkinLock != nil`, emit `pragma ci-lock PARENT-UUID CLIENT-ID`
- `processResponse()`: on receiving `pragma ci-lock-fail`, populate `result.CkinLockFail`

## Part 2: Leaf Agent Autosync Workflow

### 2.1 Types — `leaf/agent/autosync.go` (new file)

```go
package agent

// AutosyncMode controls the autosync behavior around commits.
type AutosyncMode int

const (
    AutosyncOff      AutosyncMode = iota // no sync around commits
    AutosyncOn                            // pull + commit + push
    AutosyncPullOnly                      // pull + commit (no push)
)

// AutosyncOpts configures the autosync workflow.
type AutosyncOpts struct {
    Mode        AutosyncMode
    Transport   sync.Transport
    SyncOpts    sync.SyncOpts   // auth, UV, etc.
    AllowFork   bool            // bypass fork + lock checks
    ClientID    string          // unique agent instance ID
}

var (
    // ErrWouldFork is returned when committing would create a fork.
    ErrWouldFork = errors.New(
        "would fork: run update first, or use --allow-fork or --branch")

    // ErrCkinLockHeld is returned when another client holds the
    // check-in lock on the parent commit.
    ErrCkinLockHeld = errors.New(
        "check-in lock held by another client")
)
```

### 2.2 Workflow — `autosync.Commit()`

```go
// Commit wraps checkout.Commit with autosync pull-before and push-after.
// When Mode is Off, delegates directly to co.Commit with no sync.
func Commit(ctx context.Context, co *checkout.Checkout,
    commitOpts checkout.CommitOpts, auto AutosyncOpts,
) (libfossil.FslID, string, error)
```

**Step-by-step flow:**

```
1. IF mode == Off:
      return co.Commit(commitOpts)

2. PRE-PULL + CI-LOCK:
      syncOpts := auto.SyncOpts
      syncOpts.Pull = true
      syncOpts.Push = false
      IF !allowFork && commitOpts.Branch == "":
          _, parentUUID, _ := co.Version()
          syncOpts.CkinLock = &sync.CkinLockReq{
              ParentUUID: parentUUID,
              ClientID:   auto.ClientID,
          }
      result, err := sync.Sync(ctx, co.Repo(), auto.Transport, syncOpts)
      IF err != nil:
          return 0, "", fmt.Errorf("autosync pre-pull: %w", err)

3. CHECK LOCK RESULT:
      IF result.CkinLockFail != nil && !allowFork:
          return 0, "", fmt.Errorf("%w: held by %q since %v",
              ErrCkinLockHeld,
              result.CkinLockFail.HeldBy,
              result.CkinLockFail.Since)

4. INJECT PRE-COMMIT CHECK:
      IF !allowFork && commitOpts.Branch == "":
          commitOpts.PreCommitCheck = func() error {
              forked, err := co.WouldFork()
              IF err != nil: return err
              IF forked: return ErrWouldFork
              return nil
          }

5. COMMIT:
      rid, uuid, err := co.Commit(commitOpts)
      IF err != nil:
          return 0, "", err

6. POST-SYNC (push + pull):
      IF mode == AutosyncOn:
          syncOpts.Pull = true
          syncOpts.Push = true
          syncOpts.CkinLock = nil  // lock served its purpose; parent is no longer a leaf
          postResult, postErr := sync.Sync(ctx, co.Repo(), auto.Transport, syncOpts)
          IF postErr != nil:
              // Commit succeeded — log warning, don't fail
              log.Warn("autosync post-push failed", "err", postErr)

7. POST-FORK WARNING:
      IF forked, _ := co.WouldFork(); forked:
          log.Warn("fork detected after commit")

8. RETURN (rid, uuid, nil)
```

**Error semantics summary:**

| Step | Failure | Behavior |
|------|---------|----------|
| Pre-pull | sync error | Abort, no commit |
| Lock check | lock held | Abort with `ErrCkinLockHeld` |
| Fork check | would fork | Abort with `ErrWouldFork` |
| Commit | any error | Abort, no push |
| Post-sync | sync error | Warn only — commit already succeeded |
| Post-fork | fork detected | Warn only — commit already succeeded |

### 2.3 CLI Integration

New flags on `leaf` agent and `edgesync commit`:

```
--autosync=on|off|pullonly   Autosync mode (default: off)
--allow-fork                 Allow commit even if it would fork
--override-lock              Ignore check-in lock conflicts (implies --allow-fork)
```

When `--branch` is set in commit opts, fork check and ci-lock are skipped (creating a new branch cannot fork an existing one).

### 2.4 ClientID Generation

Each agent instance generates a persistent `ClientID` on first start, stored in the repo's `config` table as `client-id`. Format: UUID v4 via `simio.Rand`.

```go
func ensureClientID(r *repo.Repo, rng simio.Rand) (string, error)
```

## Part 3: Testing Strategy

### 3.1 Unit Tests (go-libfossil)

| Test | Location | What it verifies |
|------|----------|-----------------|
| `TestWouldFork_SingleLeaf` | `checkout/fork_test.go` | Returns false when checkout is sole leaf |
| `TestWouldFork_Forked` | `checkout/fork_test.go` | Returns true when another leaf exists on branch |
| `TestWouldFork_DifferentBranch` | `checkout/fork_test.go` | Returns false when other leaf is on different branch |
| `TestWouldFork_TrunkNoSymTag` | `checkout/fork_test.go` | Returns true for trunk fork even without explicit sym-trunk tag |
| `TestBranchLeaves` | `checkout/fork_test.go` | Returns correct leaf RIDs per branch |
| `TestPreCommitCheck_Abort` | `checkout/checkin_test.go` | Commit aborts when PreCommitCheck returns error |
| `TestPreCommitCheck_Nil` | `checkout/checkin_test.go` | Commit proceeds when PreCommitCheck is nil |
| `TestCkinLock_Acquire` | `sync/ckin_lock_test.go` | Lock acquired, no conflict returned |
| `TestCkinLock_Conflict` | `sync/ckin_lock_test.go` | Different clientID gets ci-lock-fail |
| `TestCkinLock_SameClient` | `sync/ckin_lock_test.go` | Same clientID renews lock |
| `TestCkinLock_Expiry` | `sync/ckin_lock_test.go` | Stale locks expire by timeout |
| `TestCkinLock_ParentNotLeaf` | `sync/ckin_lock_test.go` | Lock expires when parent gains child |
| `TestCkinLockCard_EncodeDecode` | `xfer/card_test.go` | Wire format round-trip |

### 3.2 Integration Tests (sim)

| Test | What it verifies |
|------|-----------------|
| `TestAutosync_PullBeforeCommit` | Pre-pull brings in remote commits before fork check |
| `TestAutosync_WouldForkAborts` | Commit aborts when remote commit made parent non-leaf |
| `TestAutosync_AllowForkOverride` | `--allow-fork` lets commit proceed despite fork |
| `TestAutosync_CkinLockRace` | Two agents commit simultaneously; second gets lock-fail |
| `TestAutosync_PostPushSync` | Commit pushes to remote after success |
| `TestAutosync_BranchBypassesForkCheck` | `--branch` skips fork check and ci-lock |

### 3.3 DST Tests

| Test | What it verifies |
|------|-----------------|
| `TestDST_ConcurrentCommitForkPrevention` | Deterministic sim with two peers committing, fork correctly prevented |
| `TestDST_CkinLockExpiry` | Lock expires under simulated time advancement |

## Files Changed/Created

### New Files

| File | Package | Purpose |
|------|---------|---------|
| `go-libfossil/checkout/fork.go` | checkout | `WouldFork()`, `BranchLeaves()` |
| `go-libfossil/checkout/fork_test.go` | checkout | Fork detection unit tests |
| `go-libfossil/sync/ckin_lock.go` | sync | Server-side lock management |
| `go-libfossil/sync/ckin_lock_test.go` | sync | Lock unit tests |
| `leaf/agent/autosync.go` | agent | `Commit()` workflow |
| `leaf/agent/autosync_test.go` | agent | Workflow unit tests |

### Modified Files

| File | Change |
|------|--------|
| `go-libfossil/checkout/checkin.go` | Add `PreCommitCheck` to `CommitOpts`, call it in `Commit()` |
| `go-libfossil/xfer/encode.go` | Encode ci-lock pragma cards |
| `go-libfossil/xfer/decode.go` | Decode ci-lock pragma cards |
| `go-libfossil/sync/session.go` | Add `CkinLock` to `SyncOpts`, `CkinLockFail` to `SyncResult` |
| `go-libfossil/sync/client.go` | Emit ci-lock pragma in `buildRequest`, handle ci-lock-fail in `processResponse` |
| `go-libfossil/sync/handler.go` | Dispatch ci-lock pragma to `processCkinLock` |
| `leaf/agent/config.go` | Add `Autosync`, `AllowFork` config fields |
| `leaf/cmd/leaf/main.go` | Add `--autosync`, `--allow-fork`, `--override-lock` CLI flags |
| `cmd/edgesync/commit.go` | Add autosync flags to commit subcommand |
