# CLI Extension Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add stash, undo, annotate/blame, bisect, and branch commands to the EdgeSync CLI, matching Fossil's behavior.

**Architecture:** Five feature groups built on the existing `go-libfossil` packages. New packages: `undo/`, `stash/`, `annotate/`, `path/`, `bisect/`, `tag/`. Prerequisites: refactor `repo co` to update checkout DB, extend `CheckinOpts` with Tags, add control artifact support. TDD throughout.

**Tech Stack:** Go, `modernc.org/sqlite`, `github.com/alecthomas/kong`, `github.com/hexops/gotextdiff`

**Spec:** `docs/superpowers/specs/2026-03-16-cli-extension-design.md`

**Build command:** `go build -buildvcs=false ./cmd/edgesync/`
**Test command:** `go test -buildvcs=false ./go-libfossil/...`

---

## Chunk 1: Prerequisites

### Task 1: Refactor `repo co` to update checkout DB

The current `repo co` only extracts files to disk. It must also update `vfile` and `vvar` in the checkout DB so that undo, bisect, and stash can work.

**Files:**
- Modify: `cmd/edgesync/repo_co.go`
- Test: `cmd/edgesync/repo_co_test.go` (new)

- [ ] **Step 1: Write failing test for checkout DB update**

Create `cmd/edgesync/repo_co_test.go`:

```go
package main

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	_ "modernc.org/sqlite"
)

func TestCoUpdatesCheckoutDB(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test-user", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}

	// Create two checkins
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("v1")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	rid2, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("v2")}, {Name: "b.txt", Content: []byte("new")}},
		Comment: "second",
		User:    "test",
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}
	r.Close()

	// Open checkout at tip (rid2)
	coDir := filepath.Join(dir, "work")
	os.MkdirAll(coDir, 0o755)
	openCmd := RepoOpenCmd{Dir: coDir}
	g := &Globals{Repo: repoPath}
	if err := openCmd.Run(g); err != nil {
		t.Fatal(err)
	}

	// Now checkout rid1
	coCmd := RepoCoCmd{Version: "tip", Dir: coDir, Force: true}
	// We need to resolve rid1's UUID for version arg
	r2, _ := repo.Open(repoPath)
	var uuid1 string
	r2.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid1).Scan(&uuid1)
	r2.Close()
	coCmd.Version = uuid1[:10]

	if err := coCmd.Run(g); err != nil {
		t.Fatal(err)
	}

	// Verify checkout DB was updated
	ckout, err := openCheckout(coDir)
	if err != nil {
		t.Fatal(err)
	}
	defer ckout.Close()

	var vid int64
	ckout.QueryRow("SELECT value FROM vvar WHERE name='checkout'").Scan(&vid)
	if vid != int64(rid1) {
		t.Errorf("vvar checkout = %d, want %d", vid, rid1)
	}

	// Verify vfile has only rid1's files (just a.txt, no b.txt)
	var count int
	ckout.QueryRow("SELECT count(*) FROM vfile WHERE vid=?", rid1).Scan(&count)
	if count != 1 {
		t.Errorf("vfile count = %d, want 1", count)
	}

	_ = rid2 // used for second checkin
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `go test -buildvcs=false -run TestCoUpdatesCheckoutDB ./cmd/edgesync/ -v`
Expected: FAIL — `vvar checkout` still points at rid2, vfile not updated.

- [ ] **Step 3: Implement checkout DB update in repo_co.go**

Modify `cmd/edgesync/repo_co.go` — add checkout DB update after extracting files:

```go
func (c *RepoCoCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	rid, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	files, err := manifest.ListFiles(r, rid)
	if err != nil {
		return err
	}

	for _, f := range files {
		fileRid, ok := blob.Exists(r.DB(), f.UUID)
		if !ok {
			return fmt.Errorf("blob %s not found for file %s", f.UUID, f.Name)
		}
		data, err := content.Expand(r.DB(), fileRid)
		if err != nil {
			return fmt.Errorf("expanding %s: %w", f.Name, err)
		}

		outPath := filepath.Join(c.Dir, f.Name)
		if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
			return err
		}

		if !c.Force {
			if _, err := os.Stat(outPath); err == nil {
				return fmt.Errorf("file exists: %s (use --force to overwrite)", outPath)
			}
		}

		perm := os.FileMode(0o644)
		if f.Perm == "x" {
			perm = 0o755
		}
		if err := os.WriteFile(outPath, data, perm); err != nil {
			return err
		}

		fmt.Printf("  %s\n", f.Name)
	}

	// Update checkout DB if it exists
	if ckout, err := openCheckout(c.Dir); err == nil {
		defer ckout.Close()
		updateCheckoutDB(ckout, r, rid, files)
	}

	fmt.Printf("checked out %d files\n", len(files))
	return nil
}

// updateCheckoutDB rebuilds vfile and updates vvar for the new checkout version.
func updateCheckoutDB(ckout *sql.DB, r *repo.Repo, rid libfossil.FslID, files []manifest.FileInfo) {
	var ridHash string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&ridHash)

	ckout.Exec("DELETE FROM vfile")
	for _, f := range files {
		isExe := f.Perm == "x"
		var fileRid int64
		r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", f.UUID).Scan(&fileRid)
		ckout.Exec(
			"INSERT INTO vfile(vid, chnged, deleted, isexe, islink, rid, mrid, pathname, mhash) VALUES(?, 0, 0, ?, 0, ?, ?, ?, ?)",
			rid, isExe, fileRid, fileRid, f.Name, f.UUID,
		)
	}

	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('checkout', ?)", rid)
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('checkout-hash', ?)", ridHash)
}
```

Note: You'll need to add `"database/sql"` to imports and the `libfossil` import alias.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension && go test -buildvcs=false -run TestCoUpdatesCheckoutDB ./cmd/edgesync/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add cmd/edgesync/repo_co.go cmd/edgesync/repo_co_test.go
git commit -m "refactor: repo co updates checkout DB (vfile + vvar)"
```

---

### Task 2: Add Tags field to CheckinOpts

`manifest.Checkin()` currently hardcodes trunk tags for initial checkins. We need a `Tags` field so `branch new` can inject custom tags.

**Files:**
- Modify: `go-libfossil/manifest/manifest.go:16-74`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test for custom tags**

Add to `go-libfossil/manifest/manifest_test.go`:

```go
func TestCheckinWithCustomTags(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test-user", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	rid1, _, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Checkin with custom branch tags
	rid2, uuid2, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "branch start",
		User:    "test",
		Parent:  rid1,
		Tags: []deck.TagCard{
			{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "feature-x"},
			{Type: deck.TagSingleton, Name: "sym-feature-x", UUID: "*"},
		},
	})
	if err != nil {
		t.Fatal(err)
	}

	// Verify the manifest contains our custom tags
	d, err := GetManifest(r, rid2)
	if err != nil {
		t.Fatal(err)
	}

	foundBranch := false
	foundSym := false
	for _, tag := range d.T {
		if tag.Name == "branch" && tag.Value == "feature-x" {
			foundBranch = true
		}
		if tag.Name == "sym-feature-x" {
			foundSym = true
		}
	}
	if !foundBranch {
		t.Error("missing branch=feature-x tag")
	}
	if !foundSym {
		t.Error("missing sym-feature-x tag")
	}

	_ = rid2
	_ = uuid2
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension && go test -buildvcs=false -run TestCheckinWithCustomTags ./go-libfossil/manifest/ -v`
Expected: FAIL — `CheckinOpts` has no `Tags` field (compile error).

- [ ] **Step 3: Add Tags field and use it in Checkin()**

In `go-libfossil/manifest/manifest.go`, add `Tags` to `CheckinOpts`:

```go
type CheckinOpts struct {
	Files   []File
	Comment string
	User    string
	Parent  libfossil.FslID
	Delta   bool
	Time    time.Time
	Tags    []deck.TagCard // Custom tags (overrides default trunk tags when non-nil)
}
```

And modify the tag logic in `Checkin()` (around line 68-74):

```go
		// Tags
		if len(opts.Tags) > 0 {
			d.T = opts.Tags
		} else if opts.Parent == 0 {
			d.T = []deck.TagCard{
				{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "trunk"},
				{Type: deck.TagSingleton, Name: "sym-trunk", UUID: "*"},
			}
		}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension && go test -buildvcs=false -run TestCheckinWithCustomTags ./go-libfossil/manifest/ -v`
Expected: PASS

- [ ] **Step 5: Run all manifest tests to verify no regressions**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension && go test -buildvcs=false ./go-libfossil/manifest/ -v`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/manifest/manifest.go go-libfossil/manifest/manifest_test.go
git commit -m "feat: add Tags field to CheckinOpts for custom branch tags"
```

---

### Task 3: Create `go-libfossil/tag/` package for control artifacts

Control artifacts are standalone artifacts (not checkins) containing T-cards, D-card, U-card, Z-card. Used by `branch close` to cancel tags.

**Files:**
- Create: `go-libfossil/tag/tag.go`
- Create: `go-libfossil/tag/tag_test.go`

- [ ] **Step 1: Write failing test for control artifact creation**

Create `go-libfossil/tag/tag_test.go`:

```go
package tag

import (
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func TestAddTag(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test-user", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Add a singleton tag
	ctrlRid, err := AddTag(r, TagOpts{
		TargetRID: rid1,
		TagName:   "testlabel",
		TagType:   TagSingleton,
		Value:     "some-value",
		User:      "test",
	})
	if err != nil {
		t.Fatal(err)
	}
	if ctrlRid == 0 {
		t.Fatal("expected non-zero control artifact rid")
	}

	// Verify tagxref was updated
	var tagtype int
	var value string
	err = r.DB().QueryRow(
		"SELECT tx.tagtype, tx.value FROM tagxref tx JOIN tag t ON t.tagid=tx.tagid WHERE tx.rid=? AND t.tagname=?",
		rid1, "sym-testlabel",
	).Scan(&tagtype, &value)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagtype != 1 {
		t.Errorf("tagtype = %d, want 1 (singleton)", tagtype)
	}
	if value != "some-value" {
		t.Errorf("value = %q, want %q", value, "some-value")
	}
}

func TestCancelTag(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test-user", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Cancel a tag (used by branch close)
	_, err = AddTag(r, TagOpts{
		TargetRID: rid1,
		TagName:   "sym-trunk",
		TagType:   TagCancel,
		User:      "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Verify tagxref shows cancelled
	var tagtype int
	err = r.DB().QueryRow(
		"SELECT tx.tagtype FROM tagxref tx JOIN tag t ON t.tagid=tx.tagid WHERE tx.rid=? AND t.tagname=?",
		rid1, "sym-trunk",
	).Scan(&tagtype)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagtype != 0 {
		t.Errorf("tagtype = %d, want 0 (cancelled)", tagtype)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `go test -buildvcs=false ./go-libfossil/tag/ -v`
Expected: FAIL — package doesn't exist.

- [ ] **Step 3: Implement tag package**

Create `go-libfossil/tag/tag.go`:

```go
package tag

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

const (
	TagCancel     = 0
	TagSingleton  = 1
	TagPropagating = 2
)

type TagOpts struct {
	TargetRID libfossil.FslID
	TagName   string
	TagType   int    // 0=cancel, 1=singleton, 2=propagating
	Value     string
	User      string
	Time      time.Time
}

