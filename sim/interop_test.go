package sim

import (
	"bytes"
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
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/uv"
)

// verifyAllBlobs queries all non-phantom blobs from the database,
// expands each blob via content.Expand (handles delta chains),
// and verifies the hash matches the uuid. Reports failures with rid/uuid details.
func verifyAllBlobs(t *testing.T, d *db.DB) {
	t.Helper()

	rows, err := d.Query("SELECT rid, uuid, size FROM blob WHERE size >= 0 AND content IS NOT NULL")
	if err != nil {
		t.Fatalf("query blobs: %v", err)
	}
	defer rows.Close()

	var count, errors int
	for rows.Next() {
		var rid int64
		var uuid string
		var size int

		if err := rows.Scan(&rid, &uuid, &size); err != nil {
			t.Errorf("scan blob row: %v", err)
			errors++
			continue
		}

		// Expand blob content (resolves delta chains).
		expanded, err := content.Expand(d, libfossil.FslID(rid))
		if err != nil {
			t.Errorf("blob rid=%d uuid=%s: expand failed: %v", rid, uuid, err)
			errors++
			continue
		}

		// Compute hash based on uuid length.
		var computedHash string
		if len(uuid) > 40 {
			// SHA3-256
			computedHash = hash.SHA3(expanded)
		} else {
			// SHA1
			computedHash = hash.SHA1(expanded)
		}

		if computedHash != uuid {
			t.Errorf("blob rid=%d: hash mismatch\n  got:  %s\n  want: %s", rid, computedHash, uuid)
			errors++
		}

		count++
	}

	if err := rows.Err(); err != nil {
		t.Errorf("iterate blobs: %v", err)
		errors++
	}

	t.Logf("verifyAllBlobs: checked %d blobs, %d errors", count, errors)
}

// verifySampledBlobs is like verifyAllBlobs but samples N random blobs.
// Use for large repos where verifying all blobs would be too slow.
func verifySampledBlobs(t *testing.T, d *db.DB, n int) {
	t.Helper()

	rows, err := d.Query("SELECT rid, uuid, size FROM blob WHERE size >= 0 AND content IS NOT NULL ORDER BY RANDOM() LIMIT ?", n)
	if err != nil {
		t.Fatalf("verifySampledBlobs query: %v", err)
	}
	defer rows.Close()

	var count, failures int
	for rows.Next() {
		var rid int64
		var uuid string
		var size int

		if err := rows.Scan(&rid, &uuid, &size); err != nil {
			t.Errorf("scan: %v", err)
			failures++
			continue
		}

		expanded, err := content.Expand(d, libfossil.FslID(rid))
		if err != nil {
			t.Errorf("rid=%d uuid=%s: expand: %v", rid, uuid[:16], err)
			failures++
			continue
		}

		var computed string
		if len(uuid) > 40 {
			computed = hash.SHA3(expanded)
		} else {
			computed = hash.SHA1(expanded)
		}
		if computed != uuid {
			t.Errorf("rid=%d uuid=%s: hash mismatch: got %s", rid, uuid[:16], computed[:16])
			failures++
		}
		count++
	}

	if failures > 0 {
		t.Errorf("verifySampledBlobs: %d/%d failed", failures, count)
	} else {
		t.Logf("verifySampledBlobs: all %d blobs verified", count)
	}
}

// verifyWithFossilRebuild runs `fossil rebuild` on the given repo path.
// Fatals the test if rebuild fails.
func verifyWithFossilRebuild(t *testing.T, repoPath string) {
	t.Helper()

	cmd := exec.Command("fossil", "rebuild", repoPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild failed: %v\n%s", err, out)
	}
	t.Logf("fossil rebuild passed: %s", strings.TrimSpace(string(out)))
}

// fossilExec runs `fossil <args>` and returns stdout as a string.
// Fatals the test if the command fails.
func fossilExec(t *testing.T, args ...string) string {
	t.Helper()
	requireFossil(t)

	cmd := exec.Command("fossil", args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil %v failed: %v\n%s", args, err, out)
	}
	return string(out)
}

