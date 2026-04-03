package agent

import (
	"context"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	natsserver "github.com/nats-io/nats-server/v2/server"
)

// startEmbeddedNATS boots an in-process NATS server on a random port
// and returns the client URL. The server is shut down when the test ends.
func startEmbeddedNATS(t *testing.T) string {
	t.Helper()
	opts := &natsserver.Options{Port: -1}
	ns, err := natsserver.NewServer(opts)
	if err != nil {
		t.Fatalf("nats-server new: %v", err)
	}
	ns.Start()
	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats-server not ready within 5s")
	}
	t.Cleanup(func() { ns.Shutdown() })
	return ns.ClientURL()
}

func TestNATSTransportRoundTrip(t *testing.T) {
	url := startEmbeddedNATS(t)

	// Subscriber connection: listens and echoes back a canned response.
	subConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("subscriber connect: %v", err)
	}
	defer subConn.Close()

	subject := "fossil.testproj.sync"

	// Build a canned response (just some bytes).
	cannedResp := []byte("canned-response-data")

	sub, err := subConn.Subscribe(subject, func(msg *nats.Msg) {
		if len(msg.Data) == 0 {
			t.Error("subscriber received empty request")
			return
		}
		if err := msg.Respond(cannedResp); err != nil {
			t.Errorf("subscriber respond: %v", err)
		}
	})
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	defer sub.Unsubscribe()
	subConn.Flush()

	// Client connection: uses NATSTransport.
	clientConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("client connect: %v", err)
	}
	defer clientConn.Close()

	transport := NewNATSTransport(clientConn, "testproj", 5*time.Second, "")

	ctx := context.Background()
	resp, err := transport.RoundTrip(ctx, []byte("test-request"))
	if err != nil {
		t.Fatalf("RoundTrip: %v", err)
	}
	if string(resp) != "canned-response-data" {
		t.Errorf("response = %q, want %q", resp, "canned-response-data")
	}
}

func TestNATSTransportNoSubscriberTimeout(t *testing.T) {
	url := startEmbeddedNATS(t)

	conn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer conn.Close()

	transport := NewNATSTransport(conn, "nobody", 500*time.Millisecond, "")

	ctx := context.Background()
	start := time.Now()
	_, err = transport.RoundTrip(ctx, []byte("test-request"))
	elapsed := time.Since(start)

	if err == nil {
		t.Fatal("expected timeout error, got nil")
	}
	// Should fail within roughly the timeout window (allow some slack).
	if elapsed > 3*time.Second {
		t.Errorf("took %v, expected ~500ms timeout", elapsed)
	}
	t.Logf("timeout error (expected): %v [elapsed: %v]", err, elapsed)
}
