package sim

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"

	"github.com/dmestas/edgesync/go-libfossil/checkout"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
	"github.com/dmestas/edgesync/go-libfossil/verify"
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
// Test 2: Commit on A → Sync → Update on B
// ---------------------------------------------------------------------------

// TestCheckout_CommitSyncUpdate proves the full edit cycle: commit on Leaf A,
// sync to Leaf B via NATS, then update Leaf B's checkout to see new content.
func TestCheckout_CommitSyncUpdate(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	seedFiles := map[string]string{
		"doc.txt":    "original document content\n",
		"config.yml": "setting: default\n",
	}

	// 1. Start embedded NATS.
	natsURL := checkoutStartNATS(t)

	// 2. Create Leaf A repo with initial checkin.
	srcPath := checkoutCreateSeededRepo(t, dir, "leafA.fossil", seedFiles)

	// 3. Create Leaf B empty repo with matching project-code.
	dstPath := filepath.Join(dir, "leafB.fossil")
	dstRepo, err := repo.Create(dstPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create leafB: %v", err)
	}
	dstRepo.Close()
	checkoutMatchProjectCode(t, srcPath, dstPath)

	// 4. Count source blobs for convergence.
	srcRepo, err := repo.Open(srcPath)
	if err != nil {
		t.Fatalf("repo.Open leafA: %v", err)
	}
	var srcBlobCount int
	srcRepo.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&srcBlobCount)
	srcRepo.Close()
	t.Logf("leafA has %d blobs", srcBlobCount)

	// 5. Start agents — Leaf A pushes+serves, Leaf B pulls.
	leafA := checkoutStartAgent(t, srcPath, natsURL, true, true, true)
	leafB := checkoutStartAgent(t, dstPath, natsURL, true, true, false)

	// 6. Wait for initial sync convergence on Leaf B.
	checkoutWaitForBlobCount(t, dstPath, srcBlobCount, 30*time.Second)

	// 7. Stop both agents. Extract Leaf A checkout + Leaf B checkout.
	leafA.Stop()
	leafB.Stop()

	coADir := filepath.Join(dir, "checkoutA")
	coA := checkoutExtract(t, srcPath, coADir)

	coBDir := filepath.Join(dir, "checkoutB")
	coB := checkoutExtract(t, dstPath, coBDir)

	// Verify initial sync — both checkouts should match seed files.
	gotB := checkoutReadFiles(t, coBDir)
	for name, want := range seedFiles {
		if gotB[name] != want {
			t.Fatalf("initial sync: file %s mismatch: got %q want %q", name, gotB[name], want)
		}
	}
	t.Log("initial sync verified — Leaf B matches seed files")

	// Close Leaf B checkout for now (will reopen after second sync).
	coB.Close()

	// 8. Modify doc.txt on disk in Leaf A's checkout.
	updatedContent := "updated document content\n"
	docPath := filepath.Join(coADir, "doc.txt")
	if err := os.WriteFile(docPath, []byte(updatedContent), 0o644); err != nil {
		t.Fatalf("write doc.txt: %v", err)
	}

	// 9. ScanChanges + Commit on Leaf A checkout.
	if err := coA.ScanChanges(checkout.ScanHash); err != nil {
		t.Fatalf("ScanChanges: %v", err)
	}

	commitRID, commitUUID, err := coA.Commit(checkout.CommitOpts{
		Message: "update doc.txt",
		User:    "testuser",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	t.Logf("committed rid=%d uuid=%s", commitRID, commitUUID[:16])
	coA.Close()

	// 10. Count blobs again after commit — new commit adds blobs.
	srcRepo2, err := repo.Open(srcPath)
	if err != nil {
		t.Fatalf("repo.Open leafA after commit: %v", err)
	}
	var newBlobCount int
	srcRepo2.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&newBlobCount)
	srcRepo2.Close()
	t.Logf("leafA after commit: %d blobs (was %d)", newBlobCount, srcBlobCount)

	// 11. Restart agents for second sync round.
	leafA = checkoutStartAgent(t, srcPath, natsURL, true, true, true)
	leafB = checkoutStartAgent(t, dstPath, natsURL, true, true, false)

	// 12. Wait for Leaf B to receive new blobs.
	checkoutWaitForBlobCount(t, dstPath, newBlobCount, 30*time.Second)

	// 13. Stop agents before checkout operations.
	leafA.Stop()
	leafB.Stop()

	// 14. Crosslink Leaf B, open checkout, CalcUpdateVersion, Update.
	dstRepo2, err := repo.Open(dstPath)
	if err != nil {
		t.Fatalf("repo.Open leafB for update: %v", err)
	}
	defer dstRepo2.Close()

	n, err := manifest.Crosslink(dstRepo2)
	if err != nil {
		t.Fatalf("crosslink leafB: %v", err)
	}
	t.Logf("crosslinked %d manifests on leafB", n)

	coB2, err := checkout.Open(dstRepo2, coBDir, checkout.OpenOpts{})
	if err != nil {
		t.Fatalf("checkout.Open leafB: %v", err)
	}
	defer coB2.Close()

	updateRID, err := coB2.CalcUpdateVersion()
	if err != nil {
		t.Fatalf("CalcUpdateVersion: %v", err)
	}
	if updateRID == 0 {
		t.Fatal("CalcUpdateVersion returned 0 — expected a newer version")
	}
	t.Logf("update target: rid=%d", updateRID)

	if err := coB2.Update(checkout.UpdateOpts{}); err != nil {
		t.Fatalf("Update: %v", err)
	}

	// 15. Read Leaf B files — doc.txt should have new content, config.yml unchanged.
	gotB2 := checkoutReadFiles(t, coBDir)

	if gotB2["doc.txt"] != updatedContent {
		t.Errorf("doc.txt: got %q, want %q", gotB2["doc.txt"], updatedContent)
	}
	if gotB2["config.yml"] != seedFiles["config.yml"] {
		t.Errorf("config.yml: got %q, want %q", gotB2["config.yml"], seedFiles["config.yml"])
	}

	t.Log("PASS: commit on A → sync → update on B — doc.txt updated, config.yml unchanged")
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

// ---------------------------------------------------------------------------
// Test 3: Concurrent Edit and Sync
// ---------------------------------------------------------------------------

// TestCheckout_ConcurrentEditAndSync proves that checkout operations (open,
// write, manage, scan, commit) don't corrupt the repo when a sync agent is
// running concurrently on the same leaf. Two SQLite connections hit the same
// repo file simultaneously — WAL mode allows this.
func TestCheckout_ConcurrentEditAndSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	seedFiles := map[string]string{
		"base.txt": "base content\n",
	}

	// 1. Start embedded NATS.
	natsURL := checkoutStartNATS(t)

	// 2. Leaf A: create repo with initial checkin, start agent (push + serve).
	srcPath := checkoutCreateSeededRepo(t, dir, "leafA.fossil", seedFiles)
	checkoutStartAgent(t, srcPath, natsURL, true, false, true)

	// 3. Leaf B: create repo with same initial content + matching project-code.
	dstPath := checkoutCreateSeededRepo(t, dir, "leafB.fossil", seedFiles)
	checkoutMatchProjectCode(t, srcPath, dstPath)

	// Create checkout on B, extract tip, then close — proves initial state is good.
	coBDir := filepath.Join(dir, "checkoutB")
	coB := checkoutExtract(t, dstPath, coBDir)
	coB.Close()

	// Start Leaf B agent (push + pull).
	checkoutStartAgent(t, dstPath, natsURL, true, true, false)

	// 4. Seed 5 more checkins on Leaf A to generate sync traffic.
	srcRepo, err := repo.Open(srcPath)
	if err != nil {
		t.Fatalf("repo.Open leafA for extra checkins: %v", err)
	}
	for i := 1; i <= 5; i++ {
		fname := fmt.Sprintf("extra-%d.txt", i)
		_, _, err := manifest.Checkin(srcRepo, manifest.CheckinOpts{
			Files: []manifest.File{{
				Name:    fname,
				Content: []byte(fmt.Sprintf("extra content %d\n", i)),
			}},
			Comment: fmt.Sprintf("extra commit %d", i),
			User:    "testuser",
		})
		if err != nil {
			srcRepo.Close()
			t.Fatalf("manifest.Checkin extra-%d: %v", i, err)
		}
	}
	srcRepo.Close()
	t.Log("seeded 5 extra checkins on Leaf A")

	// 5. Wait 2 seconds to let sync start pulling on B.
	time.Sleep(2 * time.Second)

	// 6. While B's agent is running: open a SECOND repo handle, checkout, write,
	//    manage, scan, commit — concurrent with the agent's sync connection.
	bRepo, err := repo.Open(dstPath)
	if err != nil {
		t.Fatalf("repo.Open leafB for concurrent edit: %v", err)
	}

	coB2, err := checkout.Open(bRepo, coBDir, checkout.OpenOpts{})
	if err != nil {
		bRepo.Close()
		t.Fatalf("checkout.Open leafB concurrent: %v", err)
	}

	// Write a new file into the checkout directory.
	localEditPath := filepath.Join(coBDir, "local-edit.txt")
	if err := os.WriteFile(localEditPath, []byte("local edit while syncing\n"), 0o644); err != nil {
		coB2.Close()
		bRepo.Close()
		t.Fatalf("write local-edit.txt: %v", err)
	}

	// Manage the new file (add to vfile tracking).
	_, err = coB2.Manage(checkout.ManageOpts{
		Paths: []string{"local-edit.txt"},
	})
	if err != nil {
		coB2.Close()
		bRepo.Close()
		t.Fatalf("Manage local-edit.txt: %v", err)
	}

	// Scan for changes and commit.
	if err := coB2.ScanChanges(checkout.ScanHash); err != nil {
		coB2.Close()
		bRepo.Close()
		t.Fatalf("ScanChanges: %v", err)
	}

	commitRID, commitUUID, err := coB2.Commit(checkout.CommitOpts{
		Message: "local edit while sync running",
		User:    "testuser",
	})
	if err != nil {
		coB2.Close()
		bRepo.Close()
		t.Fatalf("Commit concurrent: %v", err)
	}
	t.Logf("concurrent commit: rid=%d uuid=%s", commitRID, commitUUID[:16])

	coB2.Close()
	bRepo.Close()

	// 7. Wait for convergence (5 seconds).
	time.Sleep(5 * time.Second)

	// 8. Verify: open B's repo, crosslink synced blobs, run verify.Verify.
	vRepo, err := repo.Open(dstPath)
	if err != nil {
		t.Fatalf("repo.Open leafB for verify: %v", err)
	}

	n, err := manifest.Crosslink(vRepo)
	if err != nil {
		vRepo.Close()
		t.Fatalf("crosslink leafB before verify: %v", err)
	}
	t.Logf("crosslinked %d manifests on leafB before verify", n)

	report, err := verify.Verify(vRepo)
	if err != nil {
		vRepo.Close()
		t.Fatalf("verify.Verify: %v", err)
	}
	if !report.OK() {
		for _, issue := range report.Issues {
			t.Logf("verify issue: %s (rid=%d uuid=%s)", issue.Message, issue.RID, issue.UUID)
		}
		vRepo.Close()
		t.Fatalf("repo corrupt: %d issues found", len(report.Issues))
	}
	t.Logf("verify clean: %d blobs checked, %d OK", report.BlobsChecked, report.BlobsOK)

	// 9. Verify: the committed blob UUID exists in the blob table.
	var found int
	vRepo.DB().QueryRow("SELECT count(*) FROM blob WHERE uuid=?", commitUUID).Scan(&found)
	vRepo.Close()

	if found != 1 {
		t.Fatalf("committed blob uuid=%s not found in B's blob table", commitUUID[:16])
	}

	t.Log("PASS: concurrent edit + sync — no corruption, local commit preserved")
}

