package agent

import (
	"context"
	"io"
	"net"
	"net/http"
	"path/filepath"
	"testing"

	"github.com/danmestas/go-libfossil/xfer"
)

func TestIrohTransportRoundTrip(t *testing.T) {
	// Start a mock sidecar HTTP server on a Unix socket.
	dir := t.TempDir()
	sock := filepath.Join(dir, "iroh.sock")

	listener, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	// Mock sidecar: accept any POST to /exchange/*, echo back a canned igot response.
	cannedResp := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.IGotCard{UUID: "iroh-test-uuid-1234"},
		},
	}
	cannedBytes, err := cannedResp.Encode()
	if err != nil {
		t.Fatalf("encode canned: %v", err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/exchange/", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		if _, err := xfer.Decode(body); err != nil {
			t.Errorf("mock sidecar: decode request: %v", err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusOK)
		w.Write(cannedBytes)
	})
	srv := &http.Server{Handler: mux}
	go srv.Serve(listener)
	defer srv.Close()

	transport := NewIrohTransport(sock, "fake-endpoint-id-abc123")

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
	if igot.UUID != "iroh-test-uuid-1234" {
		t.Errorf("UUID = %q, want %q", igot.UUID, "iroh-test-uuid-1234")
	}
}

func TestIrohTransportSidecarDown(t *testing.T) {
	transport := NewIrohTransport("/tmp/iroh-nonexistent.sock", "fake-id")

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srvX", ProjectCode: "testproj"},
		},
	}

	ctx := context.Background()
	_, err := transport.Exchange(ctx, req)
	if err == nil {
		t.Fatal("expected error when sidecar is down, got nil")
	}
	t.Logf("error (expected): %v", err)
}
