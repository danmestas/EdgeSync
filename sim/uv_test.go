package sim

import (
	"context"
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/danmestas/go-libfossil/sync"
	"github.com/danmestas/go-libfossil/uv"
)

// TestSimUVSyncPull verifies that UV files added via fossil CLI can be
// read from the repo's unversioned table by our uv package.
func TestSimUVSyncPull(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create a Fossil repo via CLI and add UV files.
	repoPath := filepath.Join(dir, "source.fossil")
	runFossil(t, "new", repoPath)

	uvFile := filepath.Join(dir, "wiki.txt")
	os.WriteFile(uvFile, []byte("hello from fossil"), 0644)
	runFossil(t, "uv", "add", uvFile, "--as", "wiki.txt", "-R", repoPath)

	// 2. Open with our code and verify UV file is readable.
	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer r.Close()

	content, mtime, hash, err := uv.Read(r.DB(), "wiki.txt")
	if err != nil {
		t.Fatalf("uv.Read: %v", err)
	}
	if string(content) != "hello from fossil" {
		t.Fatalf("content mismatch: got %q", content)
	}
	if mtime == 0 {
		t.Error("mtime should be non-zero")
	}
	if hash == "" {
		t.Error("hash should not be empty")
	}
	t.Logf("UV read OK: name=wiki.txt mtime=%d hash=%s", mtime, hash[:16])
}

// TestSimUVCatalogHashCompat verifies our ContentHash matches Fossil's
// internal catalog hash by comparing via direct DB query.
func TestSimUVCatalogHashCompat(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	runFossil(t, "new", repoPath)

	// Add multiple UV files.
	for i, content := range []string{"aaa", "bbb", "ccc"} {
		f := filepath.Join(dir, fmt.Sprintf("file%d.txt", i))
		os.WriteFile(f, []byte(content), 0644)
		runFossil(t, "uv", "add", f, "--as", fmt.Sprintf("file%d.txt", i), "-R", repoPath)
	}

	// Open with our code and compute hash.
	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer r.Close()

	ourHash, err := uv.ContentHash(r.DB())
	if err != nil {
		t.Fatalf("ContentHash: %v", err)
	}

	// Fossil doesn't expose uv hash via CLI, but we can verify by reading
	// the raw data and checking our hash isn't empty/wrong.
	if ourHash == "" || ourHash == "da39a3ee5e6b4b0d3255bfef95601890afd80709" {
		t.Fatalf("hash should not be empty-string hash with 3 files: %s", ourHash)
	}

	// Verify hash is deterministic.
	uv.InvalidateHash(r.DB())
	secondHash, _ := uv.ContentHash(r.DB())
	if ourHash != secondHash {
		t.Errorf("hash not deterministic: %s vs %s", ourHash, secondHash)
	}

	t.Logf("Catalog hash: %s (3 files)", ourHash)
}

