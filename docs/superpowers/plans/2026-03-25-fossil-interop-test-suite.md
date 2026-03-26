# Fossil Interop Test Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `sim/interop_test.go` — a comprehensive Fossil-as-oracle test suite that cross-checks every codec operation against the real `fossil` binary.

**Architecture:** Single test file with shared helpers. Two tiers: Tier 1 (fast seed tests, always run) and Tier 2 (large Fossil SCM repo, skipped in `-short`). Every test follows the pattern: do X with Go, verify with Fossil CLI (or vice versa). `verifyAllBlobs` is the core invariant — expand every blob, hash it, assert matches UUID.

**Tech Stack:** Go testing, `os/exec` for fossil CLI, existing sim helpers (`fossilInit`, `fossilCommitFiles`, `startFossilServe`, `serveLeafHTTP`, `requireFossil`, `freeAddr`, `waitForAddr`)

**Spec:** `docs/superpowers/specs/2026-03-25-fossil-interop-test-suite-design.md`

---

### Task 1: Shared Helpers

**Files:**
- Create: `sim/interop_test.go`

The foundation — `verifyAllBlobs`, `verifyWithFossilRebuild`, `fossilExec`, and `cloneFossilSCMRepo`. All subsequent tasks depend on these.

- [ ] **Step 1: Create `sim/interop_test.go` with helpers**

```go
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

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/uv"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
)

// verifyAllBlobs expands every non-phantom blob in the database, computes its
// hash, and asserts it matches the stored UUID. This is the single most valuable
// interop check — it would have caught the delta swap bug, the checksum bug,
// and any future content corruption.
func verifyAllBlobs(t *testing.T, d *db.DB) {
	t.Helper()
	rows, err := d.Query("SELECT rid, uuid, size FROM blob WHERE size >= 0 AND content IS NOT NULL")
	if err != nil {
		t.Fatalf("verifyAllBlobs query: %v", err)
	}
	type blobRow struct {
		rid  libfossil.FslID
		uuid string
		size int64
	}
	var blobs []blobRow
	for rows.Next() {
		var b blobRow
		rows.Scan(&b.rid, &b.uuid, &b.size)
		blobs = append(blobs, b)
	}
	rows.Close()

	if len(blobs) == 0 {
		t.Fatal("verifyAllBlobs: no blobs found")
	}

	var failures []string
	for _, b := range blobs {
		expanded, err := content.Expand(d, b.rid)
		if err != nil {
			failures = append(failures, fmt.Sprintf("rid=%d uuid=%s: expand: %v", b.rid, b.uuid[:16], err))
			continue
		}
		var computed string
		if len(b.uuid) > 40 {
			computed = hash.SHA3(expanded)
		} else {
			computed = hash.SHA1(expanded)
		}
		if computed != b.uuid {
			failures = append(failures, fmt.Sprintf("rid=%d uuid=%s: hash mismatch: got %s", b.rid, b.uuid[:16], computed[:16]))
		}
	}

	if len(failures) > 0 {
		t.Errorf("verifyAllBlobs: %d/%d blobs failed:\n  %s", len(failures), len(blobs), strings.Join(failures, "\n  "))
	} else {
		t.Logf("verifyAllBlobs: all %d blobs verified", len(blobs))
	}
}

// verifyWithFossilRebuild runs `fossil rebuild` on a repo file. Fossil's rebuild
// recomputes every content hash from scratch — the ultimate integrity check.
func verifyWithFossilRebuild(t *testing.T, repoPath string) {
	t.Helper()
	out, err := exec.Command("fossil", "rebuild", repoPath).CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild %s: %v\n%s", filepath.Base(repoPath), err, out)
	}
	t.Logf("fossil rebuild OK: %s", filepath.Base(repoPath))
}

// fossilExec runs a fossil command and returns stdout. Skips the test if
// fossil is not in PATH. Fails the test on non-zero exit.
func fossilExec(t *testing.T, args ...string) string {
	t.Helper()
	requireFossil(t)
	cmd := exec.Command("fossil", args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil %s: %v\n%s", strings.Join(args, " "), err, out)
	}
	return string(out)
}

// cloneFossilSCMRepo returns the path to testdata/fossil.fossil, cloning from
// upstream if needed. Skips the test if offline or clone fails.
func cloneFossilSCMRepo(t *testing.T) string {
	t.Helper()
	// Walk up from sim/ to project root.
	repoPath := filepath.Join("..", "testdata", "fossil.fossil")
	if abs, err := filepath.Abs(repoPath); err == nil {
		repoPath = abs
	}

	if _, err := os.Stat(repoPath); err == nil {
		return repoPath
	}

	// Ensure testdata/ exists.
	dir := filepath.Dir(repoPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Skipf("cannot create testdata dir: %v", err)
	}

	t.Log("Cloning Fossil SCM repo (first run, ~30s)...")
	cmd := exec.Command("fossil", "clone", "https://fossil-scm.org/home", repoPath)
	cmd.Stdout = os.Stderr
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		t.Skipf("fossil clone failed (offline?): %v", err)
	}
	return repoPath
}
```

