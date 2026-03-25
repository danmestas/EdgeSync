# Cluster Manifest Generation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Fossil's cluster manifest protocol to batch igot cards, reducing wire overhead for repos with hundreds+ of blobs.

**Architecture:** Three layers — `content.GenerateClusters()` creates cluster artifacts, `manifest.crosslinkCluster()` processes received/generated clusters, and `sync/` client+handler integrate clustering into the sync protocol with round-aware logic and `pragma req-clusters`.

**Tech Stack:** Go, SQLite, crypto/md5 (Z-card checksums), existing deck.Marshal for cluster artifact format.

---

## File Structure

| File | Responsibility |
|------|---------------|
| `go-libfossil/content/cluster.go` | `GenerateClusters()` — batch unclustered blobs into cluster artifacts |
| `go-libfossil/content/cluster_test.go` | Unit tests for cluster generation |
| `go-libfossil/manifest/crosslink.go` | Add `crosslinkCluster()`, wire into `Crosslink()` dispatch |
| `go-libfossil/manifest/crosslink_test.go` | New file — cluster crosslink tests |
| `go-libfossil/sync/client.go` | Refactor `buildIGotCards()` → `sendUnclustered()` + `sendAllClusters()`, add round-aware logic |
| `go-libfossil/sync/handler.go` | Change `emitIGots()` to query unclustered, add `sendAllClusters()`, handle `pragma req-clusters` |
| `go-libfossil/sync/client_test.go` | New file — tests for client cluster functions |
| `go-libfossil/sync/handler_test.go` | New file — tests for handler cluster functions |
| `dst/scenario_test.go` | New scenario: large-blob cluster convergence |

---

### Task 1: Cluster Generation — `content.GenerateClusters()`

**Files:**
- Create: `go-libfossil/content/cluster.go`
- Test: `go-libfossil/content/cluster_test.go`
- Read: `go-libfossil/content/content.go` (for `setupTestDB` pattern in tests)
- Read: `go-libfossil/deck/marshal.go` (for `Deck.Marshal()` — builds cluster artifact with M-cards + Z-card)
- Read: `go-libfossil/blob/blob.go:12-58` (for `blob.Store()` — stores cluster, marks unclustered)
- Read: `go-libfossil/manifest/crosslink.go:17-114` (for `Crosslink()` — called after storing cluster)
- Read: `fossil/src/xfer.c:914-973` (reference: Fossil's `create_cluster()`)

**Context:** `deck.Marshal()` already handles M-cards and computes an MD5 Z-card checksum. `blob.Store()` inserts into `unclustered` automatically. `Crosslink()` processes the cluster (tags it, removes members from unclustered). So `GenerateClusters` just needs to: count unclustered, build Deck with M-cards, marshal, store, crosslink.

- [ ] **Step 1: Write failing test — below threshold produces no clusters**

In `go-libfossil/content/cluster_test.go`:

```go
package content

import (
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func TestGenerateClusters_BelowThreshold(t *testing.T) {
	d := setupTestDB(t)

	// Store 99 blobs — below the 100 threshold.
	for i := range 99 {
		_, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 0 {
		t.Fatalf("expected 0 clusters, got %d", n)
	}

	// Verify unclustered count unchanged.
	var count int
	if err := d.QueryRow("SELECT count(*) FROM unclustered").Scan(&count); err != nil {
		t.Fatalf("count unclustered: %v", err)
	}
	if count != 99 {
		t.Fatalf("expected 99 unclustered, got %d", count)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_BelowThreshold -v`
Expected: FAIL — `GenerateClusters` not defined.

- [ ] **Step 3: Write minimal implementation**

In `go-libfossil/content/cluster.go`:

```go
package content

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

const (
	// ClusterThreshold is the minimum unclustered count before generating clusters.
	// Matches Fossil's create_cluster() threshold of 100.
	ClusterThreshold = 100

	// ClusterMaxSize is the maximum number of M-cards per cluster artifact.
	// Matches Fossil's limit of 800 entries per cluster.
	ClusterMaxSize = 800
)

// GenerateClusters batches unclustered non-phantom blobs into cluster
// manifests. Each cluster is a type-B artifact with M-cards listing member
// UUIDs, stored as a blob and crosslinked. Returns the number of clusters
// created. Mirrors Fossil's create_cluster() in src/xfer.c:914-973.
func GenerateClusters(q db.Querier) (int, error) {
	if q == nil {
		panic("content.GenerateClusters: q must not be nil")
	}

	// Count non-phantom unclustered entries.
	var nUncl int
	if err := q.QueryRow(`
		SELECT count(*) FROM unclustered
		WHERE NOT EXISTS(SELECT 1 FROM phantom WHERE rid=unclustered.rid)
	`).Scan(&nUncl); err != nil {
		return 0, fmt.Errorf("content.GenerateClusters count: %w", err)
	}
	if nUncl < ClusterThreshold {
		return 0, nil
	}

	// Query all non-phantom unclustered UUIDs, sorted for deterministic output.
	rows, err := q.Query(`
		SELECT b.uuid FROM unclustered u JOIN blob b ON b.rid=u.rid
		WHERE NOT EXISTS(SELECT 1 FROM phantom WHERE rid=u.rid)
		ORDER BY b.uuid
	`)
	if err != nil {
		return 0, fmt.Errorf("content.GenerateClusters query: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return 0, fmt.Errorf("content.GenerateClusters scan: %w", err)
		}
		uuids = append(uuids, uuid)
	}
	if err := rows.Err(); err != nil {
		return 0, fmt.Errorf("content.GenerateClusters rows: %w", err)
	}

	// Build cluster artifacts in batches of ClusterMaxSize.
	var clusterRIDs []int64
	nClusters := 0
	nRemaining := len(uuids)

	for len(uuids) > 0 {
		batchSize := ClusterMaxSize
		if batchSize > len(uuids) {
			batchSize = len(uuids)
		}
		// Only split if there are >ClusterThreshold entries remaining after this batch.
		if len(uuids)-batchSize > 0 && len(uuids)-batchSize < ClusterThreshold {
			batchSize = len(uuids) // take all remaining
		}

		batch := uuids[:batchSize]
		uuids = uuids[batchSize:]
		nRemaining = len(uuids)
		_ = nRemaining

		// Build cluster deck with M-cards.
		d := &deck.Deck{
			Type: deck.Cluster,
			M:    batch,
		}
		manifestBytes, err := d.Marshal()
		if err != nil {
			return nClusters, fmt.Errorf("content.GenerateClusters marshal: %w", err)
		}

		// Store cluster as blob (automatically marks it unclustered).
		rid, _, err := blob.Store(q, manifestBytes)
		if err != nil {
			return nClusters, fmt.Errorf("content.GenerateClusters store: %w", err)
		}
		clusterRIDs = append(clusterRIDs, int64(rid))

		// Crosslink: tags cluster with tagid=7, removes M-card members from unclustered.
		// We need a repo.Repo for Crosslink. Use CrosslinkCluster directly instead.
		if err := manifest.CrosslinkCluster(q, rid, d); err != nil {
			return nClusters, fmt.Errorf("content.GenerateClusters crosslink: %w", err)
		}

		nClusters++
	}

	// Delete all non-phantom entries from unclustered EXCEPT the cluster artifacts
	// themselves. Cluster artifacts stay in unclustered so they get announced via
	// sendUnclustered() as individual igots. Future GenerateClusters calls will
	// batch them into meta-clusters.
	if len(clusterRIDs) > 0 {
		// Build placeholder string: ?,?,?
		placeholders := "?"
		for i := 1; i < len(clusterRIDs); i++ {
			placeholders += ",?"
		}
		args := make([]any, len(clusterRIDs))
		for i, rid := range clusterRIDs {
			args[i] = rid
		}
		query := fmt.Sprintf(`
			DELETE FROM unclustered
			WHERE rid NOT IN (%s)
			AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=unclustered.rid)
		`, placeholders)
		if _, err := q.Exec(query, args...); err != nil {
			return nClusters, fmt.Errorf("content.GenerateClusters delete: %w", err)
		}
	}

	return nClusters, nil
}
```

