# Tag Propagation + Branch Management Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add tag propagation (plink DAG tree-walk), extend Crosslink to process control artifacts, and create a branch management package.

**Architecture:** Tag propagation in `tag/propagate.go` walks `plink` to cascade tags to descendants (matching Fossil's `tag_propagate`). `AddTag` auto-propagates. New `ApplyTag` function for Crosslink to process existing control artifacts. `branch/` package creates branch checkins via `manifest.Checkin` with T-cards, queries via `tagxref`.

**Tech Stack:** Go, SQLite, `container/heap`

**Spec:** `docs/superpowers/specs/2026-03-22-tag-propagation-branch-mgmt-design.md`

**Worktree:** `.worktrees/branch-mgmt`

**Working directory:** `/Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt`

---

### Task 1: Tag propagation algorithm

**Files:**
- Create: `go-libfossil/tag/propagate.go`
- Modify: `go-libfossil/tag/tag.go`
- Modify: `go-libfossil/tag/tag_test.go`

- [ ] **Step 1: Write failing test for propagation through a chain**

Add to `tag_test.go`:

```go
func TestPropagateChain(t *testing.T) {
	r := setupTestRepo(t)

	// Create chain: A → B → C
	ridA := makeCheckin(t, r, 0, "a.txt", "aaa", "commit A")
	ridB := makeCheckin(t, r, ridA, "a.txt", "bbb", "commit B")
	ridC := makeCheckin(t, r, ridB, "a.txt", "ccc", "commit C")

	// Add propagating tag to A.
	_, err := AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridA),
		TagName:   "branch",
		TagType:   TagPropagating,
		Value:     "trunk",
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}

	// Verify tag propagated to B and C with srcid=0.
	for _, rid := range []int64{ridB, ridC} {
		var srcid int64
		var value string
		err := r.DB().QueryRow(
			"SELECT srcid, value FROM tagxref JOIN tag USING(tagid) WHERE tagname='branch' AND rid=?", rid,
		).Scan(&srcid, &value)
		if err != nil {
			t.Fatalf("tagxref query rid=%d: %v", rid, err)
		}
		if srcid != 0 {
			t.Errorf("rid=%d srcid=%d, want 0 (propagated)", rid, srcid)
		}
		if value != "trunk" {
			t.Errorf("rid=%d value=%q, want %q", rid, value, "trunk")
		}
	}
}

// makeCheckin is a test helper that creates a checkin and returns its RID.
func makeCheckin(t *testing.T, r *repo.Repo, parent int64, name, content, comment string) int64 {
	t.Helper()
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: name, Content: []byte(content)}},
		Comment: comment,
		User:    "testuser",
		Parent:  libfossil.FslID(parent),
		Time:    time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	return int64(rid)
}
```

Add `libfossil "github.com/dmestas/edgesync/go-libfossil"` to imports if not present.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/tag/ -run TestPropagateChain -v`
Expected: FAIL — srcid != 0 (tag not propagated)

- [ ] **Step 3: Write failing test for cancel propagation**

```go
func TestCancelPropagation(t *testing.T) {
	r := setupTestRepo(t)

	ridA := makeCheckin(t, r, 0, "a.txt", "aaa", "commit A")
	ridB := makeCheckin(t, r, ridA, "a.txt", "bbb", "commit B")
	ridC := makeCheckin(t, r, ridB, "a.txt", "ccc", "commit C")

	// Add propagating tag to A.
	_, err := AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridA),
		TagName:   "mybranch",
		TagType:   TagPropagating,
		Value:     "feature-x",
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag propagate: %v", err)
	}

	// Cancel at B.
	_, err = AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridB),
		TagName:   "mybranch",
		TagType:   TagCancel,
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 13, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag cancel: %v", err)
	}

	// B should have no active tagxref for mybranch (cancel replaces propagated entry).
	// AddTag inserts a tagtype=0 (cancel) row at B, so we filter tagtype>0 for active tags.
	var count int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='mybranch' AND tagtype>0 AND rid=?", ridB,
	).Scan(&count)
	if count != 0 {
		t.Errorf("rid=%d (B) active tagxref count=%d, want 0 (cancelled)", ridB, count)
	}

	// C should also have no tagxref for mybranch (cancel propagated, deleted the row).
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='mybranch' AND rid=?", ridC,
	).Scan(&count)
	if count != 0 {
		t.Errorf("rid=%d (C) tagxref count=%d, want 0 (cancel propagated)", ridC, count)
	}
}
```

- [ ] **Step 4: Write failing test for bgcolor propagation**

```go
func TestPropagateBgcolor(t *testing.T) {
	r := setupTestRepo(t)

	ridA := makeCheckin(t, r, 0, "a.txt", "aaa", "commit A")
	ridB := makeCheckin(t, r, ridA, "a.txt", "bbb", "commit B")

	// Crosslink to populate event table (needed for bgcolor update).
	manifest.Crosslink(r)

	_, err := AddTag(r, TagOpts{
		TargetRID: libfossil.FslID(ridA),
		TagName:   "bgcolor",
		TagType:   TagPropagating,
		Value:     "#ff0000",
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag bgcolor: %v", err)
	}

	// Verify event.bgcolor updated at B.
	var bgcolor string
	err = r.DB().QueryRow("SELECT bgcolor FROM event WHERE objid=?", ridB).Scan(&bgcolor)
	if err != nil {
		t.Fatalf("query bgcolor: %v", err)
	}
	if bgcolor != "#ff0000" {
		t.Errorf("bgcolor=%q, want %q", bgcolor, "#ff0000")
	}
}
```

- [ ] **Step 5: Create `tag/propagate.go` with the propagation algorithm**

```go
package tag

import (
	"container/heap"
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
)

// propagate walks the plink DAG from pid, updating tagxref at each descendant.
// Matches Fossil's tag_propagate (tag.c:34-113).
func propagate(q db.Querier, tagid int64, tagType int, origID libfossil.FslID, mtime float64, value string, tagName string, pid libfossil.FslID) error {
	if q == nil {
		panic("tag.propagate: q must not be nil")
	}
	if tagid <= 0 {
		panic("tag.propagate: tagid must be positive")
	}

	// Seed queue with the target itself (mtime=0 so all children are visited).
	pq := &propQueue{}
	heap.Init(pq)
	heap.Push(pq, propItem{rid: pid, mtime: 0.0})

	isPropagating := tagType == TagPropagating

	for pq.Len() > 0 {
		item := heap.Pop(pq).(propItem)

		// Query primary children of this node.
		// LEFT JOIN tagxref to check existing tag state at each child.
		// doit logic matches Fossil: for propagating, visit if no tag or older;
		// for cancel, visit only if child has a propagated tag.
		rows, err := q.Query(`
			SELECT cid, plink.mtime,
				coalesce(srcid=0 AND tagxref.mtime < ?, ?) AS doit
			FROM plink
			LEFT JOIN tagxref ON cid=tagxref.rid AND tagxref.tagid=?
			WHERE pid=? AND isprim=1`,
			item.mtime, boolToInt(isPropagating), tagid, item.rid,
		)
		if err != nil {
			return fmt.Errorf("tag.propagate query children of %d: %w", item.rid, err)
		}

		type child struct {
			rid   libfossil.FslID
			mtime float64
			doit  bool
		}
		var children []child
		for rows.Next() {
			var c child
			var doitInt int
			if err := rows.Scan(&c.rid, &c.mtime, &doitInt); err != nil {
				rows.Close()
				return fmt.Errorf("tag.propagate scan: %w", err)
			}
			c.doit = doitInt != 0
			children = append(children, c)
		}
		rows.Close()
		if err := rows.Err(); err != nil {
			return fmt.Errorf("tag.propagate rows: %w", err)
		}

		for _, c := range children {
			if !c.doit {
				continue
			}

			if isPropagating {
				// Insert/replace propagated tag at descendant.
				if _, err := q.Exec(
					`REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid)
					 VALUES(?, 2, 0, ?, ?, ?, ?)`,
					tagid, origID, value, mtime, c.rid,
				); err != nil {
					return fmt.Errorf("tag.propagate insert at %d: %w", c.rid, err)
				}

				// Special: bgcolor also updates event table.
				if tagName == "bgcolor" {
					q.Exec("UPDATE event SET bgcolor=? WHERE objid=?", value, c.rid)
				}
			} else {
				// Cancel: delete ALL tagxref entries for this tag at descendant.
				if _, err := q.Exec(
					"DELETE FROM tagxref WHERE tagid=? AND rid=?",
					tagid, c.rid,
				); err != nil {
					return fmt.Errorf("tag.propagate delete at %d: %w", c.rid, err)
				}
			}

			heap.Push(pq, propItem{rid: c.rid, mtime: c.mtime})
		}
	}
	return nil
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// propItem is a priority queue element for tag propagation.
type propItem struct {
	rid   libfossil.FslID
	mtime float64
}

// propQueue implements heap.Interface, ordered by mtime ascending.
type propQueue []propItem

func (pq propQueue) Len() int            { return len(pq) }
func (pq propQueue) Less(i, j int) bool  { return pq[i].mtime < pq[j].mtime }
func (pq propQueue) Swap(i, j int)       { pq[i], pq[j] = pq[j], pq[i] }
func (pq *propQueue) Push(x any)         { *pq = append(*pq, x.(propItem)) }
func (pq *propQueue) Pop() any {
	old := *pq
	n := len(old)
	item := old[n-1]
	*pq = old[:n-1]
	return item
}
```

- [ ] **Step 6: Wire propagation into `AddTag`**

In `tag.go`, after the tagxref INSERT (line 106) and before the unsent INSERT (line 109), add:

```go
		// Propagate to descendants (matches Fossil's tag_insert → tag_propagate).
		if opts.TagType == TagPropagating || opts.TagType == TagCancel {
			if err := propagate(tx, tagid, opts.TagType, opts.TargetRID, mtime, opts.Value, opts.TagName, opts.TargetRID); err != nil {
				return fmt.Errorf("tag propagate: %w", err)
			}
		}
```

- [ ] **Step 7: Run all tag tests**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/tag/ -v`
Expected: ALL PASS (including 3 new propagation tests)

- [ ] **Step 8: Run full go-libfossil tests to check for regressions**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/... -count=1`
Expected: ALL PASS

- [ ] **Step 9: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt
git add go-libfossil/tag/propagate.go go-libfossil/tag/tag.go go-libfossil/tag/tag_test.go
git commit -m "feat: add tag propagation to AddTag (matching Fossil's tag_propagate)

Walks plink DAG to cascade propagating tags to all descendants.
Cancel tags delete entries and walk to clean up propagated tags.
Special handling for bgcolor (updates event table).

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: ApplyTag for Crosslink

**Files:**
- Modify: `go-libfossil/tag/tag.go`
- Modify: `go-libfossil/tag/tag_test.go`

`ApplyTag` inserts a tagxref row and propagates WITHOUT creating a control artifact. Used by Crosslink to process existing control artifacts.

- [ ] **Step 1: Write failing test for ApplyTag**

```go
func TestApplyTag(t *testing.T) {
	r := setupTestRepo(t)

	ridA := makeCheckin(t, r, 0, "a.txt", "aaa", "commit A")
	ridB := makeCheckin(t, r, ridA, "a.txt", "bbb", "commit B")

	// ApplyTag directly (no control artifact creation).
	err := ApplyTag(r, ApplyOpts{
		TargetRID: libfossil.FslID(ridA),
		SrcRID:    999, // fake control artifact RID
		TagName:   "sym-trunk",
		TagType:   TagPropagating,
		Value:     "",
		MTime:     libfossil.TimeToJulian(time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC)),
	})
	if err != nil {
		t.Fatalf("ApplyTag: %v", err)
	}

	// Verify tagxref at A has srcid=999.
	var srcid int64
	r.DB().QueryRow(
		"SELECT srcid FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-trunk' AND rid=?", ridA,
	).Scan(&srcid)
	if srcid != 999 {
		t.Errorf("srcid=%d, want 999", srcid)
	}

	// Verify propagated to B with srcid=0.
	r.DB().QueryRow(
		"SELECT srcid FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-trunk' AND rid=?", ridB,
	).Scan(&srcid)
	if srcid != 0 {
		t.Errorf("B srcid=%d, want 0 (propagated)", srcid)
	}
}
```

- [ ] **Step 2: Implement ApplyTag**

Add to `tag.go`:

```go
// ApplyOpts describes a tag application from an existing control artifact.
// Unlike AddTag, ApplyTag does NOT create a control artifact — the blob already exists.
type ApplyOpts struct {
	TargetRID libfossil.FslID
	SrcRID    libfossil.FslID // control artifact that introduced this tag
	TagName   string
	TagType   int
	Value     string
	MTime     float64 // Julian day
}

// ApplyTag inserts a tagxref row and propagates without creating a control artifact.
// Used by Crosslink to process existing control artifacts.
func ApplyTag(r *repo.Repo, opts ApplyOpts) error {
	if r == nil {
		panic("tag.ApplyTag: r must not be nil")
	}
	if opts.TagName == "" {
		panic("tag.ApplyTag: opts.TagName must not be empty")
	}
	if opts.TargetRID <= 0 {
		panic("tag.ApplyTag: opts.TargetRID must be positive")
	}

	return r.WithTx(func(tx *db.Tx) error {
		tagid, err := ensureTag(tx, opts.TagName)
		if err != nil {
			return fmt.Errorf("ensure tag %q: %w", opts.TagName, err)
		}

		if _, err := tx.Exec(
			`INSERT OR REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid)
			 VALUES(?, ?, ?, ?, ?, ?, ?)`,
			tagid, opts.TagType, opts.SrcRID, opts.TargetRID, opts.Value, opts.MTime, opts.TargetRID,
		); err != nil {
			return fmt.Errorf("tagxref insert: %w", err)
		}

		// Special: bgcolor updates event table directly.
		if opts.TagName == "bgcolor" && opts.TagType == TagPropagating {
			tx.Exec("UPDATE event SET bgcolor=? WHERE objid=?", opts.Value, opts.TargetRID)
		}

		if opts.TagType == TagPropagating || opts.TagType == TagCancel {
			if err := propagate(tx, tagid, opts.TagType, opts.TargetRID, opts.MTime, opts.Value, opts.TagName, opts.TargetRID); err != nil {
				return fmt.Errorf("propagate: %w", err)
			}
		}

		return nil
	})
}
```

- [ ] **Step 3: Run tests**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/tag/ -v`
Expected: ALL PASS

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt
git add go-libfossil/tag/tag.go go-libfossil/tag/tag_test.go
git commit -m "feat: add ApplyTag for Crosslink to process existing control artifacts

ApplyTag inserts tagxref + propagates without creating a new blob.
Used by Crosslink to wire up tags from control artifacts after
clone/sync.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Crosslink enhancement — process control artifacts

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Modify: `go-libfossil/manifest/crosslink_test.go`

- [ ] **Step 1: Write failing test for Crosslink processing control artifacts**

Add to `crosslink_test.go` (or create if needed):

```go
func TestCrosslinkControlArtifacts(t *testing.T) {
	r := setupTestRepo(t)

	// Create a checkin.
	rid, _, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Create a control artifact (tag card) via tag.AddTag.
	// This creates the blob + tagxref row. But after a clone, the blob
	// exists without tagxref — Crosslink should fix that.
	_, err = tag.AddTag(r, tag.TagOpts{
		TargetRID: rid,
		TagName:   "sym-trunk",
		TagType:   tag.TagSingleton,
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}

	// Now simulate a clone scenario: delete all tagxref entries.
	r.DB().Exec("DELETE FROM tagxref")

	// Run Crosslink — should re-process the control artifact.
	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	t.Logf("crosslinked %d artifacts", n)

	// Verify sym-trunk tag exists in tagxref.
	var count int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-trunk' AND rid=?", rid,
	).Scan(&count)
	if count != 1 {
		t.Errorf("sym-trunk tagxref count=%d, want 1", count)
	}
}
```

Add `"github.com/dmestas/edgesync/go-libfossil/tag"` to crosslink_test imports.

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/manifest/ -run TestCrosslinkControlArtifacts -v`
Expected: FAIL — count=0

- [ ] **Step 3: Implement control artifact processing in Crosslink**

In `crosslink.go`, after the existing checkin processing loop (line 68), add a second pass for control artifacts:

```go
	// Second pass: process control artifacts (tags/branches).
	// Find blobs that are control artifacts without tagxref.srcid entries.
	ctrlRows, err := r.DB().Query(`
		SELECT b.rid FROM blob b
		WHERE b.size >= 0
		AND NOT EXISTS (SELECT 1 FROM tagxref tx WHERE tx.srcid = b.rid)
		AND NOT EXISTS (SELECT 1 FROM event e WHERE e.objid = b.rid)
	`)
	if err != nil {
		return linked, fmt.Errorf("manifest.Crosslink ctrl query: %w", err)
	}
	defer ctrlRows.Close()

	var ctrlCandidates []libfossil.FslID
	for ctrlRows.Next() {
		var rid libfossil.FslID
		if err := ctrlRows.Scan(&rid); err != nil {
			return linked, fmt.Errorf("manifest.Crosslink ctrl scan: %w", err)
		}
		ctrlCandidates = append(ctrlCandidates, rid)
	}
	if err := ctrlRows.Err(); err != nil {
		return linked, fmt.Errorf("manifest.Crosslink ctrl rows: %w", err)
	}

	for _, rid := range ctrlCandidates {
		data, err := content.Expand(r.DB(), rid)
		if err != nil {
			continue
		}
		d, err := deck.Parse(data)
		if err != nil {
			continue
		}
		if d.Type != deck.Control {
			continue
		}
		if err := crosslinkControl(r, rid, d); err != nil {
			return linked, fmt.Errorf("manifest.Crosslink ctrl rid=%d: %w", rid, err)
		}
		linked++
	}
```

Also modify `crosslinkOne` to process inline T-cards from checkin manifests. After the mlink loop (line 133), add:

```go
		// Process inline T-cards (UUID="*" means this checkin).
		for _, tc := range d.T {
			if tc.UUID != "*" {
				continue // non-self T-cards are in control artifacts, handled separately
			}
			var tagType int
			switch tc.Type {
			case deck.TagPropagating:
				tagType = tag.TagPropagating
			case deck.TagSingleton:
				tagType = tag.TagSingleton
			case deck.TagCancel:
				tagType = tag.TagCancel
			default:
				continue
			}
			if err := tag.ApplyTag(r, tag.ApplyOpts{
				TargetRID: rid,
				SrcRID:    rid, // inline T-cards: the checkin itself is the source
				TagName:   tc.Name,
				TagType:   tagType,
				Value:     tc.Value,
				MTime:     libfossil.TimeToJulian(d.D),
			}); err != nil {
				return fmt.Errorf("inline tag %q: %w", tc.Name, err)
			}
		}
```

Add `crosslinkControl` function:

```go
func crosslinkControl(r *repo.Repo, srcRID libfossil.FslID, d *deck.Deck) error {
	mtime := libfossil.TimeToJulian(d.D)
	for _, tc := range d.T {
		// Resolve target UUID to RID.
		targetUUID := tc.UUID
		if targetUUID == "*" {
			// Self-referencing tag (inline in checkin manifest) — skip,
			// these are handled during checkin crosslink.
			continue
		}
		var targetRID int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", targetUUID).Scan(&targetRID); err != nil {
			continue // target blob not found, skip
		}

		// Map deck.TagType to tag package constants.
		var tagType int
		switch tc.Type {
		case deck.TagPropagating:
			tagType = tag.TagPropagating
		case deck.TagSingleton:
			tagType = tag.TagSingleton
		case deck.TagCancel:
			tagType = tag.TagCancel
		default:
			continue
		}

		if err := tag.ApplyTag(r, tag.ApplyOpts{
			TargetRID: libfossil.FslID(targetRID),
			SrcRID:    srcRID,
			TagName:   tc.Name,
			TagType:   tagType,
			Value:     tc.Value,
			MTime:     mtime,
		}); err != nil {
			return fmt.Errorf("apply tag %q to rid=%d: %w", tc.Name, targetRID, err)
		}
	}
	return nil
}
```

Add `"github.com/dmestas/edgesync/go-libfossil/tag"` to crosslink.go imports.

- [ ] **Step 4: Run crosslink tests**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/manifest/ -run TestCrosslink -v`
Expected: ALL PASS