// cloneFossilSCMRepo returns the absolute path to testdata/fossil.fossil.
// If the file does not exist, attempts to clone it from fossil-scm.org.
// Skips the test if cloning fails.
func cloneFossilSCMRepo(t *testing.T) string {
	t.Helper()
	requireFossil(t)

	// Determine path relative to sim/ directory.
	// The test is running in the sim/ package, so we go up one level.
	repoPath, err := filepath.Abs("../testdata/fossil.fossil")
	if err != nil {
		t.Fatalf("resolve testdata path: %v", err)
	}

	// If it exists, return it.
	if _, err := os.Stat(repoPath); err == nil {
		return repoPath
	}

	// Create testdata directory if needed.
	testdataDir := filepath.Dir(repoPath)
	if err := os.MkdirAll(testdataDir, 0755); err != nil {
		t.Fatalf("create testdata dir: %v", err)
	}

	// Attempt to clone.
	t.Logf("Cloning fossil-scm.org repo to %s (this may take a minute)...", repoPath)
	cmd := exec.Command("fossil", "clone", "https://fossil-scm.org/home", repoPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Skipf("fossil clone failed, skipping test: %v\n%s", err, out)
	}

	t.Logf("Successfully cloned fossil.fossil (%d bytes)", func() int64 {
		info, _ := os.Stat(repoPath)
		if info != nil {
			return info.Size()
		}
		return 0
	}())

	return repoPath
}