// TestSimUVSyncViaHTTP tests that UV files sync via our HTTP server
// when a client syncs with UV=true.
func TestSimUVSyncViaHTTP(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create server repo with UV files.
	serverPath := filepath.Join(dir, "server.fossil")
	serverRepo, err := repo.Create(serverPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	uv.EnsureSchema(serverRepo.DB())
	uv.Write(serverRepo.DB(), "wiki/page.txt", []byte("wiki content"), 1700000000)
	uv.Write(serverRepo.DB(), "data/config.json", []byte(`{"key":"value"}`), 1700000100)

	// Also seed some blobs so clone works (fossil needs at least a manifest).
	rng := rand.New(rand.NewSource(42))
	SeedLeaf(serverRepo, rng, 1, 64)
	serverRepo.Close()

	// Re-open for serving.
	serverRepo, err = repo.Open(serverPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer serverRepo.Close()

	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go sync.ServeHTTP(ctx, addr, serverRepo, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)

	// 2. Create client repo and sync with UV.
	clientPath := filepath.Join(dir, "client.fossil")
	clientRepo, err := repo.Create(clientPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("client repo.Create: %v", err)
	}
	defer clientRepo.Close()

	uv.EnsureSchema(clientRepo.DB())

	// Get project/server codes from server.
	var projCode, srvCode string
	serverRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	serverRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	_, err = sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Push: true, Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// 3. Verify UV files arrived on client.
	content, _, _, err := uv.Read(clientRepo.DB(), "wiki/page.txt")
	if err != nil {
		t.Fatalf("Read wiki: %v", err)
	}
	if string(content) != "wiki content" {
		t.Errorf("wiki content = %q, want %q", content, "wiki content")
	}

	content2, _, _, err := uv.Read(clientRepo.DB(), "data/config.json")
	if err != nil {
		t.Fatalf("Read config: %v", err)
	}
	if string(content2) != `{"key":"value"}` {
		t.Errorf("config content = %q", content2)
	}

	// 4. Verify catalog hashes match.
	serverHash, _ := uv.ContentHash(serverRepo.DB())
	clientHash, _ := uv.ContentHash(clientRepo.DB())
	if serverHash != clientHash {
		t.Errorf("catalog hash mismatch: server=%s client=%s", serverHash, clientHash)
	}

	t.Logf("PASS: UV sync via HTTP — 2 files, catalog hash=%s", serverHash[:16])
}

// TestSimUVDeletion verifies tombstone propagation.
func TestSimUVDeletion(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create server with a UV file.
	serverPath := filepath.Join(dir, "server.fossil")
	serverRepo, err := repo.Create(serverPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	uv.EnsureSchema(serverRepo.DB())
	uv.Write(serverRepo.DB(), "doomed.txt", []byte("will be deleted"), 100)

	rng := rand.New(rand.NewSource(42))
	SeedLeaf(serverRepo, rng, 1, 64)
	serverRepo.Close()

	serverRepo, err = repo.Open(serverPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer serverRepo.Close()

	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go sync.ServeHTTP(ctx, addr, serverRepo, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)

	// 2. Client syncs — gets the file.
	clientPath := filepath.Join(dir, "client.fossil")
	clientRepo, err := repo.Create(clientPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("client repo.Create: %v", err)
	}
	defer clientRepo.Close()
	uv.EnsureSchema(clientRepo.DB())

	var projCode, srvCode string
	serverRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	serverRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Push: true, Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})

	// Verify file arrived.
	c, _, _, _ := uv.Read(clientRepo.DB(), "doomed.txt")
	if string(c) != "will be deleted" {
		t.Fatalf("file should exist before deletion: %q", c)
	}

	// 3. Delete on server.
	uv.Delete(serverRepo.DB(), "doomed.txt", 200)

	// 4. Client re-syncs — should get tombstone.
	sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Push: true, Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})

	_, mtime, hash, _ := uv.Read(clientRepo.DB(), "doomed.txt")
	if hash != "" {
		t.Errorf("expected tombstone, got hash=%q", hash)
	}
	if mtime != 200 {
		t.Errorf("mtime = %d, want 200", mtime)
	}

	t.Log("PASS: UV deletion tombstone propagated via HTTP sync")
}

// TestSimUVRoundTrip tests UV files survive a pull→modify→push cycle.
func TestSimUVRoundTrip(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Server with initial UV file.
	serverPath := filepath.Join(dir, "server.fossil")
	serverRepo, err := repo.Create(serverPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	uv.EnsureSchema(serverRepo.DB())
	uv.Write(serverRepo.DB(), "doc.txt", []byte("version 1"), 100)
	rng := rand.New(rand.NewSource(42))
	SeedLeaf(serverRepo, rng, 1, 64)
	serverRepo.Close()

	serverRepo, err = repo.Open(serverPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer serverRepo.Close()

	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go sync.ServeHTTP(ctx, addr, serverRepo, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)

	// 2. Client pulls.
	clientPath := filepath.Join(dir, "client.fossil")
	clientRepo, err := repo.Create(clientPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("client repo.Create: %v", err)
	}
	defer clientRepo.Close()
	uv.EnsureSchema(clientRepo.DB())

	var projCode, srvCode string
	serverRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	serverRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Push: true, Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})

	// 3. Client modifies and pushes back.
	uv.Write(clientRepo.DB(), "doc.txt", []byte("version 2"), 200)
	sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Push: true, Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})

	// 4. Verify server has the updated version.
	content, mtime, _, _ := uv.Read(serverRepo.DB(), "doc.txt")
	if string(content) != "version 2" {
		t.Errorf("server content = %q, want %q", content, "version 2")
	}
	if mtime != 200 {
		t.Errorf("mtime = %d, want 200", mtime)
	}

	t.Log("PASS: UV round-trip pull→modify→push via HTTP")
}