// AddTag creates a control artifact that adds/cancels a tag on the target artifact.
// Updates tagxref accordingly.
func AddTag(r *repo.Repo, opts TagOpts) (libfossil.FslID, error) {
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	var ctrlRid libfossil.FslID

	err := r.WithTx(func(tx *db.Tx) error {
		// Look up target UUID
		var targetUUID string
		if err := tx.QueryRow("SELECT uuid FROM blob WHERE rid=?", opts.TargetRID).Scan(&targetUUID); err != nil {
			return fmt.Errorf("target rid %d not found: %w", opts.TargetRID, err)
		}

		// Build control deck
		tagType := deck.TagSingleton
		switch opts.TagType {
		case TagCancel:
			tagType = deck.TagCancel
		case TagPropagating:
			tagType = deck.TagPropagating
		}

		tagName := opts.TagName
		// Fossil convention: sym- prefix for symbolic tags
		if tagType != deck.TagCancel {
			// For add operations, ensure sym- prefix
			if len(tagName) > 4 && tagName[:4] != "sym-" {
				tagName = "sym-" + tagName
			}
		}

		d := &deck.Deck{
			Type: deck.Control,
			D:    opts.Time,
			T: []deck.TagCard{
				{Type: tagType, Name: tagName, UUID: targetUUID, Value: opts.Value},
			},
			U: opts.User,
		}

		manifestBytes, err := d.Marshal()
		if err != nil {
			return fmt.Errorf("marshal control artifact: %w", err)
		}

		ctrlRid, _, err = blob.Store(tx, manifestBytes)
		if err != nil {
			return fmt.Errorf("store control artifact: %w", err)
		}

		// Update tagxref
		tagid, err := ensureTag(tx, tagName)
		if err != nil {
			return err
		}

		// Fossil tagtype values: 0=cancel, 1=singleton, 2=propagating
		_, err = tx.Exec(
			"INSERT OR REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid) VALUES(?, ?, ?, ?, ?, ?, ?)",
			tagid, opts.TagType, ctrlRid, opts.TargetRID, opts.Value,
			libfossil.TimeToJulian(opts.Time), opts.TargetRID,
		)
		if err != nil {
			return fmt.Errorf("tagxref: %w", err)
		}

		return nil
	})
	return ctrlRid, err
}

func ensureTag(tx *db.Tx, name string) (int64, error) {
	var tagid int64
	err := tx.QueryRow("SELECT tagid FROM tag WHERE tagname=?", name).Scan(&tagid)
	if err == nil {
		return tagid, nil
	}
	result, err := tx.Exec("INSERT INTO tag(tagname) VALUES(?)", name)
	if err != nil {
		return 0, fmt.Errorf("insert tag %q: %w", name, err)
	}
	return result.LastInsertId()
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension && go test -buildvcs=false ./go-libfossil/tag/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/tag/
git commit -m "feat: add tag package for control artifacts and tagxref management"
```

---

## Chunk 2: Undo

### Task 4: Create `go-libfossil/undo/` package

The undo package manages save/restore/redo of checkout state. It operates on the checkout DB (`.fslckout`).

**Files:**
- Create: `go-libfossil/undo/undo.go`
- Create: `go-libfossil/undo/undo_test.go`

- [ ] **Step 1: Write failing tests for undo save/restore cycle**

Create `go-libfossil/undo/undo_test.go`:

```go
package undo

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"
)

// setupCheckoutDB creates a minimal checkout DB for testing.
func setupCheckoutDB(t *testing.T, dir string) *sql.DB {
	t.Helper()
	dbPath := filepath.Join(dir, ".fslckout")
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		t.Fatal(err)
	}
	_, err = db.Exec(`
		CREATE TABLE vvar(name TEXT PRIMARY KEY, value CLOB) WITHOUT ROWID;
		CREATE TABLE vfile(
			id INTEGER PRIMARY KEY, vid INTEGER, chnged INT DEFAULT 0,
			deleted BOOLEAN DEFAULT 0, isexe BOOLEAN, islink BOOLEAN,
			rid INTEGER, mrid INTEGER, mtime INTEGER, pathname TEXT,
			origname TEXT, mhash TEXT, UNIQUE(pathname, vid)
		);
		CREATE TABLE vmerge(id INTEGER, merge INTEGER, mhash TEXT);
		INSERT INTO vvar(name, value) VALUES('checkout', '10');
		INSERT INTO vvar(name, value) VALUES('undo_available', '0');
		INSERT INTO vvar(name, value) VALUES('undo_checkout', '0');
		INSERT INTO vfile(vid, chnged, rid, mrid, pathname, mhash)
			VALUES(10, 0, 1, 1, 'a.txt', 'abc123');
		INSERT INTO vfile(vid, chnged, rid, mrid, pathname, mhash)
			VALUES(10, 0, 2, 2, 'b.txt', 'def456');
	`)
	if err != nil {
		t.Fatal(err)
	}
	return db
}

func TestSaveAndUndo(t *testing.T) {
	dir := t.TempDir()
	ckout := setupCheckoutDB(t, dir)
	defer ckout.Close()

	// Create files on disk
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("original-a"), 0o644)
	os.WriteFile(filepath.Join(dir, "b.txt"), []byte("original-b"), 0o644)

	// Save undo state
	if err := Save(ckout, dir, nil); err != nil {
		t.Fatal(err)
	}

	// Verify undo_available = 1
	var avail int
	ckout.QueryRow("SELECT value FROM vvar WHERE name='undo_available'").Scan(&avail)
	if avail != 1 {
		t.Errorf("undo_available = %d, want 1", avail)
	}

	// Simulate a destructive operation: modify vfile and file content
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("modified-a"), 0o644)

	// Undo
	if err := Undo(ckout, dir); err != nil {
		t.Fatal(err)
	}

	// Verify file restored
	data, _ := os.ReadFile(filepath.Join(dir, "a.txt"))
	if string(data) != "original-a" {
		t.Errorf("a.txt = %q, want %q", data, "original-a")
	}

	// Verify vfile restored (chnged=0)
	var chnged int
	ckout.QueryRow("SELECT chnged FROM vfile WHERE pathname='a.txt'").Scan(&chnged)
	if chnged != 0 {
		t.Errorf("a.txt chnged = %d, want 0", chnged)
	}

	// Verify undo_available = 2 (redo available)
	ckout.QueryRow("SELECT value FROM vvar WHERE name='undo_available'").Scan(&avail)
	if avail != 2 {
		t.Errorf("undo_available = %d, want 2", avail)
	}
}

func TestRedo(t *testing.T) {
	dir := t.TempDir()
	ckout := setupCheckoutDB(t, dir)
	defer ckout.Close()

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("original"), 0o644)
	os.WriteFile(filepath.Join(dir, "b.txt"), []byte("original-b"), 0o644)

	Save(ckout, dir, nil)

	// Simulate modification
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("modified"), 0o644)

	// Undo
	Undo(ckout, dir)

	// Redo — should restore the modification
	if err := Redo(ckout, dir); err != nil {
		t.Fatal(err)
	}

	data, _ := os.ReadFile(filepath.Join(dir, "a.txt"))
	if string(data) != "modified" {
		t.Errorf("a.txt after redo = %q, want %q", data, "modified")
	}

	var avail int
	ckout.QueryRow("SELECT value FROM vvar WHERE name='undo_available'").Scan(&avail)
	if avail != 1 {
		t.Errorf("after redo undo_available = %d, want 1", avail)
	}
}

func TestUndoNotAvailable(t *testing.T) {
	dir := t.TempDir()
	ckout := setupCheckoutDB(t, dir)
	defer ckout.Close()

	// No save has been called, undo should fail
	err := Undo(ckout, dir)
	if err == nil {
		t.Error("expected error when undo not available")
	}
}

func TestSaveReplacesOldUndo(t *testing.T) {
	dir := t.TempDir()
	ckout := setupCheckoutDB(t, dir)
	defer ckout.Close()

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("v1"), 0o644)
	os.WriteFile(filepath.Join(dir, "b.txt"), []byte("v1-b"), 0o644)
	Save(ckout, dir, nil)

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("v2"), 0o644)
	Save(ckout, dir, nil)

	// Undo should restore v2, not v1
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("v3"), 0o644)
	Undo(ckout, dir)

	data, _ := os.ReadFile(filepath.Join(dir, "a.txt"))
	if string(data) != "v2" {
		t.Errorf("a.txt = %q, want %q (second save)", data, "v2")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `go test -buildvcs=false ./go-libfossil/undo/ -v`
Expected: FAIL — package doesn't exist.

- [ ] **Step 3: Implement undo package**

Create `go-libfossil/undo/undo.go`:

```go
package undo

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
)

// Save captures current checkout state for undo. Optionally takes a list of
// pathnames to snapshot; if nil, snapshots all files in vfile.
func Save(ckout *sql.DB, dir string, pathnames []string) error {
	// Drop and recreate undo tables
	ckout.Exec("DROP TABLE IF EXISTS undo")
	ckout.Exec("DROP TABLE IF EXISTS undo_vfile")
	ckout.Exec("DROP TABLE IF EXISTS undo_vmerge")

	_, err := ckout.Exec(`
		CREATE TABLE undo(
			pathname TEXT, content BLOB, existsflag BOOLEAN,
			isExec BOOLEAN, isLink BOOLEAN, redoflag BOOLEAN DEFAULT 0
		);
		CREATE TABLE undo_vfile AS SELECT * FROM vfile;
		CREATE TABLE undo_vmerge AS SELECT * FROM vmerge;
	`)
	if err != nil {
		return fmt.Errorf("undo save schema: %w", err)
	}

	// Snapshot file contents
	rows, err := ckout.Query("SELECT pathname, isexe FROM vfile")
	if err != nil {
		return fmt.Errorf("undo query vfile: %w", err)
	}
	defer rows.Close()

	filter := make(map[string]bool)
	if pathnames != nil {
		for _, p := range pathnames {
			filter[p] = true
		}
	}

	for rows.Next() {
		var name string
		var isExe bool
		rows.Scan(&name, &isExe)

		if pathnames != nil && !filter[name] {
			continue
		}

		fullPath := filepath.Join(dir, name)
		data, err := os.ReadFile(fullPath)
		exists := err == nil
		if !exists {
			data = nil
		}
		ckout.Exec(
			"INSERT INTO undo(pathname, content, existsflag, isExec, isLink, redoflag) VALUES(?, ?, ?, ?, 0, 0)",
			name, data, exists, isExe,
		)
	}

	// Store undo checkout vid
	var vid string
	ckout.QueryRow("SELECT value FROM vvar WHERE name='checkout'").Scan(&vid)
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('undo_checkout', ?)", vid)
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('undo_available', '1')")

	return nil
}

// Undo restores the state captured by Save. After undo, redo becomes available.
func Undo(ckout *sql.DB, dir string) error {
	return undoRedo(ckout, dir, false)
}

// Redo reverses an undo. After redo, undo becomes available again.
func Redo(ckout *sql.DB, dir string) error {
	return undoRedo(ckout, dir, true)
}

func undoRedo(ckout *sql.DB, dir string, isRedo bool) error {
	var avail int
	ckout.QueryRow("SELECT value FROM vvar WHERE name='undo_available'").Scan(&avail)

	wantAvail := 1
	if isRedo {
		wantAvail = 2
	}
	if avail != wantAvail {
		op := "undo"
		if isRedo {
			op = "redo"
		}
		return fmt.Errorf("nothing to %s", op)
	}

	redoflag := 0
	if isRedo {
		redoflag = 1
	}

	// Restore file contents
	rows, err := ckout.Query(
		"SELECT pathname, content, existsflag, isExec FROM undo WHERE redoflag=?", redoflag,
	)
	if err != nil {
		return fmt.Errorf("undo query: %w", err)
	}
	defer rows.Close()

	type fileState struct {
		name   string
		data   []byte
		exists bool
		isExe  bool
	}
	var files []fileState

	for rows.Next() {
		var fs fileState
		var data []byte
		rows.Scan(&fs.name, &data, &fs.exists, &fs.isExe)
		fs.data = data
		files = append(files, fs)
	}
	rows.Close()

	for _, fs := range files {
		fullPath := filepath.Join(dir, fs.name)

		// Read current state for swap
		curData, curErr := os.ReadFile(fullPath)
		curExists := curErr == nil
		var curExe bool
		if info, err := os.Stat(fullPath); err == nil {
			curExe = info.Mode()&0o111 != 0
		}

		// Restore old state
		if fs.exists {
			os.MkdirAll(filepath.Dir(fullPath), 0o755)
			perm := os.FileMode(0o644)
			if fs.isExe {
				perm = 0o755
			}
			os.WriteFile(fullPath, fs.data, perm)

			label := "UNDO"
			if isRedo {
				label = "REDO"
			}
			if !curExists {
				label = "NEW"
			}
			fmt.Printf("%s   %s\n", label, fs.name)
		} else {
			os.Remove(fullPath)
			fmt.Printf("DELETE %s\n", fs.name)
		}

		// Swap: store current state back into undo with flipped redoflag
		ckout.Exec(
			"UPDATE undo SET content=?, existsflag=?, isExec=?, redoflag=? WHERE pathname=? AND redoflag=?",
			curData, curExists, curExe, 1-redoflag, fs.name, redoflag,
		)
	}

	// Swap vfile <-> undo_vfile
	ckout.Exec("CREATE TEMP TABLE swap_vfile AS SELECT * FROM vfile")
	ckout.Exec("DELETE FROM vfile")
	ckout.Exec("INSERT INTO vfile SELECT * FROM undo_vfile")
	ckout.Exec("DELETE FROM undo_vfile")
	ckout.Exec("INSERT INTO undo_vfile SELECT * FROM swap_vfile")
	ckout.Exec("DROP TABLE swap_vfile")

	// Swap vmerge <-> undo_vmerge
	ckout.Exec("CREATE TEMP TABLE swap_vmerge AS SELECT * FROM vmerge")
	ckout.Exec("DELETE FROM vmerge")
	ckout.Exec("INSERT INTO vmerge SELECT * FROM undo_vmerge")
	ckout.Exec("DELETE FROM undo_vmerge")
	ckout.Exec("INSERT INTO undo_vmerge SELECT * FROM swap_vmerge")
	ckout.Exec("DROP TABLE swap_vmerge")

	// Swap checkout vids
	var undoVid, curVid string
	ckout.QueryRow("SELECT value FROM vvar WHERE name='undo_checkout'").Scan(&undoVid)
	ckout.QueryRow("SELECT value FROM vvar WHERE name='checkout'").Scan(&curVid)
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('checkout', ?)", undoVid)
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('undo_checkout', ?)", curVid)

	// Update availability: after undo -> 2 (redo available), after redo -> 1 (undo available)
	newAvail := 2
	if isRedo {
		newAvail = 1
	}
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('undo_available', ?)", newAvail)

	return nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `go test -buildvcs=false ./go-libfossil/undo/ -v`
Expected: All 4 tests PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/undo/
git commit -m "feat: add undo package — save/restore/redo checkout state"
```

---

### Task 5: CLI commands for undo/redo + integrate into existing commands

**Files:**
- Create: `cmd/edgesync/repo_undo.go`
- Modify: `cmd/edgesync/cli.go` — add Undo, Redo to RepoCmd
- Modify: `cmd/edgesync/repo_add.go` — call undo.Save before add
- Modify: `cmd/edgesync/repo_rm.go` — call undo.Save before rm
- Modify: `cmd/edgesync/repo_rename.go` — call undo.Save before rename
- Modify: `cmd/edgesync/repo_revert.go` — call undo.Save before revert
- Modify: `cmd/edgesync/repo_co.go` — call undo.Save before checkout

- [ ] **Step 1: Create undo/redo CLI commands**

Create `cmd/edgesync/repo_undo.go`:

```go
package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/undo"
)

type RepoUndoCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoUndoCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	return undo.Undo(ckout, c.Dir)
}

type RepoRedoCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoRedoCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	return undo.Redo(ckout, c.Dir)
}
```

- [ ] **Step 2: Add commands to cli.go**

In `cmd/edgesync/cli.go`, add to `RepoCmd` struct:

```go
	Undo    RepoUndoCmd    `cmd:"" help:"Undo last operation"`
	Redo    RepoRedoCmd    `cmd:"" help:"Redo undone operation"`
```

- [ ] **Step 3: Add undo.Save calls to existing commands**

For each of `repo_add.go`, `repo_rm.go`, `repo_rename.go`, `repo_revert.go`, `repo_co.go`:
add an `undo.Save(ckout, c.Dir, nil)` call at the start of the `Run` method, after opening the checkout DB.

Example for `repo_add.go` — add after `checkoutVid()`:

```go
import "github.com/dmestas/edgesync/go-libfossil/undo"
// ...
	if err := undo.Save(ckout, c.Dir, nil); err != nil {
		fmt.Fprintf(os.Stderr, "warning: could not save undo state: %v\n", err)
	}
```

For `repo_co.go`, call `undo.Save` before `updateCheckoutDB` — but only when a checkout DB exists:

```go
	if ckout, err := openCheckout(c.Dir); err == nil {
		undo.Save(ckout, c.Dir, nil)
		updateCheckoutDB(ckout, r, rid, files)
		ckout.Close()
	}
```

- [ ] **Step 4: Verify build succeeds**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension && go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add cmd/edgesync/repo_undo.go cmd/edgesync/cli.go cmd/edgesync/repo_add.go cmd/edgesync/repo_rm.go cmd/edgesync/repo_rename.go cmd/edgesync/repo_revert.go cmd/edgesync/repo_co.go
git commit -m "feat: add undo/redo CLI commands, integrate undo.Save into existing ops"
```

---

## Chunk 3: Stash

### Task 6: Create `go-libfossil/stash/` package

**Files:**
- Create: `go-libfossil/stash/stash.go`
- Create: `go-libfossil/stash/stash_test.go`

- [ ] **Step 1: Write failing tests**

Create `go-libfossil/stash/stash_test.go`:

```go
package stash

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	_ "modernc.org/sqlite"
)

