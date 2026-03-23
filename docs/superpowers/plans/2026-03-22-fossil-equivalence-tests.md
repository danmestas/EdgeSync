# Fossil-Equivalent Validation Tests Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add integration tests that validate the leaf agent as a fully functional fossil equivalent by round-tripping commits through sync and verifying with `fossil open`.

**Architecture:** One new test file `sim/equivalence_test.go` with three topology tests (leaf→fossil, fossil→leaf, leaf→leaf), each running three commit-complexity sub-tests. Shared helpers for fossil checkout and file assertion.

**Tech Stack:** Go testing, `fossil` binary, `manifest.Checkin`, `sync.ServeHTTP`, `sync.Sync`, `sync.Clone`, `manifest.Crosslink`

**Spec:** `docs/superpowers/specs/2026-03-22-fossil-equivalence-tests-design.md`

---

### Task 1: Test helpers

**Files:**
- Create: `sim/equivalence_test.go`

Write the shared helpers and a minimal smoke test to verify they work. All subsequent tasks build on these.

- [ ] **Step 1: Create equivalence_test.go with helpers**

```go
package sim

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
)

// checkinFiles is a convenience type for test data.
type checkinFiles struct {
	files   []manifest.File
	comment string
}

// fossilCheckout copies the repo to a temp path, runs `fossil open`, and
// returns the working directory. The caller can read checked-out files from it.
func fossilCheckout(t *testing.T, repoPath string) string {
	t.Helper()

	// Copy repo so fossil open doesn't modify the original.
	dir := t.TempDir()
	copyPath := filepath.Join(dir, "repo.fossil")
	data, err := os.ReadFile(repoPath)
	if err != nil {
		t.Fatalf("read repo for copy: %v", err)
	}
	if err := os.WriteFile(copyPath, data, 0644); err != nil {
		t.Fatalf("write repo copy: %v", err)
	}

	workDir := filepath.Join(dir, "checkout")
	if err := os.MkdirAll(workDir, 0755); err != nil {
		t.Fatalf("mkdir checkout: %v", err)
	}

	cmd := exec.Command("fossil", "open", copyPath)
	cmd.Dir = workDir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil open failed: %v\n%s", err, out)
	}
	return workDir
}

// assertFiles verifies that every expected file exists in dir with the
// expected content. Fails the test with a clear message on mismatch.
func assertFiles(t *testing.T, dir string, expected map[string]string) {
	t.Helper()
	for name, want := range expected {
		path := filepath.Join(dir, name)
		got, err := os.ReadFile(path)
		if err != nil {
			t.Errorf("file %q: %v", name, err)
			continue
		}
		if string(got) != want {
			t.Errorf("file %q: got %q, want %q", name, got, want)
		}
	}
}

// requireFossil skips the test if the fossil binary is not in PATH.
func requireFossil(t *testing.T) {
	t.Helper()
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
}

// leafRepo creates a new repo via repo.Create and returns it.
// The repo is closed on test cleanup.
func leafRepo(t *testing.T, dir, name string) *repo.Repo {
	t.Helper()
	path := filepath.Join(dir, name)
	r, err := repo.Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create %s: %v", name, err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

// checkin calls manifest.Checkin with the given files and returns the manifest RID.
func checkin(t *testing.T, r *repo.Repo, parent int64, files []manifest.File, comment string) int64 {
	t.Helper()
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: comment,
		User:    "testuser",
		Parent:  int64ToFslID(parent),
		Time:    time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	return int64(rid)
}

// int64ToFslID converts for the Parent field (0 means no parent).
func int64ToFslID(v int64) libfossil.FslID {
	return libfossil.FslID(v)
}

// serveLeafHTTP starts sync.ServeHTTP for the given repo and returns
// the address and a cancel function.
func serveLeafHTTP(t *testing.T, r *repo.Repo) (string, context.CancelFunc) {
	t.Helper()
	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	go sync.ServeHTTP(ctx, addr, r, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)
	return addr, cancel
}
```

Note: `fmt` is used in later tasks. Remove any unused imports that the compiler flags.

- [ ] **Step 2: Add a minimal smoke test to verify helpers compile**

```go
func TestEquivalenceSmoke(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	dir := t.TempDir()
	r := leafRepo(t, dir, "smoke.fossil")
	checkin(t, r, 0, []manifest.File{
		{Name: "smoke.txt", Content: []byte("smoke test")},
	}, "smoke checkin")

	// Close and reopen so fossil can read it.
	r.Close()
	r2, err := repo.Open(filepath.Join(dir, "smoke.fossil"))
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer r2.Close()

	workDir := fossilCheckout(t, r2.Path())
	assertFiles(t, workDir, map[string]string{"smoke.txt": "smoke test"})
}
```

