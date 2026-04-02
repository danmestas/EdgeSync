# Autosync & Fork Prevention Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent accidental forks on commit by adding fork detection primitives to go-libfossil and an autosync workflow (pull→lock→check→commit→push) to the leaf agent, matching Fossil's behavior.

**Architecture:** go-libfossil gains transport-agnostic primitives (`WouldFork()`, `PreCommitCheck` hook, ci-lock server handling). The leaf agent orchestrates the full autosync workflow using whatever transport is configured (HTTP, NATS, libp2p). ci-lock pragmas ride on the existing xfer pragma mechanism.

**Tech Stack:** Go, SQLite (via go-libfossil/db), xfer wire protocol (pragma cards)

**Spec:** `docs/superpowers/specs/2026-04-02-autosync-fork-prevention-design.md`

---

## Chunk 1: Fork Detection Primitives (go-libfossil/checkout)

### Task 1: BranchLeaves() and WouldFork()

**Files:**
- Create: `go-libfossil/checkout/fork.go`
- Create: `go-libfossil/checkout/fork_test.go`

- [ ] **Step 1: Write failing tests for BranchLeaves**

In `go-libfossil/checkout/fork_test.go`:

```go
package checkout

import (
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func TestBranchLeaves_SingleLeaf(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	leaves, err := BranchLeaves(r, "trunk")
	if err != nil {
		t.Fatalf("BranchLeaves: %v", err)
	}
	if len(leaves) != 1 {
		t.Fatalf("got %d leaves, want 1", len(leaves))
	}
}

func TestBranchLeaves_Empty(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	leaves, err := BranchLeaves(r, "nonexistent-branch")
	if err != nil {
		t.Fatalf("BranchLeaves: %v", err)
	}
	if len(leaves) != 0 {
		t.Fatalf("got %d leaves, want 0", len(leaves))
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -run TestBranchLeaves -v`
Expected: FAIL — `BranchLeaves` undefined

- [ ] **Step 3: Implement BranchLeaves and WouldFork**

Create `go-libfossil/checkout/fork.go`:

```go
package checkout

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// BranchLeaves returns the leaf RIDs for a named branch.
// A branch with >1 leaf is forked. An empty branch name queries trunk.
func BranchLeaves(r *repo.Repo, branch string) ([]libfossil.FslID, error) {
	if r == nil {
		panic("checkout.BranchLeaves: r must not be nil")
	}
	if branch == "" {
		branch = "trunk"
	}
	rows, err := r.DB().Query(`
		SELECT l.rid FROM leaf l
		JOIN tagxref tx ON tx.rid = l.rid
		JOIN tag t ON t.tagid = tx.tagid
		WHERE t.tagname = 'branch'
		  AND tx.value = ?
		  AND tx.tagtype > 0
	`, branch)
	if err != nil {
		return nil, fmt.Errorf("checkout.BranchLeaves: %w", err)
	}
	defer rows.Close()
	var leaves []libfossil.FslID
	for rows.Next() {
		var rid int64
		if err := rows.Scan(&rid); err != nil {
			return nil, fmt.Errorf("checkout.BranchLeaves scan: %w", err)
		}
		leaves = append(leaves, libfossil.FslID(rid))
	}
	return leaves, rows.Err()
}

// WouldFork reports whether committing on the current branch would
// create a fork. Returns true when another leaf exists on the same
// branch that is not the current checkout version.
func (c *Checkout) WouldFork() (bool, error) {
	if c == nil {
		panic("checkout.WouldFork: nil *Checkout")
	}
	rid, _, err := c.Version()
	if err != nil {
		return false, fmt.Errorf("checkout.WouldFork: %w", err)
	}

	branch, err := c.currentBranch(rid)
	if err != nil {
		return false, err
	}

	leaves, err := BranchLeaves(c.repo, branch)
	if err != nil {
		return false, err
	}
	for _, leaf := range leaves {
		if leaf != rid {
			return true, nil
		}
	}
	return false, nil
}

// currentBranch returns the branch name for the given RID.
// Falls back to "trunk" if no branch tag exists.
func (c *Checkout) currentBranch(rid libfossil.FslID) (string, error) {
	var branch string
	err := c.repo.DB().QueryRow(`
		SELECT tx.value FROM tagxref tx
		JOIN tag t ON t.tagid = tx.tagid
		WHERE t.tagname = 'branch'
		  AND tx.rid = ?
		  AND tx.tagtype > 0
		ORDER BY tx.mtime DESC
		LIMIT 1
	`, int64(rid)).Scan(&branch)
	if err != nil {
		return "trunk", nil // no branch tag → trunk
	}
	return branch, nil
}
```

- [ ] **Step 4: Run BranchLeaves tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -run TestBranchLeaves -v`
Expected: PASS

- [ ] **Step 5: Write failing tests for WouldFork**

Add to `go-libfossil/checkout/fork_test.go`:

```go
func TestWouldFork_SingleLeaf(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer co.Close()

	forked, err := co.WouldFork()
	if err != nil {
		t.Fatalf("WouldFork: %v", err)
	}
	if forked {
		t.Fatal("WouldFork = true, want false (single leaf)")
	}
}

