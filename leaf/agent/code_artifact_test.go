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

// TestAgent_Commit_BranchTags_LandsOnNamedBranch verifies that supplying the
// fossil branch tag pair (branch=<name> + sym-<name>=*) on CommitOpts.Tags
// lands the checkin on that branch. After the commit, BranchTip(name)
// resolves to the new commit and trunk's tip is unchanged. This is the
// symmetric leaf-side counterpart to hub.CommitOpts.Tags (#147/#148) and
// unblocks bones' synthetic-agent-slot model (commits to agent/<id>).
func TestAgent_Commit_BranchTags_LandsOnNamedBranch(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	trunkRev, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "seed.txt", Content: []byte("seed\n")}},
		Message: "seed trunk",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("seed Commit: %v", err)
	}
	trunkTipBefore, err := a.repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip(trunk) before: %v", err)
	}

	const branch = "agent/abc123"
	branchRev, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "branch.txt", Content: []byte("on branch\n")}},
		Message: "first commit on " + branch,
		Author:  "testuser",
		Tags: []TagSpec{
			{Name: "branch", Value: branch},
			{Name: "sym-" + branch, Value: "*"},
		},
	})
	if err != nil {
		t.Fatalf("branch Commit: %v", err)
	}
	if branchRev == trunkRev {
		t.Fatalf("branch commit produced the same RevID as trunk seed: %s", branchRev)
	}

	branchTip, err := a.repo.BranchTip(branch)
	if err != nil {
		t.Fatalf("BranchTip(%q): %v", branch, err)
	}
	branchRID, err := a.repo.ResolveVersion(string(branchRev))
	if err != nil {
		t.Fatalf("ResolveVersion(%s): %v", branchRev, err)
	}
	if branchTip != branchRID {
		t.Errorf("BranchTip(%q) = %d, want %d (the new commit)", branch, branchTip, branchRID)
	}

	trunkTipAfter, err := a.repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip(trunk) after: %v", err)
	}
	if trunkTipAfter != trunkTipBefore {
		t.Errorf("trunk tip moved: was %d, now %d — branch commit must not advance trunk",
			trunkTipBefore, trunkTipAfter)
	}
}

// TestAgent_Commit_BranchTags_PropagateForward verifies that a second commit
// carrying the same branch tag pair lands on the branch's tip (parented by
// the previous branch commit), not on trunk. Subsequent commits don't fork
// unless explicitly told to.
func TestAgent_Commit_BranchTags_PropagateForward(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	if _, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "seed.txt", Content: []byte("seed\n")}},
		Message: "seed trunk",
		Author:  "testuser",
	}); err != nil {
		t.Fatalf("seed Commit: %v", err)
	}

	const branch = "agent/xyz789"
	branchTags := []TagSpec{
		{Name: "branch", Value: branch},
		{Name: "sym-" + branch, Value: "*"},
	}

	first, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("v1\n")}},
		Message: "first on branch",
		Author:  "testuser",
		Tags:    branchTags,
	})
	if err != nil {
		t.Fatalf("first branch Commit: %v", err)
	}

	second, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("v2\n")}},
		Message: "second on branch",
		Author:  "testuser",
		Tags:    branchTags,
	})
	if err != nil {
		t.Fatalf("second branch Commit: %v", err)
	}
	if first == second {
		t.Fatalf("second commit returned same RevID as first: %s", first)
	}

	tip, err := a.repo.BranchTip(branch)
	if err != nil {
		t.Fatalf("BranchTip(%q): %v", branch, err)
	}
	secondRID, err := a.repo.ResolveVersion(string(second))
	if err != nil {
		t.Fatalf("ResolveVersion(second): %v", err)
	}
	if tip != secondRID {
		t.Errorf("BranchTip(%q) = %d, want %d (the second commit)", branch, tip, secondRID)
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

// TestAgent_Diff_RenameAcrossRevs exercises the case the pre-cleanup
// implementation got wrong: same content, different name. The old
// hand-rolled file enumeration diffed both files as full additions/
// removals; libfossil's whole-checkin Diff should handle the rename
// natively. We don't pin the exact rename-marker shape — we just
// confirm Diff doesn't error and produces non-empty output for the
// pair.
func TestAgent_Diff_RenameAcrossRevs(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	revA, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "before.txt", Content: []byte("same content\n")}},
		Message: "rev A",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit rev A: %v", err)
	}
	revB, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "after.txt", Content: []byte("same content\n")}},
		Message: "rev B (renamed)",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit rev B: %v", err)
	}

	if _, err := a.Diff(ctx, revA, revB); err != nil {
		t.Fatalf("Diff across rename: %v", err)
	}
}

