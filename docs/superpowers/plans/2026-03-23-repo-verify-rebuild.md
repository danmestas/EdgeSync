# Repo Verify & Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add comprehensive repo verification (report-all) and full rebuild (drop-and-recompute derived tables in transaction) to go-libfossil — a complete `fossil rebuild` replacement in Go.

**Architecture:** New `go-libfossil/verify/` package. `Verify()` performs a read-only 4-phase scan collecting all issues. `Rebuild()` drops derived tables and reconstructs them from raw blobs in a single SQLite transaction — structure pass first (event/mlink/plink/filename), then tag pass (inline + control artifacts), then leaves + sync bookkeeping. Both return a structured `Report`.

**Tech Stack:** Go, SQLite, go-libfossil packages (repo, content, blob, deck, hash, tag, manifest, db)

**Spec:** `docs/superpowers/specs/2026-03-23-repo-verify-rebuild-design.md`

**Branch:** `feature/cdg-132-repo-verify-rebuild` from `main`, in worktree

---

## File Structure

| Action | Path | Purpose |
|--------|------|---------|
| Create | `go-libfossil/verify/verify.go` | Types (`Report`, `Issue`, `IssueKind`), `Verify()` entry point, phase orchestration |
| Create | `go-libfossil/verify/check_blobs.go` | Phase 1: blob integrity (expand, hash, compare) |
| Create | `go-libfossil/verify/check_structure.go` | Phases 2-3: delta chain + phantom integrity |
| Create | `go-libfossil/verify/check_derived.go` | Phase 4: event, mlink, plink, tagxref, filename, leaf consistency |
| Create | `go-libfossil/verify/rebuild.go` | `Rebuild()` entry point, transaction wrapper, drop tables |
| Create | `go-libfossil/verify/rebuild_manifest.go` | Step 3: manifest walking, event/mlink/plink/filename insertion |
| Create | `go-libfossil/verify/rebuild_tags.go` | Step 4: inline T-card + control artifact tag application |
| Create | `go-libfossil/verify/rebuild_leaves.go` | Steps 5-6: leaf computation, sync bookkeeping |
| Create | `go-libfossil/verify/verify_test.go` | All tests |

---

### Task 0: Set Up Worktree and Branch

**Files:** None (git setup)

- [ ] **Step 1: Create worktree from main**

```bash
git worktree add .worktrees/verify-rebuild main
cd .worktrees/verify-rebuild
git checkout -b feature/cdg-132-repo-verify-rebuild
```

- [ ] **Step 2: Verify build**

```bash
cd .worktrees/verify-rebuild
go build -buildvcs=false ./go-libfossil/...
```

Expected: builds clean, no errors.

---

### Task 1: Types + Verify Skeleton

**Files:**
- Create: `go-libfossil/verify/verify.go`
- Create: `go-libfossil/verify/verify_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/verify/verify_test.go`:

```go
package verify_test

import (
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/verify"
)

func newTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	dir := t.TempDir()
	r, err := repo.Create(dir+"/test.fossil", "test", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestVerify_EmptyRepo(t *testing.T) {
	r := newTestRepo(t)
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		t.Fatalf("expected clean verify on empty repo, got %d issues", len(report.Issues))
	}
	if report.BlobsChecked != 0 {
		t.Fatalf("expected 0 blobs checked on empty repo, got %d", report.BlobsChecked)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run TestVerify_EmptyRepo`
Expected: FAIL — package does not exist.

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/verify/verify.go`:

```go
package verify

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

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
	IssueMissingReference                  // parent/file UUID in manifest doesn't resolve
)

// Issue describes a single problem found during verification.
type Issue struct {
	Kind    IssueKind       // category of problem
	RID     libfossil.FslID // affected blob RID (0 if not blob-specific)
	UUID    string          // affected blob UUID (empty if not applicable)
	Table   string          // affected table name
	Message string          // human-readable description
}

// Report summarizes the results of a Verify or Rebuild.
type Report struct {
	Issues        []Issue
	BlobsChecked  int
	BlobsOK       int
	BlobsFailed   int
	BlobsSkipped  int      // blobs that couldn't be processed
	MissingRefs   int      // parent/file UUIDs that didn't resolve
	TablesRebuilt []string // table names reconstructed (Rebuild only)
	Duration      time.Duration
}

// OK returns true if no issues were found.
func (r *Report) OK() bool { return len(r.Issues) == 0 }

