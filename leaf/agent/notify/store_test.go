package notify

import (
	"testing"

	libfossil "github.com/danmestas/go-libfossil"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
)

func createTestRepo(t *testing.T) *libfossil.Repo {
	t.Helper()
	path := t.TempDir() + "/notify.fossil"
	r, err := InitNotifyRepo(path)
	if err != nil {
		t.Fatalf("InitNotifyRepo: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestInitAndCommitMessage(t *testing.T) {
	r := createTestRepo(t)

	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Build failed. Retry?",
		Priority: PriorityActionRequired,
		Actions: []Action{
			{ID: "retry", Label: "Retry"},
			{ID: "skip", Label: "Skip"},
		},
	})

	if err := CommitMessage(r, msg); err != nil {
		t.Fatalf("CommitMessage: %v", err)
	}

	got, err := ReadMessage(r, msg.FilePath())
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}

	if got.ID != msg.ID {
		t.Errorf("ID = %q, want %q", got.ID, msg.ID)
	}
	if got.Body != msg.Body {
		t.Errorf("Body = %q, want %q", got.Body, msg.Body)
	}
	if got.Project != msg.Project {
		t.Errorf("Project = %q, want %q", got.Project, msg.Project)
	}
	if got.Priority != msg.Priority {
		t.Errorf("Priority = %q, want %q", got.Priority, msg.Priority)
	}
	if got.Thread != msg.Thread {
		t.Errorf("Thread = %q, want %q", got.Thread, msg.Thread)
	}
	if len(got.Actions) != 2 {
		t.Errorf("Actions len = %d, want 2", len(got.Actions))
	}
}

func TestOpenExistingRepo(t *testing.T) {
	path := t.TempDir() + "/notify.fossil"
	r, err := InitNotifyRepo(path)
	if err != nil {
		t.Fatalf("InitNotifyRepo: %v", err)
	}

	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "hello",
	})
	if err := CommitMessage(r, msg); err != nil {
		t.Fatalf("CommitMessage: %v", err)
	}
	r.Close()

	// Reopen.
	r2, err := libfossil.Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer r2.Close()

	got, err := ReadMessage(r2, msg.FilePath())
	if err != nil {
		t.Fatalf("ReadMessage after reopen: %v", err)
	}
	if got.ID != msg.ID {
		t.Errorf("ID = %q, want %q", got.ID, msg.ID)
	}
}
