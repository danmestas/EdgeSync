//go:build !wasip1 && !js

package telemetry

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// TestSmokeHoneycomb runs a real Sync with OTelObserver and sends traces
// to whatever OTLP endpoint is configured. Skip unless HONEYCOMB_SMOKE=1.
func TestSmokeHoneycomb(t *testing.T) {
	if os.Getenv("HONEYCOMB_SMOKE") != "1" {
		t.Skip("set HONEYCOMB_SMOKE=1 to run end-to-end Honeycomb smoke test")
	}

	ctx := context.Background()
	shutdown, err := Setup(ctx, TelemetryConfig{
		ServiceName: "edgesync-leaf-smoke-test",
	})
	if err != nil {
		t.Fatalf("Setup: %v", err)
	}

	obs := NewOTelObserver(nil, nil)

	// Create server + client repos
	env := simio.RealEnv()
	serverPath := filepath.Join(t.TempDir(), "server.fossil")
	clientPath := filepath.Join(t.TempDir(), "client.fossil")

	server, err := repo.Create(serverPath, "test", env.Rand)
	if err != nil {
		t.Fatalf("create server: %v", err)
	}
	defer server.Close()

	client, err := repo.Create(clientPath, "test", env.Rand)
	if err != nil {
		t.Fatalf("create client: %v", err)
	}
	defer client.Close()

	// MockTransport that delegates to server-side handler
	mt := &libsync.MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			resp, _ := libsync.HandleSync(context.Background(), server, req)
			return resp
		},
	}

	// Run a real sync with the OTel observer
	result, err := libsync.Sync(ctx, client, mt, libsync.SyncOpts{
		Push:     true,
		Pull:     true,
		Observer: obs,
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}
	t.Logf("Sync completed: rounds=%d sent=%d recv=%d", result.Rounds, result.FilesSent, result.FilesRecvd)

	// Flush telemetry
	shutCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := shutdown(shutCtx); err != nil {
		t.Fatalf("shutdown: %v", err)
	}

	t.Log("Traces and metrics sent to Honeycomb. Check dataset 'edgesync-leaf-smoke-test'.")
}
