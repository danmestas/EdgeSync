# Checkout Sim Integration Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add 4 integration tests to `sim/checkout_test.go` proving checkout works end-to-end with real NATS, real disk I/O, and real Fossil.

**Architecture:** Single test file with shared helpers. Each test follows: setup infrastructure → seed repos → sync → checkout operations → assertions. Uses embedded NATS, leaf agents, manifest.Crosslink, and the checkout package.

**Tech Stack:** Go, embedded NATS (`nats-server/v2`), `leaf/agent`, `go-libfossil/checkout`, `fossil` binary (Test 4 only)

**Spec:** `docs/superpowers/specs/2026-03-24-checkout-sim-tests-design.md`

**Branch:** `feature/cdg-162-checkout-sim-tests` in `.worktrees/checkout-sim`

---

## File Structure

| Action | Path | Purpose |
|--------|------|---------|
| Create | `sim/checkout_test.go` | All 4 tests + shared helpers |

---

### Task 1: Shared Helpers + Test 1 (Checkout After Clone)

**Files:**
- Create: `sim/checkout_test.go`

This task creates the file with all shared helpers and the first test. The helpers are reused by all 4 tests.

- [ ] **Step 1: Create sim/checkout_test.go with helpers + Test 1**

Create `sim/checkout_test.go` with these helpers and the first test:

```go
package sim

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"

	"github.com/dmestas/edgesync/go-libfossil/checkout"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/leaf/agent"
)

// --- Shared Helpers ---

// checkoutStartNATS starts an embedded NATS server and returns its URL.
func checkoutStartNATS(t *testing.T) string {
	t.Helper()
	ns, err := natsserver.NewServer(&natsserver.Options{Port: -1})
	if err != nil {
		t.Fatalf("nats server: %v", err)
	}
	ns.Start()
	t.Cleanup(func() { ns.Shutdown() })
	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats not ready")
	}
	return ns.ClientURL()
}

// checkoutCreateSeededRepo creates a repo with an initial checkin containing
// the given files. Returns the repo path (repo is closed after seeding).
func checkoutCreateSeededRepo(t *testing.T, dir, name string, files []manifest.File) string {
	t.Helper()
	path := filepath.Join(dir, name)
	r, err := repo.Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create %s: %v", name, err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: "initial checkin",
		User:    "testuser",
	})
	if err != nil {
		r.Close()
		t.Fatalf("Checkin: %v", err)
	}
	r.Close()
	return path
}

// checkoutStartAgent creates and starts a leaf agent. Caller must defer Stop().
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
	})
	if err != nil {
		t.Fatalf("agent.New: %v", err)
	}
	if err := a.Start(); err != nil {
		t.Fatalf("agent.Start: %v", err)
	}
	return a
}

// checkoutWaitForBlobCount polls until repoPath has >= minCount non-phantom blobs.
// Fails the test if timeout is exceeded.
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
			return
		}
		time.Sleep(500 * time.Millisecond)
	}
	t.Fatalf("timeout: repo %s did not reach %d blobs", repoPath, minCount)
}

// checkoutReadFiles reads all files from a checkout directory into a map.
// Skips checkout DB files (.fslckout, _FOSSIL_).
func checkoutReadFiles(t *testing.T, dir string) map[string]string {
	t.Helper()
	files := make(map[string]string)
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			name := info.Name()
			if name == ".git" || name == ".hg" || name == ".svn" {
				return filepath.SkipDir
			}
			return nil
		}
		name := info.Name()
		if name == ".fslckout" || name == "_FOSSIL_" {
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
		files[rel] = string(data)
		return nil
	})
	if err != nil {
		t.Fatalf("walk checkout dir: %v", err)
	}
	return files
}

// checkoutMatchProjectCode copies Leaf A's project-code to Leaf B's repo.
func checkoutMatchProjectCode(t *testing.T, srcPath, dstPath string) {
	t.Helper()
	src, err := repo.Open(srcPath)
	if err != nil {
		t.Fatal(err)
	}
	var projCode string
	src.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	src.Close()

	dst, err := repo.Open(dstPath)
	if err != nil {
		t.Fatal(err)
	}
	dst.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)
	dst.Close()
}

// checkoutExtract opens a checkout, extracts files, returns the Checkout.
// Caller must defer co.Close().
func checkoutExtract(t *testing.T, repoPath, checkoutDir string) *checkout.Checkout {
	t.Helper()
	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close() })

	// Crosslink to populate event/plink/mlink from synced blobs.
	if _, err := manifest.Crosslink(r); err != nil {
		t.Fatalf("Crosslink: %v", err)
	}

	co, err := checkout.Create(r, checkoutDir, checkout.CreateOpts{})
	if err != nil {
		t.Fatalf("checkout.Create: %v", err)
	}

	rid, _, err := co.Version()
	if err != nil {
		co.Close()
		t.Fatal(err)
	}
	if err := co.Extract(rid, checkout.ExtractOpts{}); err != nil {
		co.Close()
		t.Fatalf("Extract: %v", err)
	}
	return co
}

// --- Test 1: Checkout After Clone via NATS ---

func TestCheckout_AfterCloneViaNATS(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	expectedFiles := map[string]string{
		"README.md":    "# Hello World\n",
		"src/main.go":  "package main\n\nfunc main() {}\n",
		"src/lib.go":   "package main\n\nfunc helper() {}\n",
	}

	seedFiles := make([]manifest.File, 0, len(expectedFiles))
	for name, content := range expectedFiles {
		seedFiles = append(seedFiles, manifest.File{Name: name, Content: []byte(content)})
	}

	natsURL := checkoutStartNATS(t)
	dir := t.TempDir()

	// Leaf A: seed repo with files, start agent serving over NATS.
	leafAPath := checkoutCreateSeededRepo(t, dir, "leaf-a.fossil", seedFiles)
	agentA := checkoutStartAgent(t, leafAPath, natsURL, true, false, true)
	defer agentA.Stop()

	// Leaf B: empty repo with same project-code.
	leafBPath := filepath.Join(dir, "leaf-b.fossil")
	rB, err := repo.Create(leafBPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	rB.Close()
	checkoutMatchProjectCode(t, leafAPath, leafBPath)

	// Start Leaf B pulling.
	agentB := checkoutStartAgent(t, leafBPath, natsURL, false, true, false)

	// Wait for convergence — Leaf A has seed blobs, count them.
	rA, _ := repo.Open(leafAPath)
	var blobCountA int
	rA.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobCountA)
	rA.Close()

	checkoutWaitForBlobCount(t, leafBPath, blobCountA, 30*time.Second)
	agentB.Stop()

	// Checkout on Leaf B.
	coDir := filepath.Join(dir, "checkout-b")
	co := checkoutExtract(t, leafBPath, coDir)
	defer co.Close()

	// Assert files match.
	actualFiles := checkoutReadFiles(t, coDir)
	for name, want := range expectedFiles {
		got, ok := actualFiles[name]
		if !ok {
			t.Errorf("missing file %q", name)
			continue
		}
		if got != want {
			t.Errorf("file %q: got %q, want %q", name, got, want)
		}
	}
	t.Logf("PASS: %d files checked out correctly after NATS clone", len(expectedFiles))
}
```