- [ ] **Step 3: Run the smoke test**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run TestEquivalenceSmoke -v -timeout=30s`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add sim/equivalence_test.go
git commit -m "test: add fossil-equivalence test helpers and smoke test

Shared helpers for fossil checkout, file assertion, and repo setup.
Validates that manifest.Checkin → fossil open → file read works.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Topology A — Leaf → Fossil (via clone)

**Files:**
- Modify: `sim/equivalence_test.go`

- [ ] **Step 1: Add TestLeafToFossil with three sub-tests**

```go
// TestLeafToFossil creates commits via manifest.Checkin, serves over HTTP,
// clones with real `fossil clone`, then verifies file contents via `fossil open`.
func TestLeafToFossil(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	t.Run("single_file", func(t *testing.T) {
		testLeafToFossil(t, []checkinFiles{
			{
				files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world")}},
				comment: "single file checkin",
			},
		}, map[string]string{"hello.txt": "hello world"})
	})

	t.Run("multi_file", func(t *testing.T) {
		testLeafToFossil(t, []checkinFiles{
			{
				files: []manifest.File{
					{Name: "a.txt", Content: []byte("alpha")},
					{Name: "b.txt", Content: []byte("bravo")},
					{Name: "c.txt", Content: []byte("charlie")},
				},
				comment: "multi file checkin",
			},
		}, map[string]string{"a.txt": "alpha", "b.txt": "bravo", "c.txt": "charlie"})
	})

	t.Run("commit_chain", func(t *testing.T) {
		testLeafToFossil(t, []checkinFiles{
			{
				files:   []manifest.File{{Name: "base.txt", Content: []byte("base content")}},
				comment: "parent commit",
			},
			{
				files: []manifest.File{
					{Name: "base.txt", Content: []byte("base content")},
					{Name: "added.txt", Content: []byte("new in child")},
				},
				comment: "child commit",
			},
		}, map[string]string{"base.txt": "base content", "added.txt": "new in child"})
	})
}

func testLeafToFossil(t *testing.T, checkins []checkinFiles, expected map[string]string) {
	t.Helper()
	dir := t.TempDir()

	// 1. Create leaf repo and do checkins.
	src := leafRepo(t, dir, "src.fossil")
	var parentRid int64
	for _, ci := range checkins {
		parentRid = checkin(t, src, parentRid, ci.files, ci.comment)
	}
	src.Close()

	// 2. Reopen and serve over HTTP.
	srcReopened, err := repo.Open(filepath.Join(dir, "src.fossil"))
	if err != nil {
		t.Fatalf("reopen src: %v", err)
	}
	defer srcReopened.Close()

	addr, cancel := serveLeafHTTP(t, srcReopened)
	defer cancel()

	// 3. fossil clone from our HTTP server.
	clonePath := filepath.Join(dir, "clone.fossil")
	cmd := exec.Command("fossil", "clone", fmt.Sprintf("http://%s", addr), clonePath)
	out, err := cmd.CombinedOutput()
	t.Logf("fossil clone:\n%s", out)
	if err != nil {
		t.Fatalf("fossil clone: %v", err)
	}

	// 4. fossil open + verify files.
	workDir := fossilCheckout(t, clonePath)
	assertFiles(t, workDir, expected)
}
```

- [ ] **Step 2: Run the test**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run TestLeafToFossil -v -timeout=60s`
Expected: ALL 3 sub-tests PASS

- [ ] **Step 3: Commit**

```bash
git add sim/equivalence_test.go
git commit -m "test: add Topology A — leaf → fossil clone → open → verify files

Validates manifest.Checkin marks blobs for sync correctly by
cloning via real fossil binary and checking out file contents.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Topology B — Fossil → Leaf (via sync.Clone)

**Files:**
- Modify: `sim/equivalence_test.go`

This topology uses the real fossil binary to create commits, then `sync.Clone` to pull into a leaf repo.

- [ ] **Step 1: Add fossil init/commit helpers**

```go
// fossilInit creates a new fossil repo and returns its path.
func fossilInit(t *testing.T, dir, name string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	cmd := exec.Command("fossil", "init", path)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil init: %v\n%s", err, out)
	}
	// Add nobody user with capabilities for unauthenticated sync.
	exec.Command("fossil", "user", "new", "nobody", "", "cghijknorswz", "-R", path).Run()
	exec.Command("fossil", "user", "capabilities", "nobody", "cghijknorswz", "-R", path).Run()
	return path
}

