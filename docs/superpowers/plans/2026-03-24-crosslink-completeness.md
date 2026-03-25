# Crosslink Completeness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Achieve full parity with Fossil's `manifest_crosslink` for all 8 artifact types, with two-pass architecture, dephantomize hook, and auto-crosslink after sync.

**Architecture:** Extend `manifest/crosslink.go` with per-type handlers dispatched by `deck.ArtifactType`. Pass 1 crosslinks all artifacts into metadata tables with immediate tag propagation. Pass 2 processes deferred wiki backlinks and ticket rebuilds. `AfterDephantomize` in a new `dephantomize.go` handles phantom-to-real transitions. `sync.Sync()` auto-crosslinks after convergence.

**Tech Stack:** Go, SQLite, existing go-libfossil packages (deck, tag, blob, content, repo, db)

**Spec:** `docs/superpowers/specs/2026-03-24-crosslink-completeness-design.md`

---

### Task 1: Add `forumpost` table to schema

**Files:**
- Modify: `go-libfossil/db/schema.go`

- [ ] **Step 1: Add forumpost CREATE TABLE to schema string**

In `go-libfossil/db/schema.go`, find the end of the schema string (after the `cherrypick` table). Add:

```sql
CREATE TABLE forumpost(
  fpid INTEGER PRIMARY KEY,
  froot INT,
  fprev INT,
  firt INT,
  fmtime REAL
);
CREATE INDEX forumpost_froot ON forumpost(froot);
```

Also add `"forumpost"` to the `repo2Tables` slice in `db_test.go` (if it exists as a verification list).

- [ ] **Step 2: Run tests to verify schema compiles**

Run: `cd go-libfossil && go test -buildvcs=false ./db/ -v -run TestCreate`
Expected: PASS (repo creation includes new table)

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/db/schema.go go-libfossil/db/db_test.go
git commit -m "schema: add forumpost table for forum crosslinking"
```

---

### Task 2: Add `tag.PropagateAll`

**Files:**
- Modify: `go-libfossil/tag/propagate.go`
- Test: `go-libfossil/tag/tag_test.go`

- [ ] **Step 1: Write the failing test**

Add to `go-libfossil/tag/tag_test.go`:

```go
func TestPropagateAll(t *testing.T) {
	r := setupTestRepo(t) // uses testutil.NewTestRepo

	// Create linear chain A -> B -> C
	ridA := makeCheckin(t, r, nil, "A")
	ridB := makeCheckin(t, r, &ridA, "B")
	ridC := makeCheckin(t, r, &ridB, "C")

	// Add propagating branch tag to A
	_, err := tag.AddTag(r, tag.TagOpts{
		TargetRID: ridA,
		TagName:   "branch",
		TagType:   tag.TagPropagating,
		Value:     "feature",
		User:      "test",
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}

	// Clear propagated tags from B and C to simulate fresh state
	r.DB().Exec("DELETE FROM tagxref WHERE rid=? AND srcid=0", ridB)
	r.DB().Exec("DELETE FROM tagxref WHERE rid=? AND srcid=0", ridC)

	// Call PropagateAll on A — should re-propagate to B and C
	if err := tag.PropagateAll(r.DB(), ridA); err != nil {
		t.Fatalf("PropagateAll: %v", err)
	}

	// Verify B has propagated branch=feature
	var val string
	err = r.DB().QueryRow(
		"SELECT value FROM tagxref JOIN tag USING(tagid) WHERE tagname='branch' AND rid=? AND srcid=0",
		ridB,
	).Scan(&val)
	if err != nil {
		t.Fatalf("B branch tag: %v", err)
	}
	if val != "feature" {
		t.Errorf("B branch=%q, want feature", val)
	}

	// Verify C has propagated branch=feature
	err = r.DB().QueryRow(
		"SELECT value FROM tagxref JOIN tag USING(tagid) WHERE tagname='branch' AND rid=? AND srcid=0",
		ridC,
	).Scan(&val)
	if err != nil {
		t.Fatalf("C branch tag: %v", err)
	}
	if val != "feature" {
		t.Errorf("C branch=%q, want feature", val)
	}
}
```

Note: `makeCheckin` is a test helper — if it doesn't exist, create it using `manifest.Checkin` with appropriate params. Check existing test helpers in the file first.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./tag/ -v -run TestPropagateAll`
Expected: FAIL — `PropagateAll` undefined

- [ ] **Step 3: Implement `PropagateAll`**

Add to `go-libfossil/tag/propagate.go`:

```go
// PropagateAll re-propagates all tags from rid to its descendants.
// Matches Fossil's tag_propagate_all (tag.c:118-135).
// Singleton tags (type 1) are treated as cancel (type 0) during propagation.
func PropagateAll(q db.Querier, rid libfossil.FslID) error {
	if q == nil {
		panic("tag.PropagateAll: q must not be nil")
	}
	if rid <= 0 {
		return nil // nothing to propagate from
	}

	rows, err := q.Query(
		"SELECT tagid, tagtype, mtime, value, origid FROM tagxref WHERE rid=?",
		rid,
	)
	if err != nil {
		return fmt.Errorf("tag.PropagateAll query: %w", err)
	}
	defer rows.Close()

	type entry struct {
		tagid   int64
		tagtype int
		mtime   float64
		value   string
		origid  libfossil.FslID
	}
	var entries []entry
	for rows.Next() {
		var e entry
		if err := rows.Scan(&e.tagid, &e.tagtype, &e.mtime, &e.value, &e.origid); err != nil {
			return fmt.Errorf("tag.PropagateAll scan: %w", err)
		}
		entries = append(entries, e)
	}
	if err := rows.Err(); err != nil {
		return fmt.Errorf("tag.PropagateAll rows: %w", err)
	}

	for _, e := range entries {
		tagtype := e.tagtype
		// Singleton tags are treated as cancel during propagation.
		// Matches Fossil: if( tagtype==1 ) tagtype = 0;
		if tagtype == TagSingleton {
			tagtype = TagCancel
		}
		// Need tag name for bgcolor special case
		var tagname string
		if err := q.QueryRow("SELECT tagname FROM tag WHERE tagid=?", e.tagid).Scan(&tagname); err != nil {
			return fmt.Errorf("tag.PropagateAll tagname: %w", err)
		}
		if err := propagate(q, e.tagid, tagtype, e.origid, e.mtime, e.value, tagname, rid); err != nil {
			return fmt.Errorf("tag.PropagateAll propagate tagid=%d: %w", e.tagid, err)
		}
	}
	return nil
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./tag/ -v -run TestPropagateAll`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/tag/propagate.go go-libfossil/tag/tag_test.go
git commit -m "feat(tag): add PropagateAll matching Fossil's tag_propagate_all"
```

---

### Task 3: Restructure `Crosslink` to two-pass with updated discovery query

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write the failing test for idempotent discovery**

Add to `go-libfossil/manifest/manifest_test.go`:

```go
func TestDiscoveryQueryIdempotent(t *testing.T) {
	r := setupTestRepo(t)

	_, _, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("a")}},
		Comment: "first",
		User:    "test",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Clear metadata to simulate post-clone state
	r.DB().Exec("DELETE FROM event")
	r.DB().Exec("DELETE FROM tagxref")

	// First crosslink
	n1, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink 1: %v", err)
	}
	if n1 == 0 {
		t.Fatal("expected at least 1 artifact crosslinked")
	}

	// Second crosslink — should find nothing new
	n2, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink 2: %v", err)
	}
	if n2 != 0 {
		t.Errorf("second Crosslink linked %d, want 0 (idempotent)", n2)
	}
}
```

- [ ] **Step 2: Run test to verify current behavior**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestDiscoveryQueryIdempotent`
Expected: Likely PASS (checkins already have event rows after first crosslink). This establishes baseline.