func TestWouldFork_TrunkNoSymTag(t *testing.T) {
	// Create a repo with two trunk commits that both remain leaves
	// (simulating a fork where parent was not removed from leaf table).
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer co.Close()

	rid1, _, _ := co.Version()

	// Extract and commit to create a second checkin.
	if err := co.Extract(rid1, ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}
	co.env.Storage.WriteFile(co.dir+"/fork.txt", []byte("fork"), 0644)
	_, _, err = co.Commit(CommitOpts{Message: "second commit", User: "test"})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	// Manually re-insert the original commit as a leaf to simulate a fork.
	_, err = r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", int64(rid1))
	if err != nil {
		t.Fatalf("insert leaf: %v", err)
	}

	forked, err := co.WouldFork()
	if err != nil {
		t.Fatalf("WouldFork: %v", err)
	}
	if !forked {
		t.Fatal("WouldFork = false, want true (trunk fork without sym-trunk tag)")
	}
}

func TestWouldFork_DifferentBranch(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer co.Close()

	rid1, _, _ := co.Version()
	if err := co.Extract(rid1, ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}
	co.env.Storage.WriteFile(co.dir+"/b.txt", []byte("branch"), 0644)

	// Commit on a different branch.
	_, _, err = co.Commit(CommitOpts{
		Message: "branch commit",
		User:    "test",
		Branch:  "feature-x",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	// Re-insert original as leaf — but it's trunk, not feature-x.
	_, err = r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", int64(rid1))
	if err != nil {
		t.Fatalf("insert leaf: %v", err)
	}

	// Checkout is now on feature-x. Trunk leaf should not trigger WouldFork.
	forked, err := co.WouldFork()
	if err != nil {
		t.Fatalf("WouldFork: %v", err)
	}
	if forked {
		t.Fatal("WouldFork = true, want false (other leaf is on different branch)")
	}
}
```

- [ ] **Step 6: Run WouldFork tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -run TestWouldFork -v`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/checkout/fork.go go-libfossil/checkout/fork_test.go
git commit -m "feat(checkout): add WouldFork() and BranchLeaves() fork detection

Queries leaf + tagxref tables using 'branch' tag (not sym-*) for
correct trunk detection. Part of EDG-1/EDG-2."
```

### Task 2: PreCommitCheck Hook

**Files:**
- Modify: `go-libfossil/checkout/types.go:148-156`
- Modify: `go-libfossil/checkout/checkin.go:268-272`
- Modify: `go-libfossil/checkout/checkin_test.go`

- [ ] **Step 1: Write failing test for PreCommitCheck abort**

Add to `go-libfossil/checkout/checkin_test.go`:

```go
func TestPreCommitCheck_Abort(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer co.Close()

	rid1, _, _ := co.Version()
	if err := co.Extract(rid1, ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}
	co.env.Storage.WriteFile(co.dir+"/abort.txt", []byte("abort"), 0644)

	wantErr := fmt.Errorf("blocked by policy")
	_, _, err = co.Commit(CommitOpts{
		Message:        "should not commit",
		User:           "test",
		PreCommitCheck: func() error { return wantErr },
	})
	if err == nil {
		t.Fatal("Commit succeeded, want error from PreCommitCheck")
	}
	if !errors.Is(err, wantErr) {
		t.Fatalf("err = %v, want wrapping %v", err, wantErr)
	}
}

func TestPreCommitCheck_Nil(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer co.Close()

	rid1, _, _ := co.Version()
	if err := co.Extract(rid1, ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}
	co.env.Storage.WriteFile(co.dir+"/ok.txt", []byte("ok"), 0644)

	_, _, err = co.Commit(CommitOpts{
		Message: "normal commit",
		User:    "test",
		// PreCommitCheck is nil — should proceed normally
	})
	if err != nil {
		t.Fatalf("Commit with nil PreCommitCheck failed: %v", err)
	}
}
```

- [ ] **Step 2: Run tests to verify the abort test fails**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -run TestPreCommitCheck -v`
Expected: `TestPreCommitCheck_Abort` FAIL — `PreCommitCheck` field unknown. `TestPreCommitCheck_Nil` may pass (nil is ignored).

- [ ] **Step 3: Add PreCommitCheck field to CommitOpts**

Edit `go-libfossil/checkout/types.go` at line 156 — add field after `Time`:

```go
type CommitOpts struct {
	Message        string
	User           string
	Branch         string    // empty → current branch
	Tags           []string  // additional T-cards
	Delta          bool
	Time           time.Time // zero → env.Clock.Now()
	PreCommitCheck func() error // nil = no check; non-nil error aborts commit
}
```

- [ ] **Step 4: Call PreCommitCheck in Commit()**

Edit `go-libfossil/checkout/checkin.go`. Insert after line 270 (after `ScanChanges` succeeds) and before line 272 (`collectVFileEntries`):

```go
	if err := c.ScanChanges(ScanHash); err != nil {
		return 0, "", fmt.Errorf("checkout.Commit: scan: %w", err)
	}

	if opts.PreCommitCheck != nil {
		if err := opts.PreCommitCheck(); err != nil {
			return 0, "", fmt.Errorf("checkout.Commit: pre-commit check: %w", err)
		}
	}

	vfEntries, changedFiles, deletedFiles, err := c.collectVFileEntries(parentRID)
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -run TestPreCommitCheck -v`
Expected: PASS

- [ ] **Step 6: Run full checkout test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -v`
Expected: All tests PASS (no regressions)

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/checkout/types.go go-libfossil/checkout/checkin.go go-libfossil/checkout/checkin_test.go
git commit -m "feat(checkout): add PreCommitCheck hook to CommitOpts

General-purpose callback called after ScanChanges but before
manifest.Checkin. Returning non-nil error aborts the commit.
Part of EDG-1/EDG-2."
```

---

## Chunk 2: ci-lock Protocol (go-libfossil/xfer + sync)

### Task 3: ci-lock Pragma Encode/Decode

**Files:**
- Modify: `go-libfossil/xfer/encode.go`
- Modify: `go-libfossil/xfer/decode.go`
- Modify: `go-libfossil/xfer/card_test.go` (or create ci-lock-specific test)

ci-lock pragmas use the existing `PragmaCard` type — no new card types needed. Encoding/decoding is already handled generically by `encodePragma`/`parsePragma`. This task verifies round-trip correctness.

- [ ] **Step 1: Write failing round-trip test**

Add to `go-libfossil/xfer/card_test.go` (or existing encode/decode test file):

```go
func TestCkinLockPragma_RoundTrip(t *testing.T) {
	cards := []Card{
		&PragmaCard{Name: "ci-lock", Values: []string{"abc123def456", "client-001"}},
		&PragmaCard{Name: "ci-lock-fail", Values: []string{"alice", "1712000000"}},
	}
	msg := &Message{Cards: cards}
	encoded, err := Encode(msg)
	if err != nil {
		t.Fatalf("Encode: %v", err)
	}
	decoded, err := Decode(encoded)
	if err != nil {
		t.Fatalf("Decode: %v", err)
	}
	if len(decoded.Cards) != 2 {
		t.Fatalf("got %d cards, want 2", len(decoded.Cards))
	}

	lock := decoded.Cards[0].(*PragmaCard)
	if lock.Name != "ci-lock" || len(lock.Values) != 2 ||
		lock.Values[0] != "abc123def456" || lock.Values[1] != "client-001" {
		t.Fatalf("ci-lock = %+v", lock)
	}

	fail := decoded.Cards[1].(*PragmaCard)
	if fail.Name != "ci-lock-fail" || len(fail.Values) != 2 ||
		fail.Values[0] != "alice" || fail.Values[1] != "1712000000" {
		t.Fatalf("ci-lock-fail = %+v", fail)
	}
}
```

- [ ] **Step 2: Run test to verify it passes (existing pragma round-trip should work)**

Run: `cd go-libfossil && go test -buildvcs=false ./xfer/ -run TestCkinLockPragma -v`
Expected: PASS — pragmas already encode/decode generically. If it fails, investigate.

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/xfer/
git commit -m "test(xfer): add ci-lock pragma round-trip test

Verifies ci-lock and ci-lock-fail pragmas encode/decode correctly
using existing PragmaCard mechanism. Part of EDG-1/EDG-2."
```

### Task 4: Server-Side ci-lock Management

**Files:**
- Create: `go-libfossil/sync/ckin_lock.go`
- Create: `go-libfossil/sync/ckin_lock_test.go`

- [ ] **Step 1: Write failing tests**

Create `go-libfossil/sync/ckin_lock_test.go`:

```go
package sync

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func newTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.CreateWithEnv(path, "test-user", simio.RealEnv())
	if err != nil {
		t.Fatalf("repo.CreateWithEnv: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestCkinLock_Acquire(t *testing.T) {
	r := newTestRepo(t)
	fail := processCkinLock(r.DB(), "parent-uuid-1", "client-A", "alice", DefaultCkinLockTimeout)
	if fail != nil {
		t.Fatalf("expected nil (lock acquired), got fail: %+v", fail)
	}
}

func TestCkinLock_SameClient(t *testing.T) {
	r := newTestRepo(t)
	processCkinLock(r.DB(), "parent-uuid-1", "client-A", "alice", DefaultCkinLockTimeout)

	// Same client re-acquires — should succeed (renew).
	fail := processCkinLock(r.DB(), "parent-uuid-1", "client-A", "alice", DefaultCkinLockTimeout)
	if fail != nil {
		t.Fatalf("same client renewal should succeed, got fail: %+v", fail)
	}
}

func TestCkinLock_Conflict(t *testing.T) {
	r := newTestRepo(t)
	processCkinLock(r.DB(), "parent-uuid-1", "client-A", "alice", DefaultCkinLockTimeout)

	// Different client tries to acquire — should fail.
	fail := processCkinLock(r.DB(), "parent-uuid-1", "client-B", "bob", DefaultCkinLockTimeout)
	if fail == nil {
		t.Fatal("expected lock conflict, got nil")
	}
	if fail.HeldBy != "alice" {
		t.Fatalf("HeldBy = %q, want %q", fail.HeldBy, "alice")
	}
}

func TestCkinLock_Expiry(t *testing.T) {
	r := newTestRepo(t)
	processCkinLock(r.DB(), "parent-uuid-1", "client-A", "alice", DefaultCkinLockTimeout)

	// Manually backdate the lock's mtime to force expiry.
	expired := time.Now().Unix() - 120 // 2 minutes ago
	r.DB().Exec("UPDATE config SET mtime=? WHERE name='edgesync-ci-lock-parent-uuid-1'", expired)

	// Different client should now succeed.
	fail := processCkinLock(r.DB(), "parent-uuid-1", "client-B", "bob", DefaultCkinLockTimeout)
	if fail != nil {
		t.Fatalf("expected expired lock to be ignored, got fail: %+v", fail)
	}
}

func TestCkinLock_ParentNotLeaf(t *testing.T) {
	r := newTestRepo(t)
	// Create a phantom blob to simulate a "parent" that has a known UUID.
	r.DB().Exec("INSERT INTO blob(uuid, size, content) VALUES('parent-uuid-1', 0, '')")
	var parentRid int64
	r.DB().QueryRow("SELECT rid FROM blob WHERE uuid='parent-uuid-1'").Scan(&parentRid)

	// Acquire lock.
	processCkinLock(r.DB(), "parent-uuid-1", "client-A", "alice", DefaultCkinLockTimeout)

	// The parent is NOT in the leaf table, so expireStaleLocks should clear it.
	// Different client should succeed.
	fail := processCkinLock(r.DB(), "parent-uuid-1", "client-B", "bob", DefaultCkinLockTimeout)
	if fail != nil {
		t.Fatalf("expected lock to expire (parent not leaf), got fail: %+v", fail)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestCkinLock -v`
Expected: FAIL — `processCkinLock` undefined

- [ ] **Step 3: Implement ckin_lock.go**

Create `go-libfossil/sync/ckin_lock.go`:

```go
package sync

import (
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

const DefaultCkinLockTimeout = 60 * time.Second

// CkinLockFail reports that another client holds the check-in lock.
type CkinLockFail struct {
	HeldBy string
	Since  time.Time
}

type ckinLockEntry struct {
	ClientID string `json:"clientid"`
	Login    string `json:"login"`
	MTime    int64  `json:"mtime"`
}

func configKey(parentUUID string) string {
	return "edgesync-ci-lock-" + parentUUID
}

// processCkinLock handles a ci-lock pragma request.
// Returns non-nil CkinLockFail if another client holds the lock.
func processCkinLock(d db.Querier, parentUUID, clientID, user string, timeout time.Duration) *CkinLockFail {
	expireStaleLocks(d, timeout)

	key := configKey(parentUUID)
	var raw string
	err := d.QueryRow("SELECT value FROM config WHERE name=?", key).Scan(&raw)
	if err == nil {
		var entry ckinLockEntry
		if json.Unmarshal([]byte(raw), &entry) == nil && entry.ClientID != clientID {
			return &CkinLockFail{
				HeldBy: entry.Login,
				Since:  time.Unix(entry.MTime, 0),
			}
		}
	}

	// Upsert lock.
	now := time.Now().Unix()
	entry := ckinLockEntry{ClientID: clientID, Login: user, MTime: now}
	val, _ := json.Marshal(entry)
	d.Exec(`REPLACE INTO config(name, value, mtime) VALUES(?, ?, ?)`, key, string(val), now)
	return nil
}

// expireStaleLocks removes ci-lock entries that are older than timeout
// or whose parent UUID is no longer in the leaf table.
func expireStaleLocks(d db.Querier, timeout time.Duration) {
	cutoff := time.Now().Unix() - int64(timeout.Seconds())

	// Expire by time.
	d.Exec("DELETE FROM config WHERE name LIKE 'edgesync-ci-lock-%' AND mtime < ?", cutoff)

	// Expire locks whose parent is no longer a leaf.
	rows, err := d.Query("SELECT name FROM config WHERE name LIKE 'edgesync-ci-lock-%'")
	if err != nil {
		return
	}
	defer rows.Close()

	var toDelete []string
	for rows.Next() {
		var name string
		if rows.Scan(&name) != nil {
			continue
		}
		uuid := strings.TrimPrefix(name, "edgesync-ci-lock-")
		var rid int64
		if d.QueryRow("SELECT rid FROM blob WHERE uuid=?", uuid).Scan(&rid) != nil {
			toDelete = append(toDelete, name) // blob doesn't exist
			continue
		}
		var dummy int
		if d.QueryRow("SELECT 1 FROM leaf WHERE rid=?", rid).Scan(&dummy) != nil {
			toDelete = append(toDelete, name) // not a leaf
		}
	}
	for _, name := range toDelete {
		d.Exec("DELETE FROM config WHERE name=?", name)
	}
}
```

Note: `processCkinLock` accepts `db.Querier` — the interface with `Exec`, `QueryRow`, and `Query`. `repo.DB()` returns `*db.DB` which satisfies this interface. The handler passes `h.repo.DB()` directly.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestCkinLock -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/sync/ckin_lock.go go-libfossil/sync/ckin_lock_test.go
git commit -m "feat(sync): add server-side ci-lock management

Stores locks in config table with edgesync-ci-lock- prefix.
Expires by timeout (60s default) and when parent is no longer
a leaf. Part of EDG-1/EDG-2."
```

### Task 5: Wire ci-lock Into Sync Client and Handler

**Files:**
- Modify: `go-libfossil/sync/session.go:31-52` (SyncOpts + SyncResult)
- Modify: `go-libfossil/sync/client.go:95-128` (buildRequest pragma emission)
- Modify: `go-libfossil/sync/client.go:505-510` (processResponse pragma handling)
- Modify: `go-libfossil/sync/handler.go:227-248` (pragma dispatch)

- [ ] **Step 1: Add CkinLock fields to SyncOpts and SyncResult**

Edit `go-libfossil/sync/session.go`. Add to `SyncOpts` struct after line 42 (`Observer`):

```go
type SyncOpts struct {
	Push, Pull              bool
	ProjectCode, ServerCode string
	User, Password          string
	MaxSend                 int
	UV                      bool
	Private                 bool
	Env                     *simio.Env
	Buggify                 BuggifyChecker
	Observer                Observer
	CkinLock                *CkinLockReq   // nil = no lock requested
}

// CkinLockReq requests a server-side check-in lock.
type CkinLockReq struct {
	ParentUUID string
	ClientID   string
}
```

Add to `SyncResult` struct after line 51 (`Errors`):

```go
type SyncResult struct {
	Rounds, FilesSent, FilesRecvd int
	UVFilesSent, UVFilesRecvd     int
	UVGimmesSent                  int
	ArtifactsLinked               int
	Errors                        []string
	CkinLockFail                  *CkinLockFail // nil = no conflict
}
```

- [ ] **Step 2: Emit ci-lock pragma in buildRequest**

Edit `go-libfossil/sync/client.go`. Add after line 101 (after `send-private` pragma), before UV section:

```go
	// ci-lock: request check-in lock on first round only.
	if s.opts.CkinLock != nil && cycle == 0 {
		cards = append(cards, &xfer.PragmaCard{
			Name:   "ci-lock",
			Values: []string{s.opts.CkinLock.ParentUUID, s.opts.CkinLock.ClientID},
		})
	}
```

- [ ] **Step 3: Handle ci-lock-fail pragma in processResponse**

Edit `go-libfossil/sync/client.go`. Add to the pragma switch at line 505-510:

```go
		case *xfer.PragmaCard:
			if c.Name == "uv-push-ok" {
				s.uvPushOK = true
			} else if c.Name == "uv-pull-only" {
				s.uvPullOnly = true
			} else if c.Name == "ci-lock-fail" && len(c.Values) >= 2 {
				mtime, _ := strconv.ParseInt(c.Values[1], 10, 64)
				s.result.CkinLockFail = &CkinLockFail{
					HeldBy: c.Values[0],
					Since:  time.Unix(mtime, 0),
				}
			}
```

Add `"strconv"` and `"time"` to the imports of `client.go` if not already present.

- [ ] **Step 4: Handle ci-lock pragma in handler**

Edit `go-libfossil/sync/handler.go`. Add to the pragma switch at line 240 (after `send-private` block):

```go
		if c.Name == "ci-lock" && len(c.Values) >= 2 {
			fail := processCkinLock(h.repo.DB(), c.Values[0], c.Values[1], h.user, DefaultCkinLockTimeout)
			if fail != nil {
				h.resp = append(h.resp, &xfer.PragmaCard{
					Name:   "ci-lock-fail",
					Values: []string{fail.HeldBy, fmt.Sprintf("%d", fail.Since.Unix())},
				})
			}
		}
```

- [ ] **Step 5: Run full sync test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -v`
Expected: All tests PASS (no regressions)

- [ ] **Step 6: Write integration test for ci-lock round-trip**

Add to `go-libfossil/sync/ckin_lock_test.go`:

```go
func TestCkinLock_SyncRoundTrip(t *testing.T) {
	// Create server repo with a checkin so we have a leaf UUID.
	serverRepo := newTestRepo(t)

	// Mock transport that runs HandleSync on the server repo.
	transport := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			resp, _ := HandleSync(context.Background(), serverRepo, req)
			return resp
		},
	}

	// Client syncs with ci-lock.
	clientRepo := newTestRepo(t)
	result, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true,
		CkinLock: &CkinLockReq{
			ParentUUID: "some-parent-uuid",
			ClientID:   "client-1",
		},
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}
	if result.CkinLockFail != nil {
		t.Fatalf("unexpected lock fail: %+v", result.CkinLockFail)
	}

	// Second client should get lock-fail.
	result2, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true,
		CkinLock: &CkinLockReq{
			ParentUUID: "some-parent-uuid",
			ClientID:   "client-2",
		},
	})
	if err != nil {
		t.Fatalf("Sync 2: %v", err)
	}
	if result2.CkinLockFail == nil {
		t.Fatal("expected lock fail for second client")
	}
	if result2.CkinLockFail.HeldBy != "nobody" {
		t.Fatalf("HeldBy = %q, want %q", result2.CkinLockFail.HeldBy, "nobody")
	}
}
```

- [ ] **Step 7: Run integration test**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestCkinLock_SyncRoundTrip -v`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/sync/session.go go-libfossil/sync/client.go go-libfossil/sync/handler.go go-libfossil/sync/ckin_lock_test.go
git commit -m "feat(sync): wire ci-lock pragma into client and handler

Client emits pragma ci-lock on first round when CkinLock is set.
Handler calls processCkinLock and responds with ci-lock-fail if
another client holds the lock. Part of EDG-1/EDG-2."
```

---

## Chunk 3: Leaf Agent Autosync Workflow

### Task 6: Autosync Commit Wrapper

**Files:**
- Create: `leaf/agent/autosync.go`
- Create: `leaf/agent/autosync_test.go`

- [ ] **Step 1: Write failing test for autosync off mode (passthrough)**

Create `leaf/agent/autosync_test.go`:

```go
package agent

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/checkout"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func newTestCheckout(t *testing.T) (*checkout.Checkout, *repo.Repo) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.CreateWithEnv(path, "test-user", simio.RealEnv())
	if err != nil {
		t.Fatalf("repo.CreateWithEnv: %v", err)
	}
	t.Cleanup(func() { r.Close() })

	dir := t.TempDir()
	co, err := checkout.Create(r, dir, checkout.CreateOpts{})
	if err != nil {
		t.Fatalf("checkout.Create: %v", err)
	}
	t.Cleanup(func() { co.Close() })

	rid, _, _ := co.Version()
	if err := co.Extract(rid, checkout.ExtractOpts{}); err != nil {
		t.Fatalf("Extract: %v", err)
	}

	return co, r
}

