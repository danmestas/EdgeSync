package agent

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/danmestas/libfossil"
	_ "github.com/danmestas/libfossil/db/driver/modernc"
	"github.com/dmestas/edgesync/leaf/agent/notify"
	"github.com/nats-io/nats.go"
)

// TestNotifyOverNATS verifies that two notify Services can exchange messages
// through an embedded NATS server — the same topology as two CLI clients
// or two agents connected to the same hub.
func TestNotifyOverNATS(t *testing.T) {
	dir := t.TempDir()

	// Start a shared NATS server (simulates the hub's embedded NATS).
	natsURL := startEmbeddedNATS(t)

	// Create two notify repos.
	notifyPathA := filepath.Join(dir, "a-notify.fossil")
	notifyPathB := filepath.Join(dir, "b-notify.fossil")

	rA := initNotifyForTest(t, notifyPathA)
	defer rA.Close()
	rB := initNotifyForTest(t, notifyPathB)
	defer rB.Close()

	// Connect two NATS clients (simulates two peers on the same mesh).
	ncA, err := nats.Connect(natsURL, nats.Name("agent-a"))
	if err != nil {
		t.Fatalf("connect A: %v", err)
	}
	defer ncA.Close()

	ncB, err := nats.Connect(natsURL, nats.Name("agent-b"))
	if err != nil {
		t.Fatalf("connect B: %v", err)
	}
	defer ncB.Close()

	// Create two Services.
	svcA, err := notify.NewService(notify.ServiceConfig{
		Repo:     rA,
		NATSConn: ncA,
		From:     "endpoint-a",
		FromName: "agent-a",
	})
	if err != nil {
		t.Fatalf("new service A: %v", err)
	}
	defer svcA.Close()

	svcB, err := notify.NewService(notify.ServiceConfig{
		Repo:     rB,
		NATSConn: ncB,
		From:     "endpoint-b",
		FromName: "agent-b",
	})
	if err != nil {
		t.Fatalf("new service B: %v", err)
	}
	defer svcB.Close()

	// B watches for messages.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	ch := svcB.Watch(ctx, notify.WatchOpts{Project: "edgesync"})

	// Let subscription propagate.
	time.Sleep(100 * time.Millisecond)

	// A sends a message.
	msg, err := svcA.Send(notify.SendOpts{
		Project:  "edgesync",
		Body:     "hello from A",
		Priority: notify.PriorityUrgent,
	})
	if err != nil {
		t.Fatalf("send: %v", err)
	}
	t.Logf("A sent: %s (thread %s)", msg.ID, msg.ThreadShort())

	// B should receive it.
	select {
	case received := <-ch:
		if received.Body != "hello from A" {
			t.Errorf("body = %q, want %q", received.Body, "hello from A")
		}
		if received.Priority != notify.PriorityUrgent {
			t.Errorf("priority = %q, want %q", received.Priority, notify.PriorityUrgent)
		}
		if received.FromName != "agent-a" {
			t.Errorf("from_name = %q, want %q", received.FromName, "agent-a")
		}
		t.Logf("B received: %s", notify.FormatWatchLine(received))
	case <-ctx.Done():
		t.Fatal("timeout: B did not receive message from A")
	}

	// Verify A's repo has the message.
	readBack, err := notify.ReadMessage(svcA.Repo(), msg.FilePath())
	if err != nil {
		t.Fatalf("read from A's repo: %v", err)
	}
	if readBack.Body != "hello from A" {
		t.Errorf("repo body = %q, want %q", readBack.Body, "hello from A")
	}
}