- [ ] **Step 2: Verify it compiles and runs**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./sim/ -v -run TestCheckout_AfterCloneViaNATS -count=1`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add sim/checkout_test.go
git commit -m "test(sim): add checkout after NATS clone test + shared helpers"
```

---

### Task 2: Test 2 — Commit → Sync → Update Round-Trip

**Files:**
- Modify: `sim/checkout_test.go`

- [ ] **Step 1: Add Test 2**

Add to `sim/checkout_test.go`:

```go
// --- Test 2: Commit → Sync → Update Round-Trip ---

func TestCheckout_CommitSyncUpdate(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	initialFiles := []manifest.File{
		{Name: "doc.txt", Content: []byte("version one")},
		{Name: "config.yml", Content: []byte("key: value")},
	}

	natsURL := checkoutStartNATS(t)
	dir := t.TempDir()

	// Leaf A: seed repo, create checkout, extract.
	leafAPath := checkoutCreateSeededRepo(t, dir, "leaf-a.fossil", initialFiles)
	coADir := filepath.Join(dir, "checkout-a")
	coA := checkoutExtract(t, leafAPath, coADir)

	// Start Leaf A agent.
	agentA := checkoutStartAgent(t, leafAPath, natsURL, true, true, true)

	// Leaf B: empty repo, match project-code, pull, crosslink, checkout.
	leafBPath := filepath.Join(dir, "leaf-b.fossil")
	rB, err := repo.Create(leafBPath, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	rB.Close()
	checkoutMatchProjectCode(t, leafAPath, leafBPath)

	agentB := checkoutStartAgent(t, leafBPath, natsURL, true, true, false)

	// Wait for initial sync.
	rA, _ := repo.Open(leafAPath)
	var blobCountA int
	rA.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobCountA)
	rA.Close()
	checkoutWaitForBlobCount(t, leafBPath, blobCountA, 30*time.Second)

	// Create Leaf B checkout.
	agentB.Stop()
	coBDir := filepath.Join(dir, "checkout-b")
	coB := checkoutExtract(t, leafBPath, coBDir)

	// Leaf A: modify a file, commit.
	agentA.Stop()
	if err := os.WriteFile(filepath.Join(coADir, "doc.txt"), []byte("version two"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := coA.ScanChanges(checkout.ScanHash); err != nil {
		t.Fatal(err)
	}
	newRID, _, err := coA.Commit(checkout.CommitOpts{
		Message: "update doc.txt",
		User:    "alice",
	})
	if err != nil {
		t.Fatal("Commit:", err)
	}
	coA.Close()
	t.Logf("Leaf A committed rid=%d", newRID)

	// Restart agents to sync the new commit.
	agentA = checkoutStartAgent(t, leafAPath, natsURL, true, true, true)
	defer agentA.Stop()
	agentB = checkoutStartAgent(t, leafBPath, natsURL, true, true, false)

	// Wait for Leaf B to get the new blob.
	rA, _ = repo.Open(leafAPath)
	rA.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&blobCountA)
	rA.Close()
	checkoutWaitForBlobCount(t, leafBPath, blobCountA, 30*time.Second)
	agentB.Stop()

	// Crosslink on Leaf B to make new checkin visible.
	rB2, err := repo.Open(leafBPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := manifest.Crosslink(rB2); err != nil {
		t.Fatal("Crosslink:", err)
	}
	rB2.Close()

	// Update Leaf B checkout.
	updateRID, err := coB.CalcUpdateVersion()
	if err != nil {
		t.Fatal(err)
	}
	if updateRID == 0 {
		t.Fatal("CalcUpdateVersion returned 0 — no newer version found")
	}
	if err := coB.Update(checkout.UpdateOpts{}); err != nil {
		t.Fatal("Update:", err)
	}
	coB.Close()

	// Assert Leaf B has the updated content.
	actualFiles := checkoutReadFiles(t, coBDir)
	if got := actualFiles["doc.txt"]; got != "version two" {
		t.Fatalf("doc.txt: got %q, want %q", got, "version two")
	}
	if got := actualFiles["config.yml"]; got != "key: value" {
		t.Fatalf("config.yml: got %q, want %q", got, "key: value")
	}
	t.Log("PASS: commit → sync → update round-trip successful")
}
```