- [ ] **Step 3: Restructure `Crosslink` to two-pass with updated discovery query**

Replace the body of `Crosslink` in `go-libfossil/manifest/crosslink.go`. The new structure:

```go
// pendingItem represents deferred work for Pass 2.
type pendingItem struct {
	Type byte   // 'w' = wiki backlink, 't' = ticket rebuild
	ID   string // wiki title or ticket UUID
}

func Crosslink(r *repo.Repo) (int, error) {
	if r == nil {
		panic("manifest.Crosslink: r must not be nil")
	}

	// Updated discovery query: multi-table NOT EXISTS check.
	rows, err := r.DB().Query(`
		SELECT b.rid, b.uuid FROM blob b
		WHERE b.size >= 0
		  AND NOT EXISTS (SELECT 1 FROM event e WHERE e.objid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM tagxref tx WHERE tx.srcid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM forumpost fp WHERE fp.fpid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM attachment a WHERE a.attachid = b.rid)
	`)
	if err != nil {
		return 0, fmt.Errorf("manifest.Crosslink query: %w", err)
	}
	defer rows.Close()

	type candidate struct {
		rid  libfossil.FslID
		uuid string
	}
	var candidates []candidate
	for rows.Next() {
		var c candidate
		if err := rows.Scan(&c.rid, &c.uuid); err != nil {
			return 0, fmt.Errorf("manifest.Crosslink scan: %w", err)
		}
		candidates = append(candidates, c)
	}
	if err := rows.Err(); err != nil {
		return 0, fmt.Errorf("manifest.Crosslink rows: %w", err)
	}

	// Pass 1: dispatch each artifact to its handler.
	linked := 0
	var pending []pendingItem

	for _, c := range candidates {
		data, err := content.Expand(r.DB(), c.rid)
		if err != nil {
			continue
		}
		d, err := deck.Parse(data)
		if err != nil {
			continue
		}

		var p []pendingItem
		var linkErr error

		switch d.Type {
		case deck.Checkin:
			linkErr = crosslinkCheckin(r, c.rid, d)
		case deck.Wiki:
			p, linkErr = crosslinkWiki(r, c.rid, d)
		case deck.Ticket:
			p, linkErr = crosslinkTicket(r, c.rid, d)
		case deck.Event:
			p, linkErr = crosslinkEvent(r, c.rid, d)
		case deck.Attachment:
			linkErr = crosslinkAttachment(r, c.rid, d)
		case deck.Cluster:
			linkErr = crosslinkCluster(r, c.rid, d)
		case deck.ForumPost:
			linkErr = crosslinkForum(r, c.rid, d)
		case deck.Control:
			linkErr = crosslinkControl(r, c.rid, d)
		default:
			continue
		}

		if linkErr != nil {
			return linked, fmt.Errorf("manifest.Crosslink rid=%d type=%d: %w", c.rid, d.Type, linkErr)
		}
		pending = append(pending, p...)
		linked++
	}

	// Pass 2: deferred processing.
	for _, p := range pending {
		switch p.Type {
		case 'w':
			// Wiki backlink refresh — deferred to follow-on ticket
		case 't':
			// Ticket rebuild — minimal: ensure event row exists
		}
	}

	return linked, nil
}
```