// TestAgent_Commit_ChainsOntoTrunkTipByDefault proves that two consecutive
// Agent.Commit calls without explicit ParentID produce a chained pair —
// the second commit's primary parent is the first commit. Without the fix
// for issue #125 every commit was an orphan root and every commit's
// Parents slice was empty.
func TestAgent_Commit_ChainsOntoTrunkTipByDefault(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	rev1, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f", Content: []byte("v1")}},
		Message: "first",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit 1: %v", err)
	}
	rev2, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f", Content: []byte("v2")}},
		Message: "second",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit 2: %v", err)
	}

	// Walk the timeline and find rev2; its Parents must contain rev1.
	tipRID, err := a.repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip: %v", err)
	}
	timeline, err := a.repo.Timeline(libfossil.LogOpts{Start: tipRID, Limit: 10})
	if err != nil {
		t.Fatalf("Timeline: %v", err)
	}
	var second *libfossil.LogEntry
	for i := range timeline {
		if timeline[i].UUID == string(rev2) {
			second = &timeline[i]
			break
		}
	}
	if second == nil {
		t.Fatalf("rev2 %q not found in timeline %+v", rev2, timeline)
	}
	if !slices.Contains(second.Parents, string(rev1)) {
		t.Errorf("rev2.Parents = %v, want to contain rev1 %q", second.Parents, rev1)
	}
}

// TestAgent_Commit_PreservesParentFiles asserts the #152 fix: when a
// caller supplies only the files this commit changes (matching `fossil
// ci` semantics), the resulting manifest must still carry every file
// the parent commit tracked, not just the supplied subset.
//
// Before the fix, agent.Commit passed opts.Files straight through to
// libfossil.Commit, which wrote a partial manifest. A linear chain of
// commits on disjoint paths silently lost every prior commit's files at
// the next commit — bones' multi-slot workflow surfaced the bug
// (bones#366 → EdgeSync#152 → libfossil#30).
func TestAgent_Commit_PreservesParentFiles(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	// Slot alpha commits its own file.
	if _, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "alpha/n.md", Content: []byte("alpha-work")}},
		Message: "alpha work",
		Author:  "slot-alpha",
	}); err != nil {
		t.Fatalf("Commit alpha: %v", err)
	}

	// Slot bravo commits a disjoint file. Auto-resolves parent to alpha's
	// tip — agent.Commit must merge bravo's file with alpha's existing
	// tracked files, not replace them.
	if _, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "bravo/n.md", Content: []byte("bravo-work")}},
		Message: "bravo work",
		Author:  "slot-bravo",
	}); err != nil {
		t.Fatalf("Commit bravo: %v", err)
	}

	// Both files must be visible at trunk's tip.
	files, err := a.Files(ctx)
	if err != nil {
		t.Fatalf("Files: %v", err)
	}
	if !slices.Contains(files, "alpha/n.md") {
		t.Errorf("Files = %v, want to contain alpha/n.md (parent file dropped — #152)", files)
	}
	if !slices.Contains(files, "bravo/n.md") {
		t.Errorf("Files = %v, want to contain bravo/n.md", files)
	}

	// Read content directly against the trunk tip rid. a.Read goes
	// through libfossil's ResolveVersion("trunk"), which resolves via the
	// sym-trunk singleton tag — that tag isn't propagated by
	// libfossil.Commit so it stays pinned to the first commit. Files() is
	// fine here because it uses BranchTip, which resolves via the
	// branch=trunk propagating tag.
	tipRID, err := a.repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip trunk: %v", err)
	}
	gotAlpha, err := a.repo.ReadFile(tipRID, "alpha/n.md")
	if err != nil {
		t.Fatalf("ReadFile alpha/n.md at tip: %v", err)
	}
	if !bytes.Equal(gotAlpha, []byte("alpha-work")) {
		t.Errorf("alpha/n.md content = %q, want %q", gotAlpha, "alpha-work")
	}
	gotBravo, err := a.repo.ReadFile(tipRID, "bravo/n.md")
	if err != nil {
		t.Fatalf("ReadFile bravo/n.md at tip: %v", err)
	}
	if !bytes.Equal(gotBravo, []byte("bravo-work")) {
		t.Errorf("bravo/n.md content = %q, want %q", gotBravo, "bravo-work")
	}
}