// testEnv sets up a repo, one checkin, a checkout DB, and working files.
func testEnv(t *testing.T) (r *repo.Repo, ckout *sql.DB, dir string, cleanup func()) {
	t.Helper()
	dir = t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test-user", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}

	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Create checkout DB
	ckoutPath := filepath.Join(dir, ".fslckout")
	ckout, _ = sql.Open("sqlite", ckoutPath)
	ckout.Exec(`
		CREATE TABLE vvar(name TEXT PRIMARY KEY, value CLOB) WITHOUT ROWID;
		CREATE TABLE vfile(
			id INTEGER PRIMARY KEY, vid INTEGER, chnged INT DEFAULT 0,
			deleted BOOLEAN DEFAULT 0, isexe BOOLEAN, islink BOOLEAN,
			rid INTEGER, mrid INTEGER, mtime INTEGER, pathname TEXT,
			origname TEXT, mhash TEXT, UNIQUE(pathname, vid)
		);
		CREATE TABLE vmerge(id INTEGER, merge INTEGER, mhash TEXT);
	`)

	// Get file blob rid
	var fileRid int64
	r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?",
		"aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d").Scan(&fileRid) // sha1("hello")

	ckout.Exec("INSERT INTO vvar(name, value) VALUES('checkout', ?)", rid)
	var ridHash string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&ridHash)
	ckout.Exec("INSERT INTO vvar(name, value) VALUES('checkout-hash', ?)", ridHash)
	ckout.Exec("INSERT INTO vvar(name, value) VALUES('stash-next', '1')")
	ckout.Exec("INSERT INTO vvar(name, value) VALUES('undo_available', '0')")
	ckout.Exec("INSERT INTO vvar(name, value) VALUES('undo_checkout', '0')")
	ckout.Exec(`INSERT INTO vfile(vid, chnged, deleted, isexe, islink, rid, mrid, pathname, mhash)
		VALUES(?, 0, 0, 0, 0, ?, ?, 'a.txt', ?)`, rid, fileRid, fileRid,
		"aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d")

	// Write working file
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("hello"), 0o644)

	return r, ckout, dir, func() {
		ckout.Close()
		r.Close()
	}
}

func TestSaveAndList(t *testing.T) {
	r, ckout, dir, cleanup := testEnv(t)
	defer cleanup()

	// Modify file
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("modified"), 0o644)
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")

	err := Save(ckout, r.DB(), dir, "test stash")
	if err != nil {
		t.Fatal(err)
	}

	// File should be reverted
	data, _ := os.ReadFile(filepath.Join(dir, "a.txt"))
	if string(data) != "hello" {
		t.Errorf("after stash save, a.txt = %q, want %q", data, "hello")
	}

	// List should show one entry
	entries, err := List(ckout)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 {
		t.Fatalf("stash list = %d entries, want 1", len(entries))
	}
	if entries[0].Comment != "test stash" {
		t.Errorf("comment = %q, want %q", entries[0].Comment, "test stash")
	}
}

func TestPopRestoresChanges(t *testing.T) {
	r, ckout, dir, cleanup := testEnv(t)
	defer cleanup()

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("modified"), 0o644)
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")

	Save(ckout, r.DB(), dir, "test")

	// Pop should restore
	err := Pop(ckout, r.DB(), dir)
	if err != nil {
		t.Fatal(err)
	}

	data, _ := os.ReadFile(filepath.Join(dir, "a.txt"))
	if string(data) != "modified" {
		t.Errorf("after pop, a.txt = %q, want %q", data, "modified")
	}

	// Stash should be empty
	entries, _ := List(ckout)
	if len(entries) != 0 {
		t.Errorf("stash list after pop = %d, want 0", len(entries))
	}
}

func TestApplyKeepsStash(t *testing.T) {
	r, ckout, dir, cleanup := testEnv(t)
	defer cleanup()

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("modified"), 0o644)
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")

	Save(ckout, r.DB(), dir, "test")
	Apply(ckout, r.DB(), dir, 1)

	// Stash should still exist
	entries, _ := List(ckout)
	if len(entries) != 1 {
		t.Errorf("stash list after apply = %d, want 1", len(entries))
	}
}