func nopTransport() sync.Transport {
	return &sync.MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			return &xfer.Message{}
		},
	}
}

func TestAutosyncCommit_OffMode(t *testing.T) {
	co, _ := newTestCheckout(t)
	co.Repo() // verify accessible

	os.WriteFile(filepath.Join(co.Dir(), "test.txt"), []byte("hello"), 0644)

	rid, uuid, err := AutosyncCommit(context.Background(), co, checkout.CommitOpts{
		Message: "test commit",
		User:    "test",
	}, AutosyncOpts{
		Mode: AutosyncOff,
	})
	if err != nil {
		t.Fatalf("AutosyncCommit: %v", err)
	}
	if rid == 0 || uuid == "" {
		t.Fatal("got zero RID or empty UUID")
	}
}
```

Note: `Checkout` has `Dir()` (public) but no `Env()` accessor. Tests in `leaf/agent/` (external package) use `os.WriteFile` with `co.Dir()` to write test files. Tests in `go-libfossil/checkout/` (same package) can access `co.env` directly.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd leaf && go test -buildvcs=false ./agent/ -run TestAutosyncCommit_OffMode -v`
Expected: FAIL — `AutosyncCommit` undefined

- [ ] **Step 3: Implement autosync.go**

Create `leaf/agent/autosync.go`:

