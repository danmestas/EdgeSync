package hub

import (
	"bytes"
	"context"
	"path/filepath"
	"testing"
	"time"

	_ "github.com/danmestas/libfossil/db/driver/modernc"
)

// sharedProjectCode is a fixed 40-char lowercase hex string used as the
// project-code across the two hubs in cross-leaf tests. Format matches
// libfossil's validation (^[0-9a-f]{40}$).
const sharedProjectCode = "1111111111111111111111111111111111111111"

// TestCrossLeaf_SharedProjectCode_PropagatesCommit asserts the headline
// fix for issue #156: two hubs with a shared project-code, linked over a
// NATS leafnode connection, see each other's commits via the
// .commit → pull-on-notify flow.
func TestCrossLeaf_SharedProjectCode_PropagatesCommit(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	dirA := t.TempDir()
	hubA, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirA, "hubA.fossil"),
		ProjectCode: sharedProjectCode,
		NobodyCaps:  "gio", // allow unauthenticated xfer
	})
	if err != nil {
		t.Fatalf("NewHub A: %v", err)
	}
	t.Cleanup(func() { _ = hubA.Stop() })

	httpCtxA, httpCancelA := context.WithCancel(context.Background())
	t.Cleanup(httpCancelA)
	go func() { _ = hubA.ServeHTTP(httpCtxA) }()

	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:     filepath.Join(dirB, "hubB.fossil"),
		ProjectCode:  sharedProjectCode,
		LeafUpstream: hubA.LeafURL(),
		NobodyCaps:   "gio",
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	httpCtxB, httpCancelB := context.WithCancel(context.Background())
	t.Cleanup(httpCancelB)
	go func() { _ = hubB.ServeHTTP(httpCtxB) }()

	waitFor(t, 5*time.Second, "leaf link A↔B", func() bool {
		return hubA.NumLeafs() >= 1
	})

	// Commit on A, expect B to see the file via pull-on-notify.
	if _, err := hubA.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "from-a.txt", Content: []byte("hello-from-A")}},
		Message: "commit on A",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("hubA.Commit: %v", err)
	}

	waitFor(t, 10*time.Second, "B sees from-a.txt at trunk", func() bool {
		got, readErr := hubB.Read(ctx, "from-a.txt")
		return readErr == nil && bytes.Equal(got, []byte("hello-from-A"))
	})

	// Reverse direction: commit on B, expect A to see it.
	if _, err := hubB.Commit(ctx, CommitOpts{
		Files:   []FileToCommit{{Name: "from-b.txt", Content: []byte("hello-from-B")}},
		Message: "commit on B",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("hubB.Commit: %v", err)
	}

	waitFor(t, 10*time.Second, "A sees from-b.txt at trunk", func() bool {
		got, readErr := hubA.Read(ctx, "from-b.txt")
		return readErr == nil && bytes.Equal(got, []byte("hello-from-B"))
	})
}

// TestCrossLeaf_SubjectsAlign asserts that two hubs declaring the same
// ProjectCode end up subscribed to the same .sync and .commit subjects.
// Cheap structural check separate from the propagation gold-path.
func TestCrossLeaf_SubjectsAlign(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	dirA := t.TempDir()
	hubA, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirA, "hubA.fossil"),
		ProjectCode: sharedProjectCode,
	})
	if err != nil {
		t.Fatalf("NewHub A: %v", err)
	}
	t.Cleanup(func() { _ = hubA.Stop() })

	dirB := t.TempDir()
	hubB, err := NewHub(ctx, Config{
		RepoPath:    filepath.Join(dirB, "hubB.fossil"),
		ProjectCode: sharedProjectCode,
	})
	if err != nil {
		t.Fatalf("NewHub B: %v", err)
	}
	t.Cleanup(func() { _ = hubB.Stop() })

	if hubA.FossilSyncSubject() != hubB.FossilSyncSubject() {
		t.Errorf("sync subjects diverge: A=%q B=%q", hubA.FossilSyncSubject(), hubB.FossilSyncSubject())
	}
	if hubA.FossilCommitSubject() != hubB.FossilCommitSubject() {
		t.Errorf("commit subjects diverge: A=%q B=%q", hubA.FossilCommitSubject(), hubB.FossilCommitSubject())
	}
	wantSync := "fossil." + sharedProjectCode + ".sync"
	wantCommit := "fossil." + sharedProjectCode + ".commit"
	if hubA.FossilSyncSubject() != wantSync {
		t.Errorf("sync subject = %q, want %q", hubA.FossilSyncSubject(), wantSync)
	}
	if hubA.FossilCommitSubject() != wantCommit {
		t.Errorf("commit subject = %q, want %q", hubA.FossilCommitSubject(), wantCommit)
	}
}

// TestNewHub_ProjectCodeMismatchOnExistingRepo asserts the drift-guard:
// reopening a repo with a different ProjectCode than what's on disk
// fails fast.
func TestNewHub_ProjectCodeMismatchOnExistingRepo(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	dir := t.TempDir()
	path := filepath.Join(dir, "hub.fossil")

	h, err := NewHub(ctx, Config{
		RepoPath:    path,
		ProjectCode: sharedProjectCode,
	})
	if err != nil {
		t.Fatalf("first NewHub: %v", err)
	}
	if err := h.Stop(); err != nil {
		t.Fatalf("Stop: %v", err)
	}

	const otherCode = "2222222222222222222222222222222222222222"
	if _, err := NewHub(ctx, Config{
		RepoPath:    path,
		ProjectCode: otherCode,
	}); err == nil {
		t.Fatal("NewHub with mismatched ProjectCode returned nil err; want config-drift error")
	}
}

// waitFor polls cond every 50ms until it returns true or timeout fires.
// Reports the description on timeout for diagnostic context.
func waitFor(t *testing.T, timeout time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("timeout after %v waiting for: %s", timeout, what)
}
