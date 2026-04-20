package notify

import (
	"context"
	"sync"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/nats-io/nats.go"
)

type testService struct {
	svc     *Service
	repo    *libfossil.Repo
	natsURL string
	conn    *nats.Conn
}

func newTestService(t *testing.T) *testService {
	t.Helper()

	// Create repo.
	path := t.TempDir() + "/notify.fossil"
	r, err := InitNotifyRepo(path)
	if err != nil {
		t.Fatalf("InitNotifyRepo: %v", err)
	}
	t.Cleanup(func() { r.Close() })

	// Start embedded NATS.
	url := startTestNATS(t)

	conn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("nats connect: %v", err)
	}
	t.Cleanup(conn.Close)

	svc, err := NewService(ServiceConfig{
		Repo:     r,
		NATSConn: conn,
		From:     "endpoint-test123",
		FromName: "test-device",
	})
	if err != nil {
		t.Fatalf("NewService: %v", err)
	}
	t.Cleanup(func() { svc.Close() })

	return &testService{
		svc:     svc,
		repo:    r,
		natsURL: url,
		conn:    conn,
	}
}

func TestNewServiceNilRepo(t *testing.T) {
	_, err := NewService(ServiceConfig{Repo: nil})
	if err == nil {
		t.Fatal("expected error for nil repo")
	}
}

func TestServiceSend(t *testing.T) {
	ts := newTestService(t)

	// Set up a NATS subscriber to verify delivery.
	subConn, err := nats.Connect(ts.natsURL)
	if err != nil {
		t.Fatalf("nats connect sub: %v", err)
	}
	defer subConn.Close()

	var received Message
	var wg sync.WaitGroup
	wg.Add(1)

	subscriber := NewSubscriber(subConn)
	defer subscriber.Unsubscribe()

	if err := subscriber.Subscribe("edgesync", func(m Message) {
		received = m
		wg.Done()
	}); err != nil {
		t.Fatalf("subscribe: %v", err)
	}

	msg, err := ts.svc.Send(SendOpts{
		Project:  "edgesync",
		Body:     "Build failed",
		Priority: PriorityActionRequired,
	})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	if msg.ID == "" {
		t.Error("sent message should have an ID")
	}
	if msg.From != "endpoint-test123" {
		t.Errorf("From = %q, want %q", msg.From, "endpoint-test123")
	}

	// Wait for NATS delivery.
	done := make(chan struct{})
	go func() {
		wg.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("timeout waiting for NATS message")
	}

	if received.ID != msg.ID {
		t.Errorf("NATS received ID = %q, want %q", received.ID, msg.ID)
	}

	// Verify repo commit.
	got, err := ReadMessage(ts.repo, msg.FilePath())
	if err != nil {
		t.Fatalf("ReadMessage from repo: %v", err)
	}
	if got.Body != "Build failed" {
		t.Errorf("repo Body = %q, want %q", got.Body, "Build failed")
	}
}

func TestServiceSendToExistingThread(t *testing.T) {
	ts := newTestService(t)

	// Send first message (creates thread).
	msg1, err := ts.svc.Send(SendOpts{
		Project: "edgesync",
		Body:    "First message",
	})
	if err != nil {
		t.Fatalf("Send msg1: %v", err)
	}

	// Send second message to same thread.
	msg2, err := ts.svc.Send(SendOpts{
		Project:     "edgesync",
		Body:        "Second message",
		ThreadShort: msg1.ThreadShort(),
	})
	if err != nil {
		t.Fatalf("Send msg2: %v", err)
	}

	if msg2.Thread != msg1.Thread {
		t.Errorf("msg2 Thread = %q, want %q (same thread)", msg2.Thread, msg1.Thread)
	}
	if msg2.ID == msg1.ID {
		t.Error("msg2 should have a different ID than msg1")
	}

	// Verify both in repo.
	messages, err := ReadThread(ts.repo, "edgesync", msg1.ThreadShort())
	if err != nil {
		t.Fatalf("ReadThread: %v", err)
	}
	if len(messages) != 2 {
		t.Fatalf("thread message count = %d, want 2", len(messages))
	}
}

func TestServiceWatch(t *testing.T) {
	ts := newTestService(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	ch := ts.svc.Watch(ctx, WatchOpts{Project: "edgesync"})

	// Send a message via the service (will publish to NATS).
	sent, err := ts.svc.Send(SendOpts{
		Project: "edgesync",
		Body:    "Watch this",
	})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	// Read from watch channel.
	select {
	case got := <-ch:
		if got.ID != sent.ID {
			t.Errorf("watched ID = %q, want %q", got.ID, sent.ID)
		}
		if got.Body != "Watch this" {
			t.Errorf("watched Body = %q, want %q", got.Body, "Watch this")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timeout waiting for watched message")
	}

	// Simulate external reply via NATS.
	extConn, err := nats.Connect(ts.natsURL)
	if err != nil {
		t.Fatalf("nats connect external: %v", err)
	}
	defer extConn.Close()

	reply := NewReply(sent, ReplyOpts{
		From:     "endpoint-external",
		FromName: "external-device",
		Body:     "Got it",
	})
	if err := Publish(extConn, reply); err != nil {
		t.Fatalf("publish external reply: %v", err)
	}

	select {
	case got := <-ch:
		if got.ID != reply.ID {
			t.Errorf("watched reply ID = %q, want %q", got.ID, reply.ID)
		}
		if got.Body != "Got it" {
			t.Errorf("watched reply Body = %q, want %q", got.Body, "Got it")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timeout waiting for external reply")
	}
}

func TestFormatWatchLine(t *testing.T) {
	tests := []struct {
		name string
		msg  Message
		want string
	}{
		{
			name: "text message",
			msg: Message{
				Thread:    "thread-a1b2c3d4e5f6a7b8",
				FromName:  "claude-macbook",
				Body:      "Build started",
				Timestamp: time.Date(2026, 4, 10, 12, 0, 0, 0, time.UTC),
			},
			want: "[2026-04-10T12:00:00Z] thread:a1b2c3d4 from:claude-macbook text:Build started",
		},
		{
			name: "action response",
			msg: Message{
				Thread:         "thread-a1b2c3d4e5f6a7b8",
				FromName:       "dan-iphone",
				Body:           "retry",
				Timestamp:      time.Date(2026, 4, 10, 12, 5, 0, 0, time.UTC),
				ActionResponse: true,
			},
			want: "[2026-04-10T12:05:00Z] thread:a1b2c3d4 from:dan-iphone action:retry",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := FormatWatchLine(tt.msg)
			if got != tt.want {
				t.Errorf("FormatWatchLine() = %q, want %q", got, tt.want)
			}
		})
	}
}