// fossilCommitFiles opens a fossil repo, writes files, adds, and commits.
// Returns the working directory (caller can reuse for chained commits).
func fossilCommitFiles(t *testing.T, repoPath string, workDir string, files map[string]string, comment string) string {
	t.Helper()

	if workDir == "" {
		workDir = filepath.Join(t.TempDir(), "fossil-work")
		os.MkdirAll(workDir, 0755)
		cmd := exec.Command("fossil", "open", repoPath)
		cmd.Dir = workDir
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil open: %v\n%s", err, out)
		}
	}

	for name, content := range files {
		fpath := filepath.Join(workDir, name)
		os.MkdirAll(filepath.Dir(fpath), 0755)
		if err := os.WriteFile(fpath, []byte(content), 0644); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
	}

	addCmd := exec.Command("fossil", "add", ".")
	addCmd.Dir = workDir
	addCmd.CombinedOutput() // ignore "ADDED" output

	commitCmd := exec.Command("fossil", "commit", "-m", comment, "--no-warnings")
	commitCmd.Dir = workDir
	out, err := commitCmd.CombinedOutput()
	t.Logf("fossil commit:\n%s", out)
	if err != nil {
		t.Fatalf("fossil commit: %v", err)
	}
	return workDir
}

// startFossilServe starts `fossil server` on a free port and returns the URL.
func startFossilServe(t *testing.T, repoPath string) string {
	t.Helper()
	addr := freeAddr(t)
	_, portStr, _ := net.SplitHostPort(addr)

	cmd := exec.Command("fossil", "server", "--port="+portStr, repoPath)
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	if err := cmd.Start(); err != nil {
		t.Fatalf("fossil server: %v", err)
	}
	t.Cleanup(func() {
		cmd.Process.Kill()
		cmd.Wait()
		// Clean up WAL/SHM files so t.TempDir() cleanup doesn't fail.
		dir := filepath.Dir(repoPath)
		entries, _ := os.ReadDir(dir)
		for _, e := range entries {
			n := e.Name()
			if strings.HasSuffix(n, "-wal") || strings.HasSuffix(n, "-shm") || strings.HasSuffix(n, "-journal") {
				os.Remove(filepath.Join(dir, n))
			}
		}
	})
	waitForAddr(t, addr, 5*time.Second)
	return fmt.Sprintf("http://%s", addr)
}
```

Add `"net"` and `"strings"` to the import block.

- [ ] **Step 2: Add TestFossilToLeaf with three sub-tests**

```go
// TestFossilToLeaf creates commits with real fossil, serves them, clones
// into a leaf repo via sync.Clone, then verifies with fossil open.
func TestFossilToLeaf(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	t.Run("single_file", func(t *testing.T) {
		testFossilToLeaf(t,
			[]map[string]string{{"hello.txt": "hello world"}},
			[]string{"single file"},
			map[string]string{"hello.txt": "hello world"},
		)
	})

	t.Run("multi_file", func(t *testing.T) {
		testFossilToLeaf(t,
			[]map[string]string{{"a.txt": "alpha", "b.txt": "bravo", "c.txt": "charlie"}},
			[]string{"multi file"},
			map[string]string{"a.txt": "alpha", "b.txt": "bravo", "c.txt": "charlie"},
		)
	})

	t.Run("commit_chain", func(t *testing.T) {
		testFossilToLeaf(t,
			[]map[string]string{
				{"base.txt": "base content"},
				{"base.txt": "base content", "added.txt": "new in child"},
			},
			[]string{"parent commit", "child commit"},
			map[string]string{"base.txt": "base content", "added.txt": "new in child"},
		)
	})
}

func testFossilToLeaf(t *testing.T, commits []map[string]string, messages []string, expected map[string]string) {
	t.Helper()
	dir := t.TempDir()

	// 1. Create fossil repo and commit files.
	fossilRepoPath := fossilInit(t, dir, "fossil-src.fossil")
	var workDir string
	for i, files := range commits {
		workDir = fossilCommitFiles(t, fossilRepoPath, workDir, files, messages[i])
	}

	// Close the fossil checkout to release locks.
	closeCmd := exec.Command("fossil", "close")
	closeCmd.Dir = workDir
	closeCmd.Run()

	// 2. Start fossil serve.
	serverURL := startFossilServe(t, fossilRepoPath)

	// 3. Clone into a leaf repo via sync.Clone.
	leafPath := filepath.Join(dir, "leaf.fossil")
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	transport := &sync.HTTPTransport{URL: serverURL}
	leafRepo, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
	if err != nil {
		t.Fatalf("sync.Clone: %v", err)
	}
	leafRepo.Close()

	// 4. fossil open the leaf repo + verify files.
	leafWorkDir := fossilCheckout(t, leafPath)
	assertFiles(t, leafWorkDir, expected)
}
```

- [ ] **Step 3: Run the test**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run TestFossilToLeaf -v -timeout=60s`
Expected: ALL 3 sub-tests PASS

- [ ] **Step 4: Commit**

