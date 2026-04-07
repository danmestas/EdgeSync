# Server-Side IGot Filtering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Filter outgoing igot cards in go-libfossil's sync handler to skip blobs the client already announced, matching Fossil's `onremote` pattern.

**Architecture:** Add a `remoteHas map[string]bool` to the handler struct, populate it from incoming igot cards, and filter `emitIGots()`, `emitPrivateIGots()`, and `sendAllClusters()` against it. Pure in-memory, no schema changes, no public API changes.

**Tech Stack:** Go, go-libfossil (`sync/handler.go`, `sync/handler_test.go`)

**Target repo:** go-libfossil (at `../go-libfossil` or wherever the local checkout lives)

**Spec:** `docs/superpowers/specs/2026-04-07-igot-filter-design.md`

**IMPORTANT:** Do not add any Claude Code co-author attribution to commits in go-libfossil. Work on a feature branch, never push directly to main.

---

### Task 1: Write failing test for igot filtering

**Files:**
- Modify: `sync/handler_test.go` (append new test)

- [ ] **Step 1: Write the failing test**

Add `TestHandlerIGotFiltersEmit` to `sync/handler_test.go`. This test creates a repo with known blobs, sends a pull request with igot cards for a subset, and asserts the response only contains igot cards for the blobs the client did NOT announce.

