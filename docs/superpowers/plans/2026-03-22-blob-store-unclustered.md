# blob.Store Auto-Mark Unclustered & Single-Pass Handler Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align `blob.Store` and the sync handler with Fossil C's architecture -- auto-mark unclustered on store, single-pass card processing.

**Architecture:** `blob.Store` and `blob.StoreDelta` gain automatic `INSERT OR IGNORE INTO unclustered` after new blob inserts (matching `content_put_ex`). Redundant unclustered inserts are removed from all callers. The handler reverts from three-pass to single-pass wire-order card processing (matching `page_xfer`).

**Tech Stack:** Go, SQLite

**Spec:** `docs/superpowers/specs/2026-03-22-blob-store-unclustered-design.md`

---

### Task 1: blob.Store auto-marks unclustered

**Files:**
- Modify: `go-libfossil/blob/blob.go:36-50` (Store) and `go-libfossil/blob/blob.go:86-105` (StoreDelta)
- Test: `go-libfossil/blob/blob_test.go`

- [ ] **Step 1: Write failing test for Store auto-marking unclustered**

Add to `blob_test.go`:

```go
func TestStoreMarksUnclustered(t *testing.T) {
	d := setupTestDB(t)
	content := []byte("unclustered test blob")

	rid, _, err := Store(d, content)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	var count int
	d.QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid).Scan(&count)
	if count != 1 {
		t.Fatalf("unclustered count = %d, want 1", count)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/blob/ -run TestStoreMarksUnclustered -v`
Expected: FAIL -- unclustered count = 0, want 1

- [ ] **Step 3: Write failing test for StoreDelta auto-marking unclustered**

Add to `blob_test.go`:

```go
func TestStoreDeltaMarksUnclustered(t *testing.T) {
	d := setupTestDB(t)
	source := []byte("delta source content here")
	target := []byte("delta target content here")

	srcRid, _, _ := Store(d, source)
	tgtRid, _, err := StoreDelta(d, target, srcRid)
	if err != nil {
		t.Fatalf("StoreDelta: %v", err)
	}

	// Both source and target should be in unclustered.
	for _, rid := range []int64{int64(srcRid), int64(tgtRid)} {
		var count int
		d.QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid).Scan(&count)
		if count != 1 {
			t.Fatalf("unclustered count for rid %d = %d, want 1", rid, count)
		}
	}
}
```

- [ ] **Step 4: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/blob/ -run TestStoreDeltaMarksUnclustered -v`
Expected: FAIL

- [ ] **Step 5: Write failing test for Store idempotency (existing blob NOT re-marked)**

Add to `blob_test.go`:

```go
func TestStoreExistingBlobSkipsUnclustered(t *testing.T) {
	d := setupTestDB(t)
	content := []byte("idempotent blob test")

	rid, _, _ := Store(d, content)

	// Clear unclustered to simulate it being consumed by clustering.
	d.Exec("DELETE FROM unclustered WHERE rid=?", rid)

	// Store same content again -- should return existing rid, NOT re-mark unclustered.
	rid2, _, err := Store(d, content)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}
	if rid2 != rid {
		t.Fatalf("rid = %d, want %d (same blob)", rid2, rid)
	}

	var count int
	d.QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid).Scan(&count)
	if count != 0 {
		t.Fatalf("unclustered count = %d, want 0 (already-existing blob)", count)
	}
}
```

- [ ] **Step 6: Run test to verify it passes (existing behavior already correct)**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/blob/ -run TestStoreExistingBlobSkipsUnclustered -v`
Expected: PASS (Exists early-return means no insert, no unclustered)

- [ ] **Step 7: Implement auto-mark unclustered in Store**

In `blob.go`, add the unclustered INSERT after the successful blob insert in `Store` (after line 49, before the return):

```go
	rid = libfossil.FslID(ridInt)

	// Mark as unclustered — matches Fossil's content_put_ex (content.c:633).
	// Only new blobs reach here; Exists early-return skips this.
	if _, err := q.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
		return 0, "", fmt.Errorf("blob.Store unclustered: %w", err)
	}

	return rid, uuid, nil
```

- [ ] **Step 8: Implement auto-mark unclustered in StoreDelta**

In `blob.go`, add the unclustered INSERT after the delta table insert in `StoreDelta` (after line 103, before the return):