func TestDropAndClear(t *testing.T) {
	r, ckout, dir, cleanup := testEnv(t)
	defer cleanup()

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("m1"), 0o644)
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")
	Save(ckout, r.DB(), dir, "first")

	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("m2"), 0o644)
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='a.txt'")
	Save(ckout, r.DB(), dir, "second")

	entries, _ := List(ckout)
	if len(entries) != 2 {
		t.Fatalf("stash list = %d, want 2", len(entries))
	}

	// Drop first
	Drop(ckout, entries[0].ID)
	entries, _ = List(ckout)
	if len(entries) != 1 {
		t.Errorf("after drop, stash list = %d, want 1", len(entries))
	}

	// Clear
	Clear(ckout)
	entries, _ = List(ckout)
	if len(entries) != 0 {
		t.Errorf("after clear, stash list = %d, want 0", len(entries))
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `go test -buildvcs=false ./go-libfossil/stash/ -v`
Expected: FAIL — package doesn't exist.

- [ ] **Step 3: Implement stash package**

Create `go-libfossil/stash/stash.go`:

```go
package stash

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/delta"
)

type Entry struct {
	ID      int64
	Hash    string
	Comment string
	CTime   time.Time
}

// EnsureTables creates stash/stashfile tables if they don't exist.
func EnsureTables(ckout *sql.DB) {
	ckout.Exec(`CREATE TABLE IF NOT EXISTS stash(
		stashid INTEGER PRIMARY KEY, hash TEXT, comment TEXT, ctime TIMESTAMP
	)`)
	ckout.Exec(`CREATE TABLE IF NOT EXISTS stashfile(
		stashid INTEGER REFERENCES stash, isAdded BOOLEAN, isRemoved BOOLEAN,
		isExec BOOLEAN, isLink BOOLEAN, hash TEXT, origname TEXT, newname TEXT,
		delta BLOB, PRIMARY KEY(newname, stashid)
	)`)
	ckout.Exec("INSERT OR IGNORE INTO vvar(name, value) VALUES('stash-next', '1')")
}

// Save stashes all changed files, stores deltas against baseline, reverts working dir.
// repoDB is the repo database for reading baseline blob content.
func Save(ckout *sql.DB, repoDB *sql.DB, dir string, comment string) error {
	EnsureTables(ckout)

	// Get checkout hash (manifest UUID of baseline)
	var checkoutHash string
	ckout.QueryRow("SELECT value FROM vvar WHERE name='checkout-hash'").Scan(&checkoutHash)

	// Get next stash ID
	var stashID int64
	ckout.QueryRow("SELECT value FROM vvar WHERE name='stash-next'").Scan(&stashID)
	ckout.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES('stash-next', ?)", stashID+1)

	ckout.Exec("INSERT INTO stash(stashid, hash, comment, ctime) VALUES(?, ?, ?, ?)",
		stashID, checkoutHash, comment, time.Now().UTC())

	// Find changed/added/deleted files
	rows, err := ckout.Query(
		"SELECT pathname, origname, chnged, deleted, isexe, rid, mhash FROM vfile WHERE chnged=1 OR deleted=1 OR rid=0")
	if err != nil {
		return err
	}
	defer rows.Close()

	type stashFile struct {
		pathname, origname string
		chnged             int
		deleted            bool
		isExe              bool
		rid                int64
		mhash              string
	}
	var sfiles []stashFile
	for rows.Next() {
		var sf stashFile
		rows.Scan(&sf.pathname, &sf.origname, &sf.chnged, &sf.deleted, &sf.isExe, &sf.rid, &sf.mhash)
		sfiles = append(sfiles, sf)
	}
	rows.Close()

	for _, sf := range sfiles {
		isAdded := sf.rid == 0
		isRemoved := sf.deleted

		var fileHash string
		var deltaBytes []byte

		if isRemoved {
			fileHash = sf.mhash
		} else {
			diskData, err := os.ReadFile(filepath.Join(dir, sf.pathname))
			if err != nil {
				return fmt.Errorf("read %s: %w", sf.pathname, err)
			}

			if isAdded {
				// Store raw content for added files
				deltaBytes = diskData
			} else {
				// Compute delta against baseline
				fileHash = sf.mhash
				baseRid, ok := blob.Exists(repoDB, fileHash)
				if ok {
					baseData, err := content.Expand(repoDB, baseRid)
					if err == nil {
						deltaBytes = delta.Create(baseData, diskData)
					} else {
						deltaBytes = diskData // fallback to raw
					}
				} else {
					deltaBytes = diskData
				}
			}
		}

		origname := sf.origname
		if origname == "" {
			origname = sf.pathname
		}

		ckout.Exec(
			"INSERT INTO stashfile(stashid, isAdded, isRemoved, isExec, isLink, hash, origname, newname, delta) VALUES(?,?,?,?,0,?,?,?,?)",
			stashID, isAdded, isRemoved, sf.isExe, fileHash, origname, sf.pathname, deltaBytes,
		)
	}

	// Revert working directory
	for _, sf := range sfiles {
		fullPath := filepath.Join(dir, sf.pathname)
		if sf.rid == 0 {
			// Added file — remove from disk and vfile
			os.Remove(fullPath)
			ckout.Exec("DELETE FROM vfile WHERE pathname=? AND rid=0", sf.pathname)
		} else if sf.deleted {
			// Deleted file — restore from repo
			baseRid, ok := blob.Exists(repoDB, sf.mhash)
			if ok {
				data, err := content.Expand(repoDB, baseRid)
				if err == nil {
					os.WriteFile(fullPath, data, 0o644)
				}
			}
			ckout.Exec("UPDATE vfile SET deleted=0, chnged=0 WHERE pathname=?", sf.pathname)
		} else {
			// Modified file — restore from repo
			baseRid, ok := blob.Exists(repoDB, sf.mhash)
			if ok {
				data, err := content.Expand(repoDB, baseRid)
				if err == nil {
					os.WriteFile(fullPath, data, 0o644)
				}
			}
			if sf.origname != "" && sf.origname != sf.pathname {
				os.Rename(fullPath, filepath.Join(dir, sf.origname))
				ckout.Exec("UPDATE vfile SET pathname=?, origname=NULL, chnged=0 WHERE pathname=?",
					sf.origname, sf.pathname)
			} else {
				ckout.Exec("UPDATE vfile SET chnged=0, deleted=0, origname=NULL WHERE pathname=?", sf.pathname)
			}
		}
	}

	return nil
}

// Apply restores stashed files without removing the stash entry.
func Apply(ckout *sql.DB, repoDB *sql.DB, dir string, stashID int64) error {
	rows, err := ckout.Query(
		"SELECT isAdded, isRemoved, isExec, hash, origname, newname, delta FROM stashfile WHERE stashid=?",
		stashID)
	if err != nil {
		return fmt.Errorf("query stashfile: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var isAdded, isRemoved, isExec bool
		var hash, origname, newname string
		var deltaBytes []byte
		rows.Scan(&isAdded, &isRemoved, &isExec, &hash, &origname, &newname, &deltaBytes)

		fullPath := filepath.Join(dir, newname)

		if isRemoved {
			os.Remove(fullPath)
			fmt.Printf("DELETE %s\n", newname)
			continue
		}

		if isAdded {
			os.MkdirAll(filepath.Dir(fullPath), 0o755)
			perm := os.FileMode(0o644)
			if isExec {
				perm = 0o755
			}
			os.WriteFile(fullPath, deltaBytes, perm)
			fmt.Printf("ADD    %s\n", newname)
			continue
		}

		// Modified — apply delta against baseline
		baseRid, ok := blob.Exists(repoDB, hash)
		if !ok {
			return fmt.Errorf("baseline blob %s not found", hash)
		}
		baseData, err := content.Expand(repoDB, baseRid)
		if err != nil {
			return fmt.Errorf("expand baseline: %w", err)
		}

		restored, err := delta.Apply(baseData, deltaBytes)
		if err != nil {
			// Fallback: deltaBytes might be raw content
			restored = deltaBytes
		}

		perm := os.FileMode(0o644)
		if isExec {
			perm = 0o755
		}
		os.MkdirAll(filepath.Dir(fullPath), 0o755)
		os.WriteFile(fullPath, restored, perm)
		fmt.Printf("UPDATE %s\n", newname)
	}
	return nil
}

// Pop applies the most recent stash and drops it.
func Pop(ckout *sql.DB, repoDB *sql.DB, dir string) error {
	var stashID int64
	err := ckout.QueryRow("SELECT stashid FROM stash ORDER BY stashid DESC LIMIT 1").Scan(&stashID)
	if err != nil {
		return fmt.Errorf("no stash entries")
	}
	if err := Apply(ckout, repoDB, dir, stashID); err != nil {
		return err
	}
	return Drop(ckout, stashID)
}

// List returns all stash entries.
func List(ckout *sql.DB) ([]Entry, error) {
	EnsureTables(ckout)
	rows, err := ckout.Query("SELECT stashid, hash, comment, ctime FROM stash ORDER BY stashid")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var entries []Entry
	for rows.Next() {
		var e Entry
		rows.Scan(&e.ID, &e.Hash, &e.Comment, &e.CTime)
		entries = append(entries, e)
	}
	return entries, nil
}

// Drop removes a specific stash entry.
func Drop(ckout *sql.DB, stashID int64) error {
	ckout.Exec("DELETE FROM stashfile WHERE stashid=?", stashID)
	ckout.Exec("DELETE FROM stash WHERE stashid=?", stashID)
	return nil
}

// Clear removes all stash entries.
func Clear(ckout *sql.DB) {
	ckout.Exec("DELETE FROM stashfile")
	ckout.Exec("DELETE FROM stash")
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `go test -buildvcs=false ./go-libfossil/stash/ -v`
Expected: All tests PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/stash/
git commit -m "feat: add stash package — save/pop/apply/drop/clear with delta storage"
```

---

### Task 7: Stash CLI commands

**Files:**
- Create: `cmd/edgesync/repo_stash.go`
- Modify: `cmd/edgesync/cli.go` — add Stash to RepoCmd

- [ ] **Step 1: Create stash CLI commands**

Create `cmd/edgesync/repo_stash.go`:

```go
package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/stash"
	"github.com/dmestas/edgesync/go-libfossil/undo"
)

type RepoStashCmd struct {
	Save  RepoStashSaveCmd  `cmd:"" help:"Stash working changes"`
	Pop   RepoStashPopCmd   `cmd:"" help:"Apply top stash and drop it"`
	Apply RepoStashApplyCmd `cmd:"" help:"Apply stash without dropping"`
	Ls    RepoStashLsCmd    `cmd:"" help:"List stash entries"`
	Show  RepoStashShowCmd  `cmd:"" help:"Show stash diff"`
	Drop  RepoStashDropCmd  `cmd:"" help:"Remove stash entry"`
	Clear RepoStashClearCmd `cmd:"" help:"Remove all stash entries"`
}

type RepoStashSaveCmd struct {
	Message string `short:"m" help:"Stash message"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashSaveCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()
	return stash.Save(ckout, r.DB(), c.Dir, c.Message)
}

type RepoStashPopCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashPopCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := undo.Save(ckout, c.Dir, nil); err != nil {
		fmt.Printf("warning: could not save undo state: %v\n", err)
	}
	return stash.Pop(ckout, r.DB(), c.Dir)
}

type RepoStashApplyCmd struct {
	ID  int64  `arg:"" optional:"" help:"Stash ID (default: latest)"`
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashApplyCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := undo.Save(ckout, c.Dir, nil); err != nil {
		fmt.Printf("warning: could not save undo state: %v\n", err)
	}

	stashID := c.ID
	if stashID == 0 {
		ckout.QueryRow("SELECT stashid FROM stash ORDER BY stashid DESC LIMIT 1").Scan(&stashID)
		if stashID == 0 {
			return fmt.Errorf("no stash entries")
		}
	}
	return stash.Apply(ckout, r.DB(), c.Dir, stashID)
}

type RepoStashLsCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashLsCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	entries, err := stash.List(ckout)
	if err != nil {
		return err
	}
	if len(entries) == 0 {
		fmt.Println("no stash entries")
		return nil
	}
	for _, e := range entries {
		comment := e.Comment
		if comment == "" {
			comment = "(no message)"
		}
		fmt.Printf("%3d: %s  %s\n", e.ID, e.CTime.Format("2006-01-02 15:04"), comment)
	}
	return nil
}

type RepoStashShowCmd struct {
	ID  int64  `arg:"" optional:"" help:"Stash ID"`
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashShowCmd) Run(g *Globals) error {
	// Show is deferred — prints file list for now
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	stashID := c.ID
	if stashID == 0 {
		ckout.QueryRow("SELECT stashid FROM stash ORDER BY stashid DESC LIMIT 1").Scan(&stashID)
	}

	rows, _ := ckout.Query("SELECT isAdded, isRemoved, newname FROM stashfile WHERE stashid=?", stashID)
	defer rows.Close()
	for rows.Next() {
		var added, removed bool
		var name string
		rows.Scan(&added, &removed, &name)
		switch {
		case added:
			fmt.Printf("ADDED   %s\n", name)
		case removed:
			fmt.Printf("REMOVED %s\n", name)
		default:
			fmt.Printf("EDITED  %s\n", name)
		}
	}
	return nil
}

type RepoStashDropCmd struct {
	ID  int64  `arg:"" required:"" help:"Stash ID to drop"`
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashDropCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	stash.Drop(ckout, c.ID)
	fmt.Printf("dropped stash %d\n", c.ID)
	return nil
}

type RepoStashClearCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoStashClearCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	stash.Clear(ckout)
	fmt.Println("stash cleared")
	return nil
}
```

- [ ] **Step 2: Add Stash to RepoCmd in cli.go**

```go
	Stash   RepoStashCmd   `cmd:"" help:"Stash working changes"`
```

- [ ] **Step 3: Verify build succeeds**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add cmd/edgesync/repo_stash.go cmd/edgesync/cli.go
git commit -m "feat: add stash CLI commands — save, pop, apply, ls, show, drop, clear"
```

---

## Chunk 4: Path-Finding and Bisect

### Task 8: Create `go-libfossil/path/` package — BFS over plink

**Files:**
- Create: `go-libfossil/path/path.go`
- Create: `go-libfossil/path/path_test.go`

- [ ] **Step 1: Write failing tests**

Create `go-libfossil/path/path_test.go`:

