package notify

import (
	"context"
	"path/filepath"
	"testing"

	libfossil "github.com/danmestas/libfossil"
)

func TestNewServiceFromPath_FreshPath_CreatesRepo(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "notify.fossil")

	svc, err := NewServiceFromPath(ServiceFromPathConfig{
		RepoPath: repoPath,
		From:     "endpoint-test",
		FromName: "test",
	})
	if err != nil {
		t.Fatalf("NewServiceFromPath: %v", err)
	}
	t.Cleanup(func() { _ = svc.Close() })

	// Round-trip a message through the Service.
	msg, err := svc.Send(SendOpts{Project: "p1", Body: "hello"})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	got, err := svc.ReadThread(context.Background(), "p1", msg.ThreadShort())
	if err != nil {
		t.Fatalf("ReadThread: %v", err)
	}
	if len(got) != 1 || got[0].Body != "hello" {
		t.Errorf("ReadThread = %+v, want one msg with Body=hello", got)
	}
}

func TestNewServiceFromPath_ExistingPath_OpensRepo(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "notify.fossil")

	// Phase 1: create + send via path-based constructor.
	svc1, err := NewServiceFromPath(ServiceFromPathConfig{
		RepoPath: repoPath,
		From:     "endpoint-test",
		FromName: "test",
	})
	if err != nil {
		t.Fatalf("NewServiceFromPath phase 1: %v", err)
	}
	msg, err := svc1.Send(SendOpts{Project: "p1", Body: "first"})
	if err != nil {
		t.Fatalf("Send phase 1: %v", err)
	}
	if err := svc1.Close(); err != nil {
		t.Fatalf("Close phase 1: %v", err)
	}

	// Phase 2: re-open via the path constructor and confirm the message
	// is still readable.
	svc2, err := NewServiceFromPath(ServiceFromPathConfig{
		RepoPath: repoPath,
		From:     "endpoint-test",
		FromName: "test",
	})
	if err != nil {
		t.Fatalf("NewServiceFromPath phase 2: %v", err)
	}
	t.Cleanup(func() { _ = svc2.Close() })

	got, err := svc2.ReadThread(context.Background(), "p1", msg.ThreadShort())
	if err != nil {
		t.Fatalf("ReadThread phase 2: %v", err)
	}
	if len(got) != 1 || got[0].Body != "first" {
		t.Errorf("ReadThread after reopen = %+v, want one msg with Body=first", got)
	}
}

func TestNewServiceFromPath_RejectsEmptyPath(t *testing.T) {
	_, err := NewServiceFromPath(ServiceFromPathConfig{})
	if err == nil {
		t.Fatal("NewServiceFromPath with empty RepoPath should error")
	}
}

// TestNewServiceFromPath_ClosesOwnedRepo verifies that Close() releases the
// repo file when constructed via NewServiceFromPath. We confirm by re-
// opening the same path with libfossil.Open after Close — that should
// succeed without "database is locked" errors that the prior owner would
// otherwise hold.
func TestNewServiceFromPath_ClosesOwnedRepo(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "notify.fossil")

	svc, err := NewServiceFromPath(ServiceFromPathConfig{
		RepoPath: repoPath,
		From:     "endpoint-test",
	})
	if err != nil {
		t.Fatalf("NewServiceFromPath: %v", err)
	}
	if err := svc.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	// Should be able to open the repo independently after Service.Close.
	r, err := libfossil.Open(repoPath)
	if err != nil {
		t.Fatalf("libfossil.Open after Service.Close: %v", err)
	}
	r.Close()
}

// TestNewService_DoesNotOwnRepo verifies the existing NewService contract
// is unchanged: Close() does NOT close the repo.
func TestNewService_DoesNotOwnRepo(t *testing.T) {
	r := createTestRepo(t) // owned by test harness, registered cleanup
	svc, err := NewService(ServiceConfig{Repo: r, From: "x"})
	if err != nil {
		t.Fatalf("NewService: %v", err)
	}
	if err := svc.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	// Repo should still be open — Config() should succeed against it.
	if _, err := r.Config("project-code"); err != nil {
		t.Errorf("repo unexpectedly closed by Service.Close: %v", err)
	}
}