// addIssue appends an issue to the report.
func (r *Report) addIssue(kind IssueKind, rid libfossil.FslID, uuid, table, msg string) {
	r.Issues = append(r.Issues, Issue{
		Kind:    kind,
		RID:     rid,
		UUID:    uuid,
		Table:   table,
		Message: msg,
	})
}

// Verify performs a read-only scan of the entire repo, collecting all issues.
// Never stops early — reports every problem found.
//
// Panics if r is nil (TigerStyle precondition).
func Verify(r *repo.Repo) (*Report, error) {
	if r == nil {
		panic("verify.Verify: nil *repo.Repo")
	}

	start := time.Now()
	report := &Report{}

	if err := checkBlobs(r, report); err != nil {
		return nil, fmt.Errorf("verify.Verify: %w", err)
	}
	if err := checkDeltaChains(r, report); err != nil {
		return nil, fmt.Errorf("verify.Verify: %w", err)
	}
	if err := checkPhantoms(r, report); err != nil {
		return nil, fmt.Errorf("verify.Verify: %w", err)
	}
	if err := checkDerived(r, report); err != nil {
		return nil, fmt.Errorf("verify.Verify: %w", err)
	}

	report.Duration = time.Since(start)
	return report, nil
}
```

Also create stub functions so it compiles. Create `go-libfossil/verify/check_blobs.go`:

```go
package verify

import "github.com/dmestas/edgesync/go-libfossil/repo"

func checkBlobs(r *repo.Repo, report *Report) error {
	return nil // stub — implemented in Task 2
}
```

Create `go-libfossil/verify/check_structure.go`:

```go
package verify

import "github.com/dmestas/edgesync/go-libfossil/repo"

func checkDeltaChains(r *repo.Repo, report *Report) error {
	return nil // stub — implemented in Task 3
}

func checkPhantoms(r *repo.Repo, report *Report) error {
	return nil // stub — implemented in Task 3
}
```

Create `go-libfossil/verify/check_derived.go`:

```go
package verify

import "github.com/dmestas/edgesync/go-libfossil/repo"

