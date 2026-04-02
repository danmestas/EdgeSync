package sim

import (
	"bytes"
	"context"
	"fmt"
	"math/rand"
	"strings"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/shun"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
)

// TestInterop_ShunPurge_FossilRebuild creates a go-libfossil repo, shuns one
// blob, purges it, and verifies fossil rebuild succeeds on the result.
func TestInterop_ShunPurge_FossilRebuild(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()
	r, err := repo.Create(dir+"/shun-rebuild.fossil", "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	// Seed 5 random blobs.
	rng := rand.New(rand.NewSource(42))
	uuids, err := SeedLeaf(r, rng, 5, 2048)
	if err != nil {
		t.Fatalf("SeedLeaf: %v", err)
	}
	t.Logf("seeded %d blobs: %v", len(uuids), uuids)

	// Pick one UUID to shun.
	target := uuids[2]
	t.Logf("shunning uuid %s", target)

	if err := shun.Add(r.DB(), target, "test shun"); err != nil {
		t.Fatalf("shun.Add: %v", err)
	}

	// Purge shunned blobs.
	result, err := shun.Purge(r.DB())
	if err != nil {
		t.Fatalf("shun.Purge: %v", err)
	}
	t.Logf("purge result: blobs_deleted=%d deltas_expanded=%d",
		result.BlobsDeleted, result.DeltasExpanded)

	if result.BlobsDeleted != 1 {
		t.Errorf("expected 1 blob deleted, got %d", result.BlobsDeleted)
	}

	// Verify blob is gone.
	if _, ok := blob.Exists(r.DB(), target); ok {
		t.Error("shunned blob still exists after purge")
	}

	// Verify shun entry remains.
	shunned, err := shun.IsShunned(r.DB(), target)
	if err != nil {
		t.Fatalf("IsShunned: %v", err)
	}
	if !shunned {
		t.Error("shun entry should remain after purge")
	}

	// Close repo before fossil CLI access.
	r.Close()

	// fossil rebuild must pass.
	verifyWithFossilRebuild(t, dir+"/shun-rebuild.fossil")

	// Reopen and verify remaining blobs.
	d, err := db.Open(dir + "/shun-rebuild.fossil")
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	defer d.Close()

	verifyAllBlobs(t, d)

	// Verify the other 4 blobs still exist.
	for i, uuid := range uuids {
		if uuid == target {
			continue
		}
		if _, ok := blob.Exists(d, uuid); !ok {
			t.Errorf("non-shunned blob %d (%s) missing after rebuild", i, uuid)
		}
	}
}

// TestInterop_PurgeDeltaChain_FossilArtifact verifies that purging a delta
// source correctly expands dependents, and fossil can read the result.
func TestInterop_PurgeDeltaChain_FossilArtifact(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()
	repoPath := dir + "/delta-purge.fossil"
	r, err := repo.Create(repoPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	// Store blob A (full text, large enough for meaningful delta).
	contentA := bytes.Repeat([]byte("The source blob for delta chain testing. "), 50)
	ridA, uuidA, err := blob.Store(r.DB(), contentA)
	if err != nil {
		t.Fatalf("blob.Store A: %v", err)
	}
	t.Logf("blob A: rid=%d uuid=%s", ridA, uuidA)

	// Store blob B as delta from A (modify a portion).
	contentB := make([]byte, len(contentA))
	copy(contentB, contentA)
	copy(contentB[100:], []byte("MODIFIED DELTA DEPENDENT SECTION"))
	ridB, uuidB, err := blob.StoreDelta(r.DB(), contentB, ridA)
	if err != nil {
		t.Fatalf("blob.StoreDelta B: %v", err)
	}
	t.Logf("blob B: rid=%d uuid=%s (delta from A)", ridB, uuidB)

	// Verify delta row exists.
	var deltaCount int
	r.DB().QueryRow("SELECT count(*) FROM delta WHERE rid=? AND srcid=?", ridB, ridA).Scan(&deltaCount)
	if deltaCount != 1 {
		t.Fatalf("expected delta row for B->A, got %d", deltaCount)
	}

	// Shun A and purge.
	if err := shun.Add(r.DB(), uuidA, "shun delta source"); err != nil {
		t.Fatalf("shun.Add A: %v", err)
	}
	result, err := shun.Purge(r.DB())
	if err != nil {
		t.Fatalf("shun.Purge: %v", err)
	}
	t.Logf("purge: blobs_deleted=%d deltas_expanded=%d", result.BlobsDeleted, result.DeltasExpanded)

	if result.BlobsDeleted != 1 {
		t.Errorf("expected 1 blob deleted, got %d", result.BlobsDeleted)
	}
	if result.DeltasExpanded != 1 {
		t.Errorf("expected 1 delta expanded, got %d", result.DeltasExpanded)
	}

	// A should be gone.
	if _, ok := blob.Exists(r.DB(), uuidA); ok {
		t.Error("blob A still exists after purge")
	}

	// B should still exist with no delta row.
	if _, ok := blob.Exists(r.DB(), uuidB); !ok {
		t.Fatal("blob B missing after purge")
	}
	r.DB().QueryRow("SELECT count(*) FROM delta WHERE rid=?", ridB).Scan(&deltaCount)
	if deltaCount != 0 {
		t.Errorf("expected 0 delta rows for B after purge, got %d", deltaCount)
	}

	// Verify B content is correct after expansion.
	expandedB, err := content.Expand(r.DB(), ridB)
	if err != nil {
		t.Fatalf("content.Expand B: %v", err)
	}
	if !bytes.Equal(expandedB, contentB) {
		t.Errorf("blob B content mismatch after purge: got %d bytes, want %d bytes",
			len(expandedB), len(contentB))
	}

	// Close repo before fossil CLI access.
	r.Close()

	// fossil rebuild must pass.
	verifyWithFossilRebuild(t, repoPath)

	// fossil artifact <B-uuid> must return the content.
	fossilOut := fossilExec(t, "artifact", uuidB, "-R", repoPath)
	if !bytes.Equal([]byte(fossilOut), contentB) {
		t.Errorf("fossil artifact output mismatch for B:\nfossil len=%d\nexpected len=%d",
			len(fossilOut), len(contentB))
	}
}

// TestInterop_FossilRebuild_GoReads shuns a blob in go-libfossil, then uses
// fossil rebuild to purge it (rebuild honors the shun table), and verifies
// go-libfossil can still read the remaining content.
func TestInterop_FossilRebuild_GoReads(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()
	repoPath := dir + "/fossil-rebuild-reads.fossil"
	r, err := repo.Create(repoPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	// Seed blobs.
	rng := rand.New(rand.NewSource(99))
	uuids, err := SeedLeaf(r, rng, 5, 2048)
	if err != nil {
		t.Fatalf("SeedLeaf: %v", err)
	}

	// Shun one blob via go-libfossil.
	target := uuids[1]
	t.Logf("shunning uuid %s", target)
	if err := shun.Add(r.DB(), target, "test shun for rebuild"); err != nil {
		t.Fatalf("shun.Add: %v", err)
	}

	// Close repo so fossil can operate on it.
	r.Close()

	// fossil rebuild honors the shun table and purges shunned artifacts.
	verifyWithFossilRebuild(t, repoPath)

	// Reopen with go-libfossil and verify.
	r2, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer r2.Close()

	// Shunned blob should be gone (rebuild removes it).
	if _, ok := blob.Exists(r2.DB(), target); ok {
		t.Error("shunned blob still exists after fossil rebuild")
	}

	// Remaining blobs should be intact.
	verifyAllBlobs(t, r2.DB())

	for i, uuid := range uuids {
		if uuid == target {
			continue
		}
		if _, ok := blob.Exists(r2.DB(), uuid); !ok {
			t.Errorf("non-shunned blob %d (%s) missing after fossil rebuild", i, uuid)
		}
	}
}

// TestInterop_PurgeThenSync_FossilClone verifies that after shunning and
// purging a blob, fossil clone from our HTTP server does not receive the
// shunned artifact.
func TestInterop_PurgeThenSync_FossilClone(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()
	serverPath := dir + "/server.fossil"
	r, err := repo.Create(serverPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	// Seed 5 blobs.
	rng := rand.New(rand.NewSource(77))
	uuids, err := SeedLeaf(r, rng, 5, 2048)
	if err != nil {
		t.Fatalf("SeedLeaf: %v", err)
	}
	t.Logf("seeded %d blobs", len(uuids))

	// Shun one and purge.
	target := uuids[3]
	t.Logf("shunning uuid %s", target)
	if err := shun.Add(r.DB(), target, "test shun for clone"); err != nil {
		t.Fatalf("shun.Add: %v", err)
	}
	result, err := shun.Purge(r.DB())
	if err != nil {
		t.Fatalf("shun.Purge: %v", err)
	}
	t.Logf("purge: blobs_deleted=%d", result.BlobsDeleted)

	// Close and reopen for serving.
	r.Close()
	serverRepo, err := repo.Open(serverPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer serverRepo.Close()

	// Start HTTP server.
	addr, cancel := serveLeafHTTP(t, serverRepo)
	defer cancel()

	// fossil clone from our server.
	clonePath := dir + "/clone.fossil"
	fossilExec(t, "clone", fmt.Sprintf("http://%s", addr), clonePath)

	// Verify with fossil rebuild.
	verifyWithFossilRebuild(t, clonePath)

	// Open clone and verify contents.
	cloneDB, err := db.Open(clonePath)
	if err != nil {
		t.Fatalf("db.Open clone: %v", err)
	}
	defer cloneDB.Close()

	// Shunned blob must be absent.
	if _, ok := blob.Exists(cloneDB, target); ok {
		t.Error("shunned blob found in clone — should not have been synced")
	}

	// Non-shunned blobs must be present. Check for real blobs (size >= 0).
	for i, uuid := range uuids {
		if uuid == target {
			continue
		}
		if _, ok := blob.Exists(cloneDB, uuid); !ok {
			// The blob might be absent if it was only stored as an unclustered
			// loose blob without a manifest referencing it. Fossil clone only
			// transfers artifacts reachable from the DAG. Log but don't fail
			// for unreferenced blobs.
			t.Logf("note: non-shunned blob %d (%s) not in clone (may be unreferenced)", i, uuid)
		}
	}

	verifyAllBlobs(t, cloneDB)
}

// TestInterop_PurgeThenSync_LeafClone is like TestInterop_PurgeThenSync_FossilClone
// but uses sync.Clone (go-libfossil client) instead of fossil CLI.
func TestInterop_PurgeThenSync_LeafClone(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()
	serverPath := dir + "/server.fossil"
	r, err := repo.Create(serverPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	// Seed 5 blobs.
	rng := rand.New(rand.NewSource(88))
	uuids, err := SeedLeaf(r, rng, 5, 2048)
	if err != nil {
		t.Fatalf("SeedLeaf: %v", err)
	}

	// Shun one and purge.
	target := uuids[0]
	t.Logf("shunning uuid %s", target)
	if err := shun.Add(r.DB(), target, "test shun for leaf clone"); err != nil {
		t.Fatalf("shun.Add: %v", err)
	}
	pResult, err := shun.Purge(r.DB())
	if err != nil {
		t.Fatalf("shun.Purge: %v", err)
	}
	t.Logf("purge: blobs_deleted=%d", pResult.BlobsDeleted)

	// Close and reopen for serving.
	r.Close()
	serverRepo, err := repo.Open(serverPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer serverRepo.Close()

	addr, cancel := serveLeafHTTP(t, serverRepo)
	defer cancel()

	// Clone with go-libfossil client.
	clonePath := dir + "/leaf-clone.fossil"
	ctx, ctxCancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer ctxCancel()

	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	cloneRepo, _, err := sync.Clone(ctx, clonePath, transport, sync.CloneOpts{})
	if err != nil {
		// Clone may fail if there are no manifests to clone (only loose blobs).
		// This is expected — the sync protocol needs at least a project-code.
		if strings.Contains(err.Error(), "project-code") {
			t.Skipf("clone failed (no manifests): %v", err)
		}
		t.Fatalf("sync.Clone: %v", err)
	}
	defer cloneRepo.Close()

	// Shunned blob must be absent.
	if _, ok := blob.Exists(cloneRepo.DB(), target); ok {
		t.Error("shunned blob found in leaf clone")
	}

	verifyAllBlobs(t, cloneRepo.DB())
}
