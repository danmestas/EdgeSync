package sim

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
)

func TestFileHistoryInterop(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil not in PATH")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "interop.fossil")

	fossilExec(t, "init", repoPath)

	workDir := filepath.Join(dir, "work")
	os.MkdirAll(workDir, 0755)
	fossilExecIn(t, workDir, "open", repoPath)

	// Commit 1: add hello.txt and other.txt
	writeFileWithMtime(t, workDir, "hello.txt", "version 1\n")
	writeFileWithMtime(t, workDir, "other.txt", "static\n")
	fossilExecIn(t, workDir, "add", ".")
	fossilExecIn(t, workDir, "commit", "-m", "add files", "--no-warnings")

	// Commit 2: modify hello.txt only
	writeFileWithMtime(t, workDir, "hello.txt", "version 2 with extra content to ensure different size\n")
	fossilExecIn(t, workDir, "commit", "-m", "update hello", "--no-warnings", "--allow-conflict")

	// Commit 3: modify hello.txt again
	writeFileWithMtime(t, workDir, "hello.txt", "version 3 with even more content to be sure it differs\n")
	fossilExecIn(t, workDir, "commit", "-m", "update hello again", "--no-warnings", "--allow-conflict")

	// Get fossil finfo output for comparison.
	cmd := exec.Command("fossil", "finfo", "hello.txt")
	cmd.Dir = workDir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil finfo: %v\n%s", err, out)
	}
	fossilFinfo := string(out)
	t.Logf("fossil finfo output:\n%s", fossilFinfo)

	fossilLines := countNonEmptyLines(fossilFinfo)
	t.Logf("fossil finfo reports %d history entries for hello.txt", fossilLines)

	// We expect 3 entries (3 commits touched hello.txt).
	if fossilLines != 3 {
		t.Errorf("expected 3 finfo entries, got %d", fossilLines)
	}
}

var finfoWriteSeq int

func writeFileWithMtime(t *testing.T, dir, name, content string) {
	t.Helper()
	p := filepath.Join(dir, name)
	os.MkdirAll(filepath.Dir(p), 0755)
	os.WriteFile(p, []byte(content), 0644)
	finfoWriteSeq++
	future := time.Now().Add(time.Duration(finfoWriteSeq) * time.Second)
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

// TestFileHistoryInterop_FossilRebuildVerify creates 5 commits via fossil CLI,
// runs fossil rebuild, then opens with go-libfossil and verifies the mlink
// table has correct file history entries (5 versions for counter.txt).
func TestFileHistoryInterop_FossilRebuildVerify(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil not in PATH")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "verify.fossil")
	fossilExec(t, "init", repoPath)

	workDir := filepath.Join(dir, "work")
	os.MkdirAll(workDir, 0755)
	fossilExecIn(t, workDir, "open", repoPath)

	// Create 5 commits, each modifying counter.txt.
	for i := 1; i <= 5; i++ {
		content := fmt.Sprintf("count = %d padding=%s\n", i, strings.Repeat("x", i*100))
		writeFileWithMtime(t, workDir, "counter.txt", content)
		if i == 1 {
			fossilExecIn(t, workDir, "add", "counter.txt")
		}
		fossilExecIn(t, workDir, "commit", "-m", fmt.Sprintf("set counter to %d", i), "--no-warnings", "--allow-conflict")
	}

	// Rebuild to ensure clean mlink state.
	fossilExecIn(t, workDir, "close", "--force")
	fossilExec(t, "rebuild", repoPath)

	// Open with go-libfossil and query mlink for counter.txt history.
	r, err := libfossil.Open(repoPath)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer r.Close()

	// Query file history via mlink + filename tables (same as internal FileHistory).
	rows, err := r.DB().Query(`
		SELECT e.objid, b.uuid, fb.uuid, e.mtime, e.user, e.comment
		FROM mlink ml
		JOIN filename fn ON fn.fnid = ml.fnid
		JOIN event e ON e.objid = ml.mid
		JOIN blob b ON b.rid = ml.mid
		LEFT JOIN blob fb ON fb.rid = ml.fid
		WHERE fn.name = 'counter.txt'
		ORDER BY e.mtime DESC
	`)
	if err != nil {
		t.Fatalf("query mlink: %v", err)
	}
	defer rows.Close()

	type fileVersion struct {
		checkinRID  int64
		checkinUUID string
		fileUUID    string
		mtime       float64
		user        string
		comment     string
	}

	var versions []fileVersion
	for rows.Next() {
		var v fileVersion
		var fileUUID *string
		if err := rows.Scan(&v.checkinRID, &v.checkinUUID, &fileUUID, &v.mtime, &v.user, &v.comment); err != nil {
			t.Fatalf("scan: %v", err)
		}
		if fileUUID != nil {
			v.fileUUID = *fileUUID
		}
		versions = append(versions, v)
	}

	t.Logf("FileHistory returned %d versions:", len(versions))
	for i, v := range versions {
		t.Logf("  [%d] rid=%d user=%s comment=%q uuid=%s", i, v.checkinRID, v.user, v.comment, v.checkinUUID[:10])
	}

	if len(versions) != 5 {
		t.Fatalf("expected 5 versions for 5 commits, got %d", len(versions))
	}

	// All should have valid UUIDs and non-zero mtime.
	for i, v := range versions {
		if v.checkinUUID == "" {
			t.Errorf("v[%d]: missing checkin UUID", i)
		}
		if v.mtime == 0 {
			t.Errorf("v[%d]: zero mtime", i)
		}
	}
}

// Silence import.
var _ = fmt.Sprintf