```go
func TestHandlerIGotFiltersEmit(t *testing.T) {
	r := setupSyncTestRepo(t)

	// Store 3 blobs with known UUIDs.
	data1 := []byte("igot-filter-blob-one")
	data2 := []byte("igot-filter-blob-two")
	data3 := []byte("igot-filter-blob-three")
	uuid1 := storeTestBlob(t, r, data1)
	uuid2 := storeTestBlob(t, r, data2)
	uuid3 := storeTestBlob(t, r, data3)

	// Client announces it already has blob 1 and blob 2.
	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.IGotCard{UUID: uuid1},
		&xfer.IGotCard{UUID: uuid2},
	)

	igots := findCards[*xfer.IGotCard](resp)
	igotUUIDs := make(map[string]bool)
	for _, ig := range igots {
		igotUUIDs[ig.UUID] = true
	}

	// Server should NOT echo back blobs the client already has.
	if igotUUIDs[uuid1] {
		t.Errorf("server should not emit igot for uuid1 (%s) — client already has it", uuid1)
	}
	if igotUUIDs[uuid2] {
		t.Errorf("server should not emit igot for uuid2 (%s) — client already has it", uuid2)
	}

	// Server SHOULD emit igot for blob 3, which the client didn't announce.
	if !igotUUIDs[uuid3] {
		t.Errorf("server should emit igot for uuid3 (%s) — client doesn't have it", uuid3)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `go test ./sync/ -run TestHandlerIGotFiltersEmit -v -count=1`

Expected: FAIL — igotUUIDs will contain uuid1 and uuid2 because `emitIGots()` currently emits all blobs without filtering.

- [ ] **Step 3: Commit failing test**

```bash
git add sync/handler_test.go
git commit -m "test: add TestHandlerIGotFiltersEmit (failing — no remoteHas filtering)"
```

---

### Task 2: Add remoteHas field and populate from handleIGot

**Files:**
- Modify: `sync/handler.go:79-102` (handler struct)
- Modify: `sync/handler.go:332-351` (handleIGot method)

- [ ] **Step 1: Add remoteHas field to handler struct**

In `sync/handler.go`, add the field after `cache` (line 96):

```go
type handler struct {
	repo          *repo.Repo
	buggify       BuggifyChecker
	resp          []xfer.Card
	pushOK        bool // client sent a valid push card
	pullOK        bool // client sent a valid pull card
	cloneMode     bool // client sent a clone card
	cloneSeq      int  // clone_seqno cursor from client
	uvCatalogSent bool // true after sending UV catalog
	reqClusters   bool // client sent pragma req-clusters
	filesSent     int  // files sent in response (for observer)
	filesRecvd    int  // files received from client (for observer)
	syncPrivate   bool // true if pragma send-private was accepted
	nextIsPrivate bool // true if a private card precedes the next file/cfile
	syncedTables  map[string]*SyncedTable // cached table definitions
	xrowsSent     int  // table sync rows sent
	xrowsRecvd    int  // table sync rows received
	cache         *content.Cache // nil = passthrough to content.Expand
	remoteHas     map[string]bool // UUIDs the client announced via igot (for filtering emitIGots)

	// Auth state
	user   string // verified username ("nobody" if no login card)
	caps   string // capability string from user table
	authed bool   // whether login card was verified
}
```

- [ ] **Step 2: Populate remoteHas in handleIGot**

Replace the `handleIGot` method:

```go
func (h *handler) handleIGot(c *xfer.IGotCard) error {
	if c == nil {
		panic("handler.handleIGot: c must not be nil")
	}
	if !h.pullOK {
		return nil
	}
	_, exists := blob.Exists(h.repo.DB(), c.UUID)
	if exists {
		// Record that the client has this blob so emitIGots can skip it.
		// Mirrors Fossil's remote_has() → onremote table (xfer.c:1471).
		if h.remoteHas == nil {
			h.remoteHas = make(map[string]bool)
		}
		h.remoteHas[c.UUID] = true
		return nil
	}
	if c.IsPrivate && !h.syncPrivate {
		return nil // not authorized — don't request
	}
	h.resp = append(h.resp, &xfer.GimmeCard{UUID: c.UUID})
	return nil
}
```

- [ ] **Step 3: Run existing handler tests to verify no regression**

Run: `go test ./sync/ -run TestHandler -v -count=1`

Expected: All existing TestHandler* tests PASS. The new field is nil by default, so behavior is unchanged when no igots are processed.

- [ ] **Step 4: Commit**

```bash
git add sync/handler.go
git commit -m "feat: add remoteHas field to handler, populate from client igot cards"
```

---

### Task 3: Filter emitIGots, emitPrivateIGots, and sendAllClusters

**Files:**
- Modify: `sync/handler.go:447-488` (emitIGots)
- Modify: `sync/handler.go:493-523` (emitPrivateIGots)
- Modify: `sync/handler.go:527-551` (sendAllClusters)

- [ ] **Step 1: Add remoteHas filter to emitIGots**

In `emitIGots()`, add the filter inside the `rows.Next()` loop, after scanning:

```go
func (h *handler) emitIGots() error {
	// Emit igot for all non-phantom blobs so the client can discover
	// everything the server has. Cluster generation is a client-side
	// optimization for push; the server always advertises all blobs.
	rows, err := h.repo.DB().Query(`
		SELECT uuid FROM blob WHERE size >= 0
		AND NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)
		AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)`,
	)
	if err != nil {
		return fmt.Errorf("handler: listing blobs: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		// remoteHas is populated from client igot cards in handleIGot.
		// nil when no igots received (clone, push-only, or first round).
		if h.remoteHas[uuid] {
			continue
		}
		uuids = append(uuids, uuid)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	// BUGGIFY: 10% chance truncate igot list to test multi-round convergence.
	if h.buggify != nil && h.buggify.Check("handler.emitIGots.truncate", 0.10) && len(uuids) > 1 {
		uuids = uuids[:len(uuids)/2]
	}

	for _, uuid := range uuids {
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}

	if h.syncPrivate {
		if err := h.emitPrivateIGots(); err != nil {
			return err
		}
	}
	return nil
}
```

- [ ] **Step 2: Add remoteHas filter to emitPrivateIGots**

In `emitPrivateIGots()`, add the same filter inside the `rows.Next()` loop:

```go
func (h *handler) emitPrivateIGots() error {
	rows, err := h.repo.DB().Query(
		"SELECT b.uuid FROM private p JOIN blob b ON p.rid=b.rid WHERE b.size >= 0",
	)
	if err != nil {
		return fmt.Errorf("handler: listing private blobs: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		if h.remoteHas[uuid] {
			continue
		}
		uuids = append(uuids, uuid)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	// BUGGIFY: 10% chance truncate private igot list to test multi-round convergence.
	if h.buggify != nil && h.buggify.Check("handler.emitPrivateIGots.truncate", 0.10) && len(uuids) > 1 {
		uuids = uuids[:len(uuids)/2]
	}

	for _, uuid := range uuids {
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid, IsPrivate: true})
	}
	return nil
}
```

- [ ] **Step 3: Add remoteHas filter to sendAllClusters**

In `sendAllClusters()`, add the filter inside the `rows.Next()` loop:

```go
func (h *handler) sendAllClusters() error {
	rows, err := h.repo.DB().Query(`
		SELECT b.uuid FROM tagxref tx
		JOIN blob b ON tx.rid = b.rid
		WHERE tx.tagid = 7
		  AND NOT EXISTS (SELECT 1 FROM unclustered WHERE rid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM phantom WHERE rid = b.rid)
		  AND NOT EXISTS (SELECT 1 FROM shun WHERE uuid = b.uuid)
		  AND NOT EXISTS (SELECT 1 FROM private WHERE rid = b.rid)
		  AND b.size >= 0
	`)
	if err != nil {
		return fmt.Errorf("handler: listing clusters: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		if h.remoteHas[uuid] {
			continue
		}
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}
	return rows.Err()
}
```

- [ ] **Step 4: Run TestHandlerIGotFiltersEmit to verify it passes**

Run: `go test ./sync/ -run TestHandlerIGotFiltersEmit -v -count=1`

Expected: PASS — the server now filters igot cards for blobs the client announced.

- [ ] **Step 5: Run full sync test suite**

Run: `go test ./sync/ -v -count=1`

Expected: All tests PASS. No regressions.

- [ ] **Step 6: Commit**

```bash
git add sync/handler.go
git commit -m "feat: filter emitIGots/emitPrivateIGots/sendAllClusters against remoteHas

Skip igot cards for blobs the client already announced, matching
Fossil's onremote temp table pattern (xfer.c:1011-1012)."
```

---

### Task 4: Write test for private igot filtering

**Files:**
- Modify: `sync/handler_test.go` (append new test)

- [ ] **Step 1: Write the test**

Add `TestHandlerIGotFiltersPrivateEmit` to `sync/handler_test.go`. This test verifies that private igots are also filtered when the client announces them.

```go
func TestHandlerIGotFiltersPrivateEmit(t *testing.T) {
	r := setupSyncTestRepo(t)
	// Grant private sync capability.
	r.DB().Exec("UPDATE user SET cap='oix' WHERE login='nobody'")

	// Store a blob and mark it private.
	data := []byte("igot-filter-private-blob")
	uuid := storeTestBlob(t, r, data)
	rid, _ := blob.Exists(r.DB(), uuid)
	content.MakePrivate(r.DB(), int64(rid))

	// Store a second private blob the client does NOT have.
	data2 := []byte("igot-filter-private-blob-two")
	uuid2 := storeTestBlob(t, r, data2)
	rid2, _ := blob.Exists(r.DB(), uuid2)
	content.MakePrivate(r.DB(), int64(rid2))

	// Client announces it has the first private blob.
	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
		&xfer.IGotCard{UUID: uuid, IsPrivate: true},
	)

	igots := findCards[*xfer.IGotCard](resp)
	for _, ig := range igots {
		if ig.UUID == uuid {
			t.Errorf("server should not emit private igot for %s — client already has it", uuid)
		}
	}

	// The second private blob should still appear.
	found := false
	for _, ig := range igots {
		if ig.UUID == uuid2 && ig.IsPrivate {
			found = true
		}
	}
	if !found {
		t.Errorf("server should emit private igot for %s — client doesn't have it", uuid2)
	}
}
```

- [ ] **Step 2: Run the test**

Run: `go test ./sync/ -run TestHandlerIGotFiltersPrivateEmit -v -count=1`

Expected: PASS (the filtering from Task 3 already covers private igots).

- [ ] **Step 3: Commit**

```bash
git add sync/handler_test.go
git commit -m "test: add TestHandlerIGotFiltersPrivateEmit"
```

---

### Task 5: Run full test suite and verify

**Files:** None (verification only)

- [ ] **Step 1: Run all sync package tests**

Run: `go test ./sync/ -v -count=1`

Expected: All tests PASS.

- [ ] **Step 2: Run full go-libfossil test suite**

Run: `go test ./... -count=1`

Expected: All tests PASS.

- [ ] **Step 3: Verify no public API changes**

Run: `go doc ./sync/ HandleSync HandleSyncWithOpts SyncOpts HandleOpts SyncResult`

Verify: No new exported fields, no new parameters, no signature changes. The optimization is entirely internal.
