package notify

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestNewMessage(t *testing.T) {
	m := NewMessage("myproject", ActionCheckin, "checkin completed")

	if m.ID == "" {
		t.Error("ID should be non-empty")
	}
	if len(m.ID) != 32 {
		t.Errorf("ID length = %d, want 32 hex chars", len(m.ID))
	}
	if !strings.HasPrefix(m.ThreadID, "thread-") {
		t.Errorf("ThreadID = %q, want prefix thread-", m.ThreadID)
	}
	if len(m.ThreadID) != len("thread-")+32 {
		t.Errorf("ThreadID length = %d, want %d", len(m.ThreadID), len("thread-")+32)
	}
	if m.Project != "myproject" {
		t.Errorf("Project = %q, want %q", m.Project, "myproject")
	}
	if m.Action != ActionCheckin {
		t.Errorf("Action = %q, want %q", m.Action, ActionCheckin)
	}
	if m.Body != "checkin completed" {
		t.Errorf("Body = %q, want %q", m.Body, "checkin completed")
	}
	if m.ReplyTo != "" {
		t.Errorf("ReplyTo = %q, want empty for root message", m.ReplyTo)
	}
	if m.ActionResponse {
		t.Error("ActionResponse should be false for root message")
	}
	if m.Timestamp == 0 {
		t.Error("Timestamp should be non-zero")
	}
}

func TestNewReply(t *testing.T) {
	parent := NewMessage("proj", ActionSync, "sync started")

	reply := NewReply(parent, "sync complete")

	if reply.ID == "" {
		t.Error("ID should be non-empty")
	}
	if reply.ID == parent.ID {
		t.Error("reply ID should differ from parent ID")
	}
	if reply.ThreadID != parent.ThreadID {
		t.Errorf("ThreadID = %q, want %q", reply.ThreadID, parent.ThreadID)
	}
	if reply.ReplyTo != parent.ID {
		t.Errorf("ReplyTo = %q, want %q", reply.ReplyTo, parent.ID)
	}
	if reply.Project != parent.Project {
		t.Errorf("Project = %q, want %q", reply.Project, parent.Project)
	}
	if reply.ActionResponse {
		t.Error("ActionResponse should be false unless explicitly set")
	}
	if reply.Body != "sync complete" {
		t.Errorf("Body = %q, want %q", reply.Body, "sync complete")
	}
}

func TestActionReplyViaNewReply(t *testing.T) {
	parent := NewMessage("proj", ActionCheckin, "approve?")

	reply := NewReply(parent, "approved")
	reply.ActionResponse = true

	if !reply.ActionResponse {
		t.Error("ActionResponse should be true after setting it")
	}
	if reply.ReplyTo != parent.ID {
		t.Errorf("ReplyTo = %q, want %q", reply.ReplyTo, parent.ID)
	}
	if reply.ThreadID != parent.ThreadID {
		t.Errorf("ThreadID should be preserved from parent")
	}
}

func TestMessageJSON(t *testing.T) {
	m := NewMessage("testproject", ActionSync, "hello")
	m.Priority = PriorityUrgent

	data, err := json.Marshal(m)
	if err != nil {
		t.Fatalf("json.Marshal failed: %v", err)
	}

	var m2 Message
	if err := json.Unmarshal(data, &m2); err != nil {
		t.Fatalf("json.Unmarshal failed: %v", err)
	}

	if m2.ID != m.ID {
		t.Errorf("ID = %q, want %q", m2.ID, m.ID)
	}
	if m2.ThreadID != m.ThreadID {
		t.Errorf("ThreadID = %q, want %q", m2.ThreadID, m.ThreadID)
	}
	if m2.Project != m.Project {
		t.Errorf("Project = %q, want %q", m2.Project, m.Project)
	}
	if m2.Action != m.Action {
		t.Errorf("Action = %q, want %q", m2.Action, m.Action)
	}
	if m2.Body != m.Body {
		t.Errorf("Body = %q, want %q", m2.Body, m.Body)
	}
	if m2.Priority != PriorityUrgent {
		t.Errorf("Priority = %q, want %q", m2.Priority, PriorityUrgent)
	}
	if m2.Timestamp != m.Timestamp {
		t.Errorf("Timestamp = %d, want %d", m2.Timestamp, m.Timestamp)
	}
}

func TestMessageDefaultPriority(t *testing.T) {
	m := NewMessage("proj", ActionCheckin, "body")

	if m.Priority != PriorityInfo {
		t.Errorf("Priority = %q, want %q (default)", m.Priority, PriorityInfo)
	}
}

func TestMessageFilePath(t *testing.T) {
	m := NewMessage("myproject", ActionSync, "body")
	// Set a known timestamp for deterministic path
	m.Timestamp = 1712000000
	// Set known IDs for deterministic path
	m.ID = "abcdef1234567890abcdef1234567890"
	m.ThreadID = "thread-" + "fedcba9876543210fedcba9876543210"

	path := m.FilePath()

	// format: <project>/threads/<thread-short>/<unix-timestamp>-<msg-short>.json
	// thread-short = first 8 chars after "thread-" prefix
	// msg-short = first 8 chars of ID
	want := "myproject/threads/fedcba98/1712000000-abcdef12.json"
	if path != want {
		t.Errorf("FilePath() = %q, want %q", path, want)
	}
}

func TestMessageNATSSubject(t *testing.T) {
	m := NewMessage("myproject", ActionSync, "body")
	m.ThreadID = "thread-" + "fedcba9876543210fedcba9876543210"

	subject := m.NATSSubject()

	want := "notify.myproject.fedcba98"
	if subject != want {
		t.Errorf("NATSSubject() = %q, want %q", subject, want)
	}
}

func TestThreadShort(t *testing.T) {
	m := NewMessage("proj", ActionCheckin, "body")
	m.ThreadID = "thread-" + "abcdef1234567890abcdef1234567890"

	short := m.ThreadShort()

	if short != "abcdef12" {
		t.Errorf("ThreadShort() = %q, want %q", short, "abcdef12")
	}
}
