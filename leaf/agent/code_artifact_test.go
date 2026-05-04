package agent

import (
	"bytes"
	"context"
	"path/filepath"
	"slices"
	"testing"

	libfossil "github.com/danmestas/libfossil"
)

// newAgentForCommitTests opens a fresh repo and builds an Agent without
// touching networking. The state-machine fields are unused for code-artifact
// tests; only repo + projectCode are needed.
func newAgentForCommitTests(t *testing.T) *Agent {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := libfossil.Create(path, libfossil.CreateOpts{User: "testuser"})
	if err != nil {
		t.Fatalf("libfossil.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	applySQLiteTuning(r)

	projCode, err := r.Config("project-code")
	if err != nil {
		t.Fatalf("read project-code: %v", err)
	}
	return &Agent{repo: r, projectCode: projCode}
}

func TestAgent_Commit_RoundtripsThroughReadAndFiles(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	rev, err := a.Commit(ctx, CommitOpts{
		Files: []FileToCommit{
			{Name: "hello.txt", Content: []byte("hello\n")},
			{Name: "sub/world.txt", Content: []byte("world\n")},
		},
		Message: "initial",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}
	if rev == "" {
		t.Fatal("Commit returned empty RevID")
	}

	got, err := a.Read(ctx, "hello.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if !bytes.Equal(got, []byte("hello\n")) {
		t.Errorf("Read hello.txt = %q, want %q", got, "hello\n")
	}

	files, err := a.Files(ctx)
	if err != nil {
		t.Fatalf("Files: %v", err)
	}
	if !slices.Contains(files, "hello.txt") || !slices.Contains(files, "sub/world.txt") {
		t.Errorf("Files = %v, want hello.txt + sub/world.txt", files)
	}
}

func TestAgent_Commit_RejectsEmptyAuthor(t *testing.T) {
	a := newAgentForCommitTests(t)
	_, err := a.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "x", Content: []byte("y")}},
		Message: "no author",
	})
	if err == nil {
		t.Fatal("Commit with empty Author returned nil")
	}
}

func TestAgent_Tip_ReturnsCommitUUID(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	rev, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f", Content: []byte("v1")}},
		Message: "c1",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	tip, err := a.Tip(ctx, "trunk")
	if err != nil {
		t.Fatalf("Tip: %v", err)
	}
	if tip != rev {
		t.Errorf("Tip = %q, want %q", tip, rev)
	}
}

func TestAgent_Tip_EmptyBranchReturnsEmptyNoError(t *testing.T) {
	a := newAgentForCommitTests(t)

	tip, err := a.Tip(context.Background(), "no-such-branch")
	if err != nil {
		t.Fatalf("Tip: %v (want nil)", err)
	}
	if tip != "" {
		t.Errorf("Tip = %q, want empty", tip)
	}
}

func TestAgent_Diff_BetweenTwoRevs(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	revA, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("alpha\n")}},
		Message: "v1",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit v1: %v", err)
	}
	revB, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("beta\n")}},
		Message: "v2",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit v2: %v", err)
	}

	diff, err := a.Diff(ctx, revA, revB)
	if err != nil {
		t.Fatalf("Diff: %v", err)
	}
	if !bytes.Contains(diff, []byte("alpha")) || !bytes.Contains(diff, []byte("beta")) {
		t.Errorf("Diff missing alpha/beta markers: %s", diff)
	}
}

func TestAgent_ExtractTo_PopulatesDir(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	rev, err := a.Commit(ctx, CommitOpts{
		Files: []FileToCommit{
			{Name: "a.txt", Content: []byte("AAA")},
			{Name: "b/c.txt", Content: []byte("CCC")},
		},
		Message: "initial",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	dir := t.TempDir()
	if err := a.ExtractTo(ctx, dir, rev); err != nil {
		t.Fatalf("ExtractTo: %v", err)
	}

	matches, err := filepath.Glob(filepath.Join(dir, "*.txt"))
	if err != nil {
		t.Fatalf("glob: %v", err)
	}
	if len(matches) == 0 {
		t.Errorf("ExtractTo produced no top-level .txt files in %s", dir)
	}
}

func TestAgent_ExtractTo_EmptyRevIsNoop(t *testing.T) {
	a := newAgentForCommitTests(t)
	dir := t.TempDir()
	if err := a.ExtractTo(context.Background(), dir, ""); err != nil {
		t.Errorf("ExtractTo with empty rev should be no-op, got: %v", err)
	}
}

func TestAgent_Config_ReadsProjectCode(t *testing.T) {
	a := newAgentForCommitTests(t)

	got, err := a.Config("project-code")
	if err != nil {
		t.Fatalf("Config: %v", err)
	}
	if got == "" || got != a.projectCode {
		t.Errorf("Config(project-code) = %q, want non-empty matching agent.projectCode (%q)", got, a.projectCode)
	}
}

func TestAgent_Sync_NoTransportReturnsError(t *testing.T) {
	a := newAgentForCommitTests(t)
	if _, err := a.Sync(context.Background()); err == nil {
		t.Fatal("Sync with no transport should error")
	}
}

func TestAgent_SyncTo_RejectsEmptyURL(t *testing.T) {
	a := newAgentForCommitTests(t)
	if _, err := a.SyncTo(context.Background(), "", SyncOpts{Push: true}); err == nil {
		t.Fatal("SyncTo with empty URL should error")
	}
}