- [ ] **Step 2: Verify it compiles**

Run: `go build -buildvcs=false ./sim/`
Expected: clean compile (no tests run yet, just helpers)

- [ ] **Step 3: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop test helpers — verifyAllBlobs, verifyWithFossilRebuild"
```

---

### Task 2: Delta Codec Tests

**Files:**
- Modify: `sim/interop_test.go`

Three subtests that cross-check delta.Create/Apply against `fossil test-delta-create`/`fossil test-delta-apply`.

- [ ] **Step 1: Add `TestInterop/delta_codec` tests**

Append to `sim/interop_test.go`:

```go
func TestInterop(t *testing.T) {
	requireFossil(t)

	t.Run("delta_codec", func(t *testing.T) {
		t.Run("fossil_creates_we_apply", func(t *testing.T) {
			dir := t.TempDir()
			source := []byte(strings.Repeat("The quick brown fox jumps over the lazy dog. ", 100))
			target := make([]byte, len(source))
			copy(target, source)
			copy(target[500:], []byte("FOSSIL-CREATED DELTA VERIFIED BY GO"))

			srcFile := filepath.Join(dir, "src.bin")
			tgtFile := filepath.Join(dir, "tgt.bin")
			deltaFile := filepath.Join(dir, "delta.bin")
			os.WriteFile(srcFile, source, 0644)
			os.WriteFile(tgtFile, target, 0644)

			fossilExec(t, "test-delta-create", srcFile, tgtFile, deltaFile)
			fossilDelta, err := os.ReadFile(deltaFile)
			if err != nil {
				t.Fatal(err)
			}

			got, err := delta.Apply(source, fossilDelta)
			if err != nil {
				t.Fatalf("delta.Apply on fossil-created delta: %v", err)
			}
			if string(got) != string(target) {
				t.Fatalf("output mismatch: got %d bytes, want %d", len(got), len(target))
			}
		})

		t.Run("we_create_fossil_applies", func(t *testing.T) {
			dir := t.TempDir()
			source := []byte(strings.Repeat("abcdefghijklmnop", 500))
			target := make([]byte, len(source))
			copy(target, source)
			copy(target[2000:], []byte("GO-CREATED DELTA VERIFIED BY FOSSIL"))

			srcFile := filepath.Join(dir, "src.bin")
			deltaFile := filepath.Join(dir, "delta.bin")
			resultFile := filepath.Join(dir, "result.bin")
			os.WriteFile(srcFile, source, 0644)
			os.WriteFile(deltaFile, delta.Create(source, target), 0644)

			fossilExec(t, "test-delta-apply", srcFile, deltaFile, resultFile)
			got, err := os.ReadFile(resultFile)
			if err != nil {
				t.Fatal(err)
			}
			if string(got) != string(target) {
				t.Fatalf("fossil could not apply our delta: got %d bytes, want %d", len(got), len(target))
			}
		})

		t.Run("round_trip_large_payload", func(t *testing.T) {
			dir := t.TempDir()
			rng := rand.New(rand.NewSource(42))

			// 64KB random-ish data (text-like so delta can find matches)
			base := make([]byte, 65536)
			for i := range base {
				base[i] = byte('A' + rng.Intn(26))
			}
			modified := make([]byte, len(base))
			copy(modified, base)
			// Change ~5% of content in scattered locations
			for i := 0; i < 3000; i++ {
				pos := rng.Intn(len(modified))
				modified[pos] = byte('a' + rng.Intn(26))
			}

			srcFile := filepath.Join(dir, "src.bin")
			tgtFile := filepath.Join(dir, "tgt.bin")
			os.WriteFile(srcFile, base, 0644)
			os.WriteFile(tgtFile, modified, 0644)

			// Direction 1: fossil creates, we apply
			deltaFile1 := filepath.Join(dir, "fossil.delta")
			fossilExec(t, "test-delta-create", srcFile, tgtFile, deltaFile1)
			fossilDelta, _ := os.ReadFile(deltaFile1)
			got1, err := delta.Apply(base, fossilDelta)
			if err != nil {
				t.Fatalf("Apply fossil delta (64KB): %v", err)
			}
			if string(got1) != string(modified) {
				t.Fatal("fossil→Go mismatch on 64KB payload")
			}

			// Direction 2: we create, fossil applies
			deltaFile2 := filepath.Join(dir, "go.delta")
			resultFile := filepath.Join(dir, "result.bin")
			os.WriteFile(deltaFile2, delta.Create(base, modified), 0644)
			fossilExec(t, "test-delta-apply", srcFile, deltaFile2, resultFile)
			got2, _ := os.ReadFile(resultFile)
			if string(got2) != string(modified) {
				t.Fatal("Go→fossil mismatch on 64KB payload")
			}
		})
	})
```

**Important:** Close `TestInterop` with a `}` after the `delta_codec` block for now. Each subsequent task replaces this closing brace — insert the new `t.Run` block before it.

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -v -run 'TestInterop/delta_codec' ./sim/ -timeout=60s`
Expected: 3 PASS subtests

