package hub

import (
	"context"
	"testing"

	_ "github.com/danmestas/libfossil/db/driver/modernc"
)

// TestCommit_BranchTags_LandsOnNamedBranch verifies that supplying the fossil
// branch tag pair (branch=<name> + sym-<name>=*) on CommitOpts.Tags lands the
// checkin on that branch. After the commit, BranchTip(name) resolves to the
// new commit and trunk's tip is unchanged.
func TestCommit_BranchTags_LandsOnNamedBranch(t *testing.T) {
	h := newTestHub(t)
	ctx := context.Background()

	trunkRev, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "seed.txt", Content: []byte("seed\n")}},
		Message: "seed trunk",
		Author:  "hub",
	})
	if err != nil {
		t.Fatalf("seed Commit: %v", err)
	}
	r := h.Repo().handle
	trunkTipBefore, err := r.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip(trunk) before: %v", err)
	}

	const branch = "feature/x"
	branchRev, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "branch.txt", Content: []byte("on branch\n")}},
		Message: "first commit on " + branch,
		Author:  "hub",
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

	branchTip, err := r.BranchTip(branch)
	if err != nil {
		t.Fatalf("BranchTip(%q): %v", branch, err)
	}
	branchRID, err := r.ResolveVersion(string(branchRev))
	if err != nil {
		t.Fatalf("ResolveVersion(%s): %v", branchRev, err)
	}
	if branchTip != branchRID {
		t.Errorf("BranchTip(%q) = %d, want %d (the new commit)", branch, branchTip, branchRID)
	}

	trunkTipAfter, err := r.BranchTip("trunk")
	if err != nil {
		t.Fatalf("BranchTip(trunk) after: %v", err)
	}
	if trunkTipAfter != trunkTipBefore {
		t.Errorf("trunk tip moved: was %d, now %d — branch commit must not advance trunk",
			trunkTipBefore, trunkTipAfter)
	}

	got, err := h.ReadAt(ctx, branchRev, "branch.txt")
	if err != nil {
		t.Fatalf("ReadAt(branchRev): %v", err)
	}
	if string(got) != "on branch\n" {
		t.Errorf("ReadAt(branchRev) = %q, want %q", got, "on branch\n")
	}
}

// TestCommit_BranchTags_PropagateForward verifies that a second commit
// carrying the same branch tag pair lands on the branch's tip (parented
// by the previous branch commit), not on trunk. This is the propagation
// path: subsequent commits don't fork unless explicitly told to.
func TestCommit_BranchTags_PropagateForward(t *testing.T) {
	h := newTestHub(t)
	ctx := context.Background()

	if _, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "seed.txt", Content: []byte("seed\n")}},
		Message: "seed trunk",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("seed Commit: %v", err)
	}

	const branch = "feature/y"
	branchTags := []TagSpec{
		{Name: "branch", Value: branch},
		{Name: "sym-" + branch, Value: "*"},
	}

	first, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("v1\n")}},
		Message: "first on branch",
		Author:  "hub",
		Tags:    branchTags,
	})
	if err != nil {
		t.Fatalf("first branch Commit: %v", err)
	}

	second, err := h.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "f.txt", Content: []byte("v2\n")}},
		Message: "second on branch",
		Author:  "hub",
		Tags:    branchTags,
	})
	if err != nil {
		t.Fatalf("second branch Commit: %v", err)
	}
	if first == second {
		t.Fatalf("second commit returned same RevID as first: %s", first)
	}

	r := h.Repo().handle
	tip, err := r.BranchTip(branch)
	if err != nil {
		t.Fatalf("BranchTip(%q): %v", branch, err)
	}
	secondRID, err := r.ResolveVersion(string(second))
	if err != nil {
		t.Fatalf("ResolveVersion(second): %v", err)
	}
	if tip != secondRID {
		t.Errorf("BranchTip(%q) = %d, want %d (the second commit)", branch, tip, secondRID)
	}

	got, err := h.ReadAt(ctx, RevID(branch), "f.txt")
	if err != nil {
		t.Fatalf("ReadAt(branch symbolic): %v", err)
	}
	if string(got) != "v2\n" {
		t.Errorf("ReadAt(%q, f.txt) = %q, want %q", branch, got, "v2\n")
	}
}
