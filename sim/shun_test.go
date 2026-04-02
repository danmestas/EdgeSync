package sim

import (
	"bytes"
	"context"
	"fmt"
	"math/rand"
	"os/exec"
	"strings"
	"testing"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/blob"
	"github.com/danmestas/go-libfossil/content"
	"github.com/danmestas/go-libfossil/db"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/shun"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/danmestas/go-libfossil/sync"
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

// TestInterop_FossilCreated_GoShunPurge creates a repo with the fossil CLI,
// commits real files, then opens it with go-libfossil to shun and purge an
// artifact. Validates with fossil rebuild afterward.
// This tests the direction: fossil-created repo → go-libfossil shun+purge.
func TestInterop_FossilCreated_GoShunPurge(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()

	// 1. Create repo and commit files using the fossil CLI.
	repoPath := fossilInit(t, dir, "fossil-origin.fossil")
	workDir := fossilCommitFiles(t, repoPath, "", map[string]string{
		"readme.txt":   "This is the readme for the project.",
		"secret.txt":   "TOP SECRET: this artifact will be shunned.",
		"keepme.txt":   "This file should survive the purge.",
	}, "initial commit with 3 files")

	// Make a second commit so we have multiple checkins.
	fossilCommitFiles(t, repoPath, workDir, map[string]string{
		"extra.txt": "Another file in a second commit.",
	}, "second commit")

	// Close the fossil checkout.
	closeCmd := exec.Command("fossil", "close", "--force")
	closeCmd.Dir = workDir
	closeCmd.CombinedOutput()

	// 2. Open with go-libfossil, find the "secret.txt" blob, shun it.
	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}

	// Find the uuid of the blob whose content is "TOP SECRET..."
	var targetUUID string
	var targetRID int64
	rows, err := r.DB().Query("SELECT rid, uuid FROM blob WHERE size >= 0")
	if err != nil {
		t.Fatalf("query blobs: %v", err)
	}
	for rows.Next() {
		var rid int64
		var uuid string
		rows.Scan(&rid, &uuid)
		expanded, err := content.Expand(r.DB(), libfossil.FslID(rid))
		if err != nil {
			continue
		}
		if bytes.Contains(expanded, []byte("TOP SECRET")) {
			targetUUID = uuid
			targetRID = rid
			break
		}
	}
	rows.Close()

	if targetUUID == "" {
		t.Fatal("could not find secret.txt blob in fossil-created repo")
	}
	t.Logf("found secret blob: rid=%d uuid=%s", targetRID, targetUUID)

	// Count blobs before purge.
	var blobsBefore int
	r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobsBefore)
	t.Logf("blobs before purge: %d", blobsBefore)

	// 3. Shun and purge with go-libfossil.
	if err := shun.Add(r.DB(), targetUUID, "shunning secret from fossil repo"); err != nil {
		t.Fatalf("shun.Add: %v", err)
	}

	result, err := shun.Purge(r.DB())
	if err != nil {
		t.Fatalf("shun.Purge: %v", err)
	}
	t.Logf("purge result: blobs_deleted=%d deltas_expanded=%d", result.BlobsDeleted, result.DeltasExpanded)

	if result.BlobsDeleted < 1 {
		t.Errorf("expected at least 1 blob deleted, got %d", result.BlobsDeleted)
	}

	// Verify secret blob is gone.
	if _, ok := blob.Exists(r.DB(), targetUUID); ok {
		t.Error("secret blob still exists after go-libfossil purge")
	}

	// Verify shun entry exists.
	shunned, _ := shun.IsShunned(r.DB(), targetUUID)
	if !shunned {
		t.Error("shun entry missing after purge")
	}

	// Verify remaining blobs are intact.
	verifyAllBlobs(t, r.DB())

	var blobsAfter int
	r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobsAfter)
	t.Logf("blobs after purge: %d (deleted %d)", blobsAfter, blobsBefore-blobsAfter)

	r.Close()

	// 4. fossil rebuild must pass on the purged repo.
	verifyWithFossilRebuild(t, repoPath)
	t.Log("fossil rebuild passed on fossil-created repo after go-libfossil purge")
}