Add `"os"` to the import block if not already present.

- [ ] **Step 2: Run test**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./sim/ -v -run TestCheckout_CommitSyncUpdate -count=1 -timeout 60s`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add sim/checkout_test.go
git commit -m "test(sim): add commit → sync → update round-trip test"
```

---

### Task 3: Test 3 — Concurrent Edit + Sync

**Files:**
- Modify: `sim/checkout_test.go`

- [ ] **Step 1: Add Test 3**

Add to `sim/checkout_test.go`:

```go
import (
	"github.com/dmestas/edgesync/go-libfossil/verify"
)

// --- Test 3: Concurrent Edit + Sync ---

func TestCheckout_ConcurrentEditAndSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	initialFiles := []manifest.File{
		{Name: "base.txt", Content: []byte("base content")},
	}

	natsURL := checkoutStartNATS(t)
	dir := t.TempDir()

	// Leaf A: seed repo, start agent pushing + serving.
	leafAPath := checkoutCreateSeededRepo(t, dir, "leaf-a.fossil", initialFiles)
	agentA := checkoutStartAgent(t, leafAPath, natsURL, true, true, true)
	defer agentA.Stop()

	// Leaf B: same initial content, create checkout, start agent pulling.
	leafBPath := checkoutCreateSeededRepo(t, dir, "leaf-b.fossil", initialFiles)
	checkoutMatchProjectCode(t, leafAPath, leafBPath)

	coBDir := filepath.Join(dir, "checkout-b")
	coB := checkoutExtract(t, leafBPath, coBDir)
	coB.Close() // close checkout — will reopen after agent starts

	agentB := checkoutStartAgent(t, leafBPath, natsURL, true, true, false)

	// Seed more checkins on Leaf A to keep sync active.
	rA, err := repo.Open(leafAPath)
	if err != nil {
		t.Fatal(err)
	}
	var parentRID int64
	rA.DB().QueryRow("SELECT MAX(rid) FROM blob WHERE size >= 0").Scan(&parentRID)
	for i := 0; i < 5; i++ {
		manifest.Checkin(rA, manifest.CheckinOpts{
			Files: []manifest.File{
				{Name: "base.txt", Content: []byte("base content")},
				{Name: strings.ReplaceAll("extra_N.txt", "N", fmt.Sprintf("%d", i)),
					Content: []byte(fmt.Sprintf("extra content %d", i))},
			},
			Comment: fmt.Sprintf("extra checkin %d", i),
			User:    "alice",
		})
	}
	rA.Close()

	// While sync is running on B, commit a local change.
	time.Sleep(2 * time.Second) // let sync start pulling

	rB, err := repo.Open(leafBPath)
	if err != nil {
		t.Fatal(err)
	}
	coB2, err := checkout.Open(rB, coBDir, checkout.OpenOpts{})
	if err != nil {
		rB.Close()
		t.Fatal("checkout.Open:", err)
	}

	// Write new file, manage, scan, commit.
	newFilePath := filepath.Join(coBDir, "local-edit.txt")
	if err := os.WriteFile(newFilePath, []byte("local change"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := coB2.Manage(checkout.ManageOpts{Paths: []string{"local-edit.txt"}}); err != nil {
		t.Fatal("Manage:", err)
	}
	if err := coB2.ScanChanges(checkout.ScanHash); err != nil {
		t.Fatal("ScanChanges:", err)
	}
	localRID, localUUID, err := coB2.Commit(checkout.CommitOpts{
		Message: "local edit during sync",
		User:    "bob",
	})
	if err != nil {
		t.Fatal("Commit during sync:", err)
	}
	coB2.Close()
	rB.Close()
	t.Logf("Committed local edit: rid=%d uuid=%s", localRID, localUUID)

	// Wait for convergence.
	time.Sleep(5 * time.Second)
	agentB.Stop()

	// Verify: no corruption on Leaf B.
	rB2, err := repo.Open(leafBPath)
	if err != nil {
		t.Fatal(err)
	}
	vReport, err := verify.Verify(rB2)
	rB2.Close()
	if err != nil {
		t.Fatal("verify:", err)
	}
	if !vReport.OK() {
		for _, iss := range vReport.Issues {
			t.Logf("issue: %s", iss.Message)
		}
		t.Fatalf("Leaf B corrupted: %d issues", len(vReport.Issues))
	}

	// Verify: Leaf B's commit blob exists.
	rB3, err := repo.Open(leafBPath)
	if err != nil {
		t.Fatal(err)
	}
	var exists int
	rB3.DB().QueryRow("SELECT count(*) FROM blob WHERE uuid=?", localUUID).Scan(&exists)
	rB3.Close()
	if exists == 0 {
		t.Fatal("local commit blob missing from Leaf B")
	}

	t.Log("PASS: concurrent edit + sync — no corruption, local commit preserved")
}
```

