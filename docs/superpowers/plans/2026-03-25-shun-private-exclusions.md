# Shun/Private Table Exclusions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Exclude shunned and private blobs from cluster generation, igot emission, and unclustered sync — matching Fossil's `create_cluster()` and `send_unclustered()` behavior.

**Architecture:** The `shun` and `private` tables already exist in the schema (`db/schema.go:49-54`). We add `NOT EXISTS` subqueries to 4 SQL queries across 2 files, plus helper functions to insert shun/private entries for testing. TDD throughout.

**Tech Stack:** Go, SQLite, go-libfossil test harness

**Linear:** CDG-166

**Branch:** `feature/cdg-166-add-shunprivate-table-exclusions-to-cluster-generation`

**Worktree:** `.worktrees/cdg-166-shun-private`

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `go-libfossil/content/cluster.go` | Modify | Add shun/private exclusions to count query, UUID query, and cleanup query |
| `go-libfossil/content/cluster_test.go` | Modify | Add tests for shun/private exclusion behavior |
| `go-libfossil/sync/client.go` | Modify | Add shun/private exclusions to `sendUnclustered()` and client `sendAllClusters()` |
| `go-libfossil/sync/handler.go` | Modify | Add shun/private exclusions to `emitIGots()` and handler `sendAllClusters()` |
| `go-libfossil/sync/handler_test.go` | Modify | Add tests for shun/private exclusion in handler |

---

## Task 1: Shunned blobs excluded from GenerateClusters

**Files:**
- Modify: `go-libfossil/content/cluster_test.go`
- Modify: `go-libfossil/content/cluster.go`

- [ ] **Step 1: Write the failing test**

Add to `cluster_test.go`:

```go
func TestGenerateClusters_ShunnedExcluded(t *testing.T) {
	d := setupTestDB(t)

	// Store 100 real blobs (at threshold).
	for i := 0; i < 100; i++ {
		_, uuid, err := blob.Store(d, []byte(fmt.Sprintf("blob-%04d-content-padding", i)))
		if err != nil {
			t.Fatalf("Store %d: %v", i, err)
		}
		// Shun the last 10 blobs.
		if i >= 90 {
			if _, err := d.Exec("INSERT INTO shun(uuid, mtime) VALUES(?, 0)", uuid); err != nil {
				t.Fatalf("shun %d: %v", i, err)
			}
		}
	}

	// 90 non-shunned blobs < 100 threshold -> no clusters.
	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 0 {
		t.Fatalf("clusters = %d, want 0 (90 non-shunned < 100 threshold)", n)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/content/ -run TestGenerateClusters_ShunnedExcluded -v`

Expected: FAIL — currently generates a cluster because shun exclusion is missing.

- [ ] **Step 3: Add shun exclusions to all 3 queries in GenerateClusters**

In `cluster.go`, update the count query (line 30-33):

```go
	var count int
	err := q.QueryRow(`
		SELECT count(*) FROM unclustered u
		WHERE NOT EXISTS (SELECT 1 FROM phantom WHERE rid = u.rid)
		  AND NOT EXISTS (SELECT 1 FROM shun WHERE uuid = (SELECT uuid FROM blob WHERE rid = u.rid))
		  AND NOT EXISTS (SELECT 1 FROM private WHERE rid = u.rid)
	`).Scan(&count)
```

Update the UUID query (line 42-47):

```go
	rows, err := q.Query(`
		SELECT b.uuid FROM unclustered u
		JOIN blob b ON b.rid = u.rid
		WHERE NOT EXISTS (SELECT 1 FROM phantom WHERE rid = u.rid)
		  AND NOT EXISTS (SELECT 1 FROM shun WHERE uuid = b.uuid)
		  AND NOT EXISTS (SELECT 1 FROM private WHERE rid = u.rid)
		ORDER BY b.uuid
	`)
```

Update the cleanup query (line 121):

```go
		query := fmt.Sprintf(
			"DELETE FROM unclustered WHERE rid NOT IN (%s) AND NOT EXISTS (SELECT 1 FROM phantom WHERE rid = unclustered.rid) AND NOT EXISTS (SELECT 1 FROM shun WHERE uuid = (SELECT uuid FROM blob WHERE rid = unclustered.rid)) AND NOT EXISTS (SELECT 1 FROM private WHERE rid = unclustered.rid)",
			placeholders,
		)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/content/ -run TestGenerateClusters_ShunnedExcluded -v`

Expected: PASS

- [ ] **Step 5: Run all content tests to check for regressions**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/content/ -v`

Expected: All existing tests still pass.

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/content/cluster.go go-libfossil/content/cluster_test.go
git commit -m "feat(content): exclude shunned blobs from cluster generation

Add NOT EXISTS(shun) and NOT EXISTS(private) to all 3 SQL queries in
GenerateClusters: count, UUID selection, and cleanup.

CDG-166"
```

