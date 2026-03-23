package sim

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/tag"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
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

// fossilInit creates a new fossil repo and returns its path.
func fossilInit(t *testing.T, dir, name string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	cmd := exec.Command("fossil", "init", path)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil init: %v\n%s", err, out)
	}
	// Grant nobody user capabilities for unauthenticated sync.
	// fossil init may already create the nobody user, so "user new" can fail — that's fine.
	exec.Command("fossil", "user", "new", "nobody", "", "cghijknorswz", "-R", path).Run()
	if out, err := exec.Command("fossil", "user", "capabilities", "nobody", "cghijknorswz", "-R", path).CombinedOutput(); err != nil {
		t.Fatalf("fossil user capabilities nobody: %v\n%s", err, out)
	}
	return path
}

// fossilCommitFiles opens a fossil repo (if workDir is empty), writes files, adds, and commits.
// Returns the working directory for chained commits.
func fossilCommitFiles(t *testing.T, repoPath, workDir string, files map[string]string, comment string) string {
	t.Helper()
	if workDir == "" {
		workDir = filepath.Join(t.TempDir(), "fossil-work")
		if err := os.MkdirAll(workDir, 0755); err != nil {
			t.Fatalf("mkdir fossil-work: %v", err)
		}
		cmd := exec.Command("fossil", "open", repoPath)
		cmd.Dir = workDir
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("fossil open: %v\n%s", err, out)
		}
	}
	for name, content := range files {
		fpath := filepath.Join(workDir, name)
		if err := os.MkdirAll(filepath.Dir(fpath), 0755); err != nil {
			t.Fatalf("mkdir for %s: %v", name, err)
		}
		if err := os.WriteFile(fpath, []byte(content), 0644); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
	}
	addCmd := exec.Command("fossil", "add", ".")
	addCmd.Dir = workDir
	if out, err := addCmd.CombinedOutput(); err != nil {
		t.Fatalf("fossil add: %v\n%s", err, out)
	}

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
	_, portStr, err := net.SplitHostPort(addr)
	if err != nil {
		t.Fatalf("SplitHostPort %s: %v", addr, err)
	}

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
		t.Skip("KNOWN ISSUE: fossil server sends cfile cards with incorrect usize headers for commit chains")
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

	// Close the fossil checkout to release locks (best-effort; serve works regardless).
	closeCmd := exec.Command("fossil", "close", "--force")
	closeCmd.Dir = workDir
	if out, err := closeCmd.CombinedOutput(); err != nil {
		t.Logf("fossil close: %v\n%s", err, out)
	}

	// 2. Start fossil serve.
	serverURL := startFossilServe(t, fossilRepoPath)

	// 3. Clone into a leaf repo via sync.Clone.
	leafPath := filepath.Join(dir, "leaf.fossil")
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	transport := &sync.HTTPTransport{URL: serverURL}
	leafR, _, err := sync.Clone(ctx, leafPath, transport, sync.CloneOpts{})
	if err != nil {
		t.Fatalf("sync.Clone: %v", err)
	}

	// Find the tip checkin (most recent by mtime in event table).
	var tipRid int64
	err = leafR.DB().QueryRow(`SELECT objid FROM event WHERE type='ci' ORDER BY mtime DESC LIMIT 1`).Scan(&tipRid)
	if err != nil {
		leafR.Close()
		t.Fatalf("get tip rid: %v", err)
	}

	// Add sym-trunk tag (Crosslink doesn't process manifest tags yet).
	if _, err := tag.AddTag(leafR, tag.TagOpts{
		TargetRID: libfossil.FslID(tipRid),
		TagName:   "sym-trunk",
		TagType:   tag.TagSingleton,
		User:      "testuser",
		Time:      time.Now().UTC(),
	}); err != nil {
		leafR.Close()
		t.Fatalf("AddTag sym-trunk: %v", err)
	}

	leafR.Close()

	// 4. fossil open the leaf repo + verify files.
	leafWorkDir := fossilCheckout(t, leafPath)
	assertFiles(t, leafWorkDir, expected)
}

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

	// 4. Sync leaf B from leaf A (bounded: must converge within 10 rounds).
	transport := &sync.HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	ctx := context.Background()
	converged := false
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
			converged = true
			break
		}
	}
	if !converged {
		t.Fatal("sync did not converge within 10 rounds")
	}

	// 5. Crosslink leaf B (sync.Sync doesn't crosslink, unlike sync.Clone).
	n, err := manifest.Crosslink(leafB)
	if err != nil {
		t.Fatalf("Crosslink: %v", err)
	}
	t.Logf("crosslinked %d manifests", n)

	// Find the tip checkin (most recent by mtime in event table).
	var tipRid int64
	err = leafB.DB().QueryRow(`SELECT objid FROM event WHERE type='ci' ORDER BY mtime DESC LIMIT 1`).Scan(&tipRid)
	if err != nil {
		t.Fatalf("get tip rid: %v", err)
	}

	// Add sym-trunk tag (Crosslink doesn't process manifest tags yet).
	if _, err := tag.AddTag(leafB, tag.TagOpts{
		TargetRID: libfossil.FslID(tipRid),
		TagName:   "sym-trunk",
		TagType:   tag.TagSingleton,
		User:      "testuser",
		Time:      time.Now().UTC(),
	}); err != nil {
		t.Fatalf("AddTag sym-trunk: %v", err)
	}

	// 6. fossil open + verify.
	leafB.Close()
	leafBWorkDir := fossilCheckout(t, filepath.Join(dir, "leaf-b.fossil"))
	assertFiles(t, leafBWorkDir, expected)
}