Add `"fmt"` and `"strings"` to the import block. Add `"github.com/dmestas/edgesync/go-libfossil/verify"` to imports.

- [ ] **Step 2: Run test**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./sim/ -v -run TestCheckout_ConcurrentEditAndSync -count=1 -timeout 60s`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add sim/checkout_test.go
git commit -m "test(sim): add concurrent edit + sync test"
```

---

### Task 4: Test 4 — Fossil Interop (Both Directions)

**Files:**
- Modify: `sim/checkout_test.go`

- [ ] **Step 1: Add Test 4**

Add to `sim/checkout_test.go`:

```go
import (
	"os/exec"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

// --- Test 4: Fossil Interop ---

func TestCheckout_FossilInterop(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "interop.fossil")

	// --- Subtest A: Fossil → Go ---
	t.Run("fossil_to_go", func(t *testing.T) {
		// Create repo via fossil.
		cmd := exec.Command("fossil", "new", repoPath)
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("fossil new: %v\n%s", err, out)
		}

		// Open + commit files via fossil.
		fossilWorkDir := filepath.Join(dir, "fossil-work")
		os.MkdirAll(fossilWorkDir, 0o755)
		cmd = exec.Command("fossil", "open", repoPath)
		cmd.Dir = fossilWorkDir
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("fossil open: %v\n%s", err, out)
		}

		// Write files.
		os.WriteFile(filepath.Join(fossilWorkDir, "hello.txt"), []byte("hello from fossil"), 0o644)
		os.MkdirAll(filepath.Join(fossilWorkDir, "src"), 0o755)
		os.WriteFile(filepath.Join(fossilWorkDir, "src", "app.go"), []byte("package main\n"), 0o644)

		cmd = exec.Command("fossil", "add", ".")
		cmd.Dir = fossilWorkDir
		cmd.CombinedOutput()

		cmd = exec.Command("fossil", "commit", "-m", "fossil commit", "--no-warnings")
		cmd.Dir = fossilWorkDir
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("fossil commit: %v\n%s", err, out)
		}

		// Close fossil checkout.
		cmd = exec.Command("fossil", "close", "--force")
		cmd.Dir = fossilWorkDir
		cmd.CombinedOutput()

		// Open via Go and extract.
		goWorkDir := filepath.Join(dir, "go-checkout")
		r, err := repo.Open(repoPath)
		if err != nil {
			t.Fatal(err)
		}

		co, err := checkout.Create(r, goWorkDir, checkout.CreateOpts{})
		if err != nil {
			r.Close()
			t.Fatal(err)
		}
		rid, _, err := co.Version()
		if err != nil {
			co.Close()
			r.Close()
			t.Fatal(err)
		}
		if err := co.Extract(rid, checkout.ExtractOpts{}); err != nil {
			co.Close()
			r.Close()
			t.Fatal("Extract:", err)
		}

		// Verify files.
		files := checkoutReadFiles(t, goWorkDir)
		if got := files["hello.txt"]; got != "hello from fossil" {
			t.Fatalf("hello.txt: got %q, want %q", got, "hello from fossil")
		}
		if got := files[filepath.Join("src", "app.go")]; got != "package main\n" {
			t.Fatalf("src/app.go: got %q, want %q", got, "package main\n")
		}
		t.Log("PASS: Go reads Fossil-created checkout correctly")

		// --- Subtest B: Go → Fossil ---
		// Modify a file via Go checkout, commit.
		os.WriteFile(filepath.Join(goWorkDir, "hello.txt"), []byte("hello from go"), 0o644)
		if err := co.ScanChanges(checkout.ScanHash); err != nil {
			t.Fatal(err)
		}
		_, goUUID, err := co.Commit(checkout.CommitOpts{
			Message: "go commit",
			User:    "testuser",
		})
		if err != nil {
			t.Fatal("Go Commit:", err)
		}
		co.Close()
		r.Close()
		t.Logf("Go committed: uuid=%s", goUUID)

		// fossil rebuild — proves Go's commit is structurally valid.
		if err := testutil.FossilRebuild(repoPath); err != nil {
			t.Fatal("fossil rebuild after Go commit:", err)
		}

		// fossil open in new dir — verify Go's changes visible.
		fossilVerifyDir := filepath.Join(dir, "fossil-verify")
		os.MkdirAll(fossilVerifyDir, 0o755)
		cmd = exec.Command("fossil", "open", repoPath)
		cmd.Dir = fossilVerifyDir
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("fossil open after Go commit: %v\n%s", err, out)
		}

		data, err := os.ReadFile(filepath.Join(fossilVerifyDir, "hello.txt"))
		if err != nil {
			t.Fatal(err)
		}
		if string(data) != "hello from go" {
			t.Fatalf("fossil sees %q, want %q", data, "hello from go")
		}

		// fossil artifact on Go's commit UUID.
		tr := testutil.NewTestRepoFromPath(t, repoPath)
		artifact := tr.FossilArtifact(t, goUUID)
		if !strings.Contains(string(artifact), "go commit") {
			t.Fatalf("fossil artifact doesn't contain Go's commit message: %s", artifact)
		}
		t.Log("PASS: Fossil reads Go-created commit correctly")
	})
}
```

- [ ] **Step 2: Run test**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./sim/ -v -run TestCheckout_FossilInterop -count=1 -timeout 60s`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add sim/checkout_test.go
git commit -m "test(sim): add Fossil interop test (both directions)"
```

---

### Task 5: Run Full Test Suite

**Files:** None (verification only)

- [ ] **Step 1: Run all checkout sim tests**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./sim/ -v -run 'TestCheckout_' -count=1 -timeout 120s`
Expected: ALL 4 PASS

- [ ] **Step 2: Run all sim tests for regressions**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./sim/ -count=1 -timeout 120s`
Expected: ALL PASS

- [ ] **Step 3: Run go-libfossil tests**

Run: `cd .worktrees/checkout-sim && go test -buildvcs=false ./go-libfossil/... -count=1`
Expected: ALL PASS

- [ ] **Step 4: Build check**

Run: `cd .worktrees/checkout-sim && go build -buildvcs=false ./...`
Expected: builds clean