---

## Task 2: Private blobs excluded from GenerateClusters

**Files:**
- Modify: `go-libfossil/content/cluster_test.go`

- [ ] **Step 1: Write the failing test**

Add to `cluster_test.go`:

```go
func TestGenerateClusters_PrivateExcluded(t *testing.T) {
	d := setupTestDB(t)

	// Store 100 real blobs (at threshold).
	var rids []libfossil.FslID
	for i := 0; i < 100; i++ {
		rid, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%04d-content-padding", i)))
		if err != nil {
			t.Fatalf("Store %d: %v", i, err)
		}
		rids = append(rids, rid)
	}

	// Mark last 10 as private.
	for _, rid := range rids[90:] {
		if _, err := d.Exec("INSERT INTO private(rid) VALUES(?)", rid); err != nil {
			t.Fatalf("private: %v", err)
		}
	}

	// 90 non-private blobs < 100 threshold -> no clusters.
	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 0 {
		t.Fatalf("clusters = %d, want 0 (90 non-private < 100 threshold)", n)
	}
}
```

- [ ] **Step 2: Run test to verify it passes (exclusions already added in Task 1)**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/content/ -run TestGenerateClusters_PrivateExcluded -v`

Expected: PASS — private exclusions were added alongside shun in Task 1.

- [ ] **Step 3: Write a test for private blobs staying in unclustered after cleanup**

Private blobs should NOT be removed from the `unclustered` table during cleanup (Fossil sets `markAsUnclustered = 0` for private).

```go
func TestGenerateClusters_PrivateStayUnclustered(t *testing.T) {
	d := setupTestDB(t)

	// Store 110 blobs, mark 10 as private.
	var privateRids []libfossil.FslID
	for i := 0; i < 110; i++ {
		rid, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%05d-content-padding-extra", i)))
		if err != nil {
			t.Fatalf("Store %d: %v", i, err)
		}
		if i >= 100 {
			if _, err := d.Exec("INSERT INTO private(rid) VALUES(?)", rid); err != nil {
				t.Fatalf("private: %v", err)
			}
			privateRids = append(privateRids, rid)
		}
	}

	// 100 non-private >= threshold -> should cluster.
	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 1 {
		t.Fatalf("clusters = %d, want 1", n)
	}

	// Private blobs should still be in unclustered.
	for _, rid := range privateRids {
		var count int
		d.QueryRow("SELECT count(*) FROM unclustered WHERE rid=?", rid).Scan(&count)
		if count != 1 {
			t.Fatalf("private rid=%d missing from unclustered", rid)
		}
	}
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/content/ -run TestGenerateClusters_PrivateStayUnclustered -v`

Expected: PASS — the cleanup query already excludes private blobs.

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/content/cluster_test.go
git commit -m "test(content): add private blob exclusion tests for GenerateClusters

Verify private blobs are excluded from cluster count/selection and
remain in the unclustered table after cleanup.

CDG-166"
```

---

## Task 3: Shun/private exclusions in handler emitIGots and client sendUnclustered

**Files:**
- Modify: `go-libfossil/sync/client.go:156-179`
- Modify: `go-libfossil/sync/handler.go:374-407`
- Modify: `go-libfossil/sync/handler_test.go`

- [ ] **Step 1: Write the failing test for emitIGots (handler-side)**

Add to `handler_test.go`. This tests that shunned/private blobs are excluded from the handler's igot response:

```go
func TestEmitIGots_ExcludesShunAndPrivate(t *testing.T) {
	r := setupSyncTestRepo(t)

	// Store 3 blobs.
	_, normalUUID, err := blob.Store(r.DB(), []byte("normal-blob-content"))
	if err != nil {
		t.Fatalf("Store normal: %v", err)
	}
	_, shunnedUUID, err := blob.Store(r.DB(), []byte("shunned-blob-content"))
	if err != nil {
		t.Fatalf("Store shunned: %v", err)
	}
	privRid, _, err := blob.Store(r.DB(), []byte("private-blob-content"))
	if err != nil {
		t.Fatalf("Store private: %v", err)
	}

	// Shun one, mark one private.
	if _, err := r.DB().Exec("INSERT INTO shun(uuid, mtime) VALUES(?, 0)", shunnedUUID); err != nil {
		t.Fatalf("shun: %v", err)
	}
	if _, err := r.DB().Exec("INSERT INTO private(rid) VALUES(?)", privRid); err != nil {
		t.Fatalf("private: %v", err)
	}

	// Pull request — handler responds with igots for all non-excluded blobs.
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Collect igot UUIDs from response.
	igotUUIDs := make(map[string]bool)
	for _, c := range resp.Cards {
		if ig, ok := c.(*xfer.IGotCard); ok {
			igotUUIDs[ig.UUID] = true
		}
	}

	// Normal blob should be present.
	if !igotUUIDs[normalUUID] {
		t.Error("normal blob missing from igots")
	}

	// Shunned blob must be absent.
	if igotUUIDs[shunnedUUID] {
		t.Error("shunned blob appeared in igots")
	}

	// Private blob must be absent.
	var privUUID string
	r.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", privRid).Scan(&privUUID)
	if igotUUIDs[privUUID] {
		t.Error("private blob appeared in igots")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/sync/ -run TestEmitIGots_ExcludesShunAndPrivate -v`

Expected: FAIL — shunned/private blobs currently appear in igots.

- [ ] **Step 3: Add exclusions to sendUnclustered in client.go**

Note: The client-side `sendUnclustered()` is an internal method on the session struct, not directly testable via `HandleSync`. The exclusions are added here for correctness; coverage comes from DST and integration tests in Task 5.

Update `sendUnclustered()` query at `client.go:157-160`:

```go
	rows, err := s.repo.DB().Query(`
		SELECT b.uuid FROM unclustered u JOIN blob b ON b.rid=u.rid
		WHERE b.size >= 0
		AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=u.rid)
		AND NOT EXISTS(SELECT 1 FROM shun WHERE uuid=b.uuid)
		AND NOT EXISTS(SELECT 1 FROM private WHERE rid=u.rid)`,
	)
```

