package sim

import (
	"context"
	"fmt"
	"math/rand"
	"net"
	"net/http"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/leaf/agent"
)

// TestServeHTTPFossilClone starts a leaf serving HTTP, then uses a real
// `fossil clone` to clone from it. Verifies the cloned repo is valid and
// contains all expected blobs. This proves our handler produces output
// that real Fossil can read.
func TestServeHTTPFossilClone(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create source repo, seed blobs.
	srcPath := filepath.Join(dir, "source.fossil")
	srcRepo, err := repo.Create(srcPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	rng := rand.New(rand.NewSource(42))
	uuids, err := SeedLeaf(srcRepo, rng, 5, 4096)
	if err != nil {
		srcRepo.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	t.Logf("Seeded %d blobs into source repo", len(uuids))
	srcRepo.Close()

	// 2. Re-open and start ServeHTTP.
	srcRepo, err = repo.Open(srcPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer srcRepo.Close()

	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go sync.ServeHTTP(ctx, addr, srcRepo, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)

	// 3. Use fossil clone to clone from our Go server.
	clonePath := filepath.Join(dir, "clone.fossil")
	cmd := exec.Command("fossil", "clone", fmt.Sprintf("http://%s", addr), clonePath)
	out, err := cmd.CombinedOutput()
	t.Logf("fossil clone output:\n%s", out)
	if err != nil {
		t.Fatalf("fossil clone failed: %v", err)
	}

	// 4. Verify each seeded blob is readable by real fossil.
	for _, uuid := range uuids {
		cmd := exec.Command("fossil", "artifact", uuid, "-R", clonePath)
		artOut, artErr := cmd.CombinedOutput()
		if artErr != nil {
			t.Errorf("fossil artifact %s failed: %v\n%s", uuid[:16], artErr, artOut)
		}
	}

	// 5. Verify fossil can rebuild the cloned repo (full integrity check).
	rebuildCmd := exec.Command("fossil", "rebuild", clonePath)
	rebuildOut, rebuildErr := rebuildCmd.CombinedOutput()
	t.Logf("fossil rebuild output:\n%s", rebuildOut)
	if rebuildErr != nil {
		t.Errorf("fossil rebuild failed: %v", rebuildErr)
	}

	t.Logf("PASS: fossil clone from ServeHTTP — %d blobs, all verified by real fossil", len(uuids))
}

// TestServeHTTPFossilSync starts a leaf serving HTTP, clones with fossil,
// seeds more blobs, then syncs. Verifies incremental sync works and
// new blobs are readable by real fossil.
func TestServeHTTPFossilSync(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create source repo, seed initial blobs.
	srcPath := filepath.Join(dir, "source.fossil")
	srcRepo, err := repo.Create(srcPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	rng := rand.New(rand.NewSource(99))
	initialUUIDs, err := SeedLeaf(srcRepo, rng, 3, 2048)
	if err != nil {
		srcRepo.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	srcRepo.Close()

	// 2. Start ServeHTTP.
	srcRepo, err = repo.Open(srcPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer srcRepo.Close()

	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go sync.ServeHTTP(ctx, addr, srcRepo, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)

	// 3. Clone with fossil.
	clonePath := filepath.Join(dir, "syncrepo.fossil")
	cloneCmd := exec.Command("fossil", "clone", fmt.Sprintf("http://%s", addr), clonePath)
	if out, err := cloneCmd.CombinedOutput(); err != nil {
		t.Fatalf("fossil clone: %v\n%s", err, out)
	}

	// 4. Seed more blobs into the source (after clone).
	rng2 := rand.New(rand.NewSource(100))
	newUUIDs, err := SeedLeaf(srcRepo, rng2, 3, 2048)
	if err != nil {
		t.Fatalf("SeedLeaf round 2: %v", err)
	}
	t.Logf("Initial: %d blobs, new: %d blobs", len(initialUUIDs), len(newUUIDs))

	// 5. Sync the clone to get new blobs.
	syncCmd := exec.Command("fossil", "sync", fmt.Sprintf("http://%s", addr), "-R", clonePath)
	syncOut, syncErr := syncCmd.CombinedOutput()
	t.Logf("fossil sync output:\n%s", syncOut)
	if syncErr != nil {
		t.Fatalf("fossil sync failed: %v", syncErr)
	}

	// 6. Verify new blobs are readable by real fossil.
	for _, uuid := range newUUIDs {
		cmd := exec.Command("fossil", "artifact", uuid, "-R", clonePath)
		artOut, artErr := cmd.CombinedOutput()
		if artErr != nil {
			t.Errorf("artifact %s not in synced clone: %v\n%s", uuid[:16], artErr, artOut)
		}
	}

	t.Logf("PASS: fossil sync from ServeHTTP — %d new blobs synced and verified", len(newUUIDs))
}

// TestLeafToLeafSync sets up two leaf agents connected via NATS.
// Leaf-0 has blobs and serves via ServeNATS. Leaf-1 syncs as client.
// Verifies blob convergence using the same invariants as the sim harness.
// No bridge, no Fossil server — pure leaf-to-leaf.
func TestLeafToLeafSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Embedded NATS.
	natsOpts := &natsserver.Options{Port: -1}
	ns, err := natsserver.NewServer(natsOpts)
	if err != nil {
		t.Fatalf("nats server: %v", err)
	}
	ns.Start()
	defer ns.Shutdown()
	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats not ready")
	}
	natsURL := ns.ClientURL()

	// 2. Create two leaf repos with the same project-code.
	projCode := "abcdef0123456789abcdef0123456789abcdef01"
	var leafPaths [2]string
	for i := range 2 {
		path := filepath.Join(dir, fmt.Sprintf("leaf-%d.fossil", i))
		r, err := repo.Create(path, "testuser", simio.CryptoRand{})
		if err != nil {
			t.Fatalf("repo.Create leaf-%d: %v", i, err)
		}
		r.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)
		r.Close()
		leafPaths[i] = path
	}

	// 3. Seed blobs into leaf-0 only.
	r0, err := repo.Open(leafPaths[0])
	if err != nil {
		t.Fatalf("open leaf-0: %v", err)
	}
	rng := rand.New(rand.NewSource(77))
	seededUUIDs, err := SeedLeaf(r0, rng, 5, 4096)
	if err != nil {
		r0.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	r0.Close()
	t.Logf("Seeded %d blobs into leaf-0", len(seededUUIDs))

	// 4. Start leaf-0 with ServeNATS enabled (server + client).
	leaf0, err := agent.New(agent.Config{
		RepoPath:         leafPaths[0],
		NATSUrl:          natsURL,
		Push:             true,
		Pull:             true,
		PollInterval:     2 * time.Second,
		SubjectPrefix:    "fossil",
		ServeNATSEnabled: true,
		Observer:         testObserver,
	})
	if err != nil {
		t.Fatalf("agent.New leaf-0: %v", err)
	}
	if err := leaf0.Start(); err != nil {
		t.Fatalf("leaf-0 start: %v", err)
	}
	defer leaf0.Stop()

	// 5. Start leaf-1 as client only.
	leaf1, err := agent.New(agent.Config{
		RepoPath:      leafPaths[1],
		NATSUrl:       natsURL,
		Push:          true,
		Pull:          true,
		PollInterval:  2 * time.Second,
		SubjectPrefix: "fossil",
		Observer:      testObserver,
	})
	if err != nil {
		t.Fatalf("agent.New leaf-1: %v", err)
	}
	if err := leaf1.Start(); err != nil {
		t.Fatalf("leaf-1 start: %v", err)
	}
	defer leaf1.Stop()

	// 6. Wait for convergence (poll up to 30s).
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(3 * time.Second)

		r1, err := repo.Open(leafPaths[1])
		if err != nil {
			continue
		}
		var count1 int
		r1.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&count1)
		r1.Close()

		r0c, _ := repo.Open(leafPaths[0])
		var count0 int
		r0c.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&count0)
		r0c.Close()

		t.Logf("convergence: leaf-0=%d blobs, leaf-1=%d blobs", count0, count1)
		if count1 >= count0 && count0 > 0 {
			goto converged
		}
	}
	t.Fatal("leaf-to-leaf sync did not converge within 30s")

converged:
	// 7. Stop agents and run invariants.
	leaf0.Stop()
	leaf1.Stop()

	var repos []*repo.Repo
	var labels []string
	for i, path := range leafPaths {
		r, err := repo.Open(path)
		if err != nil {
			t.Fatalf("reopen leaf-%d: %v", i, err)
		}
		defer r.Close()
		repos = append(repos, r)
		labels = append(labels, fmt.Sprintf("leaf-%d", i))
	}

	blobConv := CheckBlobConvergence(repos, labels)
	contentInt := CheckContentIntegrity(repos, labels)
	noDups := CheckNoDuplicates(repos, labels)

	t.Logf("Invariants:\n  %s: %v (%s)\n  %s: %v (%s)\n  %s: %v (%s)",
		blobConv.Name, blobConv.Passed, blobConv.Details,
		contentInt.Name, contentInt.Passed, contentInt.Details,
		noDups.Name, noDups.Passed, noDups.Details)

	if !blobConv.Passed {
		t.Errorf("blob convergence FAILED: %s", blobConv.Details)
	}
	if !contentInt.Passed {
		t.Errorf("content integrity FAILED: %s", contentInt.Details)
	}
	if !noDups.Passed {
		t.Errorf("no duplicates FAILED: %s", noDups.Details)
	}

	t.Logf("PASS: leaf-to-leaf via NATS — %d blobs converged, all invariants pass", len(seededUUIDs))
}

// TestLeafToLeafHTTP sets up two leaf repos. Leaf-0 serves HTTP.
// Leaf-1 syncs via HTTPTransport against leaf-0. Verifies convergence
// and that both repos are readable by real fossil.
func TestLeafToLeafHTTP(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// Shared project code.
	projCode := "deadbeef0123456789abcdef0123456789abcdef"

	// 1. Create leaf-0 (server) with blobs.
	path0 := filepath.Join(dir, "leaf-0.fossil")
	r0, err := repo.Create(path0, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("create leaf-0: %v", err)
	}
	r0.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)

	rng := rand.New(rand.NewSource(55))
	seededUUIDs, err := SeedLeaf(r0, rng, 7, 4096)
	if err != nil {
		r0.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	t.Logf("Seeded %d blobs into leaf-0", len(seededUUIDs))
	r0.Close()

	// 2. Create leaf-1 (client) — empty, same project code.
	path1 := filepath.Join(dir, "leaf-1.fossil")
	r1, err := repo.Create(path1, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("create leaf-1: %v", err)
	}
	r1.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)

	var srvCode string
	r1.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)
	r1.Close()

	// 3. Start leaf-0 as HTTP server.
	r0, err = repo.Open(path0)
	if err != nil {
		t.Fatalf("reopen leaf-0: %v", err)
	}
	defer r0.Close()

	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go sync.ServeHTTP(ctx, addr, r0, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)

	// 4. Leaf-1 syncs from leaf-0 via HTTP (multiple rounds until convergence).
	r1, err = repo.Open(path1)
	if err != nil {
		t.Fatalf("reopen leaf-1: %v", err)
	}
	defer r1.Close()

	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	for round := 0; round < 10; round++ {
		result, err := sync.Sync(ctx, r1, transport, sync.SyncOpts{
			Push:        true,
			Pull:        true,
			ProjectCode: projCode,
			ServerCode:  srvCode,
			Observer:    testObserver,
		})
		if err != nil {
			t.Fatalf("sync round %d: %v", round, err)
		}
		t.Logf("round %d: sent=%d recv=%d", round, result.FilesSent, result.FilesRecvd)
		if result.FilesSent == 0 && result.FilesRecvd == 0 {
			break
		}
	}

	// 5. Close repos, run invariants.
	r0.Close()
	r1.Close()

	var repos []*repo.Repo
	var labels []string
	for i, path := range []string{path0, path1} {
		r, err := repo.Open(path)
		if err != nil {
			t.Fatalf("reopen %d: %v", i, err)
		}
		defer r.Close()
		repos = append(repos, r)
		labels = append(labels, fmt.Sprintf("leaf-%d", i))
	}

	blobConv := CheckBlobConvergence(repos, labels)
	contentInt := CheckContentIntegrity(repos, labels)

	t.Logf("Invariants: convergence=%v integrity=%v", blobConv.Passed, contentInt.Passed)

	if !blobConv.Passed {
		t.Errorf("blob convergence FAILED: %s", blobConv.Details)
	}
	if !contentInt.Passed {
		t.Errorf("content integrity FAILED: %s", contentInt.Details)
	}

	// 6. Verify real fossil can read both repos.
	for i, path := range []string{path0, path1} {
		rebuildCmd := exec.Command("fossil", "rebuild", path)
		out, err := rebuildCmd.CombinedOutput()
		if err != nil {
			t.Errorf("fossil rebuild leaf-%d failed: %v\n%s", i, err, out)
		}
	}

	t.Logf("PASS: leaf-to-leaf HTTP — %d blobs converged, both repos pass fossil rebuild", len(seededUUIDs))
}