```go
package agent

import (
	"context"
	"errors"
	"fmt"
	"log/slog"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/checkout"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
)

// AutosyncMode controls sync behavior around commits.
type AutosyncMode int

const (
	AutosyncOff      AutosyncMode = iota // no sync around commits
	AutosyncOn                            // pull + commit + push
	AutosyncPullOnly                      // pull + commit (no push)
)

// AutosyncOpts configures the autosync workflow.
type AutosyncOpts struct {
	Mode      AutosyncMode
	Transport libsync.Transport
	SyncOpts  libsync.SyncOpts
	AllowFork bool
	ClientID  string
}

var (
	ErrWouldFork   = errors.New("would fork: run update first, or use --allow-fork or --branch")
	ErrCkinLockHeld = errors.New("check-in lock held by another client")
)

// AutosyncCommit wraps checkout.Commit with autosync pull-before and push-after.
// When Mode is Off, delegates directly to co.Commit with no sync.
func AutosyncCommit(ctx context.Context, co *checkout.Checkout,
	commitOpts checkout.CommitOpts, auto AutosyncOpts,
) (libfossil.FslID, string, error) {
	if auto.Mode == AutosyncOff {
		return co.Commit(commitOpts)
	}

	// Step 1: Pre-pull + ci-lock.
	syncOpts := auto.SyncOpts
	syncOpts.Pull = true
	syncOpts.Push = false

	if !auto.AllowFork && commitOpts.Branch == "" {
		_, parentUUID, err := co.Version()
		if err != nil {
			return 0, "", fmt.Errorf("autosync: get version: %w", err)
		}
		syncOpts.CkinLock = &libsync.CkinLockReq{
			ParentUUID: parentUUID,
			ClientID:   auto.ClientID,
		}
	}

	result, err := libsync.Sync(ctx, co.Repo(), auto.Transport, syncOpts)
	if err != nil {
		return 0, "", fmt.Errorf("autosync pre-pull: %w", err)
	}

	// Step 2: Check lock result.
	if result.CkinLockFail != nil && !auto.AllowFork {
		return 0, "", fmt.Errorf("%w: held by %q since %v",
			ErrCkinLockHeld, result.CkinLockFail.HeldBy, result.CkinLockFail.Since)
	}

	// Step 3: Inject fork check.
	if !auto.AllowFork && commitOpts.Branch == "" {
		commitOpts.PreCommitCheck = func() error {
			forked, err := co.WouldFork()
			if err != nil {
				return err
			}
			if forked {
				return ErrWouldFork
			}
			return nil
		}
	}

	// Step 4: Commit.
	rid, uuid, err := co.Commit(commitOpts)
	if err != nil {
		return 0, "", err
	}

	// Step 5: Post-sync.
	if auto.Mode == AutosyncOn {
		syncOpts.Pull = true
		syncOpts.Push = true
		syncOpts.CkinLock = nil
		if _, postErr := libsync.Sync(ctx, co.Repo(), auto.Transport, syncOpts); postErr != nil {
			slog.Warn("autosync post-push failed", "err", postErr)
		}
	}

	// Step 6: Post-fork warning.
	if forked, _ := co.WouldFork(); forked {
		slog.Warn("fork detected after commit")
	}

	return rid, uuid, nil
}
```

