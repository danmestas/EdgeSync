package notify

import (
	"context"
	"slices"
	"testing"
	"time"
)

// newServiceForTests builds a Service backed by a fresh notify repo and
// no NATS connection. Sufficient for exercising the libfossil-hidden
// Service methods that delegate to the existing free functions.
func newServiceForTests(t *testing.T) *Service {
	t.Helper()
	r := createTestRepo(t)
	svc, err := NewService(ServiceConfig{Repo: r, From: "endpoint-test", FromName: "test"})
	if err != nil {
		t.Fatalf("NewService: %v", err)
	}
	t.Cleanup(func() { _ = svc.Close() })
	return svc
}

func TestService_ListThreads_Roundtrip(t *testing.T) {
	svc := newServiceForTests(t)

	msg := NewMessage(MessageOpts{
		Project: "p1",
		From:    "endpoint-test",
		Body:    "hi",
	})
	msg.Timestamp = time.Date(2026, 5, 4, 10, 0, 0, 0, time.UTC)
	if err := CommitMessage(svc.repo, msg); err != nil {
		t.Fatalf("CommitMessage: %v", err)
	}

	threads, err := svc.ListThreads(context.Background(), "p1")
	if err != nil {
		t.Fatalf("ListThreads: %v", err)
	}
	if len(threads) != 1 || threads[0].ThreadShort != msg.ThreadShort() {
		t.Errorf("ListThreads = %+v, want one thread with short=%q", threads, msg.ThreadShort())
	}
}

func TestService_ReadThread_Roundtrip(t *testing.T) {
	svc := newServiceForTests(t)

	msg := NewMessage(MessageOpts{Project: "p1", From: "endpoint-test", Body: "first"})
	msg.Timestamp = time.Date(2026, 5, 4, 10, 0, 0, 0, time.UTC)
	if err := CommitMessage(svc.repo, msg); err != nil {
		t.Fatalf("CommitMessage: %v", err)
	}

	got, err := svc.ReadThread(context.Background(), "p1", msg.ThreadShort())
	if err != nil {
		t.Fatalf("ReadThread: %v", err)
	}
	if len(got) != 1 || got[0].Body != "first" {
		t.Errorf("ReadThread = %+v, want one msg with Body=first", got)
	}
}

func TestService_MessageCount(t *testing.T) {
	svc := newServiceForTests(t)

	m1 := NewMessage(MessageOpts{Project: "p1", From: "endpoint-test", Body: "a"})
	m1.Timestamp = time.Date(2026, 5, 4, 10, 0, 0, 0, time.UTC)
	if err := CommitMessage(svc.repo, m1); err != nil {
		t.Fatalf("CommitMessage m1: %v", err)
	}
	m2 := NewReply(m1, ReplyOpts{From: "endpoint-other", Body: "b"})
	m2.Timestamp = m1.Timestamp.Add(time.Minute)
	if err := CommitMessage(svc.repo, m2); err != nil {
		t.Fatalf("CommitMessage m2: %v", err)
	}

	count, err := svc.MessageCount(context.Background(), "p1", m1.ThreadShort())
	if err != nil {
		t.Fatalf("MessageCount: %v", err)
	}
	if count != 2 {
		t.Errorf("MessageCount = %d, want 2", count)
	}
}

func TestService_DeviceMgmt_RoundTrip(t *testing.T) {
	svc := newServiceForTests(t)
	ctx := context.Background()

	dev := Device{Name: "phone", EndpointID: "endpoint-phone"}
	if err := svc.AddDevice(ctx, dev); err != nil {
		t.Fatalf("AddDevice: %v", err)
	}

	devs, err := svc.ListDevices(ctx)
	if err != nil {
		t.Fatalf("ListDevices: %v", err)
	}
	if !slices.ContainsFunc(devs, func(d Device) bool { return d.Name == "phone" }) {
		t.Errorf("ListDevices = %+v, want one named phone", devs)
	}

	if err := svc.RemoveDevice(ctx, "phone"); err != nil {
		t.Fatalf("RemoveDevice: %v", err)
	}
	devs, err = svc.ListDevices(ctx)
	if err != nil {
		t.Fatalf("ListDevices after remove: %v", err)
	}
	if slices.ContainsFunc(devs, func(d Device) bool { return d.Name == "phone" }) {
		t.Errorf("ListDevices still contains phone after RemoveDevice: %+v", devs)
	}
}

func TestService_PairingToken_CreateAndValidate(t *testing.T) {
	svc := newServiceForTests(t)
	ctx := context.Background()

	tok, err := svc.CreatePairingToken(ctx, "alice")
	if err != nil {
		t.Fatalf("CreatePairingToken: %v", err)
	}
	if tok == "" {
		t.Fatal("CreatePairingToken returned empty token")
	}

	pt, err := svc.ValidateToken(ctx, tok)
	if err != nil {
		t.Fatalf("ValidateToken: %v", err)
	}
	if pt.DeviceName != "alice" {
		t.Errorf("ValidateToken returned Name=%q, want alice", pt.DeviceName)
	}
}

func TestService_ValidateToken_RejectsUnknown(t *testing.T) {
	svc := newServiceForTests(t)
	if _, err := svc.ValidateToken(context.Background(), "totally-bogus"); err == nil {
		t.Fatal("ValidateToken with unknown token should error")
	}
}