- [ ] **Step 3: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop delta codec tests — bidirectional fossil CLI cross-check"
```

---

### Task 3: Clone-From-Us Tests

**Files:**
- Modify: `sim/interop_test.go`

Tests that `fossil clone` + `fossil rebuild` can consume our Go-served repos.

- [ ] **Step 1: Add `clone_from_us` subtests**

Insert inside `TestInterop` after the `delta_codec` block:

```go
	t.Run("clone_from_us", func(t *testing.T) {
		t.Run("single_commit", func(t *testing.T) {
			dir := t.TempDir()
			r := leafRepo(t, dir, "src.fossil")
			checkin(t, r, 0, []manifest.File{
				{Name: "hello.txt", Content: []byte("hello interop")},
				{Name: "data.bin", Content: []byte(strings.Repeat("binary-ish content\x00\xff", 200))},
			}, "initial commit")
			manifest.Crosslink(r)
			r.Close()

			r2, _ := repo.Open(filepath.Join(dir, "src.fossil"))
			defer r2.Close()
			addr, cancel := serveLeafHTTP(t, r2)
			defer cancel()

			clonePath := filepath.Join(dir, "clone.fossil")
			fossilExec(t, "clone", fmt.Sprintf("http://%s", addr), clonePath)
			verifyWithFossilRebuild(t, clonePath)

			workDir := fossilCheckout(t, clonePath)
			assertFiles(t, workDir, map[string]string{"hello.txt": "hello interop"})
		})

		t.Run("commit_chain", func(t *testing.T) {
			dir := t.TempDir()
			r := leafRepo(t, dir, "src.fossil")
			var parent int64
			for i := 0; i < 5; i++ {
				parent = checkin(t, r, parent, []manifest.File{
					{Name: "counter.txt", Content: []byte(fmt.Sprintf("version %d with enough padding to make deltas interesting %s", i, strings.Repeat(".", 200)))},
					{Name: fmt.Sprintf("file-%d.txt", i), Content: []byte(fmt.Sprintf("added in commit %d", i))},
				}, fmt.Sprintf("commit %d", i))
			}
			manifest.Crosslink(r)
			r.Close()

			r2, _ := repo.Open(filepath.Join(dir, "src.fossil"))
			defer r2.Close()
			addr, cancel := serveLeafHTTP(t, r2)
			defer cancel()

			clonePath := filepath.Join(dir, "clone.fossil")
			fossilExec(t, "clone", fmt.Sprintf("http://%s", addr), clonePath)
			verifyWithFossilRebuild(t, clonePath)
		})

		t.Run("with_uv_files", func(t *testing.T) {
			dir := t.TempDir()
			r := leafRepo(t, dir, "src.fossil")
			checkin(t, r, 0, []manifest.File{
				{Name: "readme.txt", Content: []byte("repo with UV files")},
			}, "initial")
			manifest.Crosslink(r)

			// Store UV files including binary content.
			rng := rand.New(rand.NewSource(99))
			binaryContent := make([]byte, 4096)
			rng.Read(binaryContent)
			uv.Write(r, "text-file.txt", []byte("hello UV"))
			uv.Write(r, "binary-file.bin", binaryContent)
			r.Close()

			r2, _ := repo.Open(filepath.Join(dir, "src.fossil"))
			defer r2.Close()
			addr, cancel := serveLeafHTTP(t, r2)
			defer cancel()

			clonePath := filepath.Join(dir, "clone.fossil")
			fossilExec(t, "clone", fmt.Sprintf("http://%s", addr), clonePath)

			// Verify UV files via fossil CLI.
			uvList := fossilExec(t, "uv", "list", "-R", clonePath)
			if !strings.Contains(uvList, "text-file.txt") || !strings.Contains(uvList, "binary-file.bin") {
				t.Fatalf("uv list missing files: %s", uvList)
			}
			uvText := fossilExec(t, "uv", "cat", "text-file.txt", "-R", clonePath)
			if uvText != "hello UV" {
				t.Fatalf("uv cat text-file.txt = %q, want %q", uvText, "hello UV")
			}
		})
	})
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -v -run 'TestInterop/clone_from_us' ./sim/ -timeout=60s`
Expected: 3 PASS subtests

- [ ] **Step 3: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop clone-from-us tests — fossil clone + rebuild + UV"
```