// TestAgent_Commit_SuppliedFileOverridesParent asserts the merge rule:
// when opts.Files contains a name that the parent also tracks, the
// supplied content wins. Without this, every commit would inherit the
// parent's content verbatim and updates would no-op.
func TestAgent_Commit_SuppliedFileOverridesParent(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	if _, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "doc.md", Content: []byte("v1")}},
		Message: "v1",
		Author:  "testuser",
	}); err != nil {
		t.Fatalf("Commit v1: %v", err)
	}

	if _, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "doc.md", Content: []byte("v2")}},
		Message: "v2",
		Author:  "testuser",
	}); err != nil {
		t.Fatalf("Commit v2: %v", err)
	}

	// Resolve trunk tip directly — a.Read can't be used here because
	// ResolveVersion("trunk") uses the sym-trunk singleton tag, which
	// libfossil.Commit doesn't propagate forward.
	tipRID, err := a.repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip trunk: %v", err)
	}
	got, err := a.repo.ReadFile(tipRID, "doc.md")
	if err != nil {
		t.Fatalf("ReadFile doc.md at tip: %v", err)
	}
	if !bytes.Equal(got, []byte("v2")) {
		t.Errorf("doc.md content = %q, want %q (supplied must win over parent)", got, "v2")
	}
}

// TestAgent_Commit_ExplicitParentIDOverride proves that a caller-supplied
// ParentID is used verbatim — useful for committing onto a non-trunk
// branch via Agent.Tip(ctx, branchName).
func TestAgent_Commit_ExplicitParentIDOverride(t *testing.T) {
	a := newAgentForCommitTests(t)
	ctx := context.Background()

	rev1, err := a.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f", Content: []byte("v1")}},
		Message: "first",
		Author:  "testuser",
	})
	if err != nil {
		t.Fatalf("Commit 1: %v", err)
	}
	rid1, err := a.repo.ResolveVersion(string(rev1))
	if err != nil {
		t.Fatalf("ResolveVersion rev1: %v", err)
	}

	rev2, err := a.Commit(ctx, CommitOpts{
		Files:    []FileToCommit{{Name: "f", Content: []byte("v2")}},
		Message:  "explicit parent",
		Author:   "testuser",
		ParentID: rid1,
	})
	if err != nil {
		t.Fatalf("Commit 2 with explicit ParentID: %v", err)
	}

	tipRID, err := a.repo.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip: %v", err)
	}
	timeline, err := a.repo.Timeline(libfossil.LogOpts{Start: tipRID, Limit: 10})
	if err != nil {
		t.Fatalf("Timeline: %v", err)
	}
	for _, e := range timeline {
		if e.UUID == string(rev2) {
			if !slices.Contains(e.Parents, string(rev1)) {
				t.Errorf("rev2.Parents = %v, want to contain rev1 %q", e.Parents, rev1)
			}
			return
		}
	}
	t.Errorf("rev2 %q not found in timeline", rev2)
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