- [ ] **Step 5: Run full go-libfossil tests**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/... -count=1`
Expected: ALL PASS

- [ ] **Step 6: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/crosslink_test.go
git commit -m "feat: extend Crosslink to process control artifacts (tags/branches)

Second pass in Crosslink finds control artifact blobs without
tagxref entries, parses T-cards, and calls tag.ApplyTag for each.
Eliminates need for manual sym-trunk workarounds after clone/sync.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Branch package — Create

**Files:**
- Create: `go-libfossil/branch/branch.go`
- Create: `go-libfossil/branch/branch_test.go`

- [ ] **Step 1: Write failing test for branch.Create**

```go
package branch

import (
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func setupTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestCreate(t *testing.T) {
	r := setupTestRepo(t)

	// Create initial checkin on trunk.
	parentRid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Create a branch.
	branchRid, _, err := Create(r, CreateOpts{
		Name:   "feature-x",
		Parent: parentRid,
		User:   "testuser",
		Time:   time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	if branchRid <= 0 {
		t.Fatalf("branchRid=%d, want > 0", branchRid)
	}

	// Verify branch tag at the new checkin.
	var branchValue string
	err = r.DB().QueryRow(
		"SELECT value FROM tagxref JOIN tag USING(tagid) WHERE tagname='branch' AND rid=?", branchRid,
	).Scan(&branchValue)
	if err != nil {
		t.Fatalf("branch tag query: %v", err)
	}
	if branchValue != "feature-x" {
		t.Errorf("branch value=%q, want %q", branchValue, "feature-x")
	}

	// Verify sym-feature-x tag at the new checkin.
	var symCount int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-feature-x' AND rid=?", branchRid,
	).Scan(&symCount)
	if symCount != 1 {
		t.Errorf("sym-feature-x count=%d, want 1", symCount)
	}
}
```

- [ ] **Step 2: Implement branch.Create**

Create `go-libfossil/branch/branch.go`:

```go
package branch

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// CreateOpts describes a branch creation operation.
type CreateOpts struct {
	Name   string          // Branch name (required).
	Parent libfossil.FslID // Parent checkin RID (required).
	User   string          // Author (required).
	Time   time.Time       // Timestamp (default: now).
	Color  string          // Optional bgcolor.
}

// Create creates a new branch by making a checkin with branch tags.
// Returns the new checkin's RID and UUID.
func Create(r *repo.Repo, opts CreateOpts) (libfossil.FslID, string, error) {
	if r == nil {
		panic("branch.Create: r must not be nil")
	}
	if opts.Name == "" {
		panic("branch.Create: opts.Name must not be empty")
	}
	if opts.Parent <= 0 {
		panic("branch.Create: opts.Parent must be positive")
	}
	if opts.User == "" {
		panic("branch.Create: opts.User must not be empty")
	}
	if opts.Time.IsZero() {
		opts.Time = time.Now().UTC()
	}

	// 1. Get parent's file list.
	files, err := parentFiles(r, opts.Parent)
	if err != nil {
		return 0, "", fmt.Errorf("branch.Create: %w", err)
	}

	// 2. Query existing sym-* tags on parent to build cancel cards.
	oldSyms, err := querySymTags(r, opts.Parent)
	if err != nil {
		return 0, "", fmt.Errorf("branch.Create: %w", err)
	}

	// 3. Build T-cards.
	var tags []deck.TagCard
	tags = append(tags, deck.TagCard{
		Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: opts.Name,
	})
	tags = append(tags, deck.TagCard{
		Type: deck.TagPropagating, Name: "sym-" + opts.Name, UUID: "*",
	})
	for _, oldSym := range oldSyms {
		tags = append(tags, deck.TagCard{
			Type: deck.TagCancel, Name: oldSym, UUID: "*",
		})
	}
	if opts.Color != "" {
		tags = append(tags, deck.TagCard{
			Type: deck.TagPropagating, Name: "bgcolor", UUID: "*", Value: opts.Color,
		})
	}

	// 4. Create checkin with branch tags.
	rid, uuid, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: fmt.Sprintf("Create branch %s", opts.Name),
		User:    opts.User,
		Parent:  opts.Parent,
		Time:    opts.Time,
		Tags:    tags,
	})
	if err != nil {
		return 0, "", fmt.Errorf("branch.Create checkin: %w", err)
	}

	// 5. Crosslink to process inline T-cards → triggers tag propagation.
	if _, err := manifest.Crosslink(r); err != nil {
		return 0, "", fmt.Errorf("branch.Create crosslink: %w", err)
	}

	return rid, uuid, nil
}

