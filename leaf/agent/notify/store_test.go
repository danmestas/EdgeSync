package notify

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"testing"
	"time"

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

func TestListThreads(t *testing.T) {
	r := createTestRepo(t)

	// Thread 1: two messages, action_required priority.
	msg1 := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Build started",
		Priority: PriorityInfo,
	})
	msg1.Timestamp = time.Date(2026, 4, 10, 10, 0, 0, 0, time.UTC)
	if err := CommitMessage(r, msg1); err != nil {
		t.Fatalf("CommitMessage msg1: %v", err)
	}

	msg1reply := NewReply(msg1, ReplyOpts{
		From:     "endpoint-xyz789",
		FromName: "dan-iphone",
		Body:     "Build failed — retry?",
	})
	msg1reply.Priority = PriorityActionRequired
	msg1reply.Timestamp = time.Date(2026, 4, 10, 10, 5, 0, 0, time.UTC)
	if err := CommitMessage(r, msg1reply); err != nil {
		t.Fatalf("CommitMessage msg1reply: %v", err)
	}

	// Thread 2: one message, urgent priority, later timestamp.
	msg2 := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Server down!",
		Priority: PriorityUrgent,
	})
	msg2.Timestamp = time.Date(2026, 4, 10, 11, 0, 0, 0, time.UTC)
	if err := CommitMessage(r, msg2); err != nil {
		t.Fatalf("CommitMessage msg2: %v", err)
	}

	threads, err := ListThreads(r, "edgesync")
	if err != nil {
		t.Fatalf("ListThreads: %v", err)
	}

	if len(threads) != 2 {
		t.Fatalf("thread count = %d, want 2", len(threads))
	}

	// Most recent first: thread 2 (11:00) before thread 1 (10:05).
	if threads[0].ThreadShort != msg2.ThreadShort() {
		t.Errorf("first thread = %q, want %q", threads[0].ThreadShort, msg2.ThreadShort())
	}
	if threads[1].ThreadShort != msg1.ThreadShort() {
		t.Errorf("second thread = %q, want %q", threads[1].ThreadShort, msg1.ThreadShort())
	}

	// Thread 2: urgent, 1 message.
	if threads[0].Priority != PriorityUrgent {
		t.Errorf("thread 2 priority = %q, want %q", threads[0].Priority, PriorityUrgent)
	}
	if threads[0].MessageCount != 1 {
		t.Errorf("thread 2 message count = %d, want 1", threads[0].MessageCount)
	}

	// Thread 1: action_required (highest in thread), 2 messages.
	if threads[1].Priority != PriorityActionRequired {
		t.Errorf("thread 1 priority = %q, want %q", threads[1].Priority, PriorityActionRequired)
	}
	if threads[1].MessageCount != 2 {
		t.Errorf("thread 1 message count = %d, want 2", threads[1].MessageCount)
	}
}

func TestReadThread(t *testing.T) {
	r := createTestRepo(t)

	msg1 := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Deploy request",
	})
	msg1.Timestamp = time.Date(2026, 4, 10, 10, 0, 0, 0, time.UTC)
	if err := CommitMessage(r, msg1); err != nil {
		t.Fatalf("CommitMessage msg1: %v", err)
	}

	msg2 := NewReply(msg1, ReplyOpts{
		From:     "endpoint-xyz789",
		FromName: "dan-iphone",
		Body:     "Approved",
	})
	msg2.Timestamp = time.Date(2026, 4, 10, 10, 5, 0, 0, time.UTC)
	if err := CommitMessage(r, msg2); err != nil {
		t.Fatalf("CommitMessage msg2: %v", err)
	}

	messages, err := ReadThread(r, "edgesync", msg1.ThreadShort())
	if err != nil {
		t.Fatalf("ReadThread: %v", err)
	}

	if len(messages) != 2 {
		t.Fatalf("message count = %d, want 2", len(messages))
	}

	// Oldest first.
	if messages[0].Body != "Deploy request" {
		t.Errorf("first message body = %q, want %q", messages[0].Body, "Deploy request")
	}
	if messages[1].Body != "Approved" {
		t.Errorf("second message body = %q, want %q", messages[1].Body, "Approved")
	}
	if messages[1].ReplyTo != msg1.ID {
		t.Errorf("second message ReplyTo = %q, want %q", messages[1].ReplyTo, msg1.ID)
	}
}

func TestDecompressBlobEdgeCases(t *testing.T) {
	t.Run("empty input", func(t *testing.T) {
		out, err := decompressBlob(nil)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if out != nil {
			t.Errorf("expected nil, got %v", out)
		}
	})

	t.Run("too short for header", func(t *testing.T) {
		out, err := decompressBlob([]byte{0x01, 0x02})
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if len(out) != 2 {
			t.Errorf("expected passthrough of 2 bytes, got %d", len(out))
		}
	})

	t.Run("valid compressed blob", func(t *testing.T) {
		payload := []byte("hello world")
		var zbuf bytes.Buffer
		w := zlib.NewWriter(&zbuf)
		w.Write(payload)
		w.Close()

		// Build Fossil blob: 4-byte BE size + zlib data.
		blob := make([]byte, 4+zbuf.Len())
		binary.BigEndian.PutUint32(blob[:4], uint32(len(payload)))
		copy(blob[4:], zbuf.Bytes())

		out, err := decompressBlob(blob)
		if err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		if string(out) != "hello world" {
			t.Errorf("got %q, want %q", out, "hello world")
		}
	})

	t.Run("corrupted zlib data", func(t *testing.T) {
		// Valid size header but garbage zlib payload.
		blob := make([]byte, 8)
		binary.BigEndian.PutUint32(blob[:4], 100)
		blob[4] = 0xFF
		blob[5] = 0xFE
		blob[6] = 0xFD
		blob[7] = 0xFC

		_, err := decompressBlob(blob)
		if err == nil {
			t.Fatal("expected error for corrupted zlib, got nil")
		}
	})

	t.Run("truncated zlib data", func(t *testing.T) {
		payload := []byte("hello world this is a longer string")
		var zbuf bytes.Buffer
		w := zlib.NewWriter(&zbuf)
		w.Write(payload)
		w.Close()

		// Build blob but truncate the zlib data.
		full := zbuf.Bytes()
		blob := make([]byte, 4+len(full)/2)
		binary.BigEndian.PutUint32(blob[:4], uint32(len(payload)))
		copy(blob[4:], full[:len(full)/2])

		_, err := decompressBlob(blob)
		if err == nil {
			t.Fatal("expected error for truncated zlib, got nil")
		}
	})
}