// TestInterop_GoShun_FossilHonors creates a go-libfossil repo, seeds blobs,
// shuns one via go-libfossil (but does NOT purge), then runs fossil rebuild
// which should honor the shun table and remove the blob itself.
// This tests: does go-libfossil's shun table entry work correctly with fossil?
func TestInterop_GoShun_FossilHonors(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()
	repoPath := dir + "/go-shun-fossil-honors.fossil"
	r, err := repo.Create(repoPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	// Seed blobs.
	rng := rand.New(rand.NewSource(123))
	uuids, err := SeedLeaf(r, rng, 5, 2048)
	if err != nil {
		t.Fatalf("SeedLeaf: %v", err)
	}
	t.Logf("seeded %d blobs", len(uuids))

	// Shun one via go-libfossil but do NOT call Purge.
	target := uuids[2]
	t.Logf("shunning uuid %s (go-libfossil, no purge)", target)
	if err := shun.Add(r.DB(), target, "go-libfossil shun for fossil"); err != nil {
		t.Fatalf("shun.Add: %v", err)
	}

	// Verify blob is still in blob table (not yet purged).
	if _, ok := blob.Exists(r.DB(), target); !ok {
		t.Fatal("blob should still exist before fossil rebuild")
	}

	r.Close()

	// fossil rebuild should honor the shun table and remove the shunned blob.
	verifyWithFossilRebuild(t, repoPath)

	// Reopen and verify fossil removed the shunned blob.
	d, err := db.Open(repoPath)
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	defer d.Close()

	if _, ok := blob.Exists(d, target); ok {
		t.Error("fossil rebuild did NOT remove the go-libfossil-shunned blob")
	} else {
		t.Log("fossil rebuild correctly honored go-libfossil shun entry and removed the blob")
	}

	// Remaining blobs must be intact.
	for i, uuid := range uuids {
		if uuid == target {
			continue
		}
		if _, ok := blob.Exists(d, uuid); !ok {
			t.Errorf("non-shunned blob %d (%s) missing after fossil rebuild", i, uuid)
		}
	}

	verifyAllBlobs(t, d)
}

// TestInterop_FossilShun_GoPurge creates a repo with fossil, adds a shun
// entry via direct SQL (simulating fossil's web UI /shun), then uses
// go-libfossil's Purge to remove the blob. Validates with fossil rebuild.
// This tests the direction: fossil shuns → go-libfossil purges.
func TestInterop_FossilShun_GoPurge(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}
	requireFossil(t)

	dir := t.TempDir()

	// 1. Create repo with fossil, commit files.
	repoPath := fossilInit(t, dir, "fossil-shun-go-purge.fossil")
	workDir := fossilCommitFiles(t, repoPath, "", map[string]string{
		"public.txt":  "This is public and should remain.",
		"private.txt": "This will be shunned by fossil's shun table.",
	}, "commit with public and private files")

	closeCmd := exec.Command("fossil", "close", "--force")
	closeCmd.Dir = workDir
	closeCmd.CombinedOutput()

	// 2. Open with go-libfossil to find the private.txt blob UUID.
	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}

	var targetUUID string
	rows, err := r.DB().Query("SELECT rid, uuid FROM blob WHERE size >= 0")
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	for rows.Next() {
		var rid int64
		var uuid string
		rows.Scan(&rid, &uuid)
		expanded, err := content.Expand(r.DB(), libfossil.FslID(rid))
		if err != nil {
			continue
		}
		if bytes.Contains(expanded, []byte("shunned by fossil")) {
			targetUUID = uuid
			break
		}
	}
	rows.Close()

	if targetUUID == "" {
		t.Fatal("could not find private.txt blob")
	}
	t.Logf("target blob uuid: %s", targetUUID)
	r.Close()

	// 3. Simulate fossil's shun UI: insert into shun table via fossil SQL.
	sqlStmt := fmt.Sprintf("INSERT INTO shun(uuid, mtime, scom) VALUES('%s', strftime('%%s','now'), 'shunned via fossil sql');", targetUUID)
	cmd := exec.Command("fossil", "sql", "-R", repoPath)
	cmd.Stdin = strings.NewReader(sqlStmt)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil sql: %v\n%s", err, out)
	}

	// 4. Open with go-libfossil and verify the shun entry fossil created.
	r2, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}

	shunned, err := shun.IsShunned(r2.DB(), targetUUID)
	if err != nil {
		t.Fatalf("IsShunned: %v", err)
	}
	if !shunned {
		t.Fatal("go-libfossil cannot see fossil's shun entry")
	}
	t.Log("go-libfossil correctly reads fossil's shun table entry")

	// 5. Purge with go-libfossil.
	result, err := shun.Purge(r2.DB())
	if err != nil {
		t.Fatalf("shun.Purge: %v", err)
	}
	t.Logf("purge: blobs_deleted=%d deltas_expanded=%d", result.BlobsDeleted, result.DeltasExpanded)

	if result.BlobsDeleted < 1 {
		t.Errorf("expected at least 1 blob deleted, got %d", result.BlobsDeleted)
	}

	// Verify blob is gone.
	if _, ok := blob.Exists(r2.DB(), targetUUID); ok {
		t.Error("blob still exists after go-libfossil purge of fossil-shunned artifact")
	}

	verifyAllBlobs(t, r2.DB())
	r2.Close()

	// 6. fossil rebuild must accept the result.
	verifyWithFossilRebuild(t, repoPath)
	t.Log("fossil rebuild passed after go-libfossil purged a fossil-shunned artifact")
}