// parentFiles reads the parent checkin's file list and returns manifest.File
// entries with content loaded.
func parentFiles(r *repo.Repo, parentRID libfossil.FslID) ([]manifest.File, error) {
	entries, err := manifest.ListFiles(r, parentRID)
	if err != nil {
		return nil, fmt.Errorf("list parent files: %w", err)
	}
	var files []manifest.File
	for _, e := range entries {
		rid, ok := blob.Exists(r.DB(), e.UUID)
		if !ok {
			return nil, fmt.Errorf("file blob %s not found", e.UUID)
		}
		data, err := content.Expand(r.DB(), rid)
		if err != nil {
			return nil, fmt.Errorf("expand file %s: %w", e.Name, err)
		}
		files = append(files, manifest.File{
			Name:    e.Name,
			Content: data,
			Perm:    e.Perm,
		})
	}
	return files, nil
}

// querySymTags returns existing sym-* tag names on a checkin.
func querySymTags(r *repo.Repo, rid libfossil.FslID) ([]string, error) {
	rows, err := r.DB().Query(
		"SELECT tag.tagname FROM tagxref JOIN tag USING(tagid) WHERE tagxref.rid=? AND tagxref.tagtype>0 AND tag.tagname GLOB 'sym-*'",
		rid,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var names []string
	for rows.Next() {
		var name string
		if err := rows.Scan(&name); err != nil {
			return nil, err
		}
		names = append(names, name)
	}
	return names, rows.Err()
}
```

Add `"github.com/dmestas/edgesync/go-libfossil/blob"` to imports.

- [ ] **Step 3: Run test**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/branch/ -run TestCreate -v`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt
git add go-libfossil/branch/branch.go go-libfossil/branch/branch_test.go
git commit -m "feat: add branch.Create — create branches via checkin with T-cards

Creates a new checkin with branch/sym-*/cancel tags, reusing
manifest.Checkin for construction. Crosslink processes inline
T-cards and triggers tag propagation.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Branch package — List and Close

**Files:**
- Modify: `go-libfossil/branch/branch.go`
- Modify: `go-libfossil/branch/branch_test.go`

- [ ] **Step 1: Write failing test for List**

```go
func TestList(t *testing.T) {
	r := setupTestRepo(t)

	parentRid, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("a")}},
		Comment: "initial",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})

	// Create two branches.
	Create(r, CreateOpts{Name: "feature-a", Parent: parentRid, User: "testuser", Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC)})
	Create(r, CreateOpts{Name: "feature-b", Parent: parentRid, User: "testuser", Time: time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC)})

	branches, err := List(r)
	if err != nil {
		t.Fatalf("List: %v", err)
	}

	names := map[string]bool{}
	for _, b := range branches {
		names[b.Name] = true
	}
	if !names["feature-a"] {
		t.Error("missing branch feature-a")
	}
	if !names["feature-b"] {
		t.Error("missing branch feature-b")
	}
}
```

- [ ] **Step 2: Implement List**

Add to `branch.go`:

```go
// Branch describes a branch in the repository.
type Branch struct {
	Name         string
	LastMTime    float64 // Julian day of last checkin on this branch.
	IsClosed     bool
	CheckinCount int
	LatestUUID   string
}

// List returns all branches in the repository.
func List(r *repo.Repo) ([]Branch, error) {
	if r == nil {
		panic("branch.List: r must not be nil")
	}

	rows, err := r.DB().Query(`
		SELECT tagxref.value, max(event.mtime), count(*),
			(SELECT 1 FROM tagxref tx2 JOIN tag t2 ON tx2.tagid=t2.tagid
			 WHERE t2.tagname='closed' AND tx2.rid IN
				(SELECT rid FROM tagxref tx3 JOIN tag t3 ON tx3.tagid=t3.tagid
				 WHERE t3.tagname='branch' AND tx3.value=tagxref.value AND tx3.tagtype>0)
			 AND tx2.tagtype>0
			 LIMIT 1) AS isclosed,
			(SELECT b.uuid FROM blob b WHERE b.rid=max(tagxref.rid)) AS latest
		FROM tagxref JOIN tag ON tagxref.tagid=tag.tagid
		JOIN event ON event.objid=tagxref.rid
		WHERE tag.tagname='branch' AND tagxref.tagtype>0
		GROUP BY tagxref.value
	`)
	if err != nil {
		return nil, fmt.Errorf("branch.List: %w", err)
	}
	defer rows.Close()

	var branches []Branch
	for rows.Next() {
		var b Branch
		var isClosed *int
		if err := rows.Scan(&b.Name, &b.LastMTime, &b.CheckinCount, &isClosed, &b.LatestUUID); err != nil {
			return nil, fmt.Errorf("branch.List scan: %w", err)
		}
		b.IsClosed = isClosed != nil && *isClosed == 1
		branches = append(branches, b)
	}
	return branches, rows.Err()
}
```

- [ ] **Step 3: Write failing test for Close**

```go
func TestClose(t *testing.T) {
	r := setupTestRepo(t)

	parentRid, _, _ := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("a")}},
		Comment: "initial",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})

	Create(r, CreateOpts{Name: "done-branch", Parent: parentRid, User: "testuser", Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC)})

	if err := Close(r, "done-branch", "testuser"); err != nil {
		t.Fatalf("Close: %v", err)
	}

	branches, _ := List(r)
	for _, b := range branches {
		if b.Name == "done-branch" && !b.IsClosed {
			t.Error("branch done-branch should be closed")
		}
	}
}
```

- [ ] **Step 4: Implement Close**

Add to `branch.go`:

```go
// Close closes a branch by adding a singleton "closed" tag to its latest leaf checkin.
func Close(r *repo.Repo, name string, user string) error {
	if r == nil {
		panic("branch.Close: r must not be nil")
	}
	if name == "" {
		panic("branch.Close: name must not be empty")
	}

	// Find latest checkin on the branch.
	var tipRID int64
	err := r.DB().QueryRow(`
		SELECT tagxref.rid FROM tagxref
		JOIN tag ON tagxref.tagid=tag.tagid
		JOIN event ON event.objid=tagxref.rid
		WHERE tag.tagname='branch' AND tagxref.value=? AND tagxref.tagtype>0
		ORDER BY event.mtime DESC LIMIT 1`,
		name,
	).Scan(&tipRID)
	if err != nil {
		return fmt.Errorf("branch.Close: find tip for %q: %w", name, err)
	}

	_, err = tag.AddTag(r, tag.TagOpts{
		TargetRID: libfossil.FslID(tipRID),
		TagName:   "closed",
		TagType:   tag.TagSingleton,
		User:      user,
	})
	if err != nil {
		return fmt.Errorf("branch.Close: add closed tag: %w", err)
	}
	return nil
}
```

Add `"github.com/dmestas/edgesync/go-libfossil/tag"` to imports.

- [ ] **Step 5: Run all branch tests**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./go-libfossil/branch/ -v`
Expected: ALL PASS

- [ ] **Step 6: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt
git add go-libfossil/branch/branch.go go-libfossil/branch/branch_test.go
git commit -m "feat: add branch.List and branch.Close

List queries tagxref for branch tags, groups by name.
Close adds singleton closed tag to branch tip.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: Equivalence test cleanup + final verification

**Files:**
- Modify: `sim/equivalence_test.go`

- [ ] **Step 1: Remove manual sym-trunk workarounds from equivalence tests**

In `sim/equivalence_test.go`, remove all manual `tag.AddTag(sym-trunk)` calls and `manifest.Crosslink` + `tag.AddTag` pairs. Replace with just `manifest.Crosslink(r)` — which now processes control artifacts and inline T-cards automatically.

Specifically remove from: `TestEquivalenceSmoke`, `testLeafToFossil` (commit_chain sym-trunk), `testFossilToLeaf` (sym-trunk after clone), `testLeafToLeaf` (sym-trunk after crosslink).

Note: `manifest.Checkin` with `Parent==0` already adds inline `sym-trunk` and `branch` T-cards to the initial checkin. Crosslink now processes those. So no manual tags needed.

- [ ] **Step 2: Run equivalence tests**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && go test -buildvcs=false ./sim/ -run 'TestEquivalence|TestLeafToFossil|TestFossilToLeaf|TestLeafToLeaf' -v -timeout=60s`
Expected: ALL PASS (same as before, minus the workarounds)

- [ ] **Step 3: Run full test suite**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && make test`
Expected: ALL PASS

- [ ] **Step 4: Run DST**

Run: `cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt && make dst`
Expected: ALL 8 seeds PASS

- [ ] **Step 5: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/branch-mgmt
git add sim/equivalence_test.go
git commit -m "refactor: remove manual sym-trunk workarounds from equivalence tests

Crosslink now processes inline T-cards from checkin manifests,
so sym-trunk/branch tags are populated automatically.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```