func checkDerived(r *repo.Repo, report *Report) error {
	return nil // stub — implemented in Task 4
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run TestVerify_EmptyRepo`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/verify/
git commit -m "feat(verify): add types, Report, Issue, Verify skeleton with stubs"
```

---

### Task 2: Phase 1 — Blob Integrity Check

**Files:**
- Modify: `go-libfossil/verify/check_blobs.go`
- Modify: `go-libfossil/verify/verify_test.go`

- [ ] **Step 1: Write the failing tests**

Add to `verify_test.go`:

```go
import (
	"github.com/dmestas/edgesync/go-libfossil/manifest"
)

func TestVerify_CleanRepo(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		for _, iss := range report.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("expected clean verify, got %d issues", len(report.Issues))
	}
	if report.BlobsChecked == 0 {
		t.Fatal("expected blobs to be checked")
	}
	if report.BlobsOK != report.BlobsChecked {
		t.Fatalf("expected all blobs OK, got %d/%d", report.BlobsOK, report.BlobsChecked)
	}
}

func TestVerify_DetectsHashMismatch(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("good content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Corrupt a file blob by flipping its content
	_, err = r.DB().Exec(`
		UPDATE blob SET content = X'0000000000'
		WHERE rid = (SELECT MAX(rid) FROM blob WHERE size >= 0)
	`)
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if report.OK() {
		t.Fatal("expected issues after corruption")
	}
	if report.BlobsFailed == 0 {
		t.Fatal("expected at least one failed blob")
	}

	// Check that the right issue kind was reported
	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueHashMismatch || iss.Kind == verify.IssueBlobCorrupt {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("expected IssueHashMismatch or IssueBlobCorrupt")
	}
}

func TestVerify_ReportsAll(t *testing.T) {
	r := newTestRepo(t)

	// Create two checkins so we have multiple blobs
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{
			{Name: "a.txt", Content: []byte("alpha")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Corrupt ALL non-phantom blobs
	_, err = r.DB().Exec("UPDATE blob SET content = X'0000' WHERE size >= 0")
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}

	// Should report multiple issues, not just the first
	if len(report.Issues) < 2 {
		t.Fatalf("expected multiple issues (report-all), got %d", len(report.Issues))
	}
	t.Logf("report-all: %d issues from %d blobs", len(report.Issues), report.BlobsChecked)
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestVerify_Clean|TestVerify_Detects|TestVerify_Reports'`
Expected: FAIL — `checkBlobs` is a stub returning nil.

- [ ] **Step 3: Write implementation**

Replace `go-libfossil/verify/check_blobs.go`:

```go
package verify

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// checkBlobs verifies every non-phantom blob: expand delta chains,
// hash content, compare against stored UUID.
func checkBlobs(r *repo.Repo, report *Report) error {
	if r == nil {
		panic("checkBlobs: nil *repo.Repo")
	}
	if report == nil {
		panic("checkBlobs: nil *Report")
	}

	rows, err := r.DB().Query("SELECT rid, uuid FROM blob WHERE size >= 0")
	if err != nil {
		return fmt.Errorf("checkBlobs: query: %w", err)
	}
	defer rows.Close()

	type blobEntry struct {
		rid  int64
		uuid string
	}
	var entries []blobEntry
	for rows.Next() {
		var e blobEntry
		if err := rows.Scan(&e.rid, &e.uuid); err != nil {
			return fmt.Errorf("checkBlobs: scan: %w", err)
		}
		entries = append(entries, e)
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("checkBlobs: rows: %w", err)
	}

	for _, e := range entries {
		report.BlobsChecked++
		rid := libfossil.FslID(e.rid)

		data, err := content.Expand(r.DB(), rid)
		if err != nil {
			report.BlobsFailed++
			report.addIssue(IssueBlobCorrupt, rid, e.uuid, "blob",
				fmt.Sprintf("rid %d: expand failed: %v", e.rid, err))
			continue
		}

		var computed string
		if len(e.uuid) == 64 {
			computed = hash.SHA3(data)
		} else {
			computed = hash.SHA1(data)
		}

		if computed != e.uuid {
			report.BlobsFailed++
			report.addIssue(IssueHashMismatch, rid, e.uuid, "blob",
				fmt.Sprintf("rid %d: hash mismatch (stored=%s computed=%s)", e.rid, e.uuid, computed))
			continue
		}

		report.BlobsOK++
	}

	return nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestVerify_'`
Expected: ALL PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/verify/check_blobs.go go-libfossil/verify/verify_test.go
git commit -m "feat(verify): Phase 1 blob integrity check with report-all"
```

---

### Task 3: Phases 2-3 — Delta Chain + Phantom Integrity

**Files:**
- Modify: `go-libfossil/verify/check_structure.go`
- Modify: `go-libfossil/verify/verify_test.go`

- [ ] **Step 1: Write the failing tests**

Add to `verify_test.go`:

```go
func TestVerify_DetectsDanglingDelta(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Insert a delta row pointing to a nonexistent source blob
	_, err = r.DB().Exec("INSERT INTO delta(rid, srcid) VALUES(999999, 888888)")
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}

	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueDeltaDangling {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("expected IssueDeltaDangling")
	}
}

func TestVerify_DetectsOrphanPhantom(t *testing.T) {
	r := newTestRepo(t)

	// Insert a phantom row with no corresponding blob
	_, err := r.DB().Exec("INSERT INTO phantom(rid) VALUES(999999)")
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}

	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssuePhantomOrphan {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("expected IssuePhantomOrphan")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestVerify_DetectsDangling|TestVerify_DetectsOrphan'`
Expected: FAIL — stubs return nil.

- [ ] **Step 3: Write implementation**

Replace `go-libfossil/verify/check_structure.go`:

```go
package verify

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// checkDeltaChains verifies every delta row references existing blobs.
func checkDeltaChains(r *repo.Repo, report *Report) error {
	if r == nil {
		panic("checkDeltaChains: nil *repo.Repo")
	}
	if report == nil {
		panic("checkDeltaChains: nil *Report")
	}

	rows, err := r.DB().Query(`
		SELECT d.rid, d.srcid
		FROM delta d
		WHERE NOT EXISTS (SELECT 1 FROM blob WHERE rid = d.rid)
		   OR NOT EXISTS (SELECT 1 FROM blob WHERE rid = d.srcid)
	`)
	if err != nil {
		return fmt.Errorf("checkDeltaChains: query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid, srcid int64
		if err := rows.Scan(&rid, &srcid); err != nil {
			return fmt.Errorf("checkDeltaChains: scan: %w", err)
		}
		report.addIssue(IssueDeltaDangling, libfossil.FslID(rid), "", "delta",
			fmt.Sprintf("delta rid=%d srcid=%d: one or both blobs missing", rid, srcid))
	}
	return rows.Err()
}

// checkPhantoms verifies every phantom row has a corresponding blob.
func checkPhantoms(r *repo.Repo, report *Report) error {
	if r == nil {
		panic("checkPhantoms: nil *repo.Repo")
	}
	if report == nil {
		panic("checkPhantoms: nil *Report")
	}

	rows, err := r.DB().Query(`
		SELECT p.rid FROM phantom p
		WHERE NOT EXISTS (SELECT 1 FROM blob WHERE rid = p.rid)
	`)
	if err != nil {
		return fmt.Errorf("checkPhantoms: query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid int64
		if err := rows.Scan(&rid); err != nil {
			return fmt.Errorf("checkPhantoms: scan: %w", err)
		}
		report.addIssue(IssuePhantomOrphan, libfossil.FslID(rid), "", "phantom",
			fmt.Sprintf("phantom rid=%d: no corresponding blob", rid))
	}
	return rows.Err()
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestVerify_'`
Expected: ALL PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/verify/check_structure.go go-libfossil/verify/verify_test.go
git commit -m "feat(verify): Phases 2-3 delta chain and phantom integrity checks"
```

---

### Task 4: Phase 4 — Derived Table Consistency

**Files:**
- Modify: `go-libfossil/verify/check_derived.go`
- Modify: `go-libfossil/verify/verify_test.go`

- [ ] **Step 1: Write the failing tests**

Add to `verify_test.go`:

```go
func TestVerify_DetectsMissingEvent(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Delete the event row
	_, err = r.DB().Exec("DELETE FROM event")
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}

	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueEventMissing {
			found = true
			break
		}
	}
	if !found {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d %s", iss.Kind, iss.Message)
		}
		t.Fatal("expected IssueEventMissing")
	}
}