// ---------------------------------------------------------------------------
// Test 4: Fossil Interop (both directions)
// ---------------------------------------------------------------------------

// TestCheckout_FossilInterop proves Go checkout reads Fossil-created repos AND
// Fossil reads Go-created checkouts. Both directions in one test.
func TestCheckout_FossilInterop(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "interop.fossil")

	// Files that Fossil will commit.
	wantHello := "Hello from Fossil!\n"
	wantApp := "package main\n\nfunc main() {}\n"

	// Go's modified version.
	wantHelloModified := "Hello from Go checkout!\n"
	goCommitMsg := "modify hello.txt via Go checkout"

	// Track the Go commit UUID across subtests.
	var goCommitUUID string

	t.Run("fossil_to_go", func(t *testing.T) {
		// 1. fossil new
		cmd := exec.Command("fossil", "new", repoPath)
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil new: %v\n%s", err, out)
		}
		t.Logf("fossil new: %s", strings.TrimSpace(string(out)))

		// 2. fossil open + write files + add + commit
		workDir := filepath.Join(dir, "fossil-work")
		if err := os.MkdirAll(workDir, 0o755); err != nil {
			t.Fatalf("mkdir fossil-work: %v", err)
		}

		cmd = exec.Command("fossil", "open", repoPath)
		cmd.Dir = workDir
		out, err = cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil open: %v\n%s", err, out)
		}

		// Write hello.txt
		if err := os.WriteFile(filepath.Join(workDir, "hello.txt"), []byte(wantHello), 0o644); err != nil {
			t.Fatalf("write hello.txt: %v", err)
		}

		// Write src/app.go (subdirectory)
		srcDir := filepath.Join(workDir, "src")
		if err := os.MkdirAll(srcDir, 0o755); err != nil {
			t.Fatalf("mkdir src: %v", err)
		}
		if err := os.WriteFile(filepath.Join(srcDir, "app.go"), []byte(wantApp), 0o644); err != nil {
			t.Fatalf("write src/app.go: %v", err)
		}

		cmd = exec.Command("fossil", "add", ".")
		cmd.Dir = workDir
		out, err = cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil add: %v\n%s", err, out)
		}

		cmd = exec.Command("fossil", "commit", "-m", "initial fossil commit", "--no-warnings")
		cmd.Dir = workDir
		out, err = cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil commit: %v\n%s", err, out)
		}
		t.Logf("fossil commit: %s", strings.TrimSpace(string(out)))

		// 3. fossil close --force
		cmd = exec.Command("fossil", "close", "--force")
		cmd.Dir = workDir
		out, err = cmd.CombinedOutput()
		if err != nil {
			t.Logf("fossil close: %v\n%s", err, out)
		}

		// 4. repo.Open the fossil-created repo
		r, err := repo.Open(repoPath)
		if err != nil {
			t.Fatalf("repo.Open: %v", err)
		}

		// Crosslink to populate event/mlink tables.
		n, err := manifest.Crosslink(r)
		if err != nil {
			r.Close()
			t.Fatalf("manifest.Crosslink: %v", err)
		}
		t.Logf("crosslinked %d manifests", n)

		// 5. checkout.Create, get Version, Extract
		coDir := filepath.Join(dir, "go-checkout")
		co, err := checkout.Create(r, coDir, checkout.CreateOpts{})
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

		// 6. Read files — assert they match what Fossil committed.
		gotFiles := checkoutReadFiles(t, coDir)
		if gotFiles["hello.txt"] != wantHello {
			t.Errorf("hello.txt: got %q, want %q", gotFiles["hello.txt"], wantHello)
		}
		if gotFiles["src/app.go"] != wantApp {
			t.Errorf("src/app.go: got %q, want %q", gotFiles["src/app.go"], wantApp)
		}
		t.Log("Fossil -> Go: files match")

		// 7. Modify hello.txt on disk, ScanChanges, Commit via Go.
		if err := os.WriteFile(filepath.Join(coDir, "hello.txt"), []byte(wantHelloModified), 0o644); err != nil {
			co.Close()
			r.Close()
			t.Fatalf("write modified hello.txt: %v", err)
		}

		if err := co.ScanChanges(checkout.ScanHash); err != nil {
			co.Close()
			r.Close()
			t.Fatalf("ScanChanges: %v", err)
		}

		commitRID, commitUUID, err := co.Commit(checkout.CommitOpts{
			Message: goCommitMsg,
			User:    "testuser",
		})
		if err != nil {
			co.Close()
			r.Close()
			t.Fatalf("Commit: %v", err)
		}
		t.Logf("Go commit: rid=%d uuid=%s", commitRID, commitUUID[:16])

		// Save UUID for subtest B.
		goCommitUUID = commitUUID

		// 8. Close checkout + repo.
		co.Close()
		r.Close()
	})

	t.Run("go_to_fossil", func(t *testing.T) {
		if goCommitUUID == "" {
			t.Fatal("no Go commit UUID from previous subtest")
		}

		// 9. fossil rebuild — proves Go's commit is structurally valid.
		tr := testutil.NewTestRepoFromPath(t, repoPath)
		tr.FossilRebuild(t)
		t.Log("fossil rebuild succeeded — Go commit structurally valid")

		// 10. fossil open in a new directory.
		workDir2 := filepath.Join(dir, "fossil-verify")
		if err := os.MkdirAll(workDir2, 0o755); err != nil {
			t.Fatalf("mkdir fossil-verify: %v", err)
		}

		cmd := exec.Command("fossil", "open", repoPath)
		cmd.Dir = workDir2
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil open: %v\n%s", err, out)
		}
		t.Logf("fossil open: %s", strings.TrimSpace(string(out)))

		// 11. Read hello.txt — assert it has Go's changes.
		gotHello, err := os.ReadFile(filepath.Join(workDir2, "hello.txt"))
		if err != nil {
			t.Fatalf("read hello.txt: %v", err)
		}
		if string(gotHello) != wantHelloModified {
			t.Errorf("hello.txt: got %q, want %q", gotHello, wantHelloModified)
		}
		t.Log("Go -> Fossil: hello.txt has Go's changes")

		// 12. fossil artifact <go-commit-uuid> — manifest contains Go's commit message.
		// Fossil encodes spaces as \s in the C (comment) card, so check both forms.
		artifact := tr.FossilArtifact(t, goCommitUUID)
		artifactStr := string(artifact)
		escapedMsg := strings.ReplaceAll(goCommitMsg, " ", `\s`)
		if !strings.Contains(artifactStr, goCommitMsg) && !strings.Contains(artifactStr, escapedMsg) {
			t.Errorf("artifact does not contain commit message %q (or escaped %q):\n%s",
				goCommitMsg, escapedMsg, artifactStr)
		}
		t.Logf("artifact contains Go commit message: %q", goCommitMsg)

		// Clean up: close the fossil checkout.
		closeCmd := exec.Command("fossil", "close", "--force")
		closeCmd.Dir = workDir2
		closeCmd.CombinedOutput()

		t.Log("PASS: Go -> Fossil interop — rebuild, checkout, and artifact all valid")
	})
}