```go
package path

import (
	"database/sql"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	_ "modernc.org/sqlite"
)

// buildPlink creates a test plink table with specified edges.
func buildPlink(t *testing.T, edges [][3]interface{}) *sql.DB {
	t.Helper()
	db, err := sql.Open("sqlite", ":memory:")
	if err != nil {
		t.Fatal(err)
	}
	db.Exec("CREATE TABLE plink(pid INTEGER, cid INTEGER, isprim BOOLEAN, mtime REAL, UNIQUE(pid, cid))")
	db.Exec("CREATE INDEX plink_i2 ON plink(cid, pid)")
	for _, e := range edges {
		db.Exec("INSERT INTO plink(pid, cid, isprim) VALUES(?, ?, ?)", e[0], e[1], e[2])
	}
	return db
}

func TestLinearChain(t *testing.T) {
	// 1 -> 2 -> 3 -> 4 -> 5
	edges := [][3]interface{}{{1, 2, 1}, {2, 3, 1}, {3, 4, 1}, {4, 5, 1}}
	db := buildPlink(t, edges)
	defer db.Close()

	path, err := Shortest(db, 1, 5, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(path) != 5 {
		t.Fatalf("path length = %d, want 5", len(path))
	}
	expected := []libfossil.FslID{1, 2, 3, 4, 5}
	for i, n := range path {
		if n.RID != expected[i] {
			t.Errorf("path[%d] = %d, want %d", i, n.RID, expected[i])
		}
	}
}

func TestDiamondMerge(t *testing.T) {
	//   1
	//  / \
	// 2   3
	//  \ /
	//   4
	edges := [][3]interface{}{{1, 2, 1}, {1, 3, 0}, {2, 4, 1}, {3, 4, 0}}
	db := buildPlink(t, edges)
	defer db.Close()

	path, err := Shortest(db, 1, 4, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	// Should find path of length 3 (1->2->4 or 1->3->4)
	if len(path) != 3 {
		t.Fatalf("path length = %d, want 3", len(path))
	}
	if path[0].RID != 1 || path[len(path)-1].RID != 4 {
		t.Errorf("path start=%d end=%d, want 1->4", path[0].RID, path[len(path)-1].RID)
	}
}

func TestDirectOnly(t *testing.T) {
	// 1 -> 2 (prim), 1 -> 3 (merge), 3 -> 4 (prim)
	edges := [][3]interface{}{{1, 2, 1}, {1, 3, 0}, {3, 4, 1}}
	db := buildPlink(t, edges)
	defer db.Close()

	// With directOnly, cannot reach 4 from 1 via merge edge to 3
	_, err := Shortest(db, 1, 4, true, nil)
	if err == nil {
		t.Error("expected error for unreachable path with directOnly")
	}

	// Without directOnly, should find path 1->3->4
	path, err := Shortest(db, 1, 4, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(path) != 3 {
		t.Errorf("path length = %d, want 3", len(path))
	}
}

func TestSkipSet(t *testing.T) {
	// 1 -> 2 -> 3 -> 4
	edges := [][3]interface{}{{1, 2, 1}, {2, 3, 1}, {3, 4, 1}}
	db := buildPlink(t, edges)
	defer db.Close()

	skip := map[libfossil.FslID]bool{3: true}
	_, err := Shortest(db, 1, 4, false, skip)
	// With node 3 skipped and no alternate path, should fail
	if err == nil {
		t.Error("expected error with skipped node blocking only path")
	}
}

func TestNoPath(t *testing.T) {
	// Disconnected: 1->2, 3->4
	edges := [][3]interface{}{{1, 2, 1}, {3, 4, 1}}
	db := buildPlink(t, edges)
	defer db.Close()

	_, err := Shortest(db, 1, 4, false, nil)
	if err == nil {
		t.Error("expected error for disconnected nodes")
	}
}

func TestReversePath(t *testing.T) {
	// 1 -> 2 -> 3: find path from 3 to 1 (walk backwards via parent links)
	edges := [][3]interface{}{{1, 2, 1}, {2, 3, 1}}
	db := buildPlink(t, edges)
	defer db.Close()

	path, err := Shortest(db, 3, 1, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(path) != 3 {
		t.Fatalf("path length = %d, want 3", len(path))
	}
	if path[0].RID != 3 || path[2].RID != 1 {
		t.Errorf("reverse path start=%d end=%d, want 3->1", path[0].RID, path[2].RID)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `go test -buildvcs=false ./go-libfossil/path/ -v`
Expected: FAIL — package doesn't exist.

- [ ] **Step 3: Implement BFS path-finding**

Create `go-libfossil/path/path.go`:

```go
package path

import (
	"database/sql"
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
)

type PathNode struct {
	RID   libfossil.FslID
	From  *PathNode
	Depth int
}

// Shortest finds the shortest path between from and to in the plink DAG.
// BFS explores both parent (pid->cid) and child (cid->pid) directions.
// directOnly restricts to isprim=1 edges only.
// skip excludes nodes from the path.
func Shortest(db *sql.DB, from, to libfossil.FslID, directOnly bool, skip map[libfossil.FslID]bool) ([]PathNode, error) {
	if from == to {
		return []PathNode{{RID: from, Depth: 0}}, nil
	}

	visited := map[libfossil.FslID]*PathNode{}
	startNode := &PathNode{RID: from, Depth: 0}
	visited[from] = startNode
	queue := []*PathNode{startNode}

	primClause := ""
	if directOnly {
		primClause = " AND isprim=1"
	}

	// Queries for both directions
	fwdQuery := fmt.Sprintf("SELECT cid FROM plink WHERE pid=?%s", primClause)
	revQuery := fmt.Sprintf("SELECT pid FROM plink WHERE cid=?%s", primClause)

	for len(queue) > 0 {
		cur := queue[0]
		queue = queue[1:]

		// Explore neighbors in both directions
		for _, q := range []string{fwdQuery, revQuery} {
			rows, err := db.Query(q, cur.RID)
			if err != nil {
				continue
			}
			for rows.Next() {
				var neighbor int64
				rows.Scan(&neighbor)
				nid := libfossil.FslID(neighbor)

				if skip != nil && skip[nid] {
					continue
				}
				if visited[nid] != nil {
					continue
				}

				node := &PathNode{RID: nid, From: cur, Depth: cur.Depth + 1}
				visited[nid] = node

				if nid == to {
					rows.Close()
					return buildPath(node), nil
				}

				queue = append(queue, node)
			}
			rows.Close()
		}
	}

	return nil, fmt.Errorf("no path from %d to %d", from, to)
}

func buildPath(end *PathNode) []PathNode {
	var nodes []PathNode
	for n := end; n != nil; n = n.From {
		nodes = append(nodes, *n)
	}
	// Reverse
	for i, j := 0, len(nodes)-1; i < j; i, j = i+1, j-1 {
		nodes[i], nodes[j] = nodes[j], nodes[i]
	}
	// Renumber depth from start
	for i := range nodes {
		nodes[i].Depth = i
		nodes[i].From = nil // don't leak linked list
	}
	return nodes
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `go test -buildvcs=false ./go-libfossil/path/ -v`
Expected: All 6 tests PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/path/
git commit -m "feat: add path package — BFS shortest path over plink DAG"
```

---

### Task 9: Create `go-libfossil/bisect/` package

**Files:**
- Create: `go-libfossil/bisect/bisect.go`
- Create: `go-libfossil/bisect/bisect_test.go`

- [ ] **Step 1: Write failing tests**

Create `go-libfossil/bisect/bisect_test.go`:

```go
package bisect

import (
	"database/sql"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	_ "modernc.org/sqlite"
)

func setupBisectDB(t *testing.T) *sql.DB {
	t.Helper()
	db, err := sql.Open("sqlite", ":memory:")
	if err != nil {
		t.Fatal(err)
	}
	db.Exec("CREATE TABLE vvar(name TEXT PRIMARY KEY, value CLOB) WITHOUT ROWID")
	db.Exec("CREATE TABLE plink(pid INTEGER, cid INTEGER, isprim BOOLEAN, mtime REAL, UNIQUE(pid, cid))")
	db.Exec("CREATE INDEX plink_i2 ON plink(cid, pid)")
	db.Exec("CREATE TABLE blob(rid INTEGER PRIMARY KEY, uuid TEXT, size INTEGER, content BLOB)")
	db.Exec("CREATE TABLE event(type TEXT, mtime REAL, objid INTEGER, user TEXT, comment TEXT)")

	// Linear chain: 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 8
	for i := 1; i <= 8; i++ {
		db.Exec("INSERT INTO blob(rid, uuid) VALUES(?, ?)", i, fmt.Sprintf("%040d", i))
		db.Exec("INSERT INTO event(type, mtime, objid, user, comment) VALUES('ci', ?, ?, 'test', ?)",
			float64(2460000+i), i, fmt.Sprintf("commit %d", i))
	}
	for i := 1; i < 8; i++ {
		db.Exec("INSERT INTO plink(pid, cid, isprim) VALUES(?, ?, 1)", i, i+1)
	}
	return db
}

func TestBisectSession(t *testing.T) {
	db := setupBisectDB(t)
	defer db.Close()

	s := NewSession(db)

	// Mark good and bad
	if err := s.MarkGood(1); err != nil {
		t.Fatal(err)
	}
	if err := s.MarkBad(8); err != nil {
		t.Fatal(err)
	}

	// Next should pick midpoint
	next, err := s.Next()
	if err != nil {
		t.Fatal(err)
	}
	// Path length 8, midpoint is index 4 = rid 4 or 5
	if next < 3 || next > 6 {
		t.Errorf("first next = %d, expected midpoint near 4-5", next)
	}
}

func TestBisectConverges(t *testing.T) {
	db := setupBisectDB(t)
	defer db.Close()

	s := NewSession(db)
	s.MarkGood(1)
	s.MarkBad(8)

	// Simulate: bug was introduced at commit 5
	for i := 0; i < 10; i++ {
		next, err := s.Next()
		if err != nil {
			// Converged
			break
		}
		if next >= 5 {
			s.MarkBad(next)
		} else {
			s.MarkGood(next)
		}
	}

	status := s.Status()
	if status.Good == 0 || status.Bad == 0 {
		t.Error("bisect did not converge")
	}
}

func TestBisectReset(t *testing.T) {
	db := setupBisectDB(t)
	defer db.Close()

	s := NewSession(db)
	s.MarkGood(1)
	s.MarkBad(8)

	s.Reset()

	status := s.Status()
	if status.Good != 0 || status.Bad != 0 {
		t.Errorf("after reset: good=%d bad=%d, want 0,0", status.Good, status.Bad)
	}
}

func TestBisectSkip(t *testing.T) {
	db := setupBisectDB(t)
	defer db.Close()

	s := NewSession(db)
	s.MarkGood(1)
	s.MarkBad(8)

	next1, _ := s.Next()
	s.Skip(next1)

	next2, err := s.Next()
	if err != nil {
		t.Fatal(err)
	}
	if next2 == next1 {
		t.Error("skip did not exclude the node from next selection")
	}
}
```

Note: you'll need `import "fmt"` for `fmt.Sprintf`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `go test -buildvcs=false ./go-libfossil/bisect/ -v`
Expected: FAIL — package doesn't exist.

- [ ] **Step 3: Implement bisect package**

Create `go-libfossil/bisect/bisect.go`:

```go
package bisect

import (
	"database/sql"
	"fmt"
	"strconv"
	"strings"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/path"
)

type Session struct {
	db *sql.DB
}

type StatusInfo struct {
	Good libfossil.FslID
	Bad  libfossil.FslID
	Log  string
	Steps int // approximate steps remaining
}

func NewSession(db *sql.DB) *Session {
	return &Session{db: db}
}

func (s *Session) MarkGood(rid libfossil.FslID) error {
	s.setVvar("bisect-good", fmt.Sprintf("%d", rid))
	s.appendLog(int64(rid))
	return nil
}

func (s *Session) MarkBad(rid libfossil.FslID) error {
	s.setVvar("bisect-bad", fmt.Sprintf("%d", rid))
	s.appendLog(-int64(rid))
	return nil
}

func (s *Session) Skip(rid libfossil.FslID) error {
	s.appendSkip(rid)
	return nil
}

// Next finds the midpoint of the path between good and bad, excluding skipped nodes.
// Returns the RID to check out next.
func (s *Session) Next() (libfossil.FslID, error) {
	good := s.getVvarInt("bisect-good")
	bad := s.getVvarInt("bisect-bad")
	if good == 0 || bad == 0 {
		return 0, fmt.Errorf("both good and bad must be set before running bisect next")
	}

	skip := s.parseSkipSet()
	p, err := path.Shortest(s.db, libfossil.FslID(good), libfossil.FslID(bad), true, skip)
	if err != nil {
		return 0, fmt.Errorf("no path between good (%d) and bad (%d): %w", good, bad, err)
	}

	if len(p) <= 2 {
		return 0, fmt.Errorf("bisect complete: first bad commit is %d", bad)
	}

	mid := len(p) / 2
	return p[mid].RID, nil
}

func (s *Session) Status() StatusInfo {
	good := s.getVvarInt("bisect-good")
	bad := s.getVvarInt("bisect-bad")
	log := s.getVvar("bisect-log")

	steps := 0
	if good > 0 && bad > 0 {
		skip := s.parseSkipSet()
		p, err := path.Shortest(s.db, libfossil.FslID(good), libfossil.FslID(bad), true, skip)
		if err == nil && len(p) > 2 {
			// log2(n) steps remaining
			n := len(p)
			for n > 1 {
				steps++
				n /= 2
			}
		}
	}

	return StatusInfo{
		Good:  libfossil.FslID(good),
		Bad:   libfossil.FslID(bad),
		Log:   log,
		Steps: steps,
	}
}

// List returns the path between good and bad with labels.
type ListEntry struct {
	RID    libfossil.FslID
	UUID   string
	Date   string
	Label  string // "GOOD", "BAD", "CURRENT", "NEXT", "SKIP", or ""
}

func (s *Session) List(currentRID libfossil.FslID) ([]ListEntry, error) {
	good := s.getVvarInt("bisect-good")
	bad := s.getVvarInt("bisect-bad")
	if good == 0 || bad == 0 {
		return nil, fmt.Errorf("bisect not active")
	}

	skip := s.parseSkipSet()
	p, err := path.Shortest(s.db, libfossil.FslID(good), libfossil.FslID(bad), true, skip)
	if err != nil {
		return nil, err
	}

	nextRID, _ := s.Next()

	var entries []ListEntry
	for _, node := range p {
		var uuid, date string
		s.db.QueryRow("SELECT b.uuid, datetime(e.mtime) FROM blob b JOIN event e ON e.objid=b.rid WHERE b.rid=?",
			node.RID).Scan(&uuid, &date)

		label := ""
		switch node.RID {
		case libfossil.FslID(good):
			label = "GOOD"
		case libfossil.FslID(bad):
			label = "BAD"
		}
		if node.RID == currentRID && label == "" {
			label = "CURRENT"
		}
		if node.RID == nextRID && label == "" {
			label = "NEXT"
		}
		if skip[node.RID] {
			label = "SKIP"
		}

		entries = append(entries, ListEntry{RID: node.RID, UUID: uuid, Date: date, Label: label})
	}
	return entries, nil
}

func (s *Session) Reset() {
	s.setVvar("bisect-good", "")
	s.setVvar("bisect-bad", "")
	s.setVvar("bisect-log", "")
}

// Internal helpers

func (s *Session) getVvar(name string) string {
	var val string
	s.db.QueryRow("SELECT value FROM vvar WHERE name=?", name).Scan(&val)
	return val
}

func (s *Session) getVvarInt(name string) int64 {
	v := s.getVvar(name)
	n, _ := strconv.ParseInt(v, 10, 64)
	return n
}

func (s *Session) setVvar(name, value string) {
	s.db.Exec("INSERT OR REPLACE INTO vvar(name, value) VALUES(?, ?)", name, value)
}

func (s *Session) appendLog(rid int64) {
	log := s.getVvar("bisect-log")
	if log != "" {
		log += " "
	}
	log += fmt.Sprintf("%d", rid)
	s.setVvar("bisect-log", log)
}

func (s *Session) appendSkip(rid libfossil.FslID) {
	log := s.getVvar("bisect-log")
	if log != "" {
		log += " "
	}
	log += fmt.Sprintf("s%d", rid)
	s.setVvar("bisect-log", log)
}

func (s *Session) parseSkipSet() map[libfossil.FslID]bool {
	log := s.getVvar("bisect-log")
	skip := map[libfossil.FslID]bool{}
	for _, tok := range strings.Fields(log) {
		if strings.HasPrefix(tok, "s") {
			n, err := strconv.ParseInt(tok[1:], 10, 64)
			if err == nil {
				skip[libfossil.FslID(n)] = true
			}
		}
	}
	return skip
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `go test -buildvcs=false ./go-libfossil/bisect/ -v`
Expected: All 4 tests PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/bisect/
git commit -m "feat: add bisect package — session state, BFS midpoint, skip-set"
```

---

### Task 10: Bisect CLI commands

**Files:**
- Create: `cmd/edgesync/repo_bisect.go`
- Modify: `cmd/edgesync/cli.go`

- [ ] **Step 1: Create bisect CLI commands**

Create `cmd/edgesync/repo_bisect.go`:

```go
package main

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/bisect"
)

type RepoBisectCmd struct {
	Good   RepoBisectGoodCmd   `cmd:"" help:"Mark version as good"`
	Bad    RepoBisectBadCmd    `cmd:"" help:"Mark version as bad"`
	Next   RepoBisectNextCmd   `cmd:"" help:"Check out midpoint version"`
	Skip   RepoBisectSkipCmd   `cmd:"" help:"Skip current version"`
	Reset  RepoBisectResetCmd  `cmd:"" help:"Clear bisect state"`
	Ls     RepoBisectLsCmd     `cmd:"" help:"Show bisect path"`
	Status RepoBisectStatusCmd `cmd:"" help:"Show bisect state"`
}

type RepoBisectGoodCmd struct {
	Version string `arg:"" optional:"" help:"Version (default: current checkout)"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectGoodCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	rid, err := bisectResolveVersion(g, ckout, c.Version)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	if err := s.MarkGood(rid); err != nil {
		return err
	}
	fmt.Printf("marked %d as good\n", rid)

	return bisectAutoNext(g, ckout, c.Dir, s)
}

type RepoBisectBadCmd struct {
	Version string `arg:"" optional:"" help:"Version (default: current checkout)"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectBadCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	rid, err := bisectResolveVersion(g, ckout, c.Version)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	if err := s.MarkBad(rid); err != nil {
		return err
	}
	fmt.Printf("marked %d as bad\n", rid)

	return bisectAutoNext(g, ckout, c.Dir, s)
}

type RepoBisectNextCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectNextCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	s := bisect.NewSession(ckout)
	return bisectAutoNext(g, ckout, c.Dir, s)
}

