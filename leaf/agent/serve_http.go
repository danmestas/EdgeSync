package agent

import (
	"context"
	"fmt"
	"net"
	"net/http"

	"github.com/dmestas/edgesync/go-libfossil/sync"
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
	mux.Handle("/", sync.XferHandler(a.repo, sync.HandleSync))

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