Keep existing `crosslinkOne` renamed to `crosslinkCheckin` and existing `crosslinkControl` in place. Add stub functions for new types that return nil (they'll be implemented in subsequent tasks):

```go
func crosslinkWiki(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	return nil, nil // TODO: Task 5
}

func crosslinkTicket(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	return nil, nil // TODO: Task 6
}

func crosslinkEvent(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	return nil, nil // TODO: Task 7
}

func crosslinkAttachment(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return nil // TODO: Task 8
}

func crosslinkCluster(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return nil // TODO: Task 9
}

func crosslinkForum(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	return nil // TODO: Task 10
}
```

Rename `crosslinkOne` to `crosslinkCheckin`. Update signature to match:

```go
func crosslinkCheckin(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	// ... existing body from crosslinkOne ...
}
```

- [ ] **Step 4: Run all existing tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS — existing `TestCrosslinkInlineTCards` and `TestCrosslinkControlArtifact` still pass

- [ ] **Step 5: Run discovery idempotent test**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestDiscoveryQueryIdempotent`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "refactor(crosslink): two-pass architecture with updated discovery query"
```

---

### Task 4: Extend `crosslinkCheckin` with cherrypick and baseid

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test for cherrypick crosslinking**

Add to `go-libfossil/manifest/manifest_test.go`:

```go
func TestCrosslinkCherrypick(t *testing.T) {
	r := setupTestRepo(t)

	// Create base commit
	ridA, _, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("a")}},
		Comment: "A",
		User:    "test",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin A: %v", err)
	}

	// Create commit with cherrypick Q-card
	// Need to manually construct a manifest with Q-cards since Checkin doesn't support them
	uuidA := lookupUUID(t, r, ridA)
	ridB := storeManifestWithCherrypick(t, r, ridA, uuidA)

	// Clear metadata
	r.DB().Exec("DELETE FROM event")
	r.DB().Exec("DELETE FROM tagxref")
	r.DB().Exec("DELETE FROM cherrypick")

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	t.Logf("crosslinked %d", n)

	// Verify cherrypick row
	var count int
	r.DB().QueryRow("SELECT count(*) FROM cherrypick WHERE childid=?", ridB).Scan(&count)
	if count != 1 {
		t.Errorf("cherrypick count=%d, want 1", count)
	}
}
```

Note: `storeManifestWithCherrypick` and `lookupUUID` are test helpers you'll need to write. `storeManifestWithCherrypick` builds a `deck.Deck` with `Q` cards, marshals it, and stores via `blob.Store`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkCherrypick`
Expected: FAIL — cherrypick table not populated

- [ ] **Step 3: Add cherrypick + baseid to `crosslinkCheckin`**

In `crosslinkCheckin`, inside the `r.WithTx` callback, after the mlink section add:

```go
// cherrypick — Q-cards
for _, cp := range d.Q {
	target := cp.Target
	isExclude := 0
	if cp.IsBackout {
		isExclude = 1
	}
	var parentRid int64
	if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", target).Scan(&parentRid); err != nil {
		continue // target not found
	}
	if _, err := tx.Exec(
		"REPLACE INTO cherrypick(parentid, childid, isExclude) VALUES(?, ?, ?)",
		parentRid, rid, isExclude,
	); err != nil {
		return fmt.Errorf("cherrypick: %w", err)
	}
}
```

After the inline T-card loop (outside the transaction), add the `PropagateAll` call matching Fossil line 2472:

```go
// Propagate all tags from primary parent to descendants.
// Matches Fossil: tag_propagate_all(parentid) at manifest.c:2472.
if len(d.P) > 0 {
	var primaryParentRid int64
	if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", d.P[0]).Scan(&primaryParentRid); err == nil {
		tag.PropagateAll(r.DB(), libfossil.FslID(primaryParentRid))
	}
}
```

Also update the plink INSERT to include baseid:

```go
// Resolve baseid for delta manifests
var baseid interface{} = nil // SQL NULL
if d.B != "" {
	var baseRid int64
	if err := tx.QueryRow("SELECT rid FROM blob WHERE uuid=?", d.B).Scan(&baseRid); err == nil {
		baseid = baseRid
	}
}

// In the plink INSERT:
"INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime, baseid) VALUES(?, ?, ?, ?, ?)",
parentRid, rid, isPrim, libfossil.TimeToJulian(d.D), baseid,
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS (all tests including new one)

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): add cherrypick and baseid support to checkin handler"
```

---

### Task 5: Implement `crosslinkWiki` + `addFWTPlink`

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkWiki(t *testing.T) {
	r := setupTestRepo(t)

	// Store a wiki manifest blob manually
	wikiDeck := &deck.Deck{
		Type: deck.Wiki,
		L:    "TestPage",      // wiki title
		U:    "testuser",
		D:    time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC),
		W:    []byte("Hello wiki content"),
	}
	data, err := wikiDeck.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	rid, _, err := blob.Store(r.DB(), data)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	if n == 0 {
		t.Fatal("expected crosslinked wiki artifact")
	}

	// Verify event row with type='w'
	var evType string
	err = r.DB().QueryRow("SELECT type FROM event WHERE objid=?", rid).Scan(&evType)
	if err != nil {
		t.Fatalf("event query: %v", err)
	}
	if evType != "w" {
		t.Errorf("event type=%q, want 'w'", evType)
	}

	// Verify wiki-TestPage tag
	var tagCount int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='wiki-TestPage' AND rid=?", rid,
	).Scan(&tagCount)
	if tagCount != 1 {
		t.Errorf("wiki tag count=%d, want 1", tagCount)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkWiki`
Expected: FAIL — wiki stub returns nil, no event/tag created

- [ ] **Step 3: Implement `addFWTPlink` helper**

Add to `crosslink.go`:

```go
// addFWTPlink inserts plink entries for wiki/forum/event version chains
// and calls tag.PropagateAll on the primary parent.
// Matches Fossil's manifest_add_fwt_plink (manifest.c:2287-2307).
func addFWTPlink(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	mtime := libfossil.TimeToJulian(d.D)
	var primaryParentRid libfossil.FslID

	for i, parentUUID := range d.P {
		var parentRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", parentUUID).Scan(&parentRid); err != nil {
			continue
		}
		isPrim := 0
		if i == 0 {
			isPrim = 1
			primaryParentRid = libfossil.FslID(parentRid)
		}
		if _, err := r.DB().Exec(
			"INSERT OR IGNORE INTO plink(pid, cid, isprim, mtime) VALUES(?, ?, ?, ?)",
			parentRid, rid, isPrim, mtime,
		); err != nil {
			return fmt.Errorf("addFWTPlink plink: %w", err)
		}
	}

	if primaryParentRid > 0 {
		if err := tag.PropagateAll(r.DB(), primaryParentRid); err != nil {
			return fmt.Errorf("addFWTPlink propagate: %w", err)
		}
	}
	return nil
}
```

- [ ] **Step 4: Implement `crosslinkWiki`**

Replace the stub:

```go
func crosslinkWiki(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	// 1. Plink for version chain
	if err := addFWTPlink(r, rid, d); err != nil {
		return nil, fmt.Errorf("wiki plink: %w", err)
	}

	// 2. wiki-<title> singleton tag with content length
	title := d.L
	if title == "" {
		return nil, fmt.Errorf("wiki manifest missing title (L-card)")
	}
	wikiLen := fmt.Sprintf("%d", len(d.W))
	if err := tag.ApplyTag(r, tag.ApplyOpts{
		TargetRID: rid,
		SrcRID:    rid,
		TagName:   fmt.Sprintf("wiki-%s", title),
		TagType:   tag.TagSingleton,
		Value:     wikiLen,
		MTime:     libfossil.TimeToJulian(d.D),
	}); err != nil {
		return nil, fmt.Errorf("wiki tag: %w", err)
	}

	// 3. Determine comment prefix
	var prefix byte
	hasPrior := len(d.P) > 0
	if len(d.W) == 0 {
		prefix = '-' // delete
	} else if !hasPrior {
		prefix = '+' // new page
	} else {
		prefix = ':' // edit
	}

	// 4. Event row
	comment := fmt.Sprintf("%c%s", prefix, title)
	if _, err := r.DB().Exec(
		"REPLACE INTO event(type, mtime, objid, user, comment) VALUES('w', ?, ?, ?, ?)",
		libfossil.TimeToJulian(d.D), rid, d.U, comment,
	); err != nil {
		return nil, fmt.Errorf("wiki event: %w", err)
	}

	// 5. Queue deferred backlink refresh
	return []pendingItem{{Type: 'w', ID: title}}, nil
}
```

- [ ] **Step 5: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): implement wiki handler + addFWTPlink helper"
```

---