---

### Task 4: Clone-From-Fossil Tests

**Files:**
- Modify: `sim/interop_test.go`

Tests that `sync.Clone` from `fossil serve` works, including delta-compressed cfile cards. Uses `verifyAllBlobs` as the primary check.

- [ ] **Step 1: Add `clone_from_fossil` subtests**

Insert inside `TestInterop`:

```go
	t.Run("clone_from_fossil", func(t *testing.T) {
		t.Run("single_commit", func(t *testing.T) {
			dir := t.TempDir()
			fossilRepo := fossilInit(t, dir, "src.fossil")
			fossilCommitFiles(t, fossilRepo, "", map[string]string{
				"hello.txt": "hello from fossil",
			}, "single commit")

			serverURL := startFossilServe(t, fossilRepo)
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()

			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}
			defer leafR.Close()

			verifyAllBlobs(t, leafR.DB())
		})

		t.Run("commit_chain_with_deltas", func(t *testing.T) {
			dir := t.TempDir()
			fossilRepo := fossilInit(t, dir, "src.fossil")
			var workDir string
			for i := 0; i < 5; i++ {
				workDir = fossilCommitFiles(t, fossilRepo, workDir, map[string]string{
					"counter.txt": fmt.Sprintf("version %d with padding %s", i, strings.Repeat("x", 300)),
					fmt.Sprintf("file-%d.txt", i): fmt.Sprintf("added in commit %d", i),
				}, fmt.Sprintf("commit %d", i))
			}

			serverURL := startFossilServe(t, fossilRepo)
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()

			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}
			defer leafR.Close()

			verifyAllBlobs(t, leafR.DB())
		})

		t.Run("expand_and_verify_all", func(t *testing.T) {
			dir := t.TempDir()
			fossilRepo := fossilInit(t, dir, "src.fossil")
			var workDir string
			// Larger files to force delta creation during fossil rebuild.
			bigContent := strings.Repeat("The quick brown fox jumps over the lazy dog.\n", 100)
			for i := 0; i < 5; i++ {
				workDir = fossilCommitFiles(t, fossilRepo, workDir, map[string]string{
					"big.txt": fmt.Sprintf("%sVersion %d\n", bigContent, i),
				}, fmt.Sprintf("commit %d", i))
			}

			serverURL := startFossilServe(t, fossilRepo)
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()

			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}
			leafR.Close()

			// Also verify fossil itself can read our stored blobs.
			verifyWithFossilRebuild(t, leafPath)

			// Reopen and verify all blobs.
			leafR2, err := repo.Open(leafPath)
			if err != nil {
				t.Fatal(err)
			}
			defer leafR2.Close()
			verifyAllBlobs(t, leafR2.DB())
		})
	})
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -v -run 'TestInterop/clone_from_fossil' ./sim/ -timeout=60s`
Expected: 3 PASS subtests