type RepoBisectSkipCmd struct {
	Version string `arg:"" optional:"" help:"Version to skip (default: current checkout)"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectSkipCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	rid, err := bisectResolveVersion(g, ckout, c.Version)
	if err != nil {
		return err
	}

	s := bisect.NewSession(ckout)
	s.Skip(rid)
	fmt.Printf("skipped %d\n", rid)

	return bisectAutoNext(g, ckout, c.Dir, s)
}

type RepoBisectResetCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectResetCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()
	bisect.NewSession(ckout).Reset()
	fmt.Println("bisect state cleared")
	return nil
}

type RepoBisectLsCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectLsCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	vid, _ := checkoutVid(ckout)
	s := bisect.NewSession(ckout)
	entries, err := s.List(libfossil.FslID(vid))
	if err != nil {
		return err
	}
	for _, e := range entries {
		uuid := e.UUID
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		label := ""
		if e.Label != "" {
			label = " " + e.Label
		}
		fmt.Printf("%s %s%s\n", e.Date, uuid, label)
	}
	return nil
}

type RepoBisectStatusCmd struct {
	Dir string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoBisectStatusCmd) Run(g *Globals) error {
	ckout, err := openCheckout(c.Dir)
	if err != nil {
		return err
	}
	defer ckout.Close()

	s := bisect.NewSession(ckout)
	st := s.Status()

	if st.Good == 0 && st.Bad == 0 {
		fmt.Println("bisect not active")
		return nil
	}

	fmt.Printf("good: %d\nbad:  %d\n", st.Good, st.Bad)
	if st.Steps > 0 {
		fmt.Printf("~%d steps remaining\n", st.Steps)
	}
	return nil
}

// helpers

func bisectResolveVersion(g *Globals, ckout *sql.DB, version string) (libfossil.FslID, error) {
	if version == "" {
		vid, err := checkoutVid(ckout)
		return libfossil.FslID(vid), err
	}
	r, err := openRepo(g)
	if err != nil {
		return 0, err
	}
	defer r.Close()
	return resolveRID(r, version)
}

func bisectAutoNext(g *Globals, ckout *sql.DB, dir string, s *bisect.Session) error {
	st := s.Status()
	if st.Good == 0 || st.Bad == 0 {
		return nil // not enough info yet
	}

	next, err := s.Next()
	if err != nil {
		fmt.Println(err) // "bisect complete" message
		return nil
	}

	// Checkout the midpoint
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	coCmd := RepoCoCmd{Dir: dir, Force: true}
	var uuid string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", next).Scan(&uuid)
	coCmd.Version = uuid
	return coCmd.Run(g)
}
```

Note: Add imports for `"database/sql"` and the libfossil package alias.

- [ ] **Step 2: Add Bisect to RepoCmd in cli.go**

```go
	Bisect  RepoBisectCmd  `cmd:"" help:"Binary search for bugs"`
```

- [ ] **Step 3: Verify build**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add cmd/edgesync/repo_bisect.go cmd/edgesync/cli.go
git commit -m "feat: add bisect CLI — good, bad, next, skip, reset, ls, status"
```

---

## Chunk 5: Annotate/Blame

### Task 11: Create `go-libfossil/annotate/` package

**Files:**
- Create: `go-libfossil/annotate/annotate.go`
- Create: `go-libfossil/annotate/annotate_test.go`

- [ ] **Step 1: Write failing tests**

Create `go-libfossil/annotate/annotate_test.go`:

```go
package annotate

import (
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func setupAnnotateRepo(t *testing.T) (*repo.Repo, []int64) {
	t.Helper()
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, err := repo.Create(repoPath, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}

	base := time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)

	// Commit 1: line A, line B
	rid1, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "file.txt", Content: []byte("line A\nline B\n")}},
		Comment: "initial", User: "alice", Time: base,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Commit 2: line A, line C (B changed to C)
	rid2, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "file.txt", Content: []byte("line A\nline C\n")}},
		Comment: "change B to C", User: "bob", Parent: rid1, Time: base.Add(time.Hour),
	})
	if err != nil {
		t.Fatal(err)
	}

	// Commit 3: line A, line C, line D (add new line)
	rid3, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "file.txt", Content: []byte("line A\nline C\nline D\n")}},
		Comment: "add D", User: "charlie", Parent: rid2, Time: base.Add(2 * time.Hour),
	})
	if err != nil {
		t.Fatal(err)
	}

	return r, []int64{int64(rid1), int64(rid2), int64(rid3)}
}

func TestAnnotateBasic(t *testing.T) {
	r, rids := setupAnnotateRepo(t)
	defer r.Close()

	lines, err := Annotate(r, Options{
		FilePath: "file.txt",
		StartRID: libfossil.FslID(rids[2]), // tip
	})
	if err != nil {
		t.Fatal(err)
	}

	if len(lines) != 3 {
		t.Fatalf("got %d lines, want 3", len(lines))
	}

	// "line A" was in commit 1 (alice) and never changed
	if lines[0].Version.User != "alice" {
		t.Errorf("line 1 user = %q, want alice", lines[0].Version.User)
	}
	// "line C" was changed in commit 2 (bob)
	if lines[1].Version.User != "bob" {
		t.Errorf("line 2 user = %q, want bob", lines[1].Version.User)
	}
	// "line D" was added in commit 3 (charlie)
	if lines[2].Version.User != "charlie" {
		t.Errorf("line 3 user = %q, want charlie", lines[2].Version.User)
	}
}

func TestAnnotateWithLimit(t *testing.T) {
	r, rids := setupAnnotateRepo(t)
	defer r.Close()

	// Limit to 1 version — only the tip version is examined
	lines, err := Annotate(r, Options{
		FilePath: "file.txt",
		StartRID: libfossil.FslID(rids[2]),
		Limit:    1,
	})
	if err != nil {
		t.Fatal(err)
	}

	// All lines should be attributed to tip (charlie) since we can't walk back further
	for i, l := range lines {
		if l.Version.User != "charlie" {
			t.Errorf("line %d user = %q, want charlie (limit=1)", i+1, l.Version.User)
		}
	}
}

