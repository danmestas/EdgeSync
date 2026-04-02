package agent

import (
	"context"
	"testing"
	"time"

	"github.com/danmestas/go-libfossil/xfer"
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

	// Build a canned response: a message with an igot card.
	cannedResp := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.IGotCard{UUID: "abc123def456"},
		},
	}
	cannedBytes, err := cannedResp.Encode()
	if err != nil {
		t.Fatalf("encode canned response: %v", err)
	}

	sub, err := subConn.Subscribe(subject, func(msg *nats.Msg) {
		// Verify we received valid zlib-compressed xfer data.
		_, decErr := xfer.Decode(msg.Data)
		if decErr != nil {
			t.Errorf("subscriber decode request: %v", decErr)
			return
		}
		if err := msg.Respond(cannedBytes); err != nil {
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

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srvX", ProjectCode: "testproj"},
		},
	}

	ctx := context.Background()
	resp, err := transport.Exchange(ctx, req)
	if err != nil {
		t.Fatalf("Exchange: %v", err)
	}
	if len(resp.Cards) != 1 {
		t.Fatalf("expected 1 card, got %d", len(resp.Cards))
	}
	igot, ok := resp.Cards[0].(*xfer.IGotCard)
	if !ok {
		t.Fatalf("expected *IGotCard, got %T", resp.Cards[0])
	}
	if igot.UUID != "abc123def456" {
		t.Errorf("UUID = %q, want %q", igot.UUID, "abc123def456")
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

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srvX", ProjectCode: "nobody"},
		},
	}

	ctx := context.Background()
	start := time.Now()
	_, err = transport.Exchange(ctx, req)
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