Note: This references `manifest.CrosslinkCluster` which doesn't exist yet. The below-threshold test passes because it returns early before calling crosslink. The at-threshold test (step 5) will fail to compile — that's the signal to **pause Task 1 and complete Task 2 first**, then return to Task 1 step 7.

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_BelowThreshold -v`
Expected: PASS

- [ ] **Step 5: Write failing test — at threshold generates one cluster**

Add to `go-libfossil/content/cluster_test.go`:

```go
func TestGenerateClusters_AtThreshold(t *testing.T) {
	d := setupTestDB(t)

	// Store exactly 100 blobs.
	for i := range 100 {
		_, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 1 {
		t.Fatalf("expected 1 cluster, got %d", n)
	}

	// Verify: only the cluster artifact itself remains in unclustered.
	var unclCount int
	if err := d.QueryRow("SELECT count(*) FROM unclustered").Scan(&unclCount); err != nil {
		t.Fatalf("count unclustered: %v", err)
	}
	if unclCount != 1 {
		t.Fatalf("expected 1 unclustered (cluster itself), got %d", unclCount)
	}

	// Verify: cluster is tagged with tagid=7.
	var tagCount int
	if err := d.QueryRow("SELECT count(*) FROM tagxref WHERE tagid=7").Scan(&tagCount); err != nil {
		t.Fatalf("count tagxref: %v", err)
	}
	if tagCount != 1 {
		t.Fatalf("expected 1 cluster tag, got %d", tagCount)
	}
}
```

- [ ] **Step 6: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_AtThreshold -v`
Expected: FAIL — `manifest.CrosslinkCluster` not defined (compile error).

This is the signal to implement Task 2 (crosslink) before continuing. **Pause Task 1 here, complete Task 2, then return.**

- [ ] **Step 7: Return from Task 2 — run at-threshold test again**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_AtThreshold -v`
Expected: PASS

- [ ] **Step 8: Write test — large count produces multiple clusters**

Add to `go-libfossil/content/cluster_test.go`:

```go
func TestGenerateClusters_MultipleClusters(t *testing.T) {
	d := setupTestDB(t)

	// Store 2000 blobs → expect 3 clusters (800 + 800 + 400).
	for i := range 2000 {
		_, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%05d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 3 {
		t.Fatalf("expected 3 clusters, got %d", n)
	}

	// Only the 3 cluster artifacts remain in unclustered.
	var unclCount int
	if err := d.QueryRow("SELECT count(*) FROM unclustered").Scan(&unclCount); err != nil {
		t.Fatalf("count unclustered: %v", err)
	}
	if unclCount != 3 {
		t.Fatalf("expected 3 unclustered (clusters themselves), got %d", unclCount)
	}
}
```

- [ ] **Step 9: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_MultipleClusters -v`
Expected: PASS

- [ ] **Step 10: Write test — idempotent (calling twice with no new blobs is no-op)**

```go
func TestGenerateClusters_Idempotent(t *testing.T) {
	d := setupTestDB(t)

	for i := range 200 {
		_, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	n1, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("first GenerateClusters: %v", err)
	}
	if n1 != 1 {
		t.Fatalf("expected 1 cluster first call, got %d", n1)
	}

	// Second call: only cluster artifacts remain in unclustered (< threshold).
	n2, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("second GenerateClusters: %v", err)
	}
	if n2 != 0 {
		t.Fatalf("expected 0 clusters second call, got %d", n2)
	}
}
```

- [ ] **Step 11: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_Idempotent -v`
Expected: PASS

- [ ] **Step 12: Write test — phantoms excluded from clusters**

```go
func TestGenerateClusters_PhantomsExcluded(t *testing.T) {
	d := setupTestDB(t)

	// Store 80 real blobs.
	for i := range 80 {
		_, _, err := blob.Store(d, []byte(fmt.Sprintf("real-blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	// Store 30 phantoms (size=-1, in phantom table).
	for i := range 30 {
		uuid := fmt.Sprintf("%040x", i+10000)
		_, err := blob.StorePhantom(d, uuid)
		if err != nil {
			t.Fatalf("StorePhantom %d: %v", i, err)
		}
		// Manually add to unclustered (phantoms aren't normally unclustered,
		// but test the exclusion logic).
		d.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES((SELECT rid FROM blob WHERE uuid=?))", uuid)
	}

	// 80 real + 30 phantom-in-unclustered = 110 unclustered total.
	// But only 80 non-phantom → below threshold.
	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 0 {
		t.Fatalf("expected 0 clusters (80 non-phantom < 100), got %d", n)
	}
}
```

- [ ] **Step 13: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_PhantomsExcluded -v`
Expected: PASS

- [ ] **Step 14: Write test — cluster artifact format is valid (parseable, correct Z-card)**

```go
func TestGenerateClusters_ValidArtifactFormat(t *testing.T) {
	d := setupTestDB(t)

	for i := range 100 {
		_, _, err := blob.Store(d, []byte(fmt.Sprintf("blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	n, err := GenerateClusters(d)
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 1 {
		t.Fatalf("expected 1 cluster, got %d", n)
	}

	// Find the cluster blob (tagged with tagid=7).
	var clusterRID int64
	if err := d.QueryRow("SELECT rid FROM tagxref WHERE tagid=7").Scan(&clusterRID); err != nil {
		t.Fatalf("find cluster rid: %v", err)
	}

	// Expand and parse the cluster artifact.
	data, err := Expand(d, libfossil.FslID(clusterRID))
	if err != nil {
		t.Fatalf("Expand cluster: %v", err)
	}

	parsed, err := deck.Parse(data)
	if err != nil {
		t.Fatalf("Parse cluster: %v", err)
	}
	if parsed.Type != deck.Cluster {
		t.Fatalf("expected Cluster type, got %d", parsed.Type)
	}
	if len(parsed.M) != 100 {
		t.Fatalf("expected 100 M-cards, got %d", len(parsed.M))
	}

	// Verify M-cards are sorted.
	for i := 1; i < len(parsed.M); i++ {
		if parsed.M[i] < parsed.M[i-1] {
			t.Fatalf("M-cards not sorted: %s > %s", parsed.M[i-1], parsed.M[i])
		}
	}
}
```

This test needs these additional imports in cluster_test.go:
```go
import (
	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/deck"
)
```

- [ ] **Step 15: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ -run TestGenerateClusters_ValidArtifactFormat -v`
Expected: PASS

- [ ] **Step 16: Commit**

```bash
git add go-libfossil/content/cluster.go go-libfossil/content/cluster_test.go
git commit -m "feat(content): implement GenerateClusters for igot batching (CDG-165)"
```

---

### Task 2: Crosslink Cluster Processing

**Files:**
- Modify: `go-libfossil/manifest/crosslink.go:60-63` (add Cluster case to type dispatch)
- Create: `go-libfossil/manifest/crosslink_test.go`
- Read: `go-libfossil/blob/blob.go:120-157` (for `blob.StorePhantom()` — creates phantoms for unknown M-card UUIDs)
- Read: `go-libfossil/tag/tag.go:141-183` (for `tag.ApplyTag()` — applies cluster tag)
- Read: `fossil/src/manifest.c:2431-2444` (reference: Fossil's cluster crosslink)

**Context:** The existing `Crosslink()` function dispatches by artifact type. Currently it only handles Checkin and Control. We add a Cluster case. The new `CrosslinkCluster()` function is also exported so `content.GenerateClusters()` can call it directly (avoiding a full `Crosslink()` scan).

- [ ] **Step 1: Write failing test — crosslink cluster removes members from unclustered**

In `go-libfossil/manifest/crosslink_test.go`:

```go
package manifest

import (
	"fmt"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/deck"
)

func setupTestDB(t *testing.T) *db.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := db.Open(path)
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	if err := db.CreateRepoSchema(d); err != nil {
		t.Fatalf("CreateRepoSchema: %v", err)
	}
	t.Cleanup(func() { d.Close() })
	return d
}

func TestCrosslinkCluster_RemovesFromUnclustered(t *testing.T) {
	d := setupTestDB(t)

	// Store 5 blobs — they'll be in unclustered.
	var memberUUIDs []string
	for i := range 5 {
		_, uuid, err := blob.Store(d, []byte(fmt.Sprintf("member-blob-%04d-uniqueness-pad", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
		memberUUIDs = append(memberUUIDs, uuid)
	}

	// Verify all 5 are unclustered.
	var before int
	d.QueryRow("SELECT count(*) FROM unclustered").Scan(&before)
	if before != 5 {
		t.Fatalf("expected 5 unclustered before, got %d", before)
	}

	// Build and crosslink a cluster deck referencing these UUIDs.
	clusterDeck := &deck.Deck{Type: deck.Cluster, M: memberUUIDs}
	clusterBytes, err := clusterDeck.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	clusterRID, _, err := blob.Store(d, clusterBytes)
	if err != nil {
		t.Fatalf("Store cluster: %v", err)
	}

	if err := CrosslinkCluster(d, clusterRID, clusterDeck); err != nil {
		t.Fatalf("CrosslinkCluster: %v", err)
	}

	// Members should be removed from unclustered. Only the cluster artifact remains.
	var after int
	d.QueryRow("SELECT count(*) FROM unclustered").Scan(&after)
	if after != 1 {
		t.Fatalf("expected 1 unclustered (cluster itself), got %d", after)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -run TestCrosslinkCluster_RemovesFromUnclustered -v`
Expected: FAIL — `CrosslinkCluster` not defined.

- [ ] **Step 3: Implement `CrosslinkCluster` and wire into `Crosslink()`**

Add to `go-libfossil/manifest/crosslink.go`, after the existing imports add `"github.com/dmestas/edgesync/go-libfossil/blob"`:

```go
// CrosslinkCluster processes a cluster artifact: tags it with cluster (tagid=7)
// and removes M-card member blobs from the unclustered table. Unknown UUIDs
// create phantoms. Mirrors Fossil's manifest_crosslink for CFTYPE_CLUSTER
// (manifest.c:2431-2444).
func CrosslinkCluster(q db.Querier, rid libfossil.FslID, d *deck.Deck) error {
	if q == nil {
		panic("manifest.CrosslinkCluster: q must not be nil")
	}
	if rid <= 0 {
		panic("manifest.CrosslinkCluster: rid must be > 0")
	}
	if d == nil {
		panic("manifest.CrosslinkCluster: d must not be nil")
	}
	if d.Type != deck.Cluster {
		panic("manifest.CrosslinkCluster: d.Type must be Cluster")
	}

	// Apply cluster singleton tag (tagid=7, pre-seeded in schema).
	if _, err := q.Exec(
		`INSERT OR REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid)
		 VALUES(7, 1, ?, ?, NULL, 0, ?)`,
		rid, rid, rid,
	); err != nil {
		return fmt.Errorf("manifest.CrosslinkCluster tag: %w", err)
	}

	// For each M-card UUID: resolve to rid, delete from unclustered.
	// Unknown UUIDs create phantoms.
	for _, uuid := range d.M {
		memberRID, exists := blob.Exists(q, uuid)
		if exists {
			if _, err := q.Exec("DELETE FROM unclustered WHERE rid=?", memberRID); err != nil {
				return fmt.Errorf("manifest.CrosslinkCluster delete unclustered rid=%d: %w", memberRID, err)
			}
		} else {
			// Create phantom for unknown UUID.
			if _, err := blob.StorePhantom(q, uuid); err != nil {
				return fmt.Errorf("manifest.CrosslinkCluster phantom %s: %w", uuid, err)
			}
		}
	}

	return nil
}
```

Also modify the first-pass loop in `Crosslink()` at line 61-63 to handle Cluster type:

In `go-libfossil/manifest/crosslink.go`, change:
```go
		if d.Type != deck.Checkin {
			continue // only crosslink checkin manifests
		}
```
to:
```go
		if d.Type == deck.Cluster {
			if err := CrosslinkCluster(r.DB(), c.rid, d); err != nil {
				return linked, fmt.Errorf("manifest.Crosslink cluster rid=%d: %w", c.rid, err)
			}
			linked++
			continue
		}
		if d.Type != deck.Checkin {
			continue // only crosslink checkin manifests
		}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -run TestCrosslinkCluster_RemovesFromUnclustered -v`
Expected: PASS

- [ ] **Step 5: Write test — unknown M-card UUIDs create phantoms**

Add to `go-libfossil/manifest/crosslink_test.go`:

```go
func TestCrosslinkCluster_CreatesPhantoms(t *testing.T) {
	d := setupTestDB(t)

	// Create a cluster referencing UUIDs that don't exist as blobs.
	unknownUUIDs := []string{
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
	}

	clusterDeck := &deck.Deck{Type: deck.Cluster, M: unknownUUIDs}
	clusterBytes, err := clusterDeck.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	clusterRID, _, err := blob.Store(d, clusterBytes)
	if err != nil {
		t.Fatalf("Store cluster: %v", err)
	}

	if err := CrosslinkCluster(d, clusterRID, clusterDeck); err != nil {
		t.Fatalf("CrosslinkCluster: %v", err)
	}

	// Verify phantoms were created.
	var phantomCount int
	d.QueryRow("SELECT count(*) FROM phantom").Scan(&phantomCount)
	if phantomCount != 2 {
		t.Fatalf("expected 2 phantoms, got %d", phantomCount)
	}

	// Verify the phantom blobs exist with size=-1.
	for _, uuid := range unknownUUIDs {
		var size int
		err := d.QueryRow("SELECT size FROM blob WHERE uuid=?", uuid).Scan(&size)
		if err != nil {
			t.Fatalf("phantom blob %s not found: %v", uuid, err)
		}
		if size != -1 {
			t.Fatalf("expected phantom size=-1, got %d", size)
		}
	}
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -run TestCrosslinkCluster_CreatesPhantoms -v`
Expected: PASS

- [ ] **Step 7: Write test — cluster is tagged with tagid=7**

```go
func TestCrosslinkCluster_TaggedWithCluster(t *testing.T) {
	d := setupTestDB(t)

	clusterDeck := &deck.Deck{
		Type: deck.Cluster,
		M:    []string{"cccccccccccccccccccccccccccccccccccccccc"},
	}
	clusterBytes, err := clusterDeck.Marshal()
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	clusterRID, _, err := blob.Store(d, clusterBytes)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	if err := CrosslinkCluster(d, clusterRID, clusterDeck); err != nil {
		t.Fatalf("CrosslinkCluster: %v", err)
	}

	var tagType int
	err = d.QueryRow("SELECT tagtype FROM tagxref WHERE tagid=7 AND rid=?", clusterRID).Scan(&tagType)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagType != 1 {
		t.Fatalf("expected singleton tagtype=1, got %d", tagType)
	}
}
```

- [ ] **Step 8: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./manifest/ -run TestCrosslinkCluster_TaggedWithCluster -v`
Expected: PASS

- [ ] **Step 9: Run all content + manifest tests**

Run: `cd go-libfossil && go test -buildvcs=false ./content/ ./manifest/ -v`
Expected: ALL PASS

- [ ] **Step 10: Commit**

```bash
git add go-libfossil/manifest/crosslink.go go-libfossil/manifest/crosslink_test.go
git commit -m "feat(manifest): add CrosslinkCluster for cluster artifact processing (CDG-165)"
```

Now return to Task 1 step 7 to run the remaining content tests.

---

### Task 3: Handler — Change `emitIGots()` to Query Unclustered + Add `pragma req-clusters`

**Files:**
- Modify: `go-libfossil/sync/handler.go:77-88` (add handler fields), `handler.go:135-140` (call GenerateClusters before emitIGots), `handler.go:152-174` (add pragma req-clusters dispatch), `handler.go:284-312` (rewrite emitIGots)
- Create: `go-libfossil/sync/handler_test.go`
- Read: `go-libfossil/sync/handler_uv.go:11-28` (pattern for pragma handling)
- Read: `go-libfossil/content/cluster.go` (GenerateClusters)

**Context:** The handler currently queries all blobs for igot emission. We change it to query unclustered, call GenerateClusters before emitting, and handle `pragma req-clusters` to send cluster igots on demand. The import for `content` already exists in handler.go.

- [ ] **Step 1: Write failing test — emitIGots only sends unclustered blobs**

In `go-libfossil/sync/handler_test.go`:

```go
package sync

import (
	"context"
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"

	"path/filepath"
)

func setupHandlerTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path)
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestEmitIGots_OnlyUnclustered(t *testing.T) {
	r := setupHandlerTestRepo(t)

	// Store 200 blobs and cluster them.
	for i := range 200 {
		_, _, err := blob.Store(r.DB(), []byte(fmt.Sprintf("blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}

	n, err := content.GenerateClusters(r.DB())
	if err != nil {
		t.Fatalf("GenerateClusters: %v", err)
	}
	if n != 1 {
		t.Fatalf("expected 1 cluster, got %d", n)
	}

	// Build a pull request and process it.
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test-server", ProjectCode: "test-project"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Count igot cards in response — should be 1 (just the cluster artifact
	// that's still in unclustered), NOT 201 (200 blobs + 1 cluster).
	igots := cardsByType(resp, xfer.CardIGot)
	if len(igots) > 5 {
		t.Fatalf("expected few igots (unclustered only), got %d — emitIGots may still be querying all blobs", len(igots))
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestEmitIGots_OnlyUnclustered -v`
Expected: FAIL — handler still queries all blobs, returns 201 igots.

- [ ] **Step 3: Modify `emitIGots()` to query unclustered**

In `go-libfossil/sync/handler.go`, replace `emitIGots()` (lines 284-312):

```go
func (h *handler) emitIGots() error {
	// Generate clusters first — batches unclustered blobs into cluster artifacts.
	if _, err := content.GenerateClusters(h.repo.DB()); err != nil {
		return fmt.Errorf("handler: generate clusters: %w", err)
	}

	rows, err := h.repo.DB().Query(`
		SELECT b.uuid FROM unclustered u JOIN blob b ON b.rid=u.rid
		WHERE b.size >= 0
		AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=u.rid)
	`)
	if err != nil {
		return fmt.Errorf("handler: listing unclustered: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
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
	return nil
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestEmitIGots_OnlyUnclustered -v`
Expected: PASS

- [ ] **Step 5: Write failing test — pragma req-clusters sends cluster igots**

Add to `go-libfossil/sync/handler_test.go`:

```go
func TestPragmaReqClusters(t *testing.T) {
	r := setupHandlerTestRepo(t)

	// Store 200 blobs and cluster them.
	for i := range 200 {
		_, _, err := blob.Store(r.DB(), []byte(fmt.Sprintf("blob-%04d-padding-for-uniqueness", i)))
		if err != nil {
			t.Fatalf("Store blob %d: %v", i, err)
		}
	}
	n, _ := content.GenerateClusters(r.DB())
	if n != 1 {
		t.Fatalf("expected 1 cluster, got %d", n)
	}

	// Send pragma req-clusters.
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "req-clusters"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Response should include igots for cluster artifacts
	// (from sendAllClusters) PLUS unclustered igots (from emitIGots).
	igots := cardsByType(resp, xfer.CardIGot)
	// After clustering 200 blobs: 1 cluster artifact in unclustered (from emitIGots),
	// but sendAllClusters sends clusters NOT in unclustered.
	// Since the cluster was JUST created, it IS in unclustered → sendAllClusters
	// should NOT include it (it's excluded). emitIGots will send it.
	// So we expect exactly 1 igot (the cluster, from emitIGots).
	if len(igots) != 1 {
		t.Fatalf("expected 1 igot, got %d", len(igots))
	}
}
```

- [ ] **Step 6: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestPragmaReqClusters -v`
Expected: FAIL — pragma req-clusters not handled.

- [ ] **Step 7: Implement `sendAllClusters()` and `pragma req-clusters` dispatch**

Add `sendAllClusters` method to handler in `go-libfossil/sync/handler.go`:

```go
// sendAllClusters emits igot cards for cluster artifacts that are NOT in the
// unclustered table (i.e., clusters from prior runs that have been announced
// and clustered themselves). Mirrors Fossil's send_all_clusters (xfer.c:1052-1078).
func (h *handler) sendAllClusters() error {
	rows, err := h.repo.DB().Query(`
		SELECT b.uuid
		FROM tagxref tx JOIN blob b ON tx.rid=b.rid
		WHERE tx.tagid=7
		AND NOT EXISTS(SELECT 1 FROM unclustered WHERE rid=b.rid)
		AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=b.rid)
		AND b.size >= 0
	`)
	if err != nil {
		return fmt.Errorf("handler: sendAllClusters: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}
	return rows.Err()
}
```

In `handleControlCard()` (handler.go:152-174), add a case for `pragma req-clusters` after the `uv-hash` case:

Change:
```go
		// Acknowledge client-version, ignore other unknown pragmas.
```
to:
```go
		if c.Name == "req-clusters" {
			h.reqClusters = true
		}
		// Acknowledge client-version, ignore other unknown pragmas.
```

Add `reqClusters bool` field to the `handler` struct (handler.go:77-88):

```go
	reqClusters   bool // client sent pragma req-clusters
```

In the `process()` method (handler.go:135-140), after the pullOK/emitIGots block, add:

```go
	// If client requested clusters, send cluster igots.
	if h.reqClusters {
		if err := h.sendAllClusters(); err != nil {
			return nil, err
		}
	}
```

- [ ] **Step 8: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestPragmaReqClusters -v`
Expected: PASS

- [ ] **Step 9: Run all sync tests**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -v`
Expected: ALL PASS (existing tests unaffected since they have <100 blobs).

- [ ] **Step 10: Commit**

```bash
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "feat(sync): handler emits unclustered igots + pragma req-clusters (CDG-165)"
```

---

### Task 4: Client — `sendUnclustered()` + `sendAllClusters()` + Round-Aware Logic

**Files:**
- Modify: `go-libfossil/sync/client.go:56-63` (replace buildIGotCards call with round-aware logic), `client.go:125-148` (rename to sendUnclustered, add phantom exclusion)
- Modify: `go-libfossil/sync/session.go:49-70` (add `nGimmeRcvd int` field)
- Create: `go-libfossil/sync/client_test.go`

**Context:** The client currently calls `buildIGotCards()` every round. We refactor into `sendUnclustered()` (every round) + `sendAllClusters()` (round 2+ when pushing and cumulative gimmes > 0) + `pragma req-clusters` (round 2 when pulling).

- [ ] **Step 1: Refactor `buildIGotCards` → `sendUnclustered` with phantom exclusion**

In `go-libfossil/sync/client.go`, rename `buildIGotCards` and update the query:

```go
// sendUnclustered queries the unclustered table and produces igot cards
// for non-phantom artifacts the remote doesn't already have.
// Mirrors Fossil's send_unclustered (xfer.c:1007-1045).
func (s *session) sendUnclustered() ([]xfer.Card, error) {
	rows, err := s.repo.DB().Query(`
		SELECT b.uuid FROM unclustered u JOIN blob b ON b.rid=u.rid
		WHERE b.size >= 0
		AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=u.rid)
	`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cards []xfer.Card
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		if s.remoteHas[uuid] {
			continue
		}
		cards = append(cards, &xfer.IGotCard{UUID: uuid})
	}
	return cards, rows.Err()
}
```

Update the call site in `buildRequest` (line 57) from `s.buildIGotCards()` to `s.sendUnclustered()`.

- [ ] **Step 2: Add `sendAllClusters` to client**

Add to `go-libfossil/sync/client.go`:

```go
// sendAllClusters produces igot cards for cluster artifacts that are NOT in
// the unclustered table (established clusters from prior runs).
// Mirrors Fossil's send_all_clusters (xfer.c:1052-1078).
func (s *session) sendAllClusters() ([]xfer.Card, error) {
	rows, err := s.repo.DB().Query(`
		SELECT b.uuid
		FROM tagxref tx JOIN blob b ON tx.rid=b.rid
		WHERE tx.tagid=7
		AND NOT EXISTS(SELECT 1 FROM unclustered WHERE rid=b.rid)
		AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=b.rid)
		AND b.size >= 0
	`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cards []xfer.Card
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		if s.remoteHas[uuid] {
			continue
		}
		cards = append(cards, &xfer.IGotCard{UUID: uuid})
	}
	return cards, rows.Err()
}
```

- [ ] **Step 3: Add `nGimmeRcvd` counter to session and track it**

In `go-libfossil/sync/session.go`, add field to `session` struct (after line 61):

```go
	nGimmeRcvd          int // cumulative gimmes received across all rounds
```

In `go-libfossil/sync/client.go`, in `processResponse` where GimmeCard is handled (line 335-337), add counter increment:

```go
		case *xfer.GimmeCard:
			s.pendingSend[c.UUID] = true
			s.nGimmeRcvd++
			filesSent++
```

- [ ] **Step 4: Add round-aware logic to `buildRequest`**

In `go-libfossil/sync/client.go`, replace the igot card block (lines 56-63):

```go
	// 4. IGot cards: sendUnclustered every round.
	igotCards, err := s.sendUnclustered()
	if err != nil {
		return nil, fmt.Errorf("buildRequest igot: %w", err)
	}
	s.igotSentThisRound = len(igotCards)
	s.roundStats.IgotsSent = len(igotCards)
	cards = append(cards, igotCards...)

	// 4b. Send cluster igots on round 2+ when pushing and remote has requested blobs.
	if cycle >= 1 && s.opts.Push && s.nGimmeRcvd > 0 {
		clusterCards, err := s.sendAllClusters()
		if err != nil {
			return nil, fmt.Errorf("buildRequest clusters: %w", err)
		}
		s.igotSentThisRound += len(clusterCards)
		s.roundStats.IgotsSent += len(clusterCards)
		cards = append(cards, clusterCards...)
	}

	// 4c. Request cluster catalog on round 2 when pulling.
	// cycle is 0-indexed, so cycle==1 means round 2.
	if cycle == 1 && s.opts.Pull {
		cards = append(cards, &xfer.PragmaCard{Name: "req-clusters"})
	}
```

Note: `buildRequest` currently takes `cycle int` as parameter — this is already available.

- [ ] **Step 5: Run all sync tests**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -v`
Expected: ALL PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/sync/client.go go-libfossil/sync/session.go
git commit -m "feat(sync): client sendUnclustered + sendAllClusters + pragma req-clusters (CDG-165)"
```

---

### Task 5: Integration — Full Sync Round-Trip Test

**Files:**
- Create: `go-libfossil/sync/cluster_integration_test.go`

**Context:** Test the full cluster sync flow: client stores 200 blobs, syncs to server, verify convergence with cluster igots. Uses `MockTransport` to wire client ↔ handler.

- [ ] **Step 1: Write integration test**

In `go-libfossil/sync/cluster_integration_test.go`:

```go
package sync

import (
	"context"
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"

	"path/filepath"
)

func TestClusterSync_RoundTrip(t *testing.T) {
	// Create client repo with 200 blobs.
	clientPath := filepath.Join(t.TempDir(), "client.fossil")
	clientRepo, err := repo.Create(clientPath)
	if err != nil {
		t.Fatalf("create client repo: %v", err)
	}
	defer clientRepo.Close()

	for i := range 200 {
		_, _, err := blob.Store(clientRepo.DB(), []byte(fmt.Sprintf("sync-blob-%04d-padding-unique", i)))
		if err != nil {
			t.Fatalf("Store: %v", err)
		}
	}

	// Create server repo (empty).
	serverPath := filepath.Join(t.TempDir(), "server.fossil")
	serverRepo, err := repo.Create(serverPath)
	if err != nil {
		t.Fatalf("create server repo: %v", err)
	}
	defer serverRepo.Close()

	// Sync client → server.
	transport := &MockTransport{ServerRepo: serverRepo}
	result, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Push: true,
		Pull: true,
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Verify all 200 blobs + cluster artifact(s) arrived at server.
	var serverBlobCount int
	serverRepo.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&serverBlobCount)
	if serverBlobCount < 200 {
		t.Fatalf("expected >= 200 blobs on server, got %d", serverBlobCount)
	}

	// Verify convergence happened in reasonable rounds.
	if result.Rounds > 10 {
		t.Fatalf("expected convergence in <= 10 rounds, took %d", result.Rounds)
	}

	t.Logf("Sync converged in %d rounds, %d files sent, %d files received",
		result.Rounds, result.FilesSent, result.FilesRecvd)
}
```

- [ ] **Step 2: Run test**

Run: `cd go-libfossil && go test -buildvcs=false ./sync/ -run TestClusterSync_RoundTrip -v -timeout 30s`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/sync/cluster_integration_test.go
git commit -m "test(sync): cluster round-trip integration test (CDG-165)"
```

---

### Task 6: DST Scenario — Large Blob Count with Clustering

**Files:**
- Modify: `dst/scenario_test.go` (add new test scenario)
- Read: `dst/scenario_test.go:69-110` (pattern for existing scenarios)

**Context:** Add a DST scenario with 500+ blobs to verify cluster-based convergence under fault injection. Should confirm igot count is much less than blob count after clustering.

- [ ] **Step 1: Add DST scenario**

Add to `dst/scenario_test.go`:

```go
// --- Scenario: Cluster Convergence ---
// Master has 500 blobs, verifies clustering reduces igot count.

func TestScenarioClusterConvergence(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(7)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	for i := range 500 {
		mf.StoreArtifact([]byte(fmt.Sprintf("cluster-test-artifact-%05d-pad", i)))
	}

	leafCount := 2
	sim := NewSimulator(SimConfig{
		MasterFossil: mf,
		LeafCount:    leafCount,
		Seed:         seed,
		Severity:     sev,
		MaxSteps:     stepsFor(5000),
	})
	sim.Run(t)

	for i := range leafCount {
		leaf := sim.Leaf(i)
		var blobCount int
		leaf.Repo().DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobCount)
		if blobCount < 500 {
			t.Errorf("leaf %d: expected >= 500 blobs, got %d", i, blobCount)
		}

		// Verify clusters exist on the leaf.
		var clusterCount int
		leaf.Repo().DB().QueryRow("SELECT count(*) FROM tagxref WHERE tagid=7").Scan(&clusterCount)
		if clusterCount == 0 {
			t.Errorf("leaf %d: expected cluster artifacts, got 0", i)
		}
		t.Logf("leaf %d: %d blobs, %d clusters", i, blobCount, clusterCount)
	}
}
```

- [ ] **Step 2: Run DST scenario**

Run: `go test -buildvcs=false ./dst/ -run TestScenarioClusterConvergence -v`
Expected: PASS — all leaves have 500+ blobs and cluster artifacts.

- [ ] **Step 3: Commit**

```bash
git add dst/scenario_test.go
git commit -m "test(dst): add cluster convergence scenario (CDG-165)"
```

---

### Task 7: Full Test Suite + Cleanup

**Files:**
- All files from previous tasks

- [ ] **Step 1: Run full test suite**

Run: `make test`
Expected: ALL PASS

- [ ] **Step 2: Run DST with multiple seeds**

Run: `go test -buildvcs=false ./dst/ -run 'TestScenario|TestE2E' -count=1 -v`
Expected: ALL PASS

- [ ] **Step 3: Run sim serve tests**

Run: `go test -buildvcs=false ./sim/ -run 'TestServeHTTP|TestLeafToLeaf|TestAgentServe' -count=1 -timeout=120s -v`
Expected: ALL PASS

- [ ] **Step 4: Final commit if any cleanup needed**

Only if adjustments were needed during full suite run.