- [ ] **Step 4: Add exclusions to emitIGots in handler.go**

Update `emitIGots()` query at `handler.go:378-380`:

```go
	rows, err := h.repo.DB().Query(`
		SELECT uuid FROM blob WHERE size >= 0
		AND NOT EXISTS(SELECT 1 FROM shun WHERE uuid=blob.uuid)
		AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)
	`)
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/sync/ -run TestEmitIGots_ExcludesShunAndPrivate -v`

Expected: PASS

- [ ] **Step 6: Run all sync tests for regressions**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/sync/ -v -timeout=120s`

Expected: All tests pass.

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/client.go go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "feat(sync): exclude shunned/private blobs from igot emission

Add NOT EXISTS(shun) and NOT EXISTS(private) to sendUnclustered()
(client) and emitIGots() (handler). Shunned and private blobs are
no longer advertised to sync peers.

CDG-166"
```

---

## Task 4: Shun/private exclusions in sendAllClusters (handler + client)

**Files:**
- Modify: `go-libfossil/sync/handler.go:411-433`
- Modify: `go-libfossil/sync/client.go:184+`

- [ ] **Step 1: Add exclusions to handler sendAllClusters**

Update `handler.go` `sendAllClusters()` query at line 412-418. The cluster blobs themselves won't be shunned/private, but the query should be consistent:

```go
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
```

- [ ] **Step 2: Add exclusions to client sendAllClusters**

Read `client.go` to find the client-side `sendAllClusters` query and add the same exclusions:

```go
	rows, err := s.repo.DB().Query(`
		SELECT b.uuid FROM tagxref tx JOIN blob b ON tx.rid=b.rid
		WHERE tx.tagid=7
		  AND NOT EXISTS(SELECT 1 FROM unclustered WHERE rid=b.rid)
		  AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=b.rid)
		  AND NOT EXISTS(SELECT 1 FROM shun WHERE uuid=b.uuid)
		  AND NOT EXISTS(SELECT 1 FROM private WHERE rid=b.rid)
		  AND b.size >= 0`,
	)
```

- [ ] **Step 3: Run all sync tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./go-libfossil/sync/ -v -timeout=120s`

Expected: All tests pass.

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/sync/handler.go go-libfossil/sync/client.go
git commit -m "feat(sync): exclude shunned/private blobs from cluster advertisement

Add NOT EXISTS(shun) and NOT EXISTS(private) to sendAllClusters()
in both handler and client.

CDG-166"
```

---

## Task 5: Full test suite + DST verification

**Files:** None modified — verification only.

- [ ] **Step 1: Run full make test**

Run: `cd /Users/dmestas/projects/EdgeSync && make test`

Expected: All tests pass (~15s).

- [ ] **Step 2: Run DST suite**

Run: `cd /Users/dmestas/projects/EdgeSync && make dst`

Expected: All DST scenarios pass.

- [ ] **Step 3: Verify no regressions in sim serve tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run 'TestServeHTTP|TestLeafToLeaf|TestAgentServe' -count=1 -timeout=120s -v`

Expected: All serve tests pass.