// TestAgentServeHTTPFossilClone exercises the production serving path:
// agent.Start() with ServeHTTPAddr → healthz + XferHandler mux → fossil clone.
// This is what actually runs on the VPS, not sync.ServeHTTP directly.
func TestAgentServeHTTPFossilClone(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Embedded NATS (agent.New requires a NATS connection).
	natsOpts := &natsserver.Options{Port: -1}
	ns, err := natsserver.NewServer(natsOpts)
	if err != nil {
		t.Fatalf("nats server: %v", err)
	}
	ns.Start()
	defer ns.Shutdown()
	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats not ready")
	}

	// 2. Create and seed a repo.
	repoPath := filepath.Join(dir, "agent.fossil")
	r, err := repo.Create(repoPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	rng := rand.New(rand.NewSource(88))
	uuids, err := SeedLeaf(r, rng, 5, 4096)
	if err != nil {
		r.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	t.Logf("Seeded %d blobs", len(uuids))
	r.Close()

	// 3. Start agent with ServeHTTPAddr (production path).
	addr := freeAddr(t)
	a, err := agent.New(agent.Config{
		RepoPath:      repoPath,
		NATSUrl:       ns.ClientURL(),
		ServeHTTPAddr: addr,
		PollInterval:  60 * time.Second,
		Observer:      testObserver,
	})
	if err != nil {
		t.Fatalf("agent.New: %v", err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("agent.Start: %v", err)
	}
	defer a.Stop()
	waitForAddr(t, addr, 5*time.Second)

	// 4. Verify /healthz works through agent's mux.
	resp, err := http.Get(fmt.Sprintf("http://%s/healthz", addr))
	if err != nil {
		t.Fatalf("healthz request: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("healthz: got %d, want 200", resp.StatusCode)
	}

	// 5. fossil clone through agent's mux (XferHandler path).
	clonePath := filepath.Join(dir, "clone.fossil")
	cmd := exec.Command("fossil", "clone", fmt.Sprintf("http://%s", addr), clonePath)
	out, err := cmd.CombinedOutput()
	t.Logf("fossil clone output:\n%s", out)
	if err != nil {
		t.Fatalf("fossil clone failed: %v", err)
	}

	// 6. Verify all blobs readable by real fossil.
	for _, uuid := range uuids {
		cmd := exec.Command("fossil", "artifact", uuid, "-R", clonePath)
		artOut, artErr := cmd.CombinedOutput()
		if artErr != nil {
			t.Errorf("fossil artifact %s failed: %v\n%s", uuid[:16], artErr, artOut)
		}
	}

	// 7. fossil rebuild for full integrity.
	rebuildCmd := exec.Command("fossil", "rebuild", clonePath)
	rebuildOut, rebuildErr := rebuildCmd.CombinedOutput()
	t.Logf("fossil rebuild output:\n%s", rebuildOut)
	if rebuildErr != nil {
		t.Errorf("fossil rebuild failed: %v", rebuildErr)
	}

	t.Logf("PASS: agent.Start() ServeHTTP → fossil clone — %d blobs, healthz + xfer verified", len(uuids))
}

// --- helpers ---

func freeAddr(t *testing.T) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()
	return addr
}

func waitForAddr(t *testing.T, addr string, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 200*time.Millisecond)
		if err == nil {
			conn.Close()
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("%s not reachable after %s", addr, timeout)
}