Note: `co.Version()` returns `(libfossil.FslID, string, error)`. `co.Repo()` returns `*repo.Repo`. `co.WouldFork()` returns `(bool, error)`. All verified against actual signatures.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd leaf && go test -buildvcs=false ./agent/ -run TestAutosyncCommit_OffMode -v`
Expected: PASS

- [ ] **Step 5: Write test for fork detection abort**

Add to `leaf/agent/autosync_test.go`:

```go
func TestAutosyncCommit_WouldForkAborts(t *testing.T) {
	co, r := newTestCheckout(t)

	// Create a second checkin, then re-insert original as leaf to simulate fork.
	rid1, _, _ := co.Version()
	os.WriteFile(filepath.Join(co.Dir(), "f.txt"), []byte("x"), 0644)
	co.Commit(checkout.CommitOpts{Message: "c2", User: "test"})
	r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", int64(rid1))

	os.WriteFile(filepath.Join(co.Dir(), "g.txt"), []byte("y"), 0644)

	_, _, err := AutosyncCommit(context.Background(), co, checkout.CommitOpts{
		Message: "should fail",
		User:    "test",
	}, AutosyncOpts{
		Mode:      AutosyncPullOnly,
		Transport: nopTransport(),
	})
	if !errors.Is(err, ErrWouldFork) {
		t.Fatalf("err = %v, want ErrWouldFork", err)
	}
}