func TestAnnotateWithOrigin(t *testing.T) {
	r, rids := setupAnnotateRepo(t)
	defer r.Close()

	// Origin at commit 2 — don't walk past it
	lines, err := Annotate(r, Options{
		FilePath:  "file.txt",
		StartRID:  libfossil.FslID(rids[2]),
		OriginRID: libfossil.FslID(rids[1]),
	})
	if err != nil {
		t.Fatal(err)
	}

	// "line A" would normally trace to alice (commit 1), but origin stops at commit 2 (bob)
	if lines[0].Version.User != "bob" {
		t.Errorf("line 1 user with origin = %q, want bob", lines[0].Version.User)
	}
}

func TestAnnotateSingleCommit(t *testing.T) {
	r, rids := setupAnnotateRepo(t)
	defer r.Close()

	// Annotate from the first commit — all lines should be alice
	lines, err := Annotate(r, Options{
		FilePath: "file.txt",
		StartRID: libfossil.FslID(rids[0]),
	})
	if err != nil {
		t.Fatal(err)
	}

	if len(lines) != 2 {
		t.Fatalf("got %d lines, want 2", len(lines))
	}
	for _, l := range lines {
		if l.Version.User != "alice" {
			t.Errorf("user = %q, want alice (single commit)", l.Version.User)
		}
	}
}
```

Note: Add `libfossil "github.com/dmestas/edgesync/go-libfossil"` to imports.

- [ ] **Step 2: Run tests to verify they fail**

Run: `go test -buildvcs=false ./go-libfossil/annotate/ -v`
Expected: FAIL — package doesn't exist.

- [ ] **Step 3: Implement annotate package**

Create `go-libfossil/annotate/annotate.go`:

```go
package annotate

import (
	"fmt"
	"strings"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/hexops/gotextdiff"
	"github.com/hexops/gotextdiff/myers"
	"github.com/hexops/gotextdiff/span"
)

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
	FilePath  string
	StartRID  libfossil.FslID
	Limit     int
	OriginRID libfossil.FslID
}

// Annotate attributes each line of a file to the commit that last changed it.
func Annotate(r *repo.Repo, opts Options) ([]Line, error) {
	// Resolve file in starting version
	fileContent, startInfo, err := loadFileAtVersion(r, opts.FilePath, opts.StartRID)
	if err != nil {
		return nil, fmt.Errorf("load file at start: %w", err)
	}

	rawLines := splitLines(string(fileContent))
	lines := make([]Line, len(rawLines))
	for i, text := range rawLines {
		lines[i] = Line{Text: text, Version: startInfo}
	}

	// Walk parent chain
	currentRID := opts.StartRID
	versionsWalked := 0

	for {
		if opts.Limit > 0 && versionsWalked >= opts.Limit {
			break
		}
		if opts.OriginRID != 0 && currentRID == opts.OriginRID {
			break
		}

		// Get primary parent
		var parentRID int64
		err := r.DB().QueryRow(
			"SELECT pid FROM plink WHERE cid=? AND isprim=1", currentRID,
		).Scan(&parentRID)
		if err != nil {
			break // no more parents
		}

		if opts.OriginRID != 0 && libfossil.FslID(parentRID) == opts.OriginRID {
			// Don't walk past origin
			break
		}

		// Load file in parent
		parentContent, parentInfo, err := loadFileAtVersion(r, opts.FilePath, libfossil.FslID(parentRID))
		if err != nil {
			break // file doesn't exist in parent
		}

		// Diff parent vs current: lines that exist in parent get attributed to parent
		currentText := joinAnnotatedLines(lines)
		parentText := string(parentContent)

		attributeUnchanged(lines, rawLines, parentText, currentText, parentInfo)

		currentRID = libfossil.FslID(parentRID)
		versionsWalked++

		// Check if all lines are attributed to ancestors
		allAttributed := true
		for _, l := range lines {
			if l.Version == startInfo {
				allAttributed = false
				break
			}
		}
		if allAttributed {
			break
		}
	}

	return lines, nil
}

// attributeUnchanged uses diff to find lines unchanged between parent and current,
// and pushes their attribution back to the parent version.
func attributeUnchanged(lines []Line, currentLines []string, parentText, currentText string, parentInfo VersionInfo) {
	edits := myers.ComputeEdits(span.URIFromPath("file"), parentText, currentText)
	unified := gotextdiff.ToUnified("parent", "current", parentText, edits)

	// Build a set of line numbers (0-indexed) that are unchanged in the current version
	changedLines := map[int]bool{}
	currentLineIdx := 0
	for _, hunk := range unified.Hunks {
		// Lines before hunk are unchanged
		for currentLineIdx < hunk.ToLine-len(hunk.Lines) {
			currentLineIdx++
		}
		for _, line := range hunk.Lines {
			switch line.Kind {
			case gotextdiff.Insert:
				changedLines[currentLineIdx] = true
				currentLineIdx++
			case gotextdiff.Equal:
				currentLineIdx++
			}
		}
	}

	// Alternative simpler approach: compare line by line between parent lines and current lines
	parentLines := splitLines(parentText)
	pIdx := 0
	cIdx := 0
	for pIdx < len(parentLines) && cIdx < len(lines) {
		if cIdx < len(currentLines) && pIdx < len(parentLines) && currentLines[cIdx] == parentLines[pIdx] {
			// Line unchanged — attribute to parent
			if lines[cIdx].Version != parentInfo {
				// Only push back if currently attributed to a newer version
				lines[cIdx].Version = parentInfo
			}
			pIdx++
			cIdx++
		} else {
			// Find next matching line (simple LCS-like scan)
			found := false
			for lookahead := 1; lookahead < 5 && cIdx+lookahead < len(lines); lookahead++ {
				if cIdx+lookahead < len(currentLines) && pIdx < len(parentLines) &&
					currentLines[cIdx+lookahead] == parentLines[pIdx] {
					cIdx += lookahead
					found = true
					break
				}
			}
			if !found {
				pIdx++
			}
		}
	}
}

func loadFileAtVersion(r *repo.Repo, filePath string, rid libfossil.FslID) ([]byte, VersionInfo, error) {
	files, err := manifest.ListFiles(r, rid)
	if err != nil {
		return nil, VersionInfo{}, err
	}

	var fileUUID string
	for _, f := range files {
		if f.Name == filePath {
			fileUUID = f.UUID
			break
		}
	}
	if fileUUID == "" {
		return nil, VersionInfo{}, fmt.Errorf("file %q not in version %d", filePath, rid)
	}

	fileRid, ok := blob.Exists(r.DB(), fileUUID)
	if !ok {
		return nil, VersionInfo{}, fmt.Errorf("blob %s not found", fileUUID)
	}
	data, err := content.Expand(r.DB(), fileRid)
	if err != nil {
		return nil, VersionInfo{}, err
	}

	// Get version info
	var uuid, user string
	var mtime float64
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&uuid)
	r.DB().QueryRow("SELECT user, mtime FROM event WHERE objid=?", rid).Scan(&user, &mtime)

	return data, VersionInfo{
		UUID: uuid,
		User: user,
		Date: libfossil.JulianToTime(mtime),
	}, nil
}

func splitLines(s string) []string {
	if s == "" {
		return nil
	}
	lines := strings.Split(s, "\n")
	// Remove trailing empty line from final newline
	if len(lines) > 0 && lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	return lines
}

func joinAnnotatedLines(lines []Line) string {
	var b strings.Builder
	for _, l := range lines {
		b.WriteString(l.Text)
		b.WriteByte('\n')
	}
	return b.String()
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `go test -buildvcs=false ./go-libfossil/annotate/ -v`
Expected: All 4 tests PASS

Note: The annotate algorithm uses a simple line-matching approach. If the tests reveal edge cases with the diff-based attribution, iterate on `attributeUnchanged`. The key correctness requirement: lines unchanged from parent get pushed back to parent's version info.

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/annotate/
git commit -m "feat: add annotate package — line-level history attribution"
```

---

### Task 12: Annotate/Blame CLI commands

**Files:**
- Create: `cmd/edgesync/repo_annotate.go`
- Modify: `cmd/edgesync/cli.go`

- [ ] **Step 1: Create CLI commands**

Create `cmd/edgesync/repo_annotate.go`:

```go
package main

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	ann "github.com/dmestas/edgesync/go-libfossil/annotate"
)

type RepoAnnotateCmd struct {
	File    string `required:"" help:"File to annotate"`
	Version string `short:"v" help:"Starting version (default: tip)"`
	Limit   int    `short:"n" help:"Max versions to walk (0=unlimited)" default:"0"`
	Origin  string `help:"Stop at this version"`
	Dir     string `short:"d" help:"Checkout directory" default:"."`
}

func (c *RepoAnnotateCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	startRID, err := resolveRID(r, c.Version)
	if err != nil {
		return err
	}

	var originRID libfossil.FslID
	if c.Origin != "" {
		originRID, err = resolveRID(r, c.Origin)
		if err != nil {
			return err
		}
	}

	lines, err := ann.Annotate(r, ann.Options{
		FilePath:  c.File,
		StartRID:  startRID,
		Limit:     c.Limit,
		OriginRID: originRID,
	})
	if err != nil {
		return err
	}

	for _, l := range lines {
		uuid := l.Version.UUID
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		fmt.Printf("%s %8s %s | %s\n",
			uuid,
			l.Version.User,
			l.Version.Date.Format("2006-01-02"),
			l.Text,
		)
	}
	return nil
}

// RepoBlameCmd is an alias for annotate.
type RepoBlameCmd = RepoAnnotateCmd
```

- [ ] **Step 2: Add to cli.go**

```go
	Annotate RepoAnnotateCmd `cmd:"" help:"Annotate file lines with version history"`
	Blame    RepoBlameCmd    `cmd:"" help:"Alias for annotate"`
```

- [ ] **Step 3: Verify build**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add cmd/edgesync/repo_annotate.go cmd/edgesync/cli.go
git commit -m "feat: add annotate/blame CLI commands"
```

---

## Chunk 6: Branch Operations and Final Integration

### Task 13: Branch CLI commands

**Files:**
- Create: `cmd/edgesync/repo_branch.go`
- Modify: `cmd/edgesync/cli.go`

- [ ] **Step 1: Create branch CLI commands**

Create `cmd/edgesync/repo_branch.go`:

```go
package main

import (
	"fmt"
	"strings"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/tag"
)

type RepoBranchCmd struct {
	Ls    RepoBranchLsCmd    `cmd:"" help:"List branches"`
	New   RepoBranchNewCmd   `cmd:"" help:"Create new branch"`
	Close RepoBranchCloseCmd `cmd:"" help:"Close a branch"`
}

type RepoBranchLsCmd struct {
	Closed bool `help:"Show only closed branches"`
	All    bool `help:"Show all branches (open and closed)"`
}

func (c *RepoBranchLsCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	query := `
		SELECT DISTINCT substr(t.tagname, 5) AS branch,
			b.uuid, datetime(e.mtime), e.user
		FROM tag t
		JOIN tagxref tx ON tx.tagid = t.tagid
		JOIN blob b ON b.rid = tx.rid
		LEFT JOIN event e ON e.objid = tx.rid
		WHERE t.tagname LIKE 'sym-%'
	`
	if c.Closed {
		query += " AND tx.tagtype = 0"
	} else if !c.All {
		query += " AND tx.tagtype > 0"
	}
	query += " ORDER BY e.mtime DESC"

	rows, err := r.DB().Query(query)
	if err != nil {
		return err
	}
	defer rows.Close()

	found := false
	for rows.Next() {
		var branch, uuid, date, user string
		rows.Scan(&branch, &uuid, &date, &user)
		if len(uuid) > 10 {
			uuid = uuid[:10]
		}
		fmt.Printf("%-20s %s %s %s\n", branch, uuid, date, user)
		found = true
	}
	if !found {
		fmt.Println("no branches found")
	}
	return nil
}

type RepoBranchNewCmd struct {
	Name    string `arg:"" help:"Branch name"`
	From    string `help:"Parent version (default: tip)"`
	Message string `short:"m" help:"Commit message" default:"Create branch"`
}

func (c *RepoBranchNewCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	parentRID, err := resolveRID(r, c.From)
	if err != nil {
		return err
	}

	// Read all files from parent manifest (full carry-forward)
	parentFiles, err := manifest.ListFiles(r, parentRID)
	if err != nil {
		return err
	}

	// Build File slice with content from repo
	files := make([]manifest.File, 0, len(parentFiles))
	for _, pf := range parentFiles {
		data, err := expandFile(r, pf.UUID)
		if err != nil {
			return fmt.Errorf("read parent file %s: %w", pf.Name, err)
		}
		files = append(files, manifest.File{
			Name:    pf.Name,
			Content: data,
			Perm:    pf.Perm,
		})
	}

	branchTag := c.Name
	if !strings.HasPrefix(branchTag, "sym-") {
		branchTag = c.Name
	}

	rid, uuid, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: c.Message,
		User:    currentUser(),
		Parent:  parentRID,
		Time:    time.Now().UTC(),
		Tags: []deck.TagCard{
			{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: c.Name},
			{Type: deck.TagSingleton, Name: "sym-" + c.Name, UUID: "*"},
		},
	})
	if err != nil {
		return err
	}

	fmt.Printf("created branch %q at %s\n", c.Name, uuid[:10])
	_ = rid
	return nil
}

type RepoBranchCloseCmd struct {
	Name string `arg:"" help:"Branch name to close"`
}

func (c *RepoBranchCloseCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	// Find the branch tip
	symName := "sym-" + c.Name
	var tipRID int64
	err = r.DB().QueryRow(
		"SELECT tx.rid FROM tagxref tx JOIN tag t ON t.tagid=tx.tagid WHERE t.tagname=? AND tx.tagtype>0 ORDER BY tx.mtime DESC LIMIT 1",
		symName,
	).Scan(&tipRID)
	if err != nil {
		return fmt.Errorf("branch %q not found", c.Name)
	}

	// Cancel the branch tag
	_, err = tag.AddTag(r, tag.TagOpts{
		TargetRID: libfossil.FslID(tipRID),
		TagName:   symName,
		TagType:   tag.TagCancel,
		User:      currentUser(),
	})
	if err != nil {
		return err
	}

	// Add "closed" tag
	_, err = tag.AddTag(r, tag.TagOpts{
		TargetRID: libfossil.FslID(tipRID),
		TagName:   "closed",
		TagType:   tag.TagSingleton,
		User:      currentUser(),
	})
	if err != nil {
		return err
	}

	fmt.Printf("closed branch %q\n", c.Name)
	return nil
}
```

