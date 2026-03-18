package sync

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// ServeHTTP starts an HTTP server that accepts Fossil xfer requests.
// Blocks until ctx is cancelled. Stock fossil clone/sync can connect.
func ServeHTTP(ctx context.Context, addr string, r *repo.Repo, h HandleFunc) error {
	if r == nil {
		panic("sync.ServeHTTP: r must not be nil")
	}
	if h == nil {
		panic("sync.ServeHTTP: h must not be nil")
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", xferHandler(r, h))

	srv := &http.Server{
		Handler: mux,
	}

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("sync.ServeHTTP: listen: %w", err)
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

// xferHandler returns an http.HandlerFunc that decodes xfer requests,
// dispatches to the HandleFunc, and encodes the response.
func xferHandler(r *repo.Repo, h HandleFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, req *http.Request) {
		if req.Method != http.MethodPost {
			http.Error(w, "POST required", http.StatusMethodNotAllowed)
			return
		}

		body, err := io.ReadAll(req.Body)
		if err != nil {
			http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
			return
		}

		msg, err := xfer.Decode(body)
		if err != nil {
			http.Error(w, "decode xfer: "+err.Error(), http.StatusBadRequest)
			return
		}

		resp, err := h(req.Context(), r, msg)
		if err != nil {
			http.Error(w, "handler: "+err.Error(), http.StatusInternalServerError)
			return
		}

		respBytes, err := resp.Encode()
		if err != nil {
			http.Error(w, "encode response: "+err.Error(), http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/x-fossil")
		w.Write(respBytes)
	}
}