func TestAutosyncCommit_AllowForkOverride(t *testing.T) {
	co, r := newTestCheckout(t)

	rid1, _, _ := co.Version()
	os.WriteFile(filepath.Join(co.Dir(), "f.txt"), []byte("x"), 0644)
	co.Commit(checkout.CommitOpts{Message: "c2", User: "test"})
	r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", int64(rid1))

	os.WriteFile(filepath.Join(co.Dir(), "g.txt"), []byte("y"), 0644)

	rid, _, err := AutosyncCommit(context.Background(), co, checkout.CommitOpts{
		Message: "forced",
		User:    "test",
	}, AutosyncOpts{
		Mode:      AutosyncPullOnly,
		Transport: nopTransport(),
		AllowFork: true,
	})
	if err != nil {
		t.Fatalf("AutosyncCommit with AllowFork: %v", err)
	}
	if rid == 0 {
		t.Fatal("got zero RID")
	}
}

func TestAutosyncCommit_BranchBypassesForkCheck(t *testing.T) {
	co, r := newTestCheckout(t)

	rid1, _, _ := co.Version()
	os.WriteFile(filepath.Join(co.Dir(), "f.txt"), []byte("x"), 0644)
	co.Commit(checkout.CommitOpts{Message: "c2", User: "test"})
	r.DB().Exec("INSERT OR IGNORE INTO leaf(rid) VALUES(?)", int64(rid1))

	os.WriteFile(filepath.Join(co.Dir(), "g.txt"), []byte("y"), 0644)

	rid, _, err := AutosyncCommit(context.Background(), co, checkout.CommitOpts{
		Message: "new branch",
		User:    "test",
		Branch:  "feature-x",
	}, AutosyncOpts{
		Mode:      AutosyncPullOnly,
		Transport: nopTransport(),
	})
	if err != nil {
		t.Fatalf("AutosyncCommit with Branch: %v", err)
	}
	if rid == 0 {
		t.Fatal("got zero RID")
	}
}
```

- [ ] **Step 6: Run all autosync tests**

Run: `cd leaf && go test -buildvcs=false ./agent/ -run TestAutosyncCommit -v`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add leaf/agent/autosync.go leaf/agent/autosync_test.go
git commit -m "feat(agent): add AutosyncCommit workflow

Pull-before, ci-lock, fork-check, commit, push-after.
Matches Fossil's autosync behavior with --allow-fork and
--branch escape hatches. Part of EDG-1/EDG-2."
```