- [ ] **Step 2: Add Branch to RepoCmd in cli.go**

```go
	Branch  RepoBranchCmd  `cmd:"" help:"Branch operations"`
```

- [ ] **Step 3: Verify build**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add cmd/edgesync/repo_branch.go cmd/edgesync/cli.go
git commit -m "feat: add branch CLI — ls, new, close"
```

---

### Task 14: Integration tests

End-to-end tests that create real repos and exercise full CLI flows.

**Files:**
- Create: `go-libfossil/integration_cli_test.go`

- [ ] **Step 1: Write integration tests**

Create `go-libfossil/integration_cli_test.go` (or add to existing `integration_test.go`):

```go
package libfossil_test

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/annotate"
	"github.com/dmestas/edgesync/go-libfossil/bisect"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/path"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/stash"
	"github.com/dmestas/edgesync/go-libfossil/undo"
	_ "modernc.org/sqlite"
)

func TestIntegrationStash(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, _ := repo.Create(repoPath, "test", simio.CryptoRand{})

	rid, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world\n")}},
		Comment: "init", User: "test",
	})

	// Set up checkout DB
	ckoutPath := filepath.Join(dir, ".fslckout")
	ckout, _ := sql.Open("sqlite", ckoutPath)
	ckout.Exec(`CREATE TABLE vvar(name TEXT PRIMARY KEY, value CLOB) WITHOUT ROWID;
		CREATE TABLE vfile(id INTEGER PRIMARY KEY, vid INTEGER, chnged INT DEFAULT 0,
			deleted BOOLEAN DEFAULT 0, isexe BOOLEAN, islink BOOLEAN, rid INTEGER,
			mrid INTEGER, mtime INTEGER, pathname TEXT, origname TEXT, mhash TEXT,
			UNIQUE(pathname, vid));
		CREATE TABLE vmerge(id INTEGER, merge INTEGER, mhash TEXT)`)

	var fileRid int64
	r.DB().QueryRow("SELECT rid FROM blob WHERE uuid='d3486ae9136e7856bc42212385ea797094475802'").Scan(&fileRid)
	var ridHash string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&ridHash)
	ckout.Exec("INSERT INTO vvar VALUES('checkout', ?)", rid)
	ckout.Exec("INSERT INTO vvar VALUES('checkout-hash', ?)", ridHash)
	ckout.Exec("INSERT INTO vvar VALUES('undo_available', '0')")
	ckout.Exec("INSERT INTO vvar VALUES('undo_checkout', '0')")
	ckout.Exec("INSERT INTO vvar VALUES('stash-next', '1')")
	ckout.Exec("INSERT INTO vfile(vid,rid,mrid,pathname,mhash) VALUES(?,?,?,'hello.txt',?)",
		rid, fileRid, fileRid, "d3486ae9136e7856bc42212385ea797094475802")

	// Write file and modify
	os.WriteFile(filepath.Join(dir, "hello.txt"), []byte("hello world\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "hello.txt"), []byte("modified\n"), 0o644)
	ckout.Exec("UPDATE vfile SET chnged=1 WHERE pathname='hello.txt'")

	// Stash save
	if err := stash.Save(ckout, r.DB(), dir, "test stash"); err != nil {
		t.Fatal(err)
	}

	// Verify reverted
	data, _ := os.ReadFile(filepath.Join(dir, "hello.txt"))
	if string(data) == "modified\n" {
		t.Error("file should be reverted after stash save")
	}

	// Stash pop
	if err := stash.Pop(ckout, r.DB(), dir); err != nil {
		t.Fatal(err)
	}

	data, _ = os.ReadFile(filepath.Join(dir, "hello.txt"))
	if string(data) != "modified\n" {
		t.Errorf("after pop: %q, want %q", data, "modified\n")
	}

	ckout.Close()
	r.Close()
}

func TestIntegrationUndo(t *testing.T) {
	dir := t.TempDir()
	ckoutPath := filepath.Join(dir, ".fslckout")
	ckout, _ := sql.Open("sqlite", ckoutPath)
	ckout.Exec(`CREATE TABLE vvar(name TEXT PRIMARY KEY, value CLOB) WITHOUT ROWID;
		CREATE TABLE vfile(id INTEGER PRIMARY KEY, vid INTEGER, chnged INT DEFAULT 0,
			deleted BOOLEAN DEFAULT 0, isexe BOOLEAN, islink BOOLEAN, rid INTEGER,
			mrid INTEGER, mtime INTEGER, pathname TEXT, origname TEXT, mhash TEXT,
			UNIQUE(pathname, vid));
		CREATE TABLE vmerge(id INTEGER, merge INTEGER, mhash TEXT)`)
	ckout.Exec("INSERT INTO vvar VALUES('checkout', '1')")
	ckout.Exec("INSERT INTO vvar VALUES('undo_available', '0')")
	ckout.Exec("INSERT INTO vvar VALUES('undo_checkout', '0')")
	ckout.Exec("INSERT INTO vfile(vid, rid, pathname) VALUES(1, 1, 'test.txt')")

	os.WriteFile(filepath.Join(dir, "test.txt"), []byte("original"), 0o644)
	undo.Save(ckout, dir, nil)

	os.WriteFile(filepath.Join(dir, "test.txt"), []byte("changed"), 0o644)
	undo.Undo(ckout, dir)

	data, _ := os.ReadFile(filepath.Join(dir, "test.txt"))
	if string(data) != "original" {
		t.Errorf("after undo: %q, want original", data)
	}

	undo.Redo(ckout, dir)
	data, _ = os.ReadFile(filepath.Join(dir, "test.txt"))
	if string(data) != "changed" {
		t.Errorf("after redo: %q, want changed", data)
	}

	ckout.Close()
}

func TestIntegrationAnnotate(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, _ := repo.Create(repoPath, "test", simio.CryptoRand{})

	rid1, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{{Name: "f.txt", Content: []byte("A\nB\n")}},
		Comment: "v1", User: "alice",
	})
	rid2, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{{Name: "f.txt", Content: []byte("A\nC\n")}},
		Comment: "v2", User: "bob", Parent: rid1,
	})

	lines, err := annotate.Annotate(r, annotate.Options{
		FilePath: "f.txt", StartRID: rid2,
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(lines) != 2 {
		t.Fatalf("got %d lines, want 2", len(lines))
	}
	if lines[0].Version.User != "alice" {
		t.Errorf("line 1: %s, want alice", lines[0].Version.User)
	}
	if lines[1].Version.User != "bob" {
		t.Errorf("line 2: %s, want bob", lines[1].Version.User)
	}
	r.Close()
}

func TestIntegrationBisect(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, _ := repo.Create(repoPath, "test", simio.CryptoRand{})

	// Create 8 commits
	var rids []libfossil.FslID
	var parent libfossil.FslID
	for i := 1; i <= 8; i++ {
		rid, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
			Files:   []manifest.File{{Name: "f.txt", Content: []byte(fmt.Sprintf("v%d\n", i))}},
			Comment: fmt.Sprintf("commit %d", i), User: "test", Parent: parent,
		})
		rids = append(rids, rid)
		parent = rid
	}
	r.Close()

	// Set up checkout DB with plink available (repo has them)
	r2, _ := repo.Open(repoPath)
	defer r2.Close()

	s := bisect.NewSession(r2.DB())
	s.MarkGood(rids[0])
	s.MarkBad(rids[7])

	next, err := s.Next()
	if err != nil {
		t.Fatal(err)
	}
	// Should be somewhere in the middle
	if next == rids[0] || next == rids[7] {
		t.Errorf("next = %d, should be a midpoint", next)
	}
}

func TestIntegrationPath(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	r, _ := repo.Create(repoPath, "test", simio.CryptoRand{})

	rid1, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{{Name: "f.txt", Content: []byte("1\n")}},
		Comment: "c1", User: "test",
	})
	rid2, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{{Name: "f.txt", Content: []byte("2\n")}},
		Comment: "c2", User: "test", Parent: rid1,
	})
	rid3, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{{Name: "f.txt", Content: []byte("3\n")}},
		Comment: "c3", User: "test", Parent: rid2,
	})

	p, err := path.Shortest(r.DB(), rid1, rid3, false, nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(p) != 3 {
		t.Errorf("path length = %d, want 3", len(p))
	}

	r.Close()
}
```

Note: add `"fmt"` to imports for Sprintf.

- [ ] **Step 2: Run integration tests**

Run: `go test -buildvcs=false -run TestIntegration ./go-libfossil/ -v -timeout 60s`
Expected: All PASS

- [ ] **Step 3: Run full test suite**

Run: `go test -buildvcs=false ./go-libfossil/... -timeout 120s`
Expected: All packages PASS

- [ ] **Step 4: Verify final build**

Run: `go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add go-libfossil/integration_cli_test.go
git commit -m "test: add integration tests for stash, undo, annotate, bisect, path"
```

---

### Task 15: Final cli.go update and verify all commands

- [ ] **Step 1: Verify cli.go has all new commands**

The final `RepoCmd` struct in `cli.go` should have these new entries:

```go
type RepoCmd struct {
	// ... existing commands ...
	Undo     RepoUndoCmd     `cmd:"" help:"Undo last operation"`
	Redo     RepoRedoCmd     `cmd:"" help:"Redo undone operation"`
	Stash    RepoStashCmd    `cmd:"" help:"Stash working changes"`
	Annotate RepoAnnotateCmd `cmd:"" help:"Annotate file lines with version history"`
	Blame    RepoBlameCmd    `cmd:"" help:"Alias for annotate"`
	Bisect   RepoBisectCmd   `cmd:"" help:"Binary search for bugs"`
	Branch   RepoBranchCmd   `cmd:"" help:"Branch operations"`
}
```

- [ ] **Step 2: Build and run help**

Run: `go build -buildvcs=false -o /tmp/edgesync ./cmd/edgesync/ && /tmp/edgesync repo --help`
Expected: All new subcommands listed in help output.

- [ ] **Step 3: Final commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/feat-cli-extension
git add -A
git commit -m "edgesync: add CLI extension — stash, undo, annotate, bisect, branch"
```
