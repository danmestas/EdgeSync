package sim

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"

	"github.com/dmestas/edgesync/go-libfossil/checkout"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/leaf/agent"
)

// ---------------------------------------------------------------------------
// Shared helpers for checkout sim tests
// ---------------------------------------------------------------------------

// checkoutStartNATS starts an embedded NATS server with a random port.
// The server is shut down when the test ends. Returns the client URL.
func checkoutStartNATS(t *testing.T) string {
	t.Helper()

	ns, err := natsserver.NewServer(&natsserver.Options{Port: -1})
	if err != nil {
		t.Fatalf("nats server: %v", err)
	}
	ns.Start()
	t.Cleanup(ns.Shutdown)

	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats not ready")
	}
	return ns.ClientURL()
}

// checkoutCreateSeededRepo creates a Fossil repo at dir/name, commits the
// given files as an initial checkin, then closes the repo. Returns the path.
func checkoutCreateSeededRepo(t *testing.T, dir, name string, files map[string]string) string {
	t.Helper()

	repoPath := filepath.Join(dir, name)
	r, err := repo.Create(repoPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}

	var mfiles []manifest.File
	for fname, content := range files {
		mfiles = append(mfiles, manifest.File{
			Name:    fname,
			Content: []byte(content),
		})
	}

	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   mfiles,
		Comment: "initial commit",
		User:    "testuser",
	})
	if err != nil {
		r.Close()
		t.Fatalf("manifest.Checkin: %v", err)
	}

	r.Close()
	return repoPath
}

// checkoutStartAgent creates and starts a leaf agent with the given config.
// The agent is stopped when the test ends.
func checkoutStartAgent(t *testing.T, repoPath, natsURL string, push, pull, serve bool) *agent.Agent {
	t.Helper()

	a, err := agent.New(agent.Config{
		RepoPath:         repoPath,
		NATSUrl:          natsURL,
		Push:             push,
		Pull:             pull,
		PollInterval:     1 * time.Second,
		SubjectPrefix:    "fossil",
		ServeNATSEnabled: serve,
		Observer:         testObserver,
	})
	if err != nil {
		t.Fatalf("agent.New(%s): %v", filepath.Base(repoPath), err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("agent.Start(%s): %v", filepath.Base(repoPath), err)
	}
	t.Cleanup(func() { a.Stop() })
	return a
}

// checkoutWaitForBlobCount polls the repo until the blob count reaches
// minCount or the timeout expires.
func checkoutWaitForBlobCount(t *testing.T, repoPath string, minCount int, timeout time.Duration) {
	t.Helper()

	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		r, err := repo.Open(repoPath)
		if err != nil {
			time.Sleep(500 * time.Millisecond)
			continue
		}
		var count int
		r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&count)
		r.Close()

		if count >= minCount {
			t.Logf("blob count reached %d (need %d)", count, minCount)
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("blob count did not reach %d within %s", minCount, timeout)
}

// checkoutReadFiles walks a checkout directory and returns a map of
// relative path -> file content. Skips the .fslckout/_FOSSIL_ DB file.
func checkoutReadFiles(t *testing.T, dir string) map[string]string {
	t.Helper()

	result := make(map[string]string)
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		// Skip checkout DB files
		base := filepath.Base(path)
		if base == ".fslckout" || base == "_FOSSIL_" {
			return nil
		}

		rel, err := filepath.Rel(dir, path)
		if err != nil {
			return err
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		result[rel] = string(data)
		return nil
	})
	if err != nil {
		t.Fatalf("checkoutReadFiles: %v", err)
	}
	return result
}

// checkoutMatchProjectCode copies the project-code from srcPath to dstPath
// so both repos belong to the same project for sync purposes.
func checkoutMatchProjectCode(t *testing.T, srcPath, dstPath string) {
	t.Helper()

	src, err := repo.Open(srcPath)
	if err != nil {
		t.Fatalf("open src for project-code: %v", err)
	}
	var projCode string
	src.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	src.Close()

	if projCode == "" {
		t.Fatal("source repo has no project-code")
	}

	dst, err := repo.Open(dstPath)
	if err != nil {
		t.Fatalf("open dst for project-code: %v", err)
	}
	dst.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)
	dst.Close()
}