// TestNotifyBidirectional verifies A sends, B replies, A receives the reply.
func TestNotifyBidirectional(t *testing.T) {
	dir := t.TempDir()
	natsURL := startEmbeddedNATS(t)

	rA := initNotifyForTest(t, filepath.Join(dir, "a-notify.fossil"))
	defer rA.Close()
	rB := initNotifyForTest(t, filepath.Join(dir, "b-notify.fossil"))
	defer rB.Close()

	ncA, err := nats.Connect(natsURL, nats.Name("agent-a"))
	if err != nil {
		t.Fatalf("connect A: %v", err)
	}
	defer ncA.Close()

	ncB, err := nats.Connect(natsURL, nats.Name("agent-b"))
	if err != nil {
		t.Fatalf("connect B: %v", err)
	}
	defer ncB.Close()

	svcA, err := notify.NewService(notify.ServiceConfig{
		Repo: rA, NATSConn: ncA, From: "endpoint-a", FromName: "agent-a",
	})
	if err != nil {
		t.Fatalf("new service A: %v", err)
	}
	defer svcA.Close()

	svcB, err := notify.NewService(notify.ServiceConfig{
		Repo: rB, NATSConn: ncB, From: "endpoint-b", FromName: "agent-b",
	})
	if err != nil {
		t.Fatalf("new service B: %v", err)
	}
	defer svcB.Close()

	// A sends a question.
	msg, err := svcA.Send(notify.SendOpts{
		Project:  "edgesync",
		Body:     "Deploy to prod?",
		Priority: notify.PriorityActionRequired,
		Actions:  []notify.Action{{ID: "yes", Label: "Yes"}, {ID: "no", Label: "No"}},
	})
	if err != nil {
		t.Fatalf("send: %v", err)
	}

	// A watches for replies on this thread.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	replyCh := svcA.Watch(ctx, notify.WatchOpts{
		Project:     "edgesync",
		ThreadShort: msg.ThreadShort(),
	})
	time.Sleep(100 * time.Millisecond)

	// B replies with an action.
	reply := notify.NewReply(msg, notify.ReplyOpts{
		From: "endpoint-b", FromName: "dan-iphone", Body: "yes",
	})
	reply.ActionResponse = true
	if err := notify.CommitMessage(rB, reply); err != nil {
		t.Fatalf("commit reply: %v", err)
	}
	if err := notify.Publish(ncB, reply); err != nil {
		t.Fatalf("publish reply: %v", err)
	}

	// A should receive the reply.
	select {
	case received := <-replyCh:
		if received.Body != "yes" {
			t.Errorf("reply body = %q, want %q", received.Body, "yes")
		}
		if !received.ActionResponse {
			t.Error("reply should be an action response")
		}
		if received.FromName != "dan-iphone" {
			t.Errorf("reply from = %q, want %q", received.FromName, "dan-iphone")
		}
		t.Logf("A received reply: %s", notify.FormatWatchLine(received))
	case <-ctx.Done():
		t.Fatal("timeout: A did not receive reply from B")
	}
}

// TestNotifyAgentLifecycle verifies the agent starts and stops the notify Service.
func TestNotifyAgentLifecycle(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "main.fossil")
	notifyPath := filepath.Join(dir, "notify.fossil")

	// Create main repo.
	r, err := libfossil.Create(repoPath, libfossil.CreateOpts{User: "test"})
	if err != nil {
		t.Fatalf("create repo: %v", err)
	}
	r.Close()

	// Create notify repo.
	nr, err := notify.InitNotifyRepo(notifyPath)
	if err != nil {
		t.Fatalf("init notify: %v", err)
	}
	nr.Close()

	// Start agent with notify enabled.
	a, err := New(Config{
		RepoPath:       repoPath,
		NATSRole:       NATSRoleHub,
		NotifyEnabled:  true,
		NotifyRepoPath: notifyPath,
		PeerID:         "test-agent",
	})
	if err != nil {
		t.Fatalf("new agent: %v", err)
	}

	if err := a.Start(); err != nil {
		t.Fatalf("start: %v", err)
	}

	// Verify notify service is running.
	svc := a.NotifyService()
	if svc == nil {
		t.Fatal("notify service should not be nil when enabled")
	}

	// Send a message through the agent's notify service.
	msg, err := svc.Send(notify.SendOpts{
		Project: "edgesync",
		Body:    "agent lifecycle test",
	})
	if err != nil {
		t.Fatalf("send: %v", err)
	}
	t.Logf("sent via agent: %s", msg.ID)

	// Stop should clean up.
	if err := a.Stop(); err != nil {
		t.Fatalf("stop: %v", err)
	}
}

func initNotifyForTest(t *testing.T, path string) *libfossil.Repo {
	t.Helper()
	r, err := notify.InitNotifyRepo(path)
	if err != nil {
		t.Fatalf("init notify repo %s: %v", path, err)
	}
	return r
}