### Task 7: ClientID Generation

**Files:**
- Create: `leaf/agent/clientid.go`
- Modify: `leaf/agent/autosync.go` (use ensureClientID)

- [ ] **Step 1: Implement ensureClientID**

Create `leaf/agent/clientid.go`:

```go
package agent

import (
	"fmt"
	"io"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

// ensureClientID returns the persistent client ID for this agent instance.
// If none exists, generates a UUID v4 using rng and stores it in the repo
// config table. Accepts simio.Rand for DST determinism.
func ensureClientID(r *repo.Repo, rng simio.Rand) (string, error) {
	var id string
	err := r.DB().QueryRow("SELECT value FROM config WHERE name='edgesync-client-id'").Scan(&id)
	if err == nil && id != "" {
		return id, nil
	}

	id, err = generateUUID4(rng)
	if err != nil {
		return "", fmt.Errorf("ensureClientID: %w", err)
	}

	_, err = r.DB().Exec(
		"REPLACE INTO config(name, value, mtime) VALUES('edgesync-client-id', ?, strftime('%s','now'))",
		id,
	)
	if err != nil {
		return "", fmt.Errorf("ensureClientID: store: %w", err)
	}
	return id, nil
}

func generateUUID4(rng io.Reader) (string, error) {
	var b [16]byte
	if _, err := rng.Read(b[:]); err != nil {
		return "", err
	}
	b[6] = (b[6] & 0x0f) | 0x40 // version 4
	b[8] = (b[8] & 0x3f) | 0x80 // variant 10
	return fmt.Sprintf("%08x-%04x-%04x-%04x-%012x",
		b[0:4], b[4:6], b[6:8], b[8:10], b[10:16]), nil
}
```