func TestVerify_DetectsMissingPlink(t *testing.T) {
	r := newTestRepo(t)

	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{
			{Name: "a.txt", Content: []byte("alpha")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Delete plink rows
	_, err = r.DB().Exec("DELETE FROM plink")
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}

	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssuePlinkMissing {
			found = true
			break
		}
	}
	if !found {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d %s", iss.Kind, iss.Message)
		}
		t.Fatal("expected IssuePlinkMissing")
	}
}

func TestVerify_DetectsIncorrectLeaf(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Clear the leaf table
	_, err = r.DB().Exec("DELETE FROM leaf")
	if err != nil {
		t.Fatal(err)
	}

	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}

	found := false
	for _, iss := range report.Issues {
		if iss.Kind == verify.IssueLeafIncorrect {
			found = true
			break
		}
	}
	if !found {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d %s", iss.Kind, iss.Message)
		}
		t.Fatal("expected IssueLeafIncorrect")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestVerify_DetectsMissing|TestVerify_DetectsIncorrect'`
Expected: FAIL — `checkDerived` is a stub.

- [ ] **Step 3: Write implementation**

Replace `go-libfossil/verify/check_derived.go`. This file checks checkin manifests against event/mlink/plink/filename tables, checks control artifacts against tagxref, and verifies the leaf set.

Key patterns to follow (from `manifest/crosslink.go`):
- Walk all non-phantom blobs, try `content.Expand` + `deck.Parse`
- For `deck.Checkin`: verify event exists, verify plinks for each P-card, verify mlinks for each F-card
- For `deck.Control`: verify tagxref entries for each T-card
- Compute leaf set: checkins with no children in plink
- Compare computed leaves against actual `leaf` table

Implementation should:
- Query all blobs with `size >= 0`
- For each: expand, parse, skip non-manifests
- For checkins: check event row (`SELECT 1 FROM event WHERE objid = ?`), check plinks, check mlinks
- For control artifacts: check tagxref entries
- For leaf check: `SELECT objid FROM event WHERE type='ci'`, then for each check `NOT EXISTS (SELECT 1 FROM plink WHERE pid = objid)`, compare with `SELECT rid FROM leaf`
- Use delta manifest expansion (merge B-card baseline) via same logic as `manifest.ListFiles` for full file set