```go
	_, err = q.Exec("INSERT INTO delta(rid, srcid) VALUES(?, ?)", rid, srcRid)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta insert delta: %w", err)
	}

	// Mark as unclustered — matches Fossil's content_put_ex (content.c:633).
	if _, err := q.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta unclustered: %w", err)
	}

	return rid, uuid, nil
```

- [ ] **Step 9: Run all blob tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/blob/ -v`
Expected: ALL PASS (including the 3 new tests)

- [ ] **Step 10: Commit**

```bash
git add go-libfossil/blob/blob.go go-libfossil/blob/blob_test.go
git commit -m "feat: blob.Store and StoreDelta auto-mark unclustered

Matches Fossil's content_put_ex (content.c:633). Only new blobs
are marked; Exists early-return skips the insert."
```

---

### Task 2: Remove redundant unclustered inserts from go-libfossil callers

**Files:**
- Modify: `go-libfossil/manifest/manifest.go:76-84` and `go-libfossil/manifest/manifest.go:225-228`
- Modify: `go-libfossil/tag/tag.go:108-111`
- Modify: `go-libfossil/sync/client.go:499-502`

- [ ] **Step 1: Remove unclustered insert for file blobs in Checkin**

In `manifest.go`, replace lines 76-84 (the file blob unclustered+unsent loop) with just the unsent loop:

```go
		// Mark file blobs as unsent so sync pushes them.
		// (unclustered is handled by blob.Store automatically.)
		for _, frid := range fileRids {
			if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", frid); err != nil {
				return fmt.Errorf("unsent file: %w", err)
			}
		}
```

- [ ] **Step 2: Remove unclustered insert for manifest blob in markLeafAndEvent**

In `manifest.go`, replace lines 225-231 (the unclustered+unsent block) with just unsent:

```go
	// unsent (unclustered is handled by blob.Store automatically)
	if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", manifestRid); err != nil {
		return fmt.Errorf("unsent: %w", err)
	}
```

- [ ] **Step 3: Remove unclustered insert from tag.AddTag**

In `tag.go`, replace lines 108-114 (the unclustered+unsent block) with just unsent:

```go
		// Mark as unsent (unclustered is handled by blob.Store automatically)
		if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", controlRid); err != nil {
			return fmt.Errorf("tag.AddTag: unsent: %w", err)
		}
```

- [ ] **Step 4: Remove exists-path unclustered re-insert from storeReceivedFile**

In `client.go`, replace lines 499-502 (the exists early-return with unclustered re-insert):

```go
	return r.WithTx(func(tx *db.Tx) error {
		if _, ok := blob.Exists(tx, uuid); ok {
			return nil
		}
```

The blob was already marked unclustered when first inserted. The re-insert was a no-op.

- [ ] **Step 5: Run all go-libfossil tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/... -v -count=1`
Expected: ALL PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/manifest/manifest.go go-libfossil/tag/tag.go go-libfossil/sync/client.go
git commit -m "refactor: remove redundant unclustered inserts from go-libfossil callers

blob.Store now handles unclustered automatically. Callers only need
to mark unsent for locally-created artifacts (matching Fossil's
db_add_unsent pattern)."
```

---

### Task 3: Remove redundant unclustered inserts from sim/DST callers

**Files:**
- Modify: `sim/seed.go:34-36`
- Modify: `dst/mock_fossil.go:67`
- Modify: `dst/e2e_test.go:105` and `dst/e2e_test.go:162`
- Modify: `dst/scenario_test.go:167`

- [ ] **Step 1: Remove unclustered insert from sim/seed.go**

In `seed.go`, replace lines 34-36 (unclustered insert) -- remove it entirely, keep only the unsent insert. The block becomes:

```go
			rid, uuid, err := blob.Store(tx, data)
			if err != nil {
				return fmt.Errorf("blob.Store: %w", err)
			}

			if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid); err != nil {
				return fmt.Errorf("insert unsent: %w", err)
			}
```

- [ ] **Step 2: Remove unclustered insert from dst/mock_fossil.go**

In `mock_fossil.go`, replace lines 61-68 (StoreArtifact transaction body). `blob.Store` now handles unclustered, so the explicit insert is redundant:

```go
	err := f.repo.WithTx(func(tx *db.Tx) error {
		_, u, err := blob.Store(tx, data)
		if err != nil {
			return err
		}
		uuid = u
		return nil
	})
```

Note: `MockFossil.StoreArtifact` does NOT need unsent -- it seeds the master server, which pushes via igot/file cards, not via unsent.

- [ ] **Step 3: Remove unclustered insert from dst/e2e_test.go (first site, ~line 105)**

Replace the transaction body in `TestE2EPushFromLeaf` (~lines 99-110):

```go
	err = leafRepo.WithTx(func(tx *db.Tx) error {
		rid, u, err := blob.Store(tx, data)
		if err != nil {
			return err
		}
		uuid = u
		_, err = tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid)
		return err
	})
