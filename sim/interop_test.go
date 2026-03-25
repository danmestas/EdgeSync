package sim

import (
	"bytes"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
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
}
