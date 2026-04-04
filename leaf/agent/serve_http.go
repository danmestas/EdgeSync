package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"

	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
)

// serveHTTP starts an HTTP server with the xfer handler and operational
// endpoints (/healthz). Blocks until ctx is cancelled.
func (a *Agent) serveHTTP(ctx context.Context) error {
	if a.config.ServeHTTPAddr == "" {
		panic("agent.serveHTTP: ServeHTTPAddr must not be empty")
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", healthzHandler)
	if a.config.IrohEnabled {
		mux.HandleFunc("/iroh/status", a.irohStatusHandler)
	}
	mux.Handle("/", a.repo.XferHandler())

	handler := otelhttp.NewMiddleware("edgesync-leaf-http")(mux)
	srv := &http.Server{Handler: handler}

	ln, err := net.Listen("tcp", a.config.ServeHTTPAddr)
	if err != nil {
		return fmt.Errorf("agent: serve-http: listen: %w", err)
	}

	go func() {
		<-ctx.Done()
		srv.Close()
	}()

	err = srv.Serve(ln)
	if err == http.ErrServerClosed {
		return nil
	}
	return err
}

// healthzHandler returns 200 OK for container health checks.
func healthzHandler(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"ok"}`)
}

// irohStatusHandler returns the iroh sidecar status.
func (a *Agent) irohStatusHandler(w http.ResponseWriter, _ *http.Request) {
	alive := a.irohSidecar != nil && !a.irohSidecarExited()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"enabled":     true,
		"endpoint_id": a.irohEndpointID,
		"peers":       a.config.IrohPeers,
		"sidecar":     map[string]any{"alive": alive},
	})
}

// irohSidecarExited returns true if the sidecar process has exited.
func (a *Agent) irohSidecarExited() bool {
	if a.irohSidecar == nil || a.irohSidecar.exited == nil {
		return true
	}
	select {
	case <-a.irohSidecar.exited:
		return true
	default:
		return false
	}
}