### Task 6: Implement `crosslinkTicket`

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkTicket(t *testing.T) {
	r := setupTestRepo(t)

	ticketUUID := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	ticketDeck := &deck.Deck{
		Type: deck.Ticket,
		K:    ticketUUID,
		U:    "testuser",
		D:    time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC),
		J:    []deck.TicketField{{Name: "title", Value: "Bug report"}},
	}
	data, err := ticketDeck.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	rid, _, err := blob.Store(r.DB(), data)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	if n == 0 {
		t.Fatal("expected crosslinked ticket")
	}

	// Verify tkt-* tag
	var tagCount int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname=? AND rid=?",
		"tkt-"+ticketUUID, rid,
	).Scan(&tagCount)
	if tagCount != 1 {
		t.Errorf("ticket tag count=%d, want 1", tagCount)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkTicket`
Expected: FAIL

- [ ] **Step 3: Implement `crosslinkTicket`**

```go
func crosslinkTicket(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	ticketUUID := d.K
	if ticketUUID == "" {
		return nil, fmt.Errorf("ticket manifest missing UUID (K-card)")
	}

	// 1. tkt-<uuid> singleton tag
	if err := tag.ApplyTag(r, tag.ApplyOpts{
		TargetRID: rid,
		SrcRID:    rid,
		TagName:   fmt.Sprintf("tkt-%s", ticketUUID),
		TagType:   tag.TagSingleton,
		MTime:     libfossil.TimeToJulian(d.D),
	}); err != nil {
		return nil, fmt.Errorf("ticket tag: %w", err)
	}

	// 2. Update attachment event comments targeting this ticket
	updateAttachmentComments(r, ticketUUID, 't')

	// 3. Queue deferred ticket rebuild
	return []pendingItem{{Type: 't', ID: ticketUUID}}, nil
}

// updateAttachmentComments updates event comments for attachments targeting the given ID.
func updateAttachmentComments(r *repo.Repo, targetID string, targetType byte) {
	rows, _ := r.DB().Query(
		"SELECT attachid, src, target, filename FROM attachment WHERE target=?",
		targetID,
	)
	if rows == nil {
		return
	}
	defer rows.Close()

	for rows.Next() {
		var attachid int64
		var src, target, filename string
		if err := rows.Scan(&attachid, &src, &target, &filename); err != nil {
			continue
		}
		isAdd := src != ""
		var comment string
		typeName := map[byte]string{'w': "wiki page", 't': "ticket", 'e': "tech note"}[targetType]
		if isAdd {
			comment = fmt.Sprintf("Add attachment %s to %s %s", filename, typeName, target)
		} else {
			comment = fmt.Sprintf("Delete attachment %q from %s %s", filename, typeName, target)
		}
		r.DB().Exec(
			"UPDATE event SET comment=?, type=? WHERE objid=?",
			comment, string(targetType), attachid,
		)
	}
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): implement ticket handler"
```

---

### Task 7: Implement `crosslinkEvent` (technote)

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkEvent(t *testing.T) {
	r := setupTestRepo(t)

	eventID := "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	eventDeck := &deck.Deck{
		Type: deck.Event,
		E:    &deck.EventCard{Date: time.Date(2024, 6, 15, 9, 0, 0, 0, time.UTC), UUID: eventID},
		U:    "testuser",
		D:    time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC),
		C:    "Team meeting notes",
		W:    []byte("Agenda items..."),
	}
	data, err := eventDeck.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	rid, _, err := blob.Store(r.DB(), data)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	if n == 0 {
		t.Fatal("expected crosslinked event")
	}

	// Verify event row type='e'
	var evType string
	err = r.DB().QueryRow("SELECT type FROM event WHERE objid=?", rid).Scan(&evType)
	if err != nil {
		t.Fatalf("event query: %v", err)
	}
	if evType != "e" {
		t.Errorf("event type=%q, want 'e'", evType)
	}

	// Verify event-* tag
	var tagCount int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname=? AND rid=?",
		"event-"+eventID, rid,
	).Scan(&tagCount)
	if tagCount != 1 {
		t.Errorf("event tag count=%d, want 1", tagCount)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkEvent`
Expected: FAIL

- [ ] **Step 3: Implement `crosslinkEvent`**

```go
func crosslinkEvent(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) ([]pendingItem, error) {
	if d.E == nil {
		return nil, fmt.Errorf("event manifest missing E-card")
	}

	// 1. Plink for version chain
	if err := addFWTPlink(r, rid, d); err != nil {
		return nil, fmt.Errorf("event plink: %w", err)
	}

	// 2. event-<eventid> singleton tag with content length
	eventID := d.E.UUID
	eventLen := fmt.Sprintf("%d", len(d.W))
	tagName := fmt.Sprintf("event-%s", eventID)
	mtime := libfossil.TimeToJulian(d.D)

	if err := tag.ApplyTag(r, tag.ApplyOpts{
		TargetRID: rid,
		SrcRID:    rid,
		TagName:   tagName,
		TagType:   tag.TagSingleton,
		Value:     eventLen,
		MTime:     mtime,
	}); err != nil {
		return nil, fmt.Errorf("event tag: %w", err)
	}

	// 3. Check for subsequent version
	var tagid int64
	if err := r.DB().QueryRow("SELECT tagid FROM tag WHERE tagname=?", tagName).Scan(&tagid); err != nil {
		return nil, fmt.Errorf("event tagid lookup: %w", err)
	}

	var subsequent int64
	r.DB().QueryRow(
		"SELECT rid FROM tagxref WHERE tagid=? AND mtime>=? AND rid!=? ORDER BY mtime LIMIT 1",
		tagid, mtime, rid,
	).Scan(&subsequent)

	// 4. Handle prior version
	hasPrior := len(d.P) > 0
	if hasPrior && subsequent == 0 {
		// Delete stale event rows for this tag
		r.DB().Exec(
			"DELETE FROM event WHERE type='e' AND tagid=? AND objid IN (SELECT rid FROM tagxref WHERE tagid=?)",
			tagid, tagid,
		)
	}

	// 5. Insert event row only if no subsequent version
	if subsequent == 0 {
		// Get bgcolor from tagxref
		var bgcolor interface{} = nil
		var bgStr string
		if err := r.DB().QueryRow(
			"SELECT value FROM tagxref JOIN tag USING(tagid) WHERE tagname='bgcolor' AND rid=?", rid,
		).Scan(&bgStr); err == nil {
			bgcolor = bgStr
		}

		if _, err := r.DB().Exec(
			"REPLACE INTO event(type, mtime, objid, tagid, user, comment, bgcolor) VALUES('e', ?, ?, ?, ?, ?, ?)",
			libfossil.TimeToJulian(d.E.Date), rid, tagid, d.U, d.C, bgcolor,
		); err != nil {
			return nil, fmt.Errorf("event insert: %w", err)
		}
	}

	// 6. Update attachment event comments
	updateAttachmentComments(r, eventID, 'e')

	return nil, nil
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): implement technote/event handler"
```

---

### Task 8: Implement `crosslinkAttachment`

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkAttachment(t *testing.T) {
	r := setupTestRepo(t)

	// First create a wiki page so the attachment target type detection works
	wikiDeck := &deck.Deck{
		Type: deck.Wiki, L: "MyPage", U: "test",
		D: time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC), W: []byte("content"),
	}
	wData, _ := wikiDeck.Marshal()
	blob.Store(r.DB(), wData)

	// Store attachment manifest
	attachDeck := &deck.Deck{
		Type: deck.Attachment,
		A:    &deck.AttachmentCard{Filename: "logo.png", Target: "MyPage", Source: "abc123"},
		U:    "testuser",
		D:    time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC),
		C:    "Uploading logo",
	}
	data, _ := attachDeck.Marshal()
	rid, _, _ := blob.Store(r.DB(), data)

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	if n < 1 {
		t.Fatal("expected crosslinked attachment")
	}

	// Verify attachment table
	var count int
	r.DB().QueryRow("SELECT count(*) FROM attachment WHERE attachid=?", rid).Scan(&count)
	if count != 1 {
		t.Errorf("attachment count=%d, want 1", count)
	}

	// Verify isLatest
	var isLatest bool
	r.DB().QueryRow("SELECT isLatest FROM attachment WHERE attachid=?", rid).Scan(&isLatest)
	if !isLatest {
		t.Error("expected isLatest=true")
	}

	// Verify event row
	var evType string
	r.DB().QueryRow("SELECT type FROM event WHERE objid=?", rid).Scan(&evType)
	if evType != "w" {
		t.Errorf("event type=%q, want 'w' (wiki attachment)", evType)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkAttachment`
Expected: FAIL

- [ ] **Step 3: Implement `crosslinkAttachment`**

```go
func crosslinkAttachment(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	if d.A == nil {
		return fmt.Errorf("attachment manifest missing A-card")
	}

	mtime := libfossil.TimeToJulian(d.D)
	src := d.A.Source
	target := d.A.Target
	filename := d.A.Filename
	comment := d.C
	if comment == "" {
		comment = ""
	}

	// 1. Insert into attachment table
	if _, err := r.DB().Exec(
		"INSERT INTO attachment(attachid, mtime, src, target, filename, comment, user) VALUES(?, ?, ?, ?, ?, ?, ?)",
		rid, mtime, src, target, filename, comment, d.U,
	); err != nil {
		return fmt.Errorf("attachment insert: %w", err)
	}

	// 2. Update isLatest flag
	if _, err := r.DB().Exec(
		`UPDATE attachment SET isLatest = (mtime = (SELECT max(mtime) FROM attachment WHERE target=? AND filename=?))
		 WHERE target=? AND filename=?`,
		target, filename, target, filename,
	); err != nil {
		return fmt.Errorf("attachment isLatest: %w", err)
	}

	// 3. Detect target type
	attachToType := byte('w') // default: wiki
	if isHash(target) {
		var dummy int
		if err := r.DB().QueryRow("SELECT 1 FROM tag WHERE tagname=?", "tkt-"+target).Scan(&dummy); err == nil {
			attachToType = 't'
		} else if err := r.DB().QueryRow("SELECT 1 FROM tag WHERE tagname=?", "event-"+target).Scan(&dummy); err == nil {
			attachToType = 'e'
		}
	}

	// 4. Generate comment
	isAdd := src != ""
	typeName := map[byte]string{'w': "wiki page", 't': "ticket", 'e': "tech note"}[attachToType]
	var evComment string
	if isAdd {
		evComment = fmt.Sprintf("Add attachment %s to %s %s", filename, typeName, target)
	} else {
		evComment = fmt.Sprintf("Delete attachment %q from %s %s", filename, typeName, target)
	}

	// 5. Event row
	if _, err := r.DB().Exec(
		"REPLACE INTO event(type, mtime, objid, user, comment) VALUES(?, ?, ?, ?, ?)",
		string(attachToType), mtime, rid, d.U, evComment,
	); err != nil {
		return fmt.Errorf("attachment event: %w", err)
	}

	return nil
}

// isHash returns true if s looks like a Fossil artifact hash (40 or 64 hex chars).
func isHash(s string) bool {
	if len(s) != 40 && len(s) != 64 {
		return false
	}
	for _, c := range s {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): implement attachment handler"
```

---

### Task 9: Implement `crosslinkCluster`

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkCluster(t *testing.T) {
	r := setupTestRepo(t)

	// Store some blobs that will be referenced by the cluster
	content1 := []byte("blob one content")
	rid1, uuid1, _ := blob.Store(r.DB(), content1)
	content2 := []byte("blob two content")
	rid2, uuid2, _ := blob.Store(r.DB(), content2)

	// Verify they're in unclustered
	var uc1, uc2 int
	r.DB().QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid1).Scan(&uc1)
	r.DB().QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid2).Scan(&uc2)
	if uc1 != 1 || uc2 != 1 {
		t.Fatalf("pre: unclustered counts = %d, %d; want 1, 1", uc1, uc2)
	}

	// Store cluster manifest
	clusterDeck := &deck.Deck{
		Type: deck.Cluster,
		M:    []string{uuid1, uuid2},
		D:    time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC),
	}
	data, _ := clusterDeck.Marshal()
	blob.Store(r.DB(), data)

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	if n == 0 {
		t.Fatal("expected crosslinked cluster")
	}

	// Verify members removed from unclustered
	r.DB().QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid1).Scan(&uc1)
	r.DB().QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid2).Scan(&uc2)
	if uc1 != 0 || uc2 != 0 {
		t.Errorf("post: unclustered counts = %d, %d; want 0, 0", uc1, uc2)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkCluster`
Expected: FAIL

- [ ] **Step 3: Implement `crosslinkCluster`**

```go
func crosslinkCluster(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	// 1. Apply cluster tag (tagid=7)
	if err := tag.ApplyTag(r, tag.ApplyOpts{
		TargetRID: rid,
		SrcRID:    rid,
		TagName:   "cluster",
		TagType:   tag.TagSingleton,
		MTime:     libfossil.TimeToJulian(d.D),
	}); err != nil {
		return fmt.Errorf("cluster tag: %w", err)
	}

	// 2. Remove members from unclustered
	for _, memberUUID := range d.M {
		var memberRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", memberUUID).Scan(&memberRid); err != nil {
			continue // member not found
		}
		r.DB().Exec("DELETE FROM unclustered WHERE rid=?", memberRid)
	}

	return nil
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): implement cluster handler"
```

---

### Task 10: Implement `crosslinkForum`

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkForum(t *testing.T) {
	r := setupTestRepo(t)

	// Thread starter (no I-card = not a reply)
	forumDeck := &deck.Deck{
		Type: deck.ForumPost,
		H:    "Discussion about sync",  // thread title
		U:    "testuser",
		D:    time.Date(2024, 3, 1, 12, 0, 0, 0, time.UTC),
		W:    []byte("Let's discuss..."),
	}
	data, _ := forumDeck.Marshal()
	rid, _, _ := blob.Store(r.DB(), data)

	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	if n == 0 {
		t.Fatal("expected crosslinked forum post")
	}

	// Verify forumpost row
	var froot int64
	err = r.DB().QueryRow("SELECT froot FROM forumpost WHERE fpid=?", rid).Scan(&froot)
	if err != nil {
		t.Fatalf("forumpost query: %v", err)
	}
	if froot != int64(rid) {
		t.Errorf("froot=%d, want %d (self = thread starter)", froot, rid)
	}

	// Verify event row type='f'
	var evType, evComment string
	err = r.DB().QueryRow("SELECT type, comment FROM event WHERE objid=?", rid).Scan(&evType, &evComment)
	if err != nil {
		t.Fatalf("event query: %v", err)
	}
	if evType != "f" {
		t.Errorf("event type=%q, want 'f'", evType)
	}
	if evComment != "Post: Discussion about sync" {
		t.Errorf("event comment=%q, want 'Post: Discussion about sync'", evComment)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkForum`
Expected: FAIL

- [ ] **Step 3: Implement `crosslinkForum`**

```go
func crosslinkForum(r *repo.Repo, rid libfossil.FslID, d *deck.Deck) error {
	// 1. Plink for version chain
	if err := addFWTPlink(r, rid, d); err != nil {
		return fmt.Errorf("forum plink: %w", err)
	}

	// 2. Resolve thread references
	var froot, fprev, firt libfossil.FslID

	if d.G != "" {
		var rootRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", d.G).Scan(&rootRid); err == nil {
			froot = libfossil.FslID(rootRid)
		}
	}
	if froot == 0 {
		froot = rid // self is thread root
	}

	if len(d.P) > 0 {
		var prevRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", d.P[0]).Scan(&prevRid); err == nil {
			fprev = libfossil.FslID(prevRid)
		}
	}

	if d.I != "" {
		var irtRid int64
		if err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", d.I).Scan(&irtRid); err == nil {
			firt = libfossil.FslID(irtRid)
		}
	}

	// 3. Insert forumpost
	if _, err := r.DB().Exec(
		"REPLACE INTO forumpost(fpid, froot, fprev, firt, fmtime) VALUES(?, ?, nullif(?, 0), nullif(?, 0), ?)",
		rid, froot, fprev, firt, libfossil.TimeToJulian(d.D),
	); err != nil {
		return fmt.Errorf("forumpost insert: %w", err)
	}

	mtime := libfossil.TimeToJulian(d.D)

	// 4. Event row
	if firt == 0 {
		// Thread starter or edit of thread starter
		title := d.H
		if title == "" {
			title = "(Deleted)"
		}
		fType := "Post"
		if fprev != 0 {
			fType = "Edit"
		}
		comment := fmt.Sprintf("%s: %s", fType, title)

		if _, err := r.DB().Exec(
			"REPLACE INTO event(type, mtime, objid, user, comment) VALUES('f', ?, ?, ?, ?)",
			mtime, rid, d.U, comment,
		); err != nil {
			return fmt.Errorf("forum event: %w", err)
		}

		// If most recent edit, update all event comments for thread
		var hasNewer int
		r.DB().QueryRow(
			"SELECT count(*) FROM forumpost WHERE froot=? AND firt=0 AND fpid!=? AND fmtime>?",
			froot, rid, mtime,
		).Scan(&hasNewer)
		if hasNewer == 0 {
			r.DB().Exec(
				"UPDATE event SET comment=substr(comment,1,instr(comment,':')) || ' ' || ? WHERE objid IN (SELECT fpid FROM forumpost WHERE froot=?)",
				title, froot,
			)
		}
	} else {
		// Reply
		var rootTitle string
		if err := r.DB().QueryRow(
			"SELECT substr(comment, instr(comment,':')+2) FROM event WHERE objid=?", froot,
		).Scan(&rootTitle); err != nil {
			rootTitle = "Unknown"
		}

		var fType string
		if len(d.W) == 0 {
			fType = "Delete reply"
		} else if fprev != 0 {
			fType = "Edit reply"
		} else {
			fType = "Reply"
		}
		comment := fmt.Sprintf("%s: %s", fType, rootTitle)

		if _, err := r.DB().Exec(
			"REPLACE INTO event(type, mtime, objid, user, comment) VALUES('f', ?, ?, ?, ?)",
			mtime, rid, d.U, comment,
		); err != nil {
			return fmt.Errorf("forum reply event: %w", err)
		}
	}

	return nil
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): implement forum handler with forumpost table"
```

---

### Task 11: Extend `crosslinkControl` with event row (type='g')

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go`
- Test: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write failing test**

```go
func TestCrosslinkControlEventRow(t *testing.T) {
	r := setupTestRepo(t)

	rid, _, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("a")}},
		Comment: "initial",
		User:    "test",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Create a control artifact
	_, err = tag.AddTag(r, tag.TagOpts{
		TargetRID: rid,
		TagName:   "testlabel",
		TagType:   tag.TagSingleton,
		Value:     "myvalue",
		User:      "tagger",
		Time:      time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}

	// Clear tagxref + event to force re-crosslink
	r.DB().Exec("DELETE FROM tagxref")
	r.DB().Exec("DELETE FROM event WHERE type='g'")

	Crosslink(r)

	// Verify event row type='g' exists for the control artifact
	var count int
	r.DB().QueryRow("SELECT count(*) FROM event WHERE type='g'").Scan(&count)
	if count == 0 {
		t.Error("expected event row with type='g' for control artifact")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkControlEventRow`
Expected: FAIL — no type='g' event row

- [ ] **Step 3: Add event row generation to `crosslinkControl`**

At the end of `crosslinkControl`, after the tag application loop, add:

```go
// Generate event row with type='g' and descriptive comment.
// Matches Fossil lines 2705-2808.
var comment string
for _, tc := range d.T {
	if tc.UUID == "*" {
		continue
	}
	prefix := string(tc.Type)
	name := tc.Name
	val := tc.Value

	switch {
	case prefix == "*" && name == "branch":
		comment += fmt.Sprintf(" Move to branch %s.", val)
	case prefix == "*" && name == "bgcolor":
		comment += fmt.Sprintf(" Change branch background color to %q.", val)
	case prefix == "+" && name == "bgcolor":
		comment += fmt.Sprintf(" Change background color to %q.", val)
	case prefix == "-" && name == "bgcolor":
		comment += " Cancel background color."
	case prefix == "+" && name == "comment":
		comment += " Edit check-in comment."
	case prefix == "+" && name == "user":
		comment += fmt.Sprintf(" Change user to %q.", val)
	default:
		switch prefix {
		case "-":
			comment += fmt.Sprintf(" Cancel %q.", name)
		case "+":
			comment += fmt.Sprintf(" Add %q.", name)
		case "*":
			comment += fmt.Sprintf(" Add propagating %q.", name)
		}
	}
}
if comment == "" {
	comment = " "
}

if _, err := r.DB().Exec(
	"REPLACE INTO event(type, mtime, objid, user, comment) VALUES('g', ?, ?, ?, ?)",
	mtime, srcRID, d.U, comment,
); err != nil {
	return fmt.Errorf("control event: %w", err)
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/manifest_test.go
git commit -m "feat(crosslink): add event row (type='g') for control artifacts"
```

---

### Task 12: Implement dephantomize hook

**Files:**
- Create: `go-libfossil/manifest/dephantomize.go`
- Create: `go-libfossil/manifest/dephantomize_test.go`

- [ ] **Step 1: Write failing test**

Create `go-libfossil/manifest/dephantomize_test.go`:

```go
package manifest

import (
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/deck"
)

func TestDephantomizeCrosslinks(t *testing.T) {
	r := setupTestRepo(t)

	// Create a checkin manifest
	checkinDeck := &deck.Deck{
		Type: deck.Checkin,
		D:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
		U:    "test",
		C:    "initial",
		F:    []deck.FileCard{{Name: "a.txt", UUID: "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"}},
		T:    []deck.TagCard{{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "trunk"}},
	}
	data, _ := checkinDeck.Marshal()
	uuid := computeUUID(data)

	// Store as phantom first
	phantomRid, _ := blob.StorePhantom(r.DB(), uuid)

	// Verify no event row
	var count int
	r.DB().QueryRow("SELECT count(*) FROM event WHERE objid=?", phantomRid).Scan(&count)
	if count != 0 {
		t.Fatal("phantom should have no event row")
	}

	// Now fill the phantom with real content (simulating what storeReceivedFile does)
	compressed, _ := blob.Compress(data)
	r.DB().Exec("UPDATE blob SET size=?, content=?, rcvid=1 WHERE rid=?",
		len(data), compressed, phantomRid)
	r.DB().Exec("DELETE FROM phantom WHERE rid=?", phantomRid)

	// Trigger dephantomize — should crosslink the now-real blob
	AfterDephantomize(r, phantomRid)

	// Verify event row was created by crosslinking
	r.DB().QueryRow("SELECT count(*) FROM event WHERE objid=?", phantomRid).Scan(&count)
	if count != 1 {
		t.Errorf("event count=%d, want 1 after dephantomize", count)
	}
}
```

Note: `computeUUID` is a helper that calculates the SHA1 hash of content. Use `hash.SHA1(data)`.

Also add orphan and delta chain tests:

```go
func TestDephantomizeOrphan(t *testing.T) {
	r := setupTestRepo(t)

	// Create a delta manifest that references a phantom baseline
	baselineUUID := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	baselineRid, _ := blob.StorePhantom(r.DB(), baselineUUID)

	// Create a checkin manifest with B-card pointing to phantom baseline
	checkinDeck := &deck.Deck{
		Type: deck.Checkin,
		B:    baselineUUID,
		D:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
		U:    "test", C: "delta checkin",
		T: []deck.TagCard{{Type: deck.TagPropagating, Name: "branch", UUID: "*", Value: "trunk"}},
	}
	data, _ := checkinDeck.Marshal()
	deltaRid, _, _ := blob.Store(r.DB(), data)

	// Insert orphan entry (crosslinkCheckin would do this when baseline is phantom)
	r.DB().Exec("INSERT INTO orphan(rid, baseline) VALUES(?, ?)", deltaRid, baselineRid)

	// Fill the baseline phantom
	baseContent := []byte("baseline manifest content")
	compressed, _ := blob.Compress(baseContent)
	r.DB().Exec("UPDATE blob SET size=?, content=?, rcvid=1 WHERE rid=?",
		len(baseContent), compressed, baselineRid)
	r.DB().Exec("DELETE FROM phantom WHERE rid=?", baselineRid)

	// Dephantomize should process orphaned delta manifest
	AfterDephantomize(r, baselineRid)

	// Verify orphan was cleaned up
	var orphanCount int
	r.DB().QueryRow("SELECT count(*) FROM orphan WHERE baseline=?", baselineRid).Scan(&orphanCount)
	if orphanCount != 0 {
		t.Errorf("orphan count=%d, want 0 after dephantomize", orphanCount)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestDephantomize`
Expected: FAIL — `AfterDephantomize` undefined

- [ ] **Step 3: Implement dephantomize.go**

Create `go-libfossil/manifest/dephantomize.go`:

```go
package manifest

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// AfterDephantomize crosslinks a formerly-phantom blob and any dependents.
// Matches Fossil's after_dephantomize (content.c:389-456).
func AfterDephantomize(r *repo.Repo, rid libfossil.FslID) {
	if r == nil {
		panic("manifest.AfterDephantomize: r must not be nil")
	}
	if rid <= 0 {
		return
	}
	afterDephantomize(r, rid, true)
}

func afterDephantomize(r *repo.Repo, rid libfossil.FslID, linkFlag bool) {
	for rid > 0 {
		// 1. Parse and crosslink the blob itself
		if linkFlag {
			crosslinkSingle(r, rid)
		}

		// 2. Process orphaned delta manifests whose baseline is this rid
		orphanRows, err := r.DB().Query("SELECT rid FROM orphan WHERE baseline=?", rid)
		if err == nil {
			var orphans []libfossil.FslID
			for orphanRows.Next() {
				var orid int64
				if orphanRows.Scan(&orid) == nil {
					orphans = append(orphans, libfossil.FslID(orid))
				}
			}
			orphanRows.Close()
			for _, orid := range orphans {
				crosslinkSingle(r, orid)
			}
			if len(orphans) > 0 {
				r.DB().Exec("DELETE FROM orphan WHERE baseline=?", rid)
			}
		}

		// 3. Find delta children not yet crosslinked
		childRows, err := r.DB().Query(
			`SELECT rid FROM delta WHERE srcid=?
			 AND NOT EXISTS (SELECT 1 FROM mlink WHERE mid=delta.rid)`,
			rid,
		)
		if err != nil {
			return
		}
		var children []libfossil.FslID
		for childRows.Next() {
			var crid int64
			if childRows.Scan(&crid) == nil {
				children = append(children, libfossil.FslID(crid))
			}
		}
		childRows.Close()

		// Recurse for all but the last (tail-call optimization)
		for i := 1; i < len(children); i++ {
			afterDephantomize(r, children[i], true)
		}

		// Tail recurse for the first child
		if len(children) > 0 {
			rid = children[0]
			linkFlag = true
		} else {
			rid = 0
		}
	}
}

// crosslinkSingle crosslinks a single blob by rid.
func crosslinkSingle(r *repo.Repo, rid libfossil.FslID) {
	data, err := content.Expand(r.DB(), rid)
	if err != nil {
		return
	}
	d, err := deck.Parse(data)
	if err != nil {
		return
	}

	switch d.Type {
	case deck.Checkin:
		crosslinkCheckin(r, rid, d)
	case deck.Wiki:
		crosslinkWiki(r, rid, d)
	case deck.Ticket:
		crosslinkTicket(r, rid, d)
	case deck.Event:
		crosslinkEvent(r, rid, d)
	case deck.Attachment:
		crosslinkAttachment(r, rid, d)
	case deck.Cluster:
		crosslinkCluster(r, rid, d)
	case deck.ForumPost:
		crosslinkForum(r, rid, d)
	case deck.Control:
		crosslinkControl(r, rid, d)
	}
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/manifest/dephantomize.go go-libfossil/manifest/dephantomize_test.go
git commit -m "feat(crosslink): implement AfterDephantomize matching Fossil"
```

---

### Task 13: Integrate dephantomize into `storeReceivedFile`

**Files:**
- Modify: `go-libfossil/sync/client.go`
- Modify: `go-libfossil/sync/session.go`

- [ ] **Step 1: Add dephantomize hook to sync session**

In `go-libfossil/sync/session.go`, add a field to the session struct:

```go
dephantomizeHook func(libfossil.FslID)
```

- [ ] **Step 3: Wire dephantomize into `storeReceivedFile`**

In `go-libfossil/sync/client.go`, modify `storeReceivedFile` to check for phantom-to-real transition. The function currently does a raw INSERT. Add phantom detection:

After the `blob.Exists` check at line 506, instead of returning nil immediately when the blob exists, check if it's a phantom:

```go
return r.WithTx(func(tx *db.Tx) error {
	existingRid, exists := blob.Exists(tx, uuid)
	if exists {
		// Check if phantom
		var size int64
		tx.QueryRow("SELECT size FROM blob WHERE rid=?", existingRid).Scan(&size)
		if size != -1 {
			return nil // real blob already exists
		}
		// Fill phantom
		compressed, err := blob.Compress(fullContent)
		if err != nil {
			return err
		}
		if _, err := tx.Exec("UPDATE blob SET size=?, content=?, rcvid=1 WHERE rid=?",
			len(fullContent), compressed, existingRid); err != nil {
			return err
		}
		tx.Exec("DELETE FROM phantom WHERE rid=?", existingRid)
		tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", existingRid)
		// Dephantomize hook will be called after tx commits (see handleFileCard)
		return nil
	}
	// ... existing INSERT path ...
})
```

Note: The actual `AfterDephantomize` call should happen from the session (which has access to the repo), not from `storeReceivedFile` (which operates within a transaction). Wire it in `handleFileCard` after `storeReceivedFile` returns.

- [ ] **Step 4: Run all tests**

Run: `make test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/sync/client.go go-libfossil/sync/session.go
git commit -m "feat(sync): wire dephantomize hook into storeReceivedFile"
```

---

### Task 14: Auto-crosslink after `sync.Sync()` + rename `CheckinsLinked`

**Files:**
- Modify: `go-libfossil/sync/session.go`
- Modify: `go-libfossil/sync/stubs.go`
- Modify: `go-libfossil/sync/clone.go` (update field name)
- Modify: `go-libfossil/sync/clone_test.go` (update field name references)

- [ ] **Step 1: Rename `CheckinsLinked` to `ArtifactsLinked` in stubs.go**

In `go-libfossil/sync/stubs.go`, rename `CheckinsLinked` to `ArtifactsLinked` in `CloneResult`:

```go
ArtifactsLinked int     // Artifacts crosslinked into metadata tables
```

Also add `ArtifactsLinked` to `SyncResult` so the auto-crosslink count is reported:

```go
// SyncResult reports what happened during a sync.
type SyncResult struct {
	Rounds, FilesSent, FilesRecvd int
	UVFilesSent, UVFilesRecvd     int
	UVGimmesSent                  int
	ArtifactsLinked               int      // Artifacts crosslinked after convergence
	Errors                        []string
}
```

- [ ] **Step 2: Update all references to `CheckinsLinked`**

Run `rg CheckinsLinked` to find all occurrences. Key locations:
- `go-libfossil/sync/clone.go` — where `CloneResult.CheckinsLinked` is set
- `go-libfossil/sync/clone_test.go` — 4 references at lines ~415, ~441, ~500, ~585

Update all to `ArtifactsLinked`.

- [ ] **Step 3: Add auto-crosslink after sync convergence**

In `go-libfossil/sync/session.go`, between the sync loop (ends at `break`) and `obs.Completed`, insert:

```go
// Auto-crosslink after convergence
linked, xlinkErr := manifest.Crosslink(s.repo)
if xlinkErr != nil {
	obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), xlinkErr)
	return &s.result, fmt.Errorf("sync: crosslink: %w", xlinkErr)
}
s.result.ArtifactsLinked = linked
```

Add the import: `"github.com/dmestas/edgesync/go-libfossil/manifest"`

- [ ] **Step 4: Run all tests**

Run: `make test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/sync/session.go go-libfossil/sync/stubs.go go-libfossil/sync/clone.go go-libfossil/sync/clone_test.go
git commit -m "feat(sync): auto-crosslink after Sync convergence, rename CheckinsLinked to ArtifactsLinked"
```

---

### Task 15: Two-pass ordering test

**Files:**
- Modify: `go-libfossil/manifest/manifest_test.go`

- [ ] **Step 1: Write test verifying deferred processing order**

```go
func TestCrosslinkTwoPass(t *testing.T) {
	r := setupTestRepo(t)

	// Store a wiki manifest and a checkin manifest
	// The two-pass architecture should handle both correctly
	// regardless of blob storage order

	wikiDeck := &deck.Deck{
		Type: deck.Wiki, L: "TwoPassPage", U: "test",
		D: time.Date(2024, 1, 1, 0, 0, 0, 0, time.UTC), W: []byte("content"),
	}
	wData, _ := wikiDeck.Marshal()
	wikiRid, _, _ := blob.Store(r.DB(), wData)

	checkinRid, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("a")}},
		Comment: "commit", User: "test",
		Time: time.Date(2024, 1, 2, 0, 0, 0, 0, time.UTC),
	})

	// Clear all metadata
	r.DB().Exec("DELETE FROM event")
	r.DB().Exec("DELETE FROM tagxref")

	// Single Crosslink call should handle both types
	n, err := Crosslink(r)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	t.Logf("crosslinked %d artifacts", n)
	if n < 2 {
		t.Errorf("crosslinked %d, want >= 2", n)
	}

	// Verify both event rows exist
	var wikiEv, checkinEv int
	r.DB().QueryRow("SELECT count(*) FROM event WHERE objid=? AND type='w'", wikiRid).Scan(&wikiEv)
	r.DB().QueryRow("SELECT count(*) FROM event WHERE objid=? AND type='ci'", checkinRid).Scan(&checkinEv)
	if wikiEv != 1 {
		t.Errorf("wiki event count=%d, want 1", wikiEv)
	}
	if checkinEv != 1 {
		t.Errorf("checkin event count=%d, want 1", checkinEv)
	}
}
```

- [ ] **Step 2: Run test**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -v -run TestCrosslinkTwoPass`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/manifest/manifest_test.go
git commit -m "test(crosslink): add two-pass ordering verification test"
```

---

### Task 16: DST crosslink completeness test

**Files:**
- Create: `dst/crosslink_completeness_test.go`

- [ ] **Step 1: Write DST test**

Create `dst/crosslink_completeness_test.go`. **IMPORTANT:** The pseudocode below uses placeholder APIs. Before writing the test, read these files to understand the actual DST infrastructure:
- `dst/e2e_test.go` — existing tests use `Sim`, `MockFossil`, `createMasterRepo`
- `dst/network.go` — `SimNetwork` constructor and methods
- `dst/tag_branch_test.go` — closest pattern to follow (crosslink after sync)

The test should:
1. Set up a `SimNetwork` (or `PeerNetwork`) with 3 leaves using the actual constructors
2. Create 5 checkins on the master repo
3. Sync all leaves
4. Run `manifest.Crosslink` on each leaf repo
5. Verify: event rows > 0, plink rows >= 4, branch tags > 0 on each leaf

```go
// PSEUDOCODE — adapt to actual DST APIs from dst/e2e_test.go and dst/network.go
func TestCrosslinkCompletenessAfterSync(t *testing.T) {
	// Use actual DST setup pattern from existing tests
	// Create master + 3 leaves, sync, crosslink, verify metadata tables
}
```

- [ ] **Step 2: Run DST test**

Run: `cd dst && go test -buildvcs=false -v -run TestCrosslinkCompleteness`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add dst/crosslink_completeness_test.go
git commit -m "test(dst): add crosslink completeness verification after sync"
```

---

### Task 17: Run full test suite and verify

**Files:** None (verification only)

- [ ] **Step 1: Run full make test**

Run: `make test`
Expected: All tests pass

- [ ] **Step 2: Run DST full suite**

Run: `make dst`
Expected: All DST tests pass including new crosslink completeness test

- [ ] **Step 3: Run sim tests**

Run: `make sim`
Expected: All sim tests pass

- [ ] **Step 4: Final commit if any fixups needed**

If any tests required fixes, commit them here.
