package sim

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/tag"
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
		Parent:  libfossil.FslID(parent),
		Time:    time.Now().UTC(),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	return int64(rid)
}

func TestEquivalenceSmoke(t *testing.T) {
	requireFossil(t)
	if testing.Short() {
		t.Skip("skipping in short mode")
	}

	dir := t.TempDir()
	r := leafRepo(t, dir, "smoke.fossil")
	rid := checkin(t, r, 0, []manifest.File{
		{Name: "smoke.txt", Content: []byte("smoke test")},
	}, "smoke checkin")

	// Crosslink manifests to populate event table (required for fossil open).
	if _, err := manifest.Crosslink(r); err != nil {
		t.Fatalf("Crosslink: %v", err)
	}

	// Add sym-trunk tag (Crosslink doesn't process manifest tags yet).
	if _, err := tag.AddTag(r, tag.TagOpts{
		TargetRID: libfossil.FslID(rid),
		TagName:   "sym-trunk",
		TagType:   tag.TagSingleton,
		User:      "testuser",
		Time:      time.Now().UTC(),
	}); err != nil {
		t.Fatalf("AddTag sym-trunk: %v", err)
	}

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