The file should be under 70 lines per function — split into `checkCheckins`, `checkControlArtifacts`, `checkLeaves` helpers called from `checkDerived`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestVerify_'`
Expected: ALL PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/verify/check_derived.go go-libfossil/verify/verify_test.go
git commit -m "feat(verify): Phase 4 derived table consistency checks"
```

---

### Task 5: Rebuild — Drop + Structure Pass

**Files:**
- Modify: `go-libfossil/verify/rebuild.go`
- Create: `go-libfossil/verify/rebuild_manifest.go`
- Modify: `go-libfossil/verify/verify_test.go`

- [ ] **Step 1: Write the failing tests**

Add to `verify_test.go`:

```go
func TestRebuild_ReconstructsFromScratch(t *testing.T) {
	r := newTestRepo(t)

	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "a.txt", Content: []byte("alpha")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Snapshot derived table counts before
	var eventCount, mlinkCount, plinkCount int
	r.DB().QueryRow("SELECT count(*) FROM event").Scan(&eventCount)
	r.DB().QueryRow("SELECT count(*) FROM mlink").Scan(&mlinkCount)
	r.DB().QueryRow("SELECT count(*) FROM plink").Scan(&plinkCount)

	// Delete all derived tables
	r.DB().Exec("DELETE FROM event")
	r.DB().Exec("DELETE FROM mlink")
	r.DB().Exec("DELETE FROM plink")
	r.DB().Exec("DELETE FROM tagxref")
	r.DB().Exec("DELETE FROM filename")
	r.DB().Exec("DELETE FROM leaf")
	r.DB().Exec("DELETE FROM unclustered")
	r.DB().Exec("DELETE FROM unsent")

	// Rebuild
	report, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}

	if len(report.TablesRebuilt) == 0 {
		t.Fatal("expected TablesRebuilt to be populated")
	}

	// Verify counts match original
	var newEventCount, newMlinkCount, newPlinkCount int
	r.DB().QueryRow("SELECT count(*) FROM event").Scan(&newEventCount)
	r.DB().QueryRow("SELECT count(*) FROM mlink").Scan(&newMlinkCount)
	r.DB().QueryRow("SELECT count(*) FROM plink").Scan(&newPlinkCount)

	if newEventCount != eventCount {
		t.Fatalf("event count: expected %d, got %d", eventCount, newEventCount)
	}
	if newPlinkCount != plinkCount {
		t.Fatalf("plink count: expected %d, got %d", plinkCount, newPlinkCount)
	}
	// mlink count may differ slightly due to pmid/pid differences, but should be nonzero
	if newMlinkCount == 0 {
		t.Fatal("expected mlink rows after rebuild")
	}
}