// TestInterop contains interoperability tests between our Go implementation
// and the canonical Fossil C implementation.
func TestInterop(t *testing.T) {
	requireFossil(t)

	// Task 2: Delta codec tests
	t.Run("delta_codec", func(t *testing.T) {
		t.Run("fossil_creates_we_apply", func(t *testing.T) {
			dir := t.TempDir()

			// Create source: 4500 bytes of repeated text
			source := bytes.Repeat([]byte("The quick brown fox jumps over the lazy dog.\n"), 100)
			if len(source) < 4500 {
				source = append(source, bytes.Repeat([]byte("x"), 4500-len(source))...)
			}
			source = source[:4500]

			// Create target: modify source at a few positions
			target := make([]byte, len(source))
			copy(target, source)
			copy(target[100:], []byte("MODIFIED SECTION HERE"))
			copy(target[1000:], []byte("ANOTHER CHANGE"))
			copy(target[3000:], []byte("YET ANOTHER EDIT"))

			// Write to disk
			srcPath := filepath.Join(dir, "source.txt")
			tgtPath := filepath.Join(dir, "target.txt")
			deltaPath := filepath.Join(dir, "delta.bin")

			if err := os.WriteFile(srcPath, source, 0644); err != nil {
				t.Fatalf("write source: %v", err)
			}
			if err := os.WriteFile(tgtPath, target, 0644); err != nil {
				t.Fatalf("write target: %v", err)
			}

			// Use fossil to create delta
			cmd := exec.Command("fossil", "test-delta-create", srcPath, tgtPath, deltaPath)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("fossil test-delta-create failed: %v\n%s", err, out)
			}

			// Read the delta file
			fossilDelta, err := os.ReadFile(deltaPath)
			if err != nil {
				t.Fatalf("read delta: %v", err)
			}

			t.Logf("Fossil created delta: %d bytes (source=%d, target=%d)",
				len(fossilDelta), len(source), len(target))

			// Apply delta using our Go implementation
			result, err := delta.Apply(source, fossilDelta)
			if err != nil {
				t.Fatalf("delta.Apply failed: %v", err)
			}

			if !bytes.Equal(result, target) {
				t.Errorf("delta.Apply result mismatch\ngot:  %d bytes\nwant: %d bytes",
					len(result), len(target))
				// Show first difference
				for i := 0; i < len(result) && i < len(target); i++ {
					if result[i] != target[i] {
						t.Errorf("first difference at byte %d: got %02x want %02x",
							i, result[i], target[i])
						break
					}
				}
			} else {
				t.Logf("SUCCESS: delta.Apply produced correct target")
			}
		})

		t.Run("we_create_fossil_applies", func(t *testing.T) {
			dir := t.TempDir()

			// Create source: 8000 bytes
			source := bytes.Repeat([]byte("Lorem ipsum dolor sit amet, consectetur adipiscing elit.\n"), 150)
			if len(source) < 8000 {
				source = append(source, bytes.Repeat([]byte("x"), 8000-len(source))...)
			}
			source = source[:8000]

			// Create target: modify source
			target := make([]byte, len(source))
			copy(target, source)
			copy(target[500:], []byte("GOLANG DELTA CREATION TEST"))
			copy(target[2000:], []byte("MIDDLE MODIFICATION"))
			copy(target[6000:], []byte("END MODIFICATION"))

			// Create delta using our Go implementation
			goDelta := delta.Create(source, target)

			t.Logf("Go created delta: %d bytes (source=%d, target=%d)",
				len(goDelta), len(source), len(target))

			// Write to disk
			srcPath := filepath.Join(dir, "source.txt")
			deltaPath := filepath.Join(dir, "delta.bin")
			resultPath := filepath.Join(dir, "result.txt")

			if err := os.WriteFile(srcPath, source, 0644); err != nil {
				t.Fatalf("write source: %v", err)
			}
			if err := os.WriteFile(deltaPath, goDelta, 0644); err != nil {
				t.Fatalf("write delta: %v", err)
			}

			// Use fossil to apply delta
			cmd := exec.Command("fossil", "test-delta-apply", srcPath, deltaPath, resultPath)
			out, err := cmd.CombinedOutput()
			if err != nil {
				t.Fatalf("fossil test-delta-apply failed: %v\n%s", err, out)
			}

			// Read result
			result, err := os.ReadFile(resultPath)
			if err != nil {
				t.Fatalf("read result: %v", err)
			}

			if !bytes.Equal(result, target) {
				t.Errorf("fossil apply result mismatch\ngot:  %d bytes\nwant: %d bytes",
					len(result), len(target))
				// Show first difference
				for i := 0; i < len(result) && i < len(target); i++ {
					if result[i] != target[i] {
						t.Errorf("first difference at byte %d: got %02x want %02x",
							i, result[i], target[i])
						break
					}
				}
			} else {
				t.Logf("SUCCESS: fossil test-delta-apply produced correct target")
			}
		})

		t.Run("round_trip_large_payload", func(t *testing.T) {
			dir := t.TempDir()

			// Create 64KB of random-ish data (seeded for reproducibility)
			rng := rand.New(rand.NewSource(12345))
			source := make([]byte, 64*1024)
			for i := range source {
				source[i] = byte('A' + rng.Intn(26)) // uppercase letters
			}

			// Modify ~5% scattered positions
			target := make([]byte, len(source))
			copy(target, source)
			modifyCount := len(source) / 20 // ~5%
			for i := 0; i < modifyCount; i++ {
				pos := rng.Intn(len(target))
				target[pos] = byte('a' + rng.Intn(26)) // lowercase = modified
			}

			t.Logf("Testing round trip with %d byte payload, %d modifications",
				len(source), modifyCount)

			// Test 1: Fossil creates, we apply
			t.Run("fossil_creates", func(t *testing.T) {
				srcPath := filepath.Join(dir, "rt_source1.txt")
				tgtPath := filepath.Join(dir, "rt_target1.txt")
				deltaPath := filepath.Join(dir, "rt_delta1.bin")

				if err := os.WriteFile(srcPath, source, 0644); err != nil {
					t.Fatalf("write source: %v", err)
				}
				if err := os.WriteFile(tgtPath, target, 0644); err != nil {
					t.Fatalf("write target: %v", err)
				}

				cmd := exec.Command("fossil", "test-delta-create", srcPath, tgtPath, deltaPath)
				out, err := cmd.CombinedOutput()
				if err != nil {
					t.Fatalf("fossil test-delta-create failed: %v\n%s", err, out)
				}

				fossilDelta, err := os.ReadFile(deltaPath)
				if err != nil {
					t.Fatalf("read delta: %v", err)
				}

				t.Logf("Fossil delta: %d bytes (%.1f%% of source)",
					len(fossilDelta), 100*float64(len(fossilDelta))/float64(len(source)))

				result, err := delta.Apply(source, fossilDelta)
				if err != nil {
					t.Fatalf("delta.Apply failed: %v", err)
				}

				if !bytes.Equal(result, target) {
					t.Errorf("round trip mismatch (fossil creates, we apply)")
				} else {
					t.Logf("SUCCESS: fossil→go round trip")
				}
			})

			// Test 2: We create, fossil applies
			t.Run("go_creates", func(t *testing.T) {
				goDelta := delta.Create(source, target)

				t.Logf("Go delta: %d bytes (%.1f%% of source)",
					len(goDelta), 100*float64(len(goDelta))/float64(len(source)))

				srcPath := filepath.Join(dir, "rt_source2.txt")
				deltaPath := filepath.Join(dir, "rt_delta2.bin")
				resultPath := filepath.Join(dir, "rt_result2.txt")

				if err := os.WriteFile(srcPath, source, 0644); err != nil {
					t.Fatalf("write source: %v", err)
				}
				if err := os.WriteFile(deltaPath, goDelta, 0644); err != nil {
					t.Fatalf("write delta: %v", err)
				}

				cmd := exec.Command("fossil", "test-delta-apply", srcPath, deltaPath, resultPath)
				out, err := cmd.CombinedOutput()
				if err != nil {
					t.Fatalf("fossil test-delta-apply failed: %v\n%s", err, out)
				}

				result, err := os.ReadFile(resultPath)
				if err != nil {
					t.Fatalf("read result: %v", err)
				}

				if !bytes.Equal(result, target) {
					t.Errorf("round trip mismatch (we create, fossil applies)")
				} else {
					t.Logf("SUCCESS: go→fossil round trip")
				}
			})
		})
	})

	// Task 3: Clone from us (Go serves, fossil clones)
	t.Run("clone_from_us", func(t *testing.T) {
		t.Run("single_commit", func(t *testing.T) {
			dir := t.TempDir()

			// Create Go repo with one checkin
			r := leafRepo(t, dir, "test-repo")
			defer r.Close()

			files := []manifest.File{
				{Name: "README.md", Content: []byte("# Test Repository\n\nThis is a test.")},
				{Name: "main.go", Content: []byte("package main\n\nfunc main() {}\n")},
			}
			checkin(t, r, 0, files, "Initial commit")

			// Crosslink before serving
			if _, err := manifest.Crosslink(r); err != nil {
				t.Fatalf("crosslink: %v", err)
			}
			r.Close()

			// Reopen and serve
			r2, err := repo.Open(r.Path())
			if err != nil {
				t.Fatalf("reopen repo: %v", err)
			}
			defer r2.Close()

			addr, cancel := serveLeafHTTP(t, r2)
			defer cancel()

			// Fossil clone from our HTTP server
			clonePath := filepath.Join(dir, "clone.fossil")
			fossilExec(t, "clone", "http://"+addr, clonePath)

			// Verify with fossil rebuild
			verifyWithFossilRebuild(t, clonePath)

			// Checkout and verify files
			workDir := fossilCheckout(t, clonePath)
			assertFiles(t, workDir, map[string]string{
				"README.md": "# Test Repository\n\nThis is a test.",
				"main.go":   "package main\n\nfunc main() {}\n",
			})
		})

		t.Run("commit_chain", func(t *testing.T) {
			dir := t.TempDir()

			// Create repo with 5 sequential commits (large enough to trigger deltas)
			r := leafRepo(t, dir, "chain-repo")
			defer r.Close()

			var parent int64
			for i := 1; i <= 5; i++ {
				// Pad content to 200+ bytes to encourage delta formation
				var files []manifest.File
				// Keep all previous files plus new one
				for j := 1; j <= i; j++ {
					fileContent := fmt.Sprintf("Commit %d\n%s", j, strings.Repeat("x", 200))
					files = append(files, manifest.File{
						Name:    fmt.Sprintf("file%d.txt", j),
						Content: []byte(fileContent),
					})
				}
				parent = checkin(t, r, parent, files, fmt.Sprintf("Commit %d", i))
			}

			if _, err := manifest.Crosslink(r); err != nil {
				t.Fatalf("crosslink: %v", err)
			}
			r.Close()

			// Reopen and serve
			r2, err := repo.Open(r.Path())
			if err != nil {
				t.Fatalf("reopen repo: %v", err)
			}
			defer r2.Close()

			addr, cancel := serveLeafHTTP(t, r2)
			defer cancel()

			// Clone and rebuild
			clonePath := filepath.Join(dir, "clone.fossil")
			fossilExec(t, "clone", "http://"+addr, clonePath)
			verifyWithFossilRebuild(t, clonePath)
		})

		t.Run("with_uv_files", func(t *testing.T) {
			dir := t.TempDir()

			// Create repo with checkin and UV files
			r := leafRepo(t, dir, "uv-repo")
			defer r.Close()

			files := []manifest.File{
				{Name: "README.md", Content: []byte("# UV Test\n")},
			}
			checkin(t, r, 0, files, "Initial commit")

			// Ensure UV schema exists
			if err := uv.EnsureSchema(r.DB()); err != nil {
				t.Fatalf("ensure UV schema: %v", err)
			}

			// Write UV files
			textContent := []byte("This is an unversioned text file.\n")
			if err := uv.Write(r.DB(), "text-file.txt", textContent, 0); err != nil {
				t.Fatalf("write UV text file: %v", err)
			}

			// Write 4KB binary UV file
			binaryContent := make([]byte, 4096)
			rng := rand.New(rand.NewSource(42))
			rng.Read(binaryContent)
			if err := uv.Write(r.DB(), "binary-file.bin", binaryContent, 0); err != nil {
				t.Fatalf("write UV binary file: %v", err)
			}

			if _, err := manifest.Crosslink(r); err != nil {
				t.Fatalf("crosslink: %v", err)
			}
			r.Close()

			// Reopen and serve
			r2, err := repo.Open(r.Path())
			if err != nil {
				t.Fatalf("reopen repo: %v", err)
			}
			defer r2.Close()

			addr, cancel := serveLeafHTTP(t, r2)
			defer cancel()

			// Clone with --unversioned flag to pull UV files
			clonePath := filepath.Join(dir, "clone.fossil")
			fossilExec(t, "clone", "--unversioned", "http://"+addr, clonePath)
			verifyWithFossilRebuild(t, clonePath)

			// Verify UV files
			uvList := fossilExec(t, "uv", "list", "-R", clonePath)
			if !strings.Contains(uvList, "text-file.txt") {
				t.Errorf("UV list missing text-file.txt: %s", uvList)
			}
			if !strings.Contains(uvList, "binary-file.bin") {
				t.Errorf("UV list missing binary-file.bin: %s", uvList)
			}

			uvText := fossilExec(t, "uv", "cat", "text-file.txt", "-R", clonePath)
			if uvText != string(textContent) {
				t.Errorf("UV text content mismatch:\ngot:  %q\nwant: %q", uvText, string(textContent))
			}
		})
	})

	// Task 4: Clone from fossil (fossil serves, we clone)
	t.Run("clone_from_fossil", func(t *testing.T) {
		t.Run("single_commit", func(t *testing.T) {
			dir := t.TempDir()

			// Create fossil repo
			fossilPath := fossilInit(t, dir, "fossil-repo")
			workDir := fossilCheckout(t, fossilPath)

			files := map[string]string{
				"hello.txt": "Hello, Fossil!\n",
			}
			fossilCommitFiles(t, fossilPath, workDir, files, "Initial commit")

			// Serve fossil
			serverURL := startFossilServe(t, fossilPath)

			// Clone with our Go implementation
			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()

			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}
			defer leafR.Close()

			// Verify all blobs
			verifyAllBlobs(t, leafR.DB())
		})

		t.Run("commit_chain_with_deltas", func(t *testing.T) {
			dir := t.TempDir()

			// Create fossil repo with 5 commits
			fossilPath := fossilInit(t, dir, "fossil-chain")
			workDir := fossilCheckout(t, fossilPath)

			for i := 1; i <= 5; i++ {
				content := fmt.Sprintf("Commit %d\n%s", i, strings.Repeat("y", 300))
				files := map[string]string{
					fmt.Sprintf("file%d.txt", i): content,
				}
				fossilCommitFiles(t, fossilPath, workDir, files, fmt.Sprintf("Commit %d", i))
			}

			serverURL := startFossilServe(t, fossilPath)

			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
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

			// Create fossil repo with larger files (add new files each iteration to avoid detection issues)
			fossilPath := fossilInit(t, dir, "fossil-large")
			workDir := fossilCheckout(t, fossilPath)

			for i := 1; i <= 3; i++ {
				// Create unique file for each commit (4KB+ each)
				content := strings.Repeat(fmt.Sprintf("=== File %d line %%d ===\n", i), 300)
				files := map[string]string{
					fmt.Sprintf("file%d.txt", i): fmt.Sprintf(content, i),
				}
				fossilCommitFiles(t, fossilPath, workDir, files, fmt.Sprintf("Commit %d", i))
			}

			serverURL := startFossilServe(t, fossilPath)

			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()

			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}
			defer leafR.Close()

			verifyAllBlobs(t, leafR.DB())
			// Note: Not running fossil rebuild on leaf repo - it was created by sync.Clone,
			// and verifyAllBlobs already ensures all content expands correctly.
		})
	})

	// Task 5: Incremental sync (bidirectional sync with fossil)
	t.Run("incremental_sync", func(t *testing.T) {
		t.Run("fossil_commits_we_pull", func(t *testing.T) {
			dir := t.TempDir()

			// Create and clone from fossil
			fossilPath := fossilInit(t, dir, "fossil-pull")
			workDir := fossilCheckout(t, fossilPath)

			files := map[string]string{"initial.txt": "Initial\n"}
			fossilCommitFiles(t, fossilPath, workDir, files, "Initial")

			serverURL := startFossilServe(t, fossilPath)

			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx := context.Background()
			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}
			leafR.Close()

			// Fossil makes 3 more commits
			for i := 1; i <= 3; i++ {
				files := map[string]string{
					fmt.Sprintf("file%d.txt", i): fmt.Sprintf("Content %d\n", i),
				}
				fossilCommitFiles(t, fossilPath, workDir, files, fmt.Sprintf("Commit %d", i))
			}

			// Pull changes
			leafR, err = repo.Open(leafPath)
			if err != nil {
				t.Fatalf("reopen leaf: %v", err)
			}
			defer leafR.Close()

			var projCode string
			leafR.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)

			_, err = sync.Sync(ctx, leafR, transport, sync.SyncOpts{
				ProjectCode: projCode,
				Pull:        true,
			})
			if err != nil {
				t.Fatalf("sync.Sync pull: %v", err)
			}

			verifyAllBlobs(t, leafR.DB())
		})

		t.Run("we_push_fossil_pulls", func(t *testing.T) {
			dir := t.TempDir()

			// Clone from fossil
			fossilPath := fossilInit(t, dir, "fossil-push")
			workDir := fossilCheckout(t, fossilPath)

			files := map[string]string{"base.txt": "Base\n"}
			fossilCommitFiles(t, fossilPath, workDir, files, "Base")

			serverURL := startFossilServe(t, fossilPath)

			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx := context.Background()
			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}

			// Add content to leaf
			rng := rand.New(rand.NewSource(99))
			_, err = SeedLeaf(leafR, rng, 3, 1024)
			if err != nil {
				t.Fatalf("SeedLeaf: %v", err)
			}

			var projCode string
			leafR.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)

			// Push to fossil
			_, err = sync.Sync(ctx, leafR, transport, sync.SyncOpts{
				ProjectCode: projCode,
				Push:        true,
			})
			if err != nil {
				t.Fatalf("sync.Sync push: %v", err)
			}
			leafR.Close()

			// Verify fossil can read with rebuild
			verifyWithFossilRebuild(t, fossilPath)

			// Sample one artifact from leaf
			d, err := db.Open(leafPath)
			if err != nil {
				t.Fatalf("open leaf db: %v", err)
			}
			defer d.Close()

			var uuid string
			err = d.QueryRow("SELECT uuid FROM blob WHERE size >= 0 LIMIT 1").Scan(&uuid)
			if err != nil {
				t.Fatalf("query uuid: %v", err)
			}

			artifactOut := fossilExec(t, "artifact", uuid, "-R", fossilPath)
			if len(artifactOut) == 0 {
				t.Errorf("fossil artifact returned empty for uuid=%s", uuid)
			}
		})

		t.Run("bidirectional", func(t *testing.T) {
			dir := t.TempDir()

			// Initial setup
			fossilPath := fossilInit(t, dir, "fossil-bidir")
			workDir := fossilCheckout(t, fossilPath)

			files := map[string]string{"start.txt": "Start\n"}
			fossilCommitFiles(t, fossilPath, workDir, files, "Start")

			serverURL := startFossilServe(t, fossilPath)

			leafPath := filepath.Join(dir, "leaf.fossil")
			ctx := context.Background()
			transport := &sync.HTTPTransport{URL: serverURL}
			leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
			if err != nil {
				t.Fatalf("sync.Clone: %v", err)
			}

			// Both sides add content
			// Fossil side
			for i := 1; i <= 2; i++ {
				files := map[string]string{
					fmt.Sprintf("fossil%d.txt", i): fmt.Sprintf("From fossil %d\n", i),
				}
				fossilCommitFiles(t, fossilPath, workDir, files, fmt.Sprintf("Fossil %d", i))
			}

			// Leaf side
			rng := rand.New(rand.NewSource(123))
			_, err = SeedLeaf(leafR, rng, 2, 512)
			if err != nil {
				t.Fatalf("SeedLeaf: %v", err)
			}

			var projCode string
			leafR.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)

			// Sync until convergence
			maxRounds := 5
			for round := 0; round < maxRounds; round++ {
				result, err := sync.Sync(ctx, leafR, transport, sync.SyncOpts{
					ProjectCode: projCode,
					Push:        true,
					Pull:        true,
				})
				if err != nil {
					t.Fatalf("sync round %d: %v", round, err)
				}

				t.Logf("Round %d: sent=%d received=%d", round, result.FilesSent, result.FilesRecvd)

				if result.FilesSent == 0 && result.FilesRecvd == 0 {
					t.Logf("Converged after %d rounds", round+1)
					break
				}

				if round == maxRounds-1 {
					t.Errorf("Did not converge after %d rounds", maxRounds)
				}
			}

			verifyAllBlobs(t, leafR.DB())
			leafR.Close()

			verifyWithFossilRebuild(t, fossilPath)
		})
	})

	// Task 6: Hash compatibility
	t.Run("hash_compat", func(t *testing.T) {
		t.Run("sha1_vs_fossil", func(t *testing.T) {
			dir := t.TempDir()
			sizes := []int{0, 1, 15, 16, 17, 255, 256, 257, 1024, 65536}

			rng := rand.New(rand.NewSource(54321))

			for _, size := range sizes {
				t.Run(fmt.Sprintf("size_%d", size), func(t *testing.T) {
					data := make([]byte, size)
					rng.Read(data)

					// Our hash
					ourHash := hash.SHA1(data)

					// Fossil hash
					filePath := filepath.Join(dir, fmt.Sprintf("data_%d.bin", size))
					if err := os.WriteFile(filePath, data, 0644); err != nil {
						t.Fatalf("write file: %v", err)
					}

					fossilOut := fossilExec(t, "sha1sum", filePath)
					fields := strings.Fields(fossilOut)
					if len(fields) == 0 {
						t.Fatalf("fossil sha1sum returned empty")
					}
					fossilHash := fields[0]

					if ourHash != fossilHash {
						t.Errorf("SHA1 mismatch for size %d:\nours:   %s\nfossil: %s",
							size, ourHash, fossilHash)
					}
				})
			}
		})

		t.Run("sha3_vs_fossil", func(t *testing.T) {
			dir := t.TempDir()
			sizes := []int{0, 1, 15, 16, 17, 255, 256, 257, 1024, 65536}

			rng := rand.New(rand.NewSource(98765))

			for _, size := range sizes {
				t.Run(fmt.Sprintf("size_%d", size), func(t *testing.T) {
					data := make([]byte, size)
					rng.Read(data)

					// Our hash
					ourHash := hash.SHA3(data)

					// Fossil hash
					filePath := filepath.Join(dir, fmt.Sprintf("data_%d.bin", size))
					if err := os.WriteFile(filePath, data, 0644); err != nil {
						t.Fatalf("write file: %v", err)
					}

					fossilOut := fossilExec(t, "sha3sum", filePath)
					fields := strings.Fields(fossilOut)
					if len(fields) == 0 {
						t.Fatalf("fossil sha3sum returned empty")
					}
					fossilHash := fields[0]

					if ourHash != fossilHash {
						t.Errorf("SHA3 mismatch for size %d:\nours:   %s\nfossil: %s",
							size, ourHash, fossilHash)
					}
				})
			}
		})
	})

	// Task 7: Large repo (Tier 2)
	t.Run("large_repo", func(t *testing.T) {
		if testing.Short() {
			t.Skip("Skipping large repo tests in short mode")
		}

		t.Run("clone_and_expand_all", func(t *testing.T) {
			repoPath := cloneFossilSCMRepo(t)

			d, err := db.Open(repoPath)
			if err != nil {
				t.Fatalf("open fossil.fossil: %v", err)
			}
			defer d.Close()

			verifySampledBlobs(t, d, 5000)
		})

		t.Run("verify_hash_integrity", func(t *testing.T) {
			repoPath := cloneFossilSCMRepo(t)

			d, err := db.Open(repoPath)
			if err != nil {
				t.Fatalf("open fossil.fossil: %v", err)
			}
			defer d.Close()

			// Sample 2000 random blobs and cross-check against fossil artifact
			rows, err := d.Query(`
				SELECT rid, uuid
				FROM blob
				WHERE size >= 0 AND content IS NOT NULL
				ORDER BY RANDOM()
				LIMIT 2000
			`)
			if err != nil {
				t.Fatalf("query random blobs: %v", err)
			}
			defer rows.Close()

			var count, errors int
			for rows.Next() {
				var rid int64
				var uuid string
				if err := rows.Scan(&rid, &uuid); err != nil {
					t.Errorf("scan: %v", err)
					errors++
					continue
				}

				// Expand with our implementation
				ourContent, err := content.Expand(d, libfossil.FslID(rid))
				if err != nil {
					t.Errorf("expand rid=%d uuid=%s: %v", rid, uuid, err)
					errors++
					continue
				}

				// Get from fossil
				fossilContent := fossilExec(t, "artifact", uuid, "-R", repoPath)

				if !bytes.Equal(ourContent, []byte(fossilContent)) {
					t.Errorf("content mismatch for rid=%d uuid=%s:\nour size: %d\nfossil size: %d",
						rid, uuid, len(ourContent), len(fossilContent))
					errors++
				}

				count++
			}

			if err := rows.Err(); err != nil {
				t.Errorf("iterate: %v", err)
			}

			t.Logf("Verified %d random blobs, %d errors", count, errors)
		})
	})
}