- [ ] **Step 3: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop clone-from-fossil tests — verifyAllBlobs on cfile deltas"
```

---

### Task 5: Incremental Sync Tests

**Files:**
- Modify: `sim/interop_test.go`

Tests sync after initial clone — the production steady-state path.

- [ ] **Step 1: Add `incremental_sync` subtests**

Insert inside `TestInterop`:

```go
	t.Run("incremental_sync", func(t *testing.T) {
		t.Run("fossil_commits_we_pull", func(t *testing.T) {
			dir := t.TempDir()
			fossilRepo := fossilInit(t, dir, "src.fossil")
			workDir := fossilCommitFiles(t, fossilRepo, "", map[string]string{
				"base.txt": "initial content",
			}, "initial")

			serverURL := startFossilServe(t, fossilRepo)

			// Clone into leaf.
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx := context.Background()
			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("Clone: %v", err)
			}

			// Fossil makes 3 more commits.
			for i := 0; i < 3; i++ {
				workDir = fossilCommitFiles(t, fossilRepo, workDir, map[string]string{
					"base.txt":                      fmt.Sprintf("updated %d", i),
					fmt.Sprintf("new-%d.txt", i): fmt.Sprintf("new file %d", i),
				}, fmt.Sprintf("post-clone commit %d", i))
			}

			// Sync to pull new artifacts.
			var projCode string
			leafR.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
			for round := 0; round < 10; round++ {
				result, err := sync.Sync(ctx, leafR, transport, sync.SyncOpts{
					Pull:        true,
					ProjectCode: projCode,
				})
				if err != nil {
					t.Fatalf("sync round %d: %v", round, err)
				}
				if result.FilesRecvd == 0 {
					break
				}
			}
			leafR.Close()

			leafR2, _ := repo.Open(leafPath)
			defer leafR2.Close()
			verifyAllBlobs(t, leafR2.DB())
		})

		t.Run("we_push_fossil_pulls", func(t *testing.T) {
			dir := t.TempDir()
			fossilRepo := fossilInit(t, dir, "src.fossil")
			fossilCommitFiles(t, fossilRepo, "", map[string]string{
				"base.txt": "initial",
			}, "initial")

			serverURL := startFossilServe(t, fossilRepo)

			// Clone into leaf.
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx := context.Background()
			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("Clone: %v", err)
			}

			// Add blobs to the leaf.
			rng := rand.New(rand.NewSource(77))
			uuids, err := SeedLeaf(leafR, rng, 3, 2048)
			if err != nil {
				t.Fatal(err)
			}

			// Push to fossil.
			var projCode string
			leafR.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
			for round := 0; round < 10; round++ {
				result, err := sync.Sync(ctx, leafR, transport, sync.SyncOpts{
					Push:        true,
					ProjectCode: projCode,
				})
				if err != nil {
					t.Fatalf("sync round %d: %v", round, err)
				}
				if result.FilesSent == 0 {
					break
				}
			}
			leafR.Close()

			// Verify fossil can read the pushed blobs.
			for _, uuid := range uuids {
				fossilExec(t, "artifact", uuid, "-R", fossilRepo)
			}
			verifyWithFossilRebuild(t, fossilRepo)
		})

		t.Run("bidirectional", func(t *testing.T) {
			dir := t.TempDir()
			fossilRepo := fossilInit(t, dir, "src.fossil")
			workDir := fossilCommitFiles(t, fossilRepo, "", map[string]string{
				"base.txt": "initial",
			}, "initial")

			serverURL := startFossilServe(t, fossilRepo)

			// Clone into leaf.
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx := context.Background()
			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("Clone: %v", err)
			}

			// Both sides add content.
			fossilCommitFiles(t, fossilRepo, workDir, map[string]string{
				"from-fossil.txt": "fossil added this",
			}, "fossil-side commit")

			rng := rand.New(rand.NewSource(88))
			SeedLeaf(leafR, rng, 3, 1024)

			// Sync until convergence.
			var projCode string
			leafR.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
			for round := 0; round < 15; round++ {
				result, err := sync.Sync(ctx, leafR, transport, sync.SyncOpts{
					Push:        true,
					Pull:        true,
					ProjectCode: projCode,
				})
				if err != nil {
					t.Fatalf("sync round %d: %v", round, err)
				}
				if result.FilesSent == 0 && result.FilesRecvd == 0 {
					break
				}
			}
			leafR.Close()

			// Both sides pass integrity checks.
			verifyWithFossilRebuild(t, fossilRepo)
			leafR2, _ := repo.Open(leafPath)
			defer leafR2.Close()
			verifyAllBlobs(t, leafR2.DB())
		})
	})
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -v -run 'TestInterop/incremental_sync' ./sim/ -timeout=120s`
Expected: 3 PASS subtests

- [ ] **Step 3: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop incremental sync tests — pull, push, bidirectional"
```