// checkoutExtract opens the repo, crosslinks, creates a checkout, extracts
// tip, and returns the Checkout (caller must Close).
func checkoutExtract(t *testing.T, repoPath, checkoutDir string) *checkout.Checkout {
	t.Helper()

	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}

	n, err := manifest.Crosslink(r)
	if err != nil {
		r.Close()
		t.Fatalf("manifest.Crosslink: %v", err)
	}
	t.Logf("crosslinked %d manifests", n)

	co, err := checkout.Create(r, checkoutDir, checkout.CreateOpts{})
	if err != nil {
		r.Close()
		t.Fatalf("checkout.Create: %v", err)
	}

	rid, uuid, err := co.Version()
	if err != nil {
		co.Close()
		r.Close()
		t.Fatalf("checkout.Version: %v", err)
	}
	t.Logf("checkout version: rid=%d uuid=%s", rid, uuid[:16])

	if err := co.Extract(rid, checkout.ExtractOpts{}); err != nil {
		co.Close()
		r.Close()
		t.Fatalf("checkout.Extract: %v", err)
	}

	// Stash repo close in cleanup (after co.Close by caller)
	t.Cleanup(func() { r.Close() })

	return co
}

// ---------------------------------------------------------------------------
// Test 1: Checkout After Clone via NATS
// ---------------------------------------------------------------------------

// TestCheckout_AfterCloneViaNATS proves that a repo cloned through NATS
// leaf-to-leaf sync can be checked out and produces byte-identical files.
func TestCheckout_AfterCloneViaNATS(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// Seed files for the source repo.
	seedFiles := map[string]string{
		"README.md":  "# Test Project\n\nThis is a test.\n",
		"src/main.go": "package main\n\nimport \"fmt\"\n\nfunc main() {\n\tfmt.Println(\"hello\")\n}\n",
		"src/lib.go":  "package main\n\nfunc Add(a, b int) int { return a + b }\n",
	}

	// 1. Start embedded NATS.
	natsURL := checkoutStartNATS(t)

	// 2. Create source repo with initial checkin.
	srcPath := checkoutCreateSeededRepo(t, dir, "source.fossil", seedFiles)

	// 3. Create empty destination repo with matching project-code.
	dstPath := filepath.Join(dir, "dest.fossil")
	dstRepo, err := repo.Create(dstPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create dest: %v", err)
	}
	dstRepo.Close()
	checkoutMatchProjectCode(t, srcPath, dstPath)

	// 4. Count source blobs for convergence target.
	srcRepo, err := repo.Open(srcPath)
	if err != nil {
		t.Fatalf("repo.Open src: %v", err)
	}
	var srcBlobCount int
	srcRepo.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&srcBlobCount)
	srcRepo.Close()
	t.Logf("source has %d blobs", srcBlobCount)

	// 5. Start Leaf A (source) — push + serve NATS.
	leafA := checkoutStartAgent(t, srcPath, natsURL, true, true, true)

	// 6. Start Leaf B (dest) — pull only.
	_ = checkoutStartAgent(t, dstPath, natsURL, true, true, false)

	// 7. Wait for convergence.
	checkoutWaitForBlobCount(t, dstPath, srcBlobCount, 30*time.Second)

	// 8. Stop agents before checkout (avoid DB contention).
	leafA.Stop()

	// 9. Crosslink + checkout dest repo.
	coDir := filepath.Join(dir, "checkout")
	co := checkoutExtract(t, dstPath, coDir)
	defer co.Close()

	// 10. Read extracted files and compare.
	gotFiles := checkoutReadFiles(t, coDir)

	if len(gotFiles) != len(seedFiles) {
		t.Fatalf("file count: got %d, want %d", len(gotFiles), len(seedFiles))
	}

	for name, wantContent := range seedFiles {
		got, ok := gotFiles[name]
		if !ok {
			t.Errorf("missing file: %s", name)
			continue
		}
		if got != wantContent {
			t.Errorf("file %s content mismatch:\n  got:  %q\n  want: %q", name, got, wantContent)
		}
	}

	t.Logf("PASS: checkout after NATS clone — %d files byte-identical", len(seedFiles))
}
