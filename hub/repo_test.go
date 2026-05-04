package hub

import (
	"bytes"
	"context"
	"path/filepath"
	"testing"
)

// TestOpenRepo_RoundTrip exercises every *Repo method via OpenRepo on a
// path that NewHub previously bootstrapped — proving CLI-style and
// daemon-style code paths produce equivalent observable state.
func TestOpenRepo_RoundTrip(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "hub.fossil")

	// Phase 1: bootstrap via NewHub, seed some state.
	h, err := NewHub(context.Background(), Config{RepoPath: repoPath})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	if _, err := h.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "seed.txt", Content: []byte("seed")}},
		Message: "seed",
		Author:  "hub",
	}); err != nil {
		t.Fatalf("Hub.Commit: %v", err)
	}
	if err := h.AddUser(User{Login: "alice", Caps: "admin"}); err != nil {
		t.Fatalf("Hub.AddUser: %v", err)
	}
	_ = h.Stop()

	// Phase 2: re-open via OpenRepo — same state should be visible.
	r, err := OpenRepo(repoPath)
	if err != nil {
		t.Fatalf("OpenRepo: %v", err)
	}
	t.Cleanup(func() { _ = r.Close() })

	got, err := r.Read(context.Background(), "seed.txt")
	if err != nil {
		t.Fatalf("Repo.Read: %v", err)
	}
	if !bytes.Equal(got, []byte("seed")) {
		t.Errorf("Repo.Read seed.txt = %q, want %q", got, "seed")
	}

	if !r.HasUser("alice") {
		t.Error("Repo.HasUser(alice) = false after NewHub seeded")
	}

	user, err := r.GetUser("alice")
	if err != nil {
		t.Fatalf("Repo.GetUser: %v", err)
	}
	if user.Login != "alice" || user.Caps != "admin" {
		t.Errorf("Repo.GetUser = %+v, want {alice admin}", user)
	}

	users, err := r.ListUsers()
	if err != nil {
		t.Fatalf("Repo.ListUsers: %v", err)
	}
	if len(users) == 0 {
		t.Error("Repo.ListUsers returned empty")
	}

	// New commit + new user via the *Repo path.
	rev, err := r.Commit(context.Background(), CommitOpts{
		Files:   []FileToCommit{{Name: "from-repo.txt", Content: []byte("v")}},
		Message: "from repo",
		Author:  "alice",
	})
	if err != nil {
		t.Fatalf("Repo.Commit: %v", err)
	}
	if rev == "" {
		t.Fatal("Repo.Commit returned empty RevID")
	}
	gotRev, err := r.ReadAt(context.Background(), rev, "from-repo.txt")
	if err != nil {
		t.Fatalf("Repo.ReadAt: %v", err)
	}
	if !bytes.Equal(gotRev, []byte("v")) {
		t.Errorf("Repo.ReadAt = %q, want %q", gotRev, "v")
	}

	if err := r.AddUser(User{Login: "bob", Caps: "ro"}); err != nil {
		t.Fatalf("Repo.AddUser: %v", err)
	}
	if err := r.RemoveUser("bob"); err != nil {
		t.Fatalf("Repo.RemoveUser: %v", err)
	}
	if r.HasUser("bob") {
		t.Error("HasUser(bob) = true after RemoveUser")
	}
}

func TestOpenRepo_RejectsEmptyPath(t *testing.T) {
	if _, err := OpenRepo(""); err == nil {
		t.Fatal("OpenRepo with empty path should error")
	}
}

func TestOpenRepo_RejectsMissingPath(t *testing.T) {
	if _, err := OpenRepo(filepath.Join(t.TempDir(), "does-not-exist.fossil")); err == nil {
		t.Fatal("OpenRepo with non-existent path should error")
	}
}

func TestRepo_Close_IsIdempotent(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "hub.fossil")

	// Bootstrap a fossil at this path so OpenRepo can succeed.
	h, err := NewHub(context.Background(), Config{RepoPath: repoPath})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	_ = h.Stop()

	r, err := OpenRepo(repoPath)
	if err != nil {
		t.Fatalf("OpenRepo: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("first Close: %v", err)
	}
	if err := r.Close(); err != nil {
		t.Fatalf("second Close: %v (want nil)", err)
	}
}

// TestHub_Repo_AccessorReturnsLiveHandle proves Hub.Repo() returns a *Repo
// whose lifecycle is bound to the Hub. Operations against it observe the
// same state as Hub.* methods.
func TestHub_Repo_AccessorReturnsLiveHandle(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "hub.fossil")

	h, err := NewHub(context.Background(), Config{RepoPath: repoPath})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	if err := h.AddUser(User{Login: "alice"}); err != nil {
		t.Fatalf("Hub.AddUser: %v", err)
	}

	r := h.Repo()
	if r == nil {
		t.Fatal("Hub.Repo returned nil")
	}
	if !r.HasUser("alice") {
		t.Error("Repo.HasUser(alice) = false after Hub.AddUser")
	}

	// Adding via *Repo is observable via *Hub.
	if err := r.AddUser(User{Login: "bob"}); err != nil {
		t.Fatalf("Repo.AddUser: %v", err)
	}
	if !h.HasUser("bob") {
		t.Error("Hub.HasUser(bob) = false after Repo.AddUser")
	}
}