---

### Task 6: Hash Compat Tests

**Files:**
- Modify: `sim/interop_test.go`

Cross-check SHA1 and SHA3 against `fossil sha1sum` / `fossil sha3sum`.

- [ ] **Step 1: Add `hash_compat` subtests**

Insert inside `TestInterop`:

```go
	t.Run("hash_compat", func(t *testing.T) {
		t.Run("sha1_vs_fossil", func(t *testing.T) {
			dir := t.TempDir()
			rng := rand.New(rand.NewSource(1))
			sizes := []int{0, 1, 15, 16, 17, 255, 256, 257, 1024, 65536}

			for _, sz := range sizes {
				data := make([]byte, sz)
				if sz > 0 {
					rng.Read(data)
				}
				path := filepath.Join(dir, fmt.Sprintf("data_%d.bin", sz))
				os.WriteFile(path, data, 0644)

				goHash := hash.SHA1(data)
				fossilOut := strings.TrimSpace(fossilExec(t, "sha1sum", path))
				// fossil sha1sum outputs: HASH  FILENAME
				fossilHash := strings.Fields(fossilOut)[0]

				if goHash != fossilHash {
					t.Errorf("SHA1 mismatch at size %d: go=%s fossil=%s", sz, goHash, fossilHash)
				}
			}
		})

		t.Run("sha3_vs_fossil", func(t *testing.T) {
			dir := t.TempDir()
			rng := rand.New(rand.NewSource(2))
			sizes := []int{0, 1, 15, 16, 17, 255, 256, 257, 1024, 65536}

			for _, sz := range sizes {
				data := make([]byte, sz)
				if sz > 0 {
					rng.Read(data)
				}
				path := filepath.Join(dir, fmt.Sprintf("data_%d.bin", sz))
				os.WriteFile(path, data, 0644)

				goHash := hash.SHA3(data)
				fossilOut := strings.TrimSpace(fossilExec(t, "sha3sum", path))
				fossilHash := strings.Fields(fossilOut)[0]

				if goHash != fossilHash {
					t.Errorf("SHA3 mismatch at size %d: go=%s fossil=%s", sz, goHash, fossilHash)
				}
			}
		})
	})
```

- [ ] **Step 2: Run tests**

Run: `go test -buildvcs=false -v -run 'TestInterop/hash_compat' ./sim/ -timeout=30s`
Expected: 2 PASS subtests (20 hash comparisons total)

