package sync

import (
	"context"
	"fmt"
	"io"
	"log"
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
		// Fossil sends GET as a server probe — respond with a basic page.
		if req.Method != http.MethodPost {
			w.Header().Set("Content-Type", "text/html")
			fmt.Fprint(w, "<html><body><h1>EdgeSync Fossil Server</h1></body></html>")
			return
		}

		body, err := io.ReadAll(req.Body)
		if err != nil {
			http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
			return
		}

		if len(body) == 0 {
			// Empty POST — respond with empty xfer message.
			empty := &xfer.Message{}
			respBytes, _ := empty.Encode()
			w.Header().Set("Content-Type", "application/x-fossil")
			w.Write(respBytes)
			return
		}

		msg, err := xfer.Decode(body)
		if err != nil {
			log.Printf("serve-http: decode failed (%d bytes, first 4: %x): %v", len(body), body[:min(4, len(body))], err)
			http.Error(w, fmt.Sprintf("decode xfer (%d bytes): %v", len(body), err), http.StatusBadRequest)
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
