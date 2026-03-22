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

// serveLeafHTTP starts an HTTP server for the repo and returns the address
// and a cancel function. The caller should defer cancel().
func serveLeafHTTP(t *testing.T, r *repo.Repo) (string, context.CancelFunc) {
	t.Helper()
	addr := freeAddr(t)
	ctx, cancel := context.WithCancel(context.Background())
	go sync.ServeHTTP(ctx, addr, r, sync.HandleSync)
	waitForAddr(t, addr, 5*time.Second)
	return addr, cancel
}

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

	// For commit chains, the last commit needs sym-trunk tag.
	// Checkin only adds trunk tags when Parent==0 (initial commit).
	if len(checkins) > 1 {
		if _, err := tag.AddTag(src, tag.TagOpts{
			TargetRID: libfossil.FslID(parentRid),
			TagName:   "sym-trunk",
			TagType:   tag.TagSingleton,
			User:      "testuser",
			Time:      time.Now().UTC(),
		}); err != nil {
			t.Fatalf("AddTag sym-trunk: %v", err)
		}
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