- [ ] **Step 3: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop hash compat tests — SHA1 + SHA3 vs fossil CLI"
```

---

### Task 7: Large Repo Tests (Tier 2)

**Files:**
- Modify: `sim/interop_test.go`

Tier 2 tests using the Fossil SCM repo. Skipped in `-short` mode.

- [ ] **Step 1: Add `large_repo` subtests and close `TestInterop`**

Insert inside `TestInterop`, then close the function:

```go
	t.Run("large_repo", func(t *testing.T) {
		if testing.Short() {
			t.Skip("skipping large repo tests in -short mode")
		}
		requireFossil(t)

		repoPath := cloneFossilSCMRepo(t)

		t.Run("clone_and_expand_all", func(t *testing.T) {
			d, err := db.Open(repoPath)
			if err != nil {
				t.Fatalf("db.Open: %v", err)
			}
			defer d.Close()

			verifyAllBlobs(t, d)
		})

		t.Run("verify_hash_integrity", func(t *testing.T) {
			d, err := db.Open(repoPath)
			if err != nil {
				t.Fatalf("db.Open: %v", err)
			}
			defer d.Close()

			// Sample 500 random blobs and compare our Expand output
			// against `fossil artifact -R` output.
			rows, err := d.Query("SELECT rid, uuid FROM blob WHERE size >= 0 AND content IS NOT NULL ORDER BY RANDOM() LIMIT 500")
			if err != nil {
				t.Fatal(err)
			}
			type sample struct {
				rid  libfossil.FslID
				uuid string
			}
			var samples []sample
			for rows.Next() {
				var s sample
				rows.Scan(&s.rid, &s.uuid)
				samples = append(samples, s)
			}
			rows.Close()

			var mismatches int
			for _, s := range samples {
				expanded, err := content.Expand(d, s.rid)
				if err != nil {
					t.Errorf("Expand rid=%d: %v", s.rid, err)
					mismatches++
					continue
				}

				cmd := exec.Command("fossil", "artifact", s.uuid, "-R", repoPath)
				fossilOut, err := cmd.Output()
				if err != nil {
					t.Errorf("fossil artifact %s: %v", s.uuid[:16], err)
					mismatches++
					continue
				}

				if string(expanded) != string(fossilOut) {
					t.Errorf("rid=%d uuid=%s: content mismatch (go=%d bytes, fossil=%d bytes)",
						s.rid, s.uuid[:16], len(expanded), len(fossilOut))
					mismatches++
				}
			}
			t.Logf("Compared %d blobs: %d matches, %d mismatches", len(samples), len(samples)-mismatches, mismatches)
		})
	})
} // end TestInterop
```

- [ ] **Step 2: Run Tier 1 tests (should skip large_repo)**

Run: `go test -buildvcs=false -v -run 'TestInterop' ./sim/ -short -timeout=60s`
Expected: `large_repo` subtests SKIP, all others PASS

- [ ] **Step 3: Run Tier 2 tests (requires testdata/fossil.fossil)**

Run: `go test -buildvcs=false -v -run 'TestInterop/large_repo' ./sim/ -timeout=300s`
Expected: 2 PASS subtests (may take 30-60s for 66K blobs)

- [ ] **Step 4: Commit**

```bash
git add sim/interop_test.go
git commit -m "test(sim): add interop large repo tests — Tier 2 Fossil SCM repo verification"
```

---

### Task 8: Integration with Makefile + Full Verification

**Files:**
- Modify: `Makefile` (add `test-interop` target)

- [ ] **Step 1: Check current Makefile test targets**

Read `Makefile` and find the existing `test` and `sim` targets.

- [ ] **Step 2: Add `test-interop` target**

Add after the existing `sim-full` target:

```makefile
.PHONY: test-interop
test-interop: ## Run Fossil interop tests (Tier 1 + Tier 2)
	go test -buildvcs=false ./sim/ -run TestInterop -timeout=300s -v
```

- [ ] **Step 3: Add Tier 1 interop to `make test`**

Ensure the existing `sim` test line includes `TestInterop` with `-short`:

Add a new line to the `test` target (after the existing sim serve tests):

```makefile
	go test ./sim/ -run 'TestInterop' -count=1 -short -timeout=60s
```

- [ ] **Step 4: Run full test suite**

Run: `make test`
Expected: All existing tests pass. New `TestInterop` Tier 1 tests pass. Tier 2 skipped (short mode).

- [ ] **Step 5: Run interop specifically**

Run: `make test-interop`
Expected: All Tier 1 + Tier 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git add Makefile
git commit -m "build: add test-interop target, include Tier 1 interop in make test"
```

---

### Task 9: Final Verification

- [ ] **Step 1: Run full test suite**

Run: `make test`
Expected: All green, including new interop Tier 1 tests.

- [ ] **Step 2: Run Tier 2 interop**

Run: `make test-interop`
Expected: All green including large repo tests.

- [ ] **Step 3: Verify test count**

Run: `go test -buildvcs=false -v -run TestInterop ./sim/ -timeout=300s 2>&1 | grep -c 'PASS:'`
Expected: 16 (3 delta + 3 clone_from_us + 3 clone_from_fossil + 3 incremental_sync + 2 hash + 2 large_repo)

- [ ] **Step 4: Final commit if any cleanup needed**