func TestRebuild_Idempotent(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Rebuild twice
	report1, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}
	report2, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}

	// Second rebuild should have same blob results
	if report1.BlobsChecked != report2.BlobsChecked {
		t.Fatalf("blobs checked: %d vs %d", report1.BlobsChecked, report2.BlobsChecked)
	}

	// Verify the repo is clean after rebuild
	vReport, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !vReport.OK() {
		for _, iss := range vReport.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("expected clean verify after rebuild, got %d issues", len(vReport.Issues))
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestRebuild_'`
Expected: FAIL — `Rebuild` not defined.

- [ ] **Step 3: Write Rebuild entry point**

Create `go-libfossil/verify/rebuild.go`:

```go
package verify

import (
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// tables that Rebuild drops and recomputes.
var rebuildTables = []string{
	"event", "mlink", "plink", "tagxref",
	"filename", "leaf", "unclustered", "unsent",
}

// Rebuild drops all derived tables and recomputes them from raw blobs
// in a single transaction. If anything fails, the transaction rolls back
// and the repo is untouched. Includes blob verification.
//
// Panics if r is nil (TigerStyle precondition).
func Rebuild(r *repo.Repo) (*Report, error) {
	if r == nil {
		panic("verify.Rebuild: nil *repo.Repo")
	}

	start := time.Now()
	report := &Report{}

	// Phase 1: verify blobs (outside transaction — read-only)
	if err := checkBlobs(r, report); err != nil {
		return nil, fmt.Errorf("verify.Rebuild: %w", err)
	}

	// Phases 2-6: drop + reconstruct in transaction
	if err := r.WithTx(func(tx *db.Tx) error {
		// Step 2: drop derived tables
		for _, table := range rebuildTables {
			if _, err := tx.Exec("DELETE FROM " + table); err != nil {
				return fmt.Errorf("drop %s: %w", table, err)
			}
		}

		// Step 3: structure pass (event, mlink, plink, filename)
		if err := rebuildManifests(r, tx, report); err != nil {
			return fmt.Errorf("rebuild manifests: %w", err)
		}

		// Step 4: tag pass (inline + control artifacts)
		if err := rebuildTags(r, tx, report); err != nil {
			return fmt.Errorf("rebuild tags: %w", err)
		}

		// Step 5: compute leaves
		if err := rebuildLeaves(tx); err != nil {
			return fmt.Errorf("rebuild leaves: %w", err)
		}

		// Step 6: sync bookkeeping
		if err := rebuildBookkeeping(tx); err != nil {
			return fmt.Errorf("rebuild bookkeeping: %w", err)
		}

		return nil
	}); err != nil {
		return nil, fmt.Errorf("verify.Rebuild: %w", err)
	}

	report.TablesRebuilt = rebuildTables
	report.Duration = time.Since(start)
	return report, nil
}
```

Create `go-libfossil/verify/rebuild_manifest.go` with the structure pass. This walks all non-phantom blobs, expands + parses, and for checkin manifests inserts event/mlink/plink/filename rows. Follows the pattern in `crosslink.go:crosslinkOne`.

**Key implementation notes:**
- Use `tx` for ALL SQL (both reads and writes) — we're inside a transaction
- For delta manifests (`d.B != ""`), use `manifest.ListFiles(r, rid)` to get the full file set as `[]manifest.FileEntry{Name, UUID, Perm}`. For non-delta manifests, iterate `d.F` directly.
- `ensureFilename` pattern: `SELECT fnid FROM filename WHERE name=?`, if not found `INSERT INTO filename(name) VALUES(?)`
- Skip blobs that fail `content.Expand` or `deck.Parse` — increment `report.BlobsSkipped`
- When `blob.Exists` returns false for a parent/file UUID, increment `report.MissingRefs` and append `IssueMissingReference` — do NOT error, continue
- Do NOT process T-cards here — that's Task 6 (plinks must exist first for propagation)
- Collect T-card data (checkin rid + T-cards, control rid + T-cards) into slices for Task 6's `rebuildTags` to consume. Pass these via a shared struct or return them.

**Transaction boundary note:** Unlike `crosslink.go` which processes inline T-cards AFTER the transaction commits, Rebuild must keep everything in one transaction for atomicity. This is why Task 6 uses `tag.ApplyTagWithTx` instead of `tag.ApplyTag`.

**Scaffold structure** — split into helpers to stay under 70 lines per function:
- `rebuildManifests(r, tx, report)` — outer loop: walk blobs, expand, parse, dispatch to helpers
- `rebuildCheckin(tx, rid, d, report)` — insert event + mlink + plink for one checkin manifest
- `rebuildEnsureFilename(tx, name)` — ensure filename row exists, return fnid

SQL patterns (from `crosslink.go` and `manifest.go`):

```sql
-- event
INSERT OR IGNORE INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, ?, ?)
-- mtime = libfossil.TimeToJulian(d.D), objid = manifestRid

-- plink (for each P-card)
INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, ?, ?)
-- pid = parentRid (from blob.Exists(tx, parentUUID))
-- cid = manifestRid, isprim = 1 for first parent / 0 for others

-- filename
INSERT OR IGNORE INTO filename(name) VALUES(?)
SELECT fnid FROM filename WHERE name = ?

-- mlink (for each file)
INSERT OR IGNORE INTO mlink(mid, fid, fnid) VALUES(?, ?, ?)
-- mid = manifestRid, fid = fileRid (from blob.Exists), fnid from ensureFilename
```

Create `go-libfossil/verify/rebuild_tags.go` as a stub (implemented in Task 6):

```go
package verify

import (
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

func rebuildTags(r *repo.Repo, tx *db.Tx, report *Report) error {
	return nil // stub — implemented in Task 6
}
```

Create `go-libfossil/verify/rebuild_leaves.go`:

```go
package verify

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

// rebuildLeaves computes the leaf set from the plink graph.
func rebuildLeaves(tx *db.Tx) error {
	_, err := tx.Exec(`
		INSERT INTO leaf(rid)
		SELECT e.objid FROM event e
		WHERE e.type = 'ci'
		AND NOT EXISTS (SELECT 1 FROM plink WHERE pid = e.objid)
	`)
	if err != nil {
		return fmt.Errorf("rebuildLeaves: %w", err)
	}
	return nil
}

// rebuildBookkeeping marks all non-phantom blobs as unclustered and unsent.
func rebuildBookkeeping(tx *db.Tx) error {
	if _, err := tx.Exec(`
		INSERT INTO unclustered(rid)
		SELECT rid FROM blob WHERE size >= 0
	`); err != nil {
		return fmt.Errorf("rebuildBookkeeping: unclustered: %w", err)
	}
	if _, err := tx.Exec(`
		INSERT INTO unsent(rid)
		SELECT rid FROM blob WHERE size >= 0
	`); err != nil {
		return fmt.Errorf("rebuildBookkeeping: unsent: %w", err)
	}
	return nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestRebuild_'`
Expected: PASS (tags are stubbed but basic structure tests should pass)

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/verify/rebuild.go go-libfossil/verify/rebuild_manifest.go \
        go-libfossil/verify/rebuild_tags.go go-libfossil/verify/rebuild_leaves.go \
        go-libfossil/verify/verify_test.go
git commit -m "feat(verify): Rebuild with structure pass, leaves, and bookkeeping"
```

---

### Task 6: Rebuild — Tag Pass (Inline + Control Artifacts)

**Files:**
- Modify: `go-libfossil/tag/tag.go` (add `ApplyTagWithTx`)
- Modify: `go-libfossil/verify/rebuild_tags.go`
- Modify: `go-libfossil/verify/verify_test.go`

**IMPORTANT — Transaction Conflict:**
`tag.ApplyTag` wraps its own `r.WithTx()` internally. Since Rebuild runs inside a single transaction, calling `ApplyTag` from within would create a nested transaction and fail. We must add a `tag.ApplyTagWithTx(tx, opts)` variant that operates within an existing transaction. This follows the same SQL as `ApplyTag` but accepts `*db.Tx` instead of `*repo.Repo`.

- [ ] **Step 0: Add ApplyTagWithTx to tag package**

Add to `go-libfossil/tag/tag.go`:

```go
// ApplyTagWithTx is like ApplyTag but operates within an existing transaction.
// Used by Rebuild to maintain single-transaction atomicity.
//
// Panics if tx or opts are invalid (TigerStyle precondition).
func ApplyTagWithTx(tx *db.Tx, opts ApplyOpts) error {
	if tx == nil {
		panic("tag.ApplyTagWithTx: nil *db.Tx")
	}
	if opts.TagName == "" {
		panic("tag.ApplyTagWithTx: empty TagName")
	}
	if opts.TargetRID <= 0 {
		panic("tag.ApplyTagWithTx: TargetRID must be positive")
	}
	return applyTagInner(tx, opts)
}
```

Extract the inner logic of `ApplyTag` into `applyTagInner(q db.Querier, opts ApplyOpts) error` so both `ApplyTag` and `ApplyTagWithTx` share the same implementation. `ApplyTag` wraps it in `r.WithTx`, `ApplyTagWithTx` calls it directly on the provided tx.

Commit this separately:
```bash
git add go-libfossil/tag/tag.go
git commit -m "feat(tag): add ApplyTagWithTx for use within existing transactions"
```

- [ ] **Step 1: Write the failing test**

Add to `verify_test.go`:

```go
import (
	"github.com/dmestas/edgesync/go-libfossil/deck"
)

func TestRebuild_ReconstructsTags(t *testing.T) {
	r := newTestRepo(t)

	// Create a checkin with branch tags (inline T-cards)
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("content")}},
		Comment: "initial on trunk",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Count tagxref rows before
	var tagxrefCount int
	r.DB().QueryRow("SELECT count(*) FROM tagxref").Scan(&tagxrefCount)
	if tagxrefCount == 0 {
		t.Fatal("expected tagxref rows after checkin with trunk tags")
	}

	// Delete all derived tables
	r.DB().Exec("DELETE FROM event")
	r.DB().Exec("DELETE FROM mlink")
	r.DB().Exec("DELETE FROM plink")
	r.DB().Exec("DELETE FROM tagxref")
	r.DB().Exec("DELETE FROM filename")
	r.DB().Exec("DELETE FROM leaf")
	r.DB().Exec("DELETE FROM unclustered")
	r.DB().Exec("DELETE FROM unsent")

	// Rebuild
	report, err := verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}
	_ = report

	// Verify tagxref was reconstructed
	var newTagxrefCount int
	r.DB().QueryRow("SELECT count(*) FROM tagxref").Scan(&newTagxrefCount)
	if newTagxrefCount == 0 {
		t.Fatal("expected tagxref rows after rebuild")
	}

	// Verify repo is clean
	vReport, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !vReport.OK() {
		for _, iss := range vReport.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("expected clean verify after rebuild, got %d issues", len(vReport.Issues))
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run TestRebuild_ReconstructsTags`
Expected: FAIL — `rebuildTags` is a stub, tagxref count will be 0.

- [ ] **Step 3: Write implementation**

Replace `go-libfossil/verify/rebuild_tags.go`. This is the second pass — runs after all event/plink/mlink are inserted (plink graph must exist for tag propagation).

Key implementation (follows `crosslink.go` two-pass pattern):
- Walk all non-phantom blobs again, expand + parse
- For each checkin manifest: process inline T-cards (where UUID == "*") via `tag.ApplyTagWithTx(tx, ...)`
- For each control artifact (deck.Control): process T-cards (where UUID != "*") via `tag.ApplyTagWithTx(tx, ...)`
- Convert deck tag types to tag package constants:
  - `deck.TagPropagating ('*')` → `tag.TagPropagating (2)`
  - `deck.TagSingleton ('+')` → `tag.TagSingleton (1)`
  - `deck.TagCancel ('-')` → `tag.TagCancel (0)`
- Use `libfossil.TimeToJulian(d.D)` for mtime
- Resolve target UUIDs via `blob.Exists(tx, uuid)` — if missing, increment `MissingRefs`, continue
- Use `tag.ApplyTagWithTx(tx, ...)` (NOT `tag.ApplyTag`) — we're inside Rebuild's transaction

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -run 'TestRebuild_'`
Expected: ALL PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/verify/rebuild_tags.go go-libfossil/verify/verify_test.go
git commit -m "feat(verify): Rebuild tag pass — inline T-cards and control artifacts"
```

---

### Task 7: BUGGIFY Resilience + Transaction Rollback Tests

**Files:**
- Modify: `go-libfossil/verify/verify_test.go`

- [ ] **Step 1: Write the tests**

Add to `verify_test.go`:

```go
func TestRebuild_BuggifyResilience(t *testing.T) {
	r := newTestRepo(t)

	// Create repo BEFORE enabling BUGGIFY
	var files []manifest.File
	for i := 0; i < 20; i++ {
		files = append(files, manifest.File{
			Name:    fmt.Sprintf("file%d.txt", i),
			Content: []byte(fmt.Sprintf("content %d", i)),
		})
	}
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: "buggify test",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Enable BUGGIFY after checkin
	simio.EnableBuggify(42)
	defer simio.DisableBuggify()

	report, err := verify.Rebuild(r)
	if err != nil {
		// Rebuild may error if manifest blob is corrupted by BUGGIFY — acceptable
		t.Logf("Rebuild under BUGGIFY returned error (expected): %v", err)
		return
	}

	// If it succeeded, verify the repo is usable
	if len(report.TablesRebuilt) == 0 {
		t.Fatal("expected TablesRebuilt after successful rebuild")
	}
	t.Logf("BUGGIFY rebuild: %d blobs checked, %d failed, %d skipped",
		report.BlobsChecked, report.BlobsFailed, report.BlobsSkipped)
}

func TestVerify_AfterRebuild_IsClean(t *testing.T) {
	r := newTestRepo(t)

	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("alpha")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "a.txt", Content: []byte("alpha modified")},
			{Name: "b.txt", Content: []byte("bravo")},
		},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Rebuild from scratch
	_, err = verify.Rebuild(r)
	if err != nil {
		t.Fatal(err)
	}

	// Verify should be completely clean
	report, err := verify.Verify(r)
	if err != nil {
		t.Fatal(err)
	}
	if !report.OK() {
		for _, iss := range report.Issues {
			t.Logf("issue: kind=%d table=%s %s", iss.Kind, iss.Table, iss.Message)
		}
		t.Fatalf("expected clean verify after rebuild, got %d issues", len(report.Issues))
	}
}
```

Add `"fmt"` and `"github.com/dmestas/edgesync/go-libfossil/simio"` to test imports.

- [ ] **Step 2: Run tests**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v`
Expected: ALL PASS

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/verify/verify_test.go
git commit -m "test(verify): add BUGGIFY resilience and verify-after-rebuild tests"
```

---

### Task 8: Full Test Suite + Cross-Module Check

**Files:** None (verification only)

- [ ] **Step 1: Run verify package tests**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/verify/ -v -count=1`
Expected: ALL PASS

- [ ] **Step 2: Run all go-libfossil tests for regressions**

Run: `cd .worktrees/verify-rebuild && go test -buildvcs=false ./go-libfossil/...`
Expected: ALL PASS

- [ ] **Step 3: Build check**

Run: `cd .worktrees/verify-rebuild && go build -buildvcs=false ./go-libfossil/...`
Expected: builds clean

- [ ] **Step 4: Final commit if any cleanup needed**

No new commit unless cleanup was required.
