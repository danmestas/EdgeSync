package agent

import (
	"context"
	"io"
	"net"
	"net/http"
	"path/filepath"
	"testing"
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

	// Mock sidecar: accept any POST to /exchange/*, echo back a canned response.
	cannedResp := []byte("mock-xfer-response-payload")

	mux := http.NewServeMux()
	mux.HandleFunc("/exchange/", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		if len(body) == 0 {
			t.Errorf("mock sidecar: empty request body")
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusOK)
		w.Write(cannedResp)
	})
	srv := &http.Server{Handler: mux}
	go srv.Serve(listener)
	defer srv.Close()

	transport := NewIrohTransport(sock, "fake-endpoint-id-abc123")

	reqPayload := []byte("mock-xfer-request-payload")

	ctx := context.Background()
	resp, err := transport.RoundTrip(ctx, reqPayload)
	if err != nil {
		t.Fatalf("RoundTrip: %v", err)
	}
	if string(resp) != string(cannedResp) {
		t.Errorf("response = %q, want %q", resp, cannedResp)
	}
}

func TestIrohTransportSidecarDown(t *testing.T) {
	transport := NewIrohTransport("/tmp/iroh-nonexistent.sock", "fake-id")

	reqPayload := []byte("mock-xfer-request-payload")

	ctx := context.Background()
	_, err := transport.RoundTrip(ctx, reqPayload)
	if err == nil {
		t.Fatal("expected error when sidecar is down, got nil")
	}
	t.Logf("error (expected): %v", err)
}
