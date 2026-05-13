package hub

import (
	"context"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
)

// TestNATSTransport_RoundTrip exercises the request/reply path end-to-end
// against an embedded NATS server: a fake responder echoes the payload
// with a known prefix; the transport hands the response back unchanged.
func TestNATSTransport_RoundTrip(t *testing.T) {
	h := newTestHub(t)

	nc, err := nats.Connect(h.NATSURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	defer nc.Close()

	const subject = "fossil.test.sync"
	sub, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		_ = msg.Respond(append([]byte("REPLY:"), msg.Data...))
	})
	if err != nil {
		t.Fatalf("Subscribe: %v", err)
	}
	defer sub.Unsubscribe()
	if err := nc.FlushTimeout(time.Second); err != nil {
		t.Fatalf("Flush: %v", err)
	}

	tr := newNATSTransport(nc, subject, 5*time.Second)
	got, err := tr.RoundTrip(context.Background(), []byte("hello"))
	if err != nil {
		t.Fatalf("RoundTrip: %v", err)
	}
	if string(got) != "REPLY:hello" {
		t.Errorf("RoundTrip response = %q, want %q", string(got), "REPLY:hello")
	}
}

// TestNATSTransport_EmptyReplyIsError asserts the contract that an
// empty reply body (the hub's HandleSync failure signal) surfaces as
// an error rather than being silently treated as a valid no-op.
func TestNATSTransport_EmptyReplyIsError(t *testing.T) {
	h := newTestHub(t)

	nc, err := nats.Connect(h.NATSURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	defer nc.Close()

	const subject = "fossil.test.empty"
	sub, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		_ = msg.Respond([]byte{})
	})
	if err != nil {
		t.Fatalf("Subscribe: %v", err)
	}
	defer sub.Unsubscribe()
	if err := nc.FlushTimeout(time.Second); err != nil {
		t.Fatalf("Flush: %v", err)
	}

	tr := newNATSTransport(nc, subject, 2*time.Second)
	if _, err := tr.RoundTrip(context.Background(), []byte("ping")); err == nil {
		t.Fatal("RoundTrip on empty reply: expected error, got nil")
	}
}

// TestNATSTransport_TimeoutHonored asserts that a non-responsive subject
// causes RoundTrip to error within the transport's configured timeout
// rather than blocking indefinitely.
func TestNATSTransport_TimeoutHonored(t *testing.T) {
	h := newTestHub(t)

	nc, err := nats.Connect(h.NATSURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	defer nc.Close()

	tr := newNATSTransport(nc, "fossil.test.silent", 200*time.Millisecond)
	start := time.Now()
	_, err = tr.RoundTrip(context.Background(), []byte("ping"))
	elapsed := time.Since(start)
	if err == nil {
		t.Fatal("RoundTrip against silent subject: expected error, got nil")
	}
	if elapsed > time.Second {
		t.Errorf("RoundTrip took %v with 200ms timeout — timeout not honored", elapsed)
	}
}