- [ ] **Step 2: Run build check**

Run: `cd leaf && go build -buildvcs=false ./agent/`
Expected: BUILD OK

- [ ] **Step 3: Commit**

```bash
git add leaf/agent/clientid.go
git commit -m "feat(agent): add persistent ClientID generation

Stores edgesync-client-id in repo config table. Used by ci-lock
protocol to identify agent instances. Part of EDG-1/EDG-2."
```

### Task 8: CLI Flags

**Files:**
- Modify: `leaf/agent/config.go`
- Modify: `leaf/cmd/leaf/main.go` (or wherever CLI flags are defined)
- Modify: `cmd/edgesync/repo_ci.go`

- [ ] **Step 1: Add autosync config fields**

Edit `leaf/agent/config.go`. Add after `PostSyncHook` field (line 82):

```go
	Autosync     AutosyncMode // default: AutosyncOff
	AllowFork    bool         // bypass fork + lock checks
	OverrideLock bool         // ignore lock conflicts (implies AllowFork)
```

When wiring config to `AutosyncOpts`, set `AllowFork = cfg.AllowFork || cfg.OverrideLock` to implement the "implies AllowFork" semantics.

- [ ] **Step 2: Add CLI flags to leaf command**

Edit `leaf/cmd/leaf/main.go`. Add flags for `--autosync`, `--allow-fork`, `--override-lock`. The exact location depends on how flags are parsed (kong struct tags). Add the flags alongside existing flags.

- [ ] **Step 3: Add CLI flags to edgesync commit command**

Edit `cmd/edgesync/repo_ci.go`. Add to `RepoCiCmd` struct:

```go
type RepoCiCmd struct {
	Message      string   `short:"m" required:"" help:"Checkin comment"`
	Files        []string `arg:"" required:"" help:"Files to checkin"`
	User         string   `help:"Checkin user (default: OS username)"`
	Parent       string   `help:"Parent version UUID (default: tip)"`
	Autosync     string   `help:"Autosync mode: on, off, pullonly" default:"off" enum:"on,off,pullonly"`
	AllowFork    bool     `help:"Allow commit even if it would fork"`
	OverrideLock bool     `help:"Ignore check-in lock conflicts (implies --allow-fork)"`
	Branch       string   `help:"Create commit on new branch"`
}
```

- [ ] **Step 4: Run build check**

Run: `go build -buildvcs=false ./cmd/edgesync/ && go build -buildvcs=false ./leaf/cmd/leaf/`
Expected: BUILD OK

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/config.go leaf/cmd/leaf/main.go cmd/edgesync/repo_ci.go
git commit -m "feat(cli): add --autosync, --allow-fork, --override-lock flags

Available on both leaf agent and edgesync commit command.
Part of EDG-1/EDG-2."
```

---

## Chunk 4: Final Verification

### Task 9: Full Test Suite and Build Verification

- [ ] **Step 1: Run go-libfossil tests**

Run: `cd go-libfossil && go test -buildvcs=false ./... -v`
Expected: All PASS

- [ ] **Step 2: Run leaf module tests**

Run: `cd leaf && go test -buildvcs=false ./... -v`
Expected: All PASS

- [ ] **Step 3: Run make test (CI-level)**

Run: `make test`
Expected: All PASS

- [ ] **Step 4: Run make build**

Run: `make build`
Expected: All binaries built successfully

- [ ] **Step 5: Final commit if any cleanup needed**

If any files need formatting or minor fixes from the test run, commit them:

```bash
git add -A
git commit -m "chore: cleanup after autosync implementation"
```

---

## Deferred: Sim & DST Integration Tests

The spec (sections 3.2-3.3) calls for 6 sim integration tests and 2 DST tests. These are deferred to a follow-up task because they require:
- Real NATS + TCP fault proxy infrastructure (sim/)
- DST event model wiring (dst/)
- Both are separate module boundaries with their own test harnesses

Tests to add in follow-up:
- `TestAutosync_PullBeforeCommit` — pre-pull brings in remote commits
- `TestAutosync_WouldForkAborts` — fork abort with real sync
- `TestAutosync_AllowForkOverride` — override with real sync
- `TestAutosync_CkinLockRace` — two agents racing
- `TestAutosync_PostPushSync` — push after commit
- `TestAutosync_BranchBypassesForkCheck` — branch bypass with real sync
- `TestDST_ConcurrentCommitForkPrevention` — deterministic sim
- `TestDST_CkinLockExpiry` — simulated time advancement
