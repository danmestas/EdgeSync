package hub

import (
	"context"
	"net/url"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
)

// TestHub_NATSWebSocketURL_DisabledByDefault asserts that WebSocket is opt-in:
// a default Config leaves NATSWebSocketURL empty so consumers know the
// listener isn't running.
func TestHub_NATSWebSocketURL_DisabledByDefault(t *testing.T) {
	h := newTestHub(t)

	if got := h.NATSWebSocketURL(); got != "" {
		t.Errorf("NATSWebSocketURL() = %q, want empty when EnableWebSocket is false", got)
	}
}

// TestHub_NATSWebSocketURL_AutoPick confirms EnableWebSocket=true with
// NATSWebSocketPort=0 yields a parseable ws://127.0.0.1:NNNN URL with a
// non-zero resolved port.
func TestHub_NATSWebSocketURL_AutoPick(t *testing.T) {
	dir := t.TempDir()
	h, err := NewHub(context.Background(), Config{
		RepoPath:        filepath.Join(dir, "hub.fossil"),
		EnableWebSocket: true,
	})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	got := h.NATSWebSocketURL()
	if got == "" {
		t.Fatal("NATSWebSocketURL() empty when EnableWebSocket=true")
	}
	u, err := url.Parse(got)
	if err != nil {
		t.Fatalf("url.Parse(%q): %v", got, err)
	}
	if u.Scheme != "ws" {
		t.Errorf("scheme = %q, want %q", u.Scheme, "ws")
	}
	if !strings.HasPrefix(u.Host, "127.0.0.1:") {
		t.Errorf("host = %q, want 127.0.0.1: prefix (loopback parity with TCP/leaf)", u.Host)
	}
	if port := u.Port(); port == "" || port == "0" {
		t.Errorf("port = %q, want auto-picked non-zero port", port)
	}
}

// TestHub_NATSWebSocket_RoundTrip dials the WS listener via the standard
// nats.go client (which supports ws://) and proves end-to-end pub/sub works
// across the WS<->TCP boundary: a WS subscriber receives a message published
// by a TCP publisher on the same embedded server.
func TestHub_NATSWebSocket_RoundTrip(t *testing.T) {
	dir := t.TempDir()
	h, err := NewHub(context.Background(), Config{
		RepoPath:        filepath.Join(dir, "hub.fossil"),
		EnableWebSocket: true,
	})
	if err != nil {
		t.Fatalf("NewHub: %v", err)
	}
	t.Cleanup(func() { _ = h.Stop() })

	wsConn, err := nats.Connect(h.NATSWebSocketURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect(ws %s): %v", h.NATSWebSocketURL(), err)
	}
	defer wsConn.Close()

	tcpConn, err := nats.Connect(h.NATSURL(), nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect(tcp %s): %v", h.NATSURL(), err)
	}
	defer tcpConn.Close()

	const subject = "ws.roundtrip.test"
	const payload = "hello-over-ws"

	sub, err := wsConn.SubscribeSync(subject)
	if err != nil {
		t.Fatalf("SubscribeSync over WS: %v", err)
	}
	// Flush so the server registers the WS subscription before the TCP
	// publish races ahead.
	if err := wsConn.FlushTimeout(2 * time.Second); err != nil {
		t.Fatalf("flush WS subscription: %v", err)
	}

	if err := tcpConn.Publish(subject, []byte(payload)); err != nil {
		t.Fatalf("Publish over TCP: %v", err)
	}

	msg, err := sub.NextMsg(2 * time.Second)
	if err != nil {
		t.Fatalf("NextMsg over WS: %v", err)
	}
	if got := string(msg.Data); got != payload {
		t.Errorf("WS subscriber got %q, want %q", got, payload)
	}
}
