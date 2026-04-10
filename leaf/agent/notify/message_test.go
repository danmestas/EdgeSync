package notify

import (
	"encoding/json"
	"testing"
	"time"
)

func TestNewMessage(t *testing.T) {
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

	if msg.V != 1 {
		t.Errorf("V = %d, want 1", msg.V)
	}
	if msg.Project != "edgesync" {
		t.Errorf("Project = %q, want %q", msg.Project, "edgesync")
	}
	if msg.From != "endpoint-abc123" {
		t.Errorf("From = %q, want %q", msg.From, "endpoint-abc123")
	}
	if msg.Body != "Build failed. Retry?" {
		t.Errorf("Body = %q, want %q", msg.Body, "Build failed. Retry?")
	}
	if msg.Priority != PriorityActionRequired {
		t.Errorf("Priority = %q, want %q", msg.Priority, PriorityActionRequired)
	}
	if len(msg.Actions) != 2 {
		t.Fatalf("Actions len = %d, want 2", len(msg.Actions))
	}
	if msg.ID == "" {
		t.Error("ID should not be empty")
	}
	if msg.Thread == "" {
		t.Error("Thread should not be empty")
	}
	if msg.Timestamp.IsZero() {
		t.Error("Timestamp should not be zero")
	}
}

func TestNewReply(t *testing.T) {
	original := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Deploy?",
	})

	reply := NewReply(original, ReplyOpts{
		From:     "endpoint-xyz789",
		FromName: "dan-iphone",
		Body:     "yes",
	})

	if reply.Thread != original.Thread {
		t.Errorf("reply Thread = %q, want %q (same as original)", reply.Thread, original.Thread)
	}
	if reply.ReplyTo != original.ID {
		t.Errorf("reply ReplyTo = %q, want %q", reply.ReplyTo, original.ID)
	}
	if reply.Project != original.Project {
		t.Errorf("reply Project = %q, want %q", reply.Project, original.Project)
	}
}

func TestActionReplyViaNewReply(t *testing.T) {
	original := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Retry?",
		Actions:  []Action{{ID: "retry", Label: "Retry"}},
	})

	reply := NewReply(original, ReplyOpts{From: "endpoint-xyz789", FromName: "dan-iphone", Body: "retry"})
	reply.ActionResponse = true

	if reply.Body != "retry" {
		t.Errorf("Body = %q, want %q", reply.Body, "retry")
	}
	if !reply.ActionResponse {
		t.Error("ActionResponse should be true")
	}
	if reply.Thread != original.Thread {
		t.Errorf("Thread = %q, want %q", reply.Thread, original.Thread)
	}
}

func TestMessageJSON(t *testing.T) {
	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "hello",
	})

	data, err := json.Marshal(msg)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}

	var decoded Message
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}

	if decoded.ID != msg.ID {
		t.Errorf("roundtrip ID = %q, want %q", decoded.ID, msg.ID)
	}
	if decoded.Body != msg.Body {
		t.Errorf("roundtrip Body = %q, want %q", decoded.Body, msg.Body)
	}
}

func TestMessageDefaultPriority(t *testing.T) {
	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "FYI: build done",
	})

	if msg.Priority != PriorityInfo {
		t.Errorf("default Priority = %q, want %q", msg.Priority, PriorityInfo)
	}
}

func TestMessageFilePath(t *testing.T) {
	msg := Message{
		Project:   "edgesync",
		Thread:    "thread-a1b2c3d4e5f6a7b8",
		ID:        "msg-f9e8d7c6b5a4f3e2",
		Timestamp: time.Date(2026, 4, 10, 12, 0, 0, 0, time.UTC),
	}

	got := msg.FilePath()
	want := "edgesync/threads/a1b2c3d4/1775822400-f9e8d7c6.json"
	if got != want {
		t.Errorf("FilePath() = %q, want %q", got, want)
	}
}

func TestMessageNATSSubject(t *testing.T) {
	msg := Message{
		Project: "edgesync",
		Thread:  "thread-a1b2c3d4e5f6a7b8",
	}

	got := msg.NATSSubject()
	want := "notify.edgesync.a1b2c3d4"
	if got != want {
		t.Errorf("NATSSubject() = %q, want %q", got, want)
	}
}

func TestThreadShort(t *testing.T) {
	msg := Message{Thread: "thread-abcdef1234567890"}
	got := msg.ThreadShort()
	want := "abcdef12"
	if got != want {
		t.Errorf("ThreadShort() = %q, want %q", got, want)
	}
}