```

- [ ] **Step 4: Remove unclustered insert from dst/e2e_test.go (second site, ~line 162)**

Replace the transaction body in `TestE2EBidirectional` (~lines 156-165):

```go
	leaf0Repo.WithTx(func(tx *db.Tx) error {
		rid, u, err := blob.Store(tx, leafData)
		if err != nil {
			return err
		}
		leafUUID = u
		tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid)
		return nil
	})
```

- [ ] **Step 5: Remove unclustered insert from dst/scenario_test.go (~line 167)**

Replace the transaction body (~lines 164-169):

```go
			r.WithTx(func(tx *db.Tx) error {
				rid, u, _ := blob.Store(tx, data)
				uuid = u
				tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid)
				return nil
			})
```

- [ ] **Step 6: Run sim and DST tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -v -count=1 && go test -buildvcs=false ./dst/ -v -count=1`
Expected: ALL PASS

- [ ] **Step 7: Commit**

```bash
git add sim/seed.go dst/mock_fossil.go dst/e2e_test.go dst/scenario_test.go
git commit -m "refactor: remove redundant unclustered inserts from sim/DST

blob.Store now handles unclustered automatically. Only unsent
inserts remain where needed (leaf push scenarios)."
```

---

### Task 4: Revert handler to single-pass wire-order card processing

**Files:**
- Modify: `go-libfossil/sync/handler.go:90-117`
- Test: `go-libfossil/sync/handler_test.go` (existing tests validate behavior)

- [ ] **Step 1: Revert to single-pass processing**

In `handler.go`, replace lines 96-117 (the three-pass file-then-remaining logic) with a single loop:

```go
	// Second pass: handle data cards in wire order (matches Fossil's page_xfer).
	for _, card := range req.Cards {
		if err := h.handleDataCard(card); err != nil {
			return nil, err
		}
	}
```

- [ ] **Step 2: Run handler tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/sync/ -run TestHandle -v`
Expected: ALL PASS

- [ ] **Step 3: Run full test suite (pre-commit equivalent)**

Run: `cd /Users/dmestas/projects/EdgeSync && make test`
Expected: ALL PASS

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/sync/handler.go
git commit -m "refactor: revert handler to single-pass wire-order card processing

Matches Fossil's page_xfer (xfer.c:1349). Igot and file cards
reference disjoint blob sets within a request, so wire-order
processing is safe."
```

---

### Task 5: Final verification

- [ ] **Step 1: Run make test (full CI suite)**

Run: `cd /Users/dmestas/projects/EdgeSync && make test`
Expected: ALL PASS

- [ ] **Step 2: Run DST with multiple seeds**

Run: `cd /Users/dmestas/projects/EdgeSync && make dst`
Expected: ALL PASS

- [ ] **Step 3: Verify no remaining redundant unclustered inserts**

Run: `cd /Users/dmestas/projects/EdgeSync && rg 'INSERT.*unclustered' --type go -l`

Expected files with unclustered inserts (these are correct):
- `go-libfossil/blob/blob.go` -- the new auto-mark in Store/StoreDelta
- `go-libfossil/sync/client.go` -- the new-blob raw SQL path in storeReceivedFile (bypasses blob.Store)
- `go-libfossil/blob/blob_test.go` -- test assertions
- `go-libfossil/sync/sync_test.go` -- test setup (manual blob seeding in test helpers)
- `dst/scenario_test.go:460` -- standalone test line that doesn't use blob.Store

Any other file with `INSERT.*unclustered` is a missed cleanup.