```bash
git add sim/equivalence_test.go
git commit -m "test: add Topology B — fossil commit → fossil serve → sync.Clone → verify

Validates sync.Clone produces repos where fossil open can check
out all committed files.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Topology C — Leaf → Leaf (via sync.Sync)

**Files:**
- Modify: `sim/equivalence_test.go`

- [ ] **Step 1: Add TestLeafToLeaf with three sub-tests**

```go
// TestLeafToLeaf creates commits on leaf A via manifest.Checkin, syncs to
// leaf B via sync.Sync over HTTP, crosslinks leaf B, then verifies with fossil open.
func TestLeafToLeaf(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	t.Run("single_file", func(t *testing.T) {
		testLeafToLeaf(t, []checkinFiles{
			{
				files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world")}},
				comment: "single file",
			},
		}, map[string]string{"hello.txt": "hello world"})
	})

	t.Run("multi_file", func(t *testing.T) {
		testLeafToLeaf(t, []checkinFiles{
			{
				files: []manifest.File{
					{Name: "a.txt", Content: []byte("alpha")},
					{Name: "b.txt", Content: []byte("bravo")},
					{Name: "c.txt", Content: []byte("charlie")},
				},
				comment: "multi file",
			},
		}, map[string]string{"a.txt": "alpha", "b.txt": "bravo", "c.txt": "charlie"})
	})

	t.Run("commit_chain", func(t *testing.T) {
		testLeafToLeaf(t, []checkinFiles{
			{
				files:   []manifest.File{{Name: "base.txt", Content: []byte("base content")}},
				comment: "parent",
			},
			{
				files: []manifest.File{
					{Name: "base.txt", Content: []byte("base content")},
					{Name: "added.txt", Content: []byte("new in child")},
				},
				comment: "child",
			},
		}, map[string]string{"base.txt": "base content", "added.txt": "new in child"})
	})
}

func testLeafToLeaf(t *testing.T, checkins []checkinFiles, expected map[string]string) {
	t.Helper()
	dir := t.TempDir()

	// 1. Create leaf A, do checkins.
	leafA := leafRepo(t, dir, "leaf-a.fossil")
	var parentRid int64
	for _, ci := range checkins {
		parentRid = checkin(t, leafA, parentRid, ci.files, ci.comment)
	}

	// Read project/server codes from leaf A.
	var projCode, srvCode string
	leafA.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	leafA.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

	// 2. Create leaf B (empty, same project code).
	leafB := leafRepo(t, dir, "leaf-b.fossil")
	leafB.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)

	// 3. Serve leaf A over HTTP.
	leafA.Close()
	leafAReopened, err := repo.Open(filepath.Join(dir, "leaf-a.fossil"))
	if err != nil {
		t.Fatalf("reopen leaf-a: %v", err)
	}
	defer leafAReopened.Close()

	addr, cancel := serveLeafHTTP(t, leafAReopened)
	defer cancel()

	// 4. Sync leaf B from leaf A.
	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	ctx := context.Background()
	for round := 0; round < 10; round++ {
		result, err := sync.Sync(ctx, leafB, transport, sync.SyncOpts{
			Push:        true,
			Pull:        true,
			ProjectCode: projCode,
			ServerCode:  srvCode,
		})
		if err != nil {
			t.Fatalf("sync round %d: %v", round, err)
		}
		if result.FilesSent == 0 && result.FilesRecvd == 0 {
			break
		}
	}

	// 5. Crosslink leaf B (sync.Sync doesn't crosslink, unlike sync.Clone).
	n, err := manifest.Crosslink(leafB)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	t.Logf("crosslinked %d manifests", n)

	// 6. fossil open + verify.
	leafB.Close()
	leafBWorkDir := fossilCheckout(t, filepath.Join(dir, "leaf-b.fossil"))
	assertFiles(t, leafBWorkDir, expected)
}
```

- [ ] **Step 2: Run the test**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run TestLeafToLeaf/single_file -v -timeout=30s`
Expected: PASS

Then run all sub-tests:
Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run TestLeafToLeaf -v -timeout=60s`
Expected: ALL 3 sub-tests PASS

- [ ] **Step 3: Commit**

```bash
git add sim/equivalence_test.go
git commit -m "test: add Topology C — leaf checkin → sync.Sync → leaf → fossil open → verify

Validates the full Go-to-Go sync path: manifest.Checkin marks blobs,
sync.Sync transfers them, Crosslink links manifests, fossil open
checks out correct file contents.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Final verification

- [ ] **Step 1: Run all equivalence tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./sim/ -run 'TestEquivalence|TestLeafToFossil|TestFossilToLeaf|TestLeafToLeaf' -v -timeout=120s`
Expected: ALL 10 tests PASS (1 smoke + 3×3 topology/complexity)

- [ ] **Step 2: Run full test suite to verify no regressions**

Run: `cd /Users/dmestas/projects/EdgeSync && make test`
Expected: ALL PASS

- [ ] **Step 3: Run make dst**

Run: `cd /Users/dmestas/projects/EdgeSync && make dst`
Expected: ALL 8 seeds PASS