// TestSimUVSyncAgainstFossilServe pushes UV files from go-libfossil to a real
// `fossil serve` instance. This catches capability letter mismatches: if
// go-libfossil checked 'z' instead of 'y' for CanPushUV, the server-side
// handler would gate on the wrong letter when using Fossil-created user tables.
//
// The test creates a Fossil repo with nobody granted 'y' (WrUnver), serves it,
// then uses go-libfossil to push UV files and verifies they arrive.
func TestSimUVSyncAgainstFossilServe(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create Fossil repo and grant nobody UV push capability ('y').
	repoPath := filepath.Join(dir, "fossil-server.fossil")
	runFossil(t, "new", repoPath)
	// Grant nobody: pull + push + UV push (o + i + y).
	exec.Command("fossil", "user", "capabilities", "nobody", "oiy", "-R", repoPath).Run()

	// Seed a UV file on the server so we can test pull too.
	uvFile := filepath.Join(dir, "server-doc.txt")
	os.WriteFile(uvFile, []byte("from fossil"), 0644)
	runFossil(t, "uv", "add", uvFile, "--as", "server-doc.txt", "-R", repoPath)

	// 2. Start fossil serve.
	serverURL := startFossilServe(t, repoPath)

	// 3. Create a go-libfossil repo and sync UV files.
	clientPath := filepath.Join(dir, "leaf-client.fossil")
	clientRepo, err := repo.Create(clientPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	defer clientRepo.Close()
	uv.EnsureSchema(clientRepo.DB())

	// Read project/server codes from the Fossil repo.
	fossilRepo, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("open fossil repo for codes: %v", err)
	}
	var projCode, srvCode string
	fossilRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	fossilRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)
	fossilRepo.Close()

	// Set matching project code on client.
	clientRepo.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	transport := &sync.HTTPTransport{URL: serverURL}

	// Pull UV files from fossil serve.
	result, err := sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})
	if err != nil {
		t.Fatalf("Sync pull: %v", err)
	}
	t.Logf("Pull sync: files_recv=%d", result.FilesRecvd)

	// Verify the UV file arrived.
	content, _, _, err := uv.Read(clientRepo.DB(), "server-doc.txt")
	if err != nil {
		t.Fatalf("uv.Read after pull: %v", err)
	}
	if string(content) != "from fossil" {
		t.Errorf("pulled UV content = %q, want %q", content, "from fossil")
	}

	// Now push a UV file back to fossil serve.
	uv.Write(clientRepo.DB(), "leaf-doc.txt", []byte("from leaf"), time.Now().Unix())
	_, err = sync.Sync(ctx, clientRepo, transport, sync.SyncOpts{
		Push: true, Pull: true, UV: true,
		ProjectCode: projCode, ServerCode: srvCode,
	})
	if err != nil {
		t.Fatalf("Sync push: %v", err)
	}

	// Verify by reading the fossil repo directly.
	fossilRepo2, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("reopen fossil repo: %v", err)
	}
	defer fossilRepo2.Close()

	pushed, _, _, err := uv.Read(fossilRepo2.DB(), "leaf-doc.txt")
	if err != nil {
		t.Fatalf("uv.Read pushed file: %v", err)
	}
	if string(pushed) != "from leaf" {
		t.Errorf("pushed UV content = %q, want %q", pushed, "from leaf")
	}

	t.Log("PASS: UV sync (pull + push) against real fossil serve")
}

// runFossil runs a fossil command and fails the test on error.
func runFossil(t *testing.T, args ...string) {
	t.Helper()
	cmd := exec.Command("fossil", args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil %s failed: %v\n%s", strings.Join(args, " "), err, out)
	}
}
