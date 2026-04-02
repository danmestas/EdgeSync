package sim

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

// TestFileHistoryInterop creates a Fossil repo using the fossil binary,
// makes several commits with file modifications, then compares
// manifest.FileHistory output against fossil finfo output.
func TestFileHistoryInterop(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "interop.fossil")

	// Create repo
	run(t, "fossil", "init", repoPath)

	// Open checkout
	workDir := filepath.Join(dir, "work")
	if err := os.MkdirAll(workDir, 0755); err != nil {
		t.Fatal(err)
	}
	runIn(t, workDir, "fossil", "open", repoPath)

	// Commit 1: add hello.txt and other.txt
	writeFile(t, workDir, "hello.txt", "version 1\n")
	writeFile(t, workDir, "other.txt", "static\n")
	runIn(t, workDir, "fossil", "add", ".")
	runIn(t, workDir, "fossil", "commit", "-m", "add files", "--no-warnings")

	// Commit 2: modify hello.txt only
	// fossil uses mtime for change detection — use --allow-conflict to force check
	writeFile(t, workDir, "hello.txt", "version 2 with extra content to ensure different size\n")
	runIn(t, workDir, "fossil", "commit", "-m", "update hello", "--no-warnings", "--allow-conflict")

	// Commit 3: modify hello.txt again
	writeFile(t, workDir, "hello.txt", "version 3 with even more content to be sure it differs\n")
	runIn(t, workDir, "fossil", "commit", "-m", "update hello again", "--no-warnings", "--allow-conflict")

	// Get fossil finfo output for comparison
	fossilFinfo := runOutput(t, workDir, "fossil", "finfo", "hello.txt")
	t.Logf("fossil finfo output:\n%s", fossilFinfo)

	// Count how many history entries fossil reports
	fossilLines := countNonEmptyLines(fossilFinfo)

	// Now open with go-libfossil and run FileHistory
	// First rebuild to ensure mlink is populated from Fossil's data
	if err := testutil.FossilRebuild(repoPath); err != nil {
		t.Fatalf("fossil rebuild: %v", err)
	}

	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	defer r.Close()

	versions, err := manifest.FileHistory(r, manifest.FileHistoryOpts{Path: "hello.txt"})
	if err != nil {
		t.Fatalf("FileHistory: %v", err)
	}

	t.Logf("FileHistory returned %d versions:", len(versions))
	for i, v := range versions {
		t.Logf("  [%d] %s action=%v user=%s comment=%q uuid=%s",
			i, v.Date.Format("2006-01-02"), v.Action, v.User, v.Comment, v.CheckinUUID[:10])
	}

	// Both should agree on the number of changes.
	// fossil finfo shows one line per checkin that touched the file.
	if len(versions) != fossilLines {
		t.Errorf("FileHistory returned %d versions, fossil finfo showed %d lines — mismatch",
			len(versions), fossilLines)
	}

	// Verify other.txt was not modified — should have exactly 1 history entry
	otherVersions, err := manifest.FileHistory(r, manifest.FileHistoryOpts{Path: "other.txt"})
	if err != nil {
		t.Fatalf("FileHistory other.txt: %v", err)
	}
	if len(otherVersions) != 1 {
		t.Errorf("other.txt: expected 1 version (only added), got %d", len(otherVersions))
	}

	// Verify ordering: most recent first
	for i := 1; i < len(versions); i++ {
		if versions[i].Date.After(versions[i-1].Date) {
			t.Errorf("versions not in descending date order: v[%d]=%v > v[%d]=%v",
				i, versions[i].Date, i-1, versions[i-1].Date)
		}
	}
}

func TestFileHistoryInterop_FossilRebuildVerify(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "verify.fossil")
	run(t, "fossil", "init", repoPath)

	workDir := filepath.Join(dir, "work")
	os.MkdirAll(workDir, 0755)
	runIn(t, workDir, "fossil", "open", repoPath)

	// Create several commits
	for i := 1; i <= 5; i++ {
		// Vary content size to ensure fossil detects changes (mtime granularity)
		writeFile(t, workDir, "counter.txt", fmt.Sprintf("count = %d padding=%s\n", i, strings.Repeat("x", i*100)))
		if i == 1 {
			runIn(t, workDir, "fossil", "add", "counter.txt")
		}
		runIn(t, workDir, "fossil", "commit", "-m", fmt.Sprintf("set counter to %d", i), "--no-warnings", "--allow-conflict")
	}

	// Rebuild to ensure clean mlink state
	if err := testutil.FossilRebuild(repoPath); err != nil {
		t.Fatalf("fossil rebuild: %v", err)
	}

	r, err := repo.Open(repoPath)
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()

	versions, err := manifest.FileHistory(r, manifest.FileHistoryOpts{Path: "counter.txt"})
	if err != nil {
		t.Fatal(err)
	}

	if len(versions) != 5 {
		t.Fatalf("expected 5 versions for 5 commits, got %d", len(versions))
	}

	// All should have valid UUIDs and dates
	for i, v := range versions {
		if v.CheckinUUID == "" {
			t.Errorf("v[%d]: missing CheckinUUID", i)
		}
		if v.FileUUID == "" {
			t.Errorf("v[%d]: missing FileUUID", i)
		}
		if v.Date.IsZero() {
			t.Errorf("v[%d]: zero date", i)
		}
	}
}

// helpers

func run(t *testing.T, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
	}
}

func runIn(t *testing.T, dir, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%s %s (in %s): %v\n%s", name, strings.Join(args, " "), dir, err, out)
	}
}

func runOutput(t *testing.T, dir, name string, args ...string) string {
	t.Helper()
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
	}
	return string(out)
}

var writeSeq int

func writeFile(t *testing.T, dir, name, content string) {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.MkdirAll(filepath.Dir(p), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	// Fossil uses mtime for change detection. Bump mtime by a unique
	// offset to ensure changes are detected even within the same second.
	writeSeq++
	future := time.Now().Add(time.Duration(writeSeq) * time.Second)
	os.Chtimes(p, future, future)
}

func countNonEmptyLines(s string) int {
	count := 0
	for _, line := range strings.Split(strings.TrimSpace(s), "\n") {
		line = strings.TrimSpace(line)
		if line != "" && !strings.HasPrefix(line, "History") {
			count++
		}
	}
	return count
}
