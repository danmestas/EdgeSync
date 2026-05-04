// Package hub hosts an EdgeSync hub in-process: a fossil hub repo, an
// embedded NATS server with JetStream, and an HTTP server exposing the
// fossil timeline UI / xfer endpoint. All libfossil and nats-server types
// are wrapped internally — the public API exposes only stdlib types and
// types defined here, so consumers can host a hub without importing
// libfossil or nats-server.
//
// The package lives inside the EdgeSync root module rather than a separate
// go.mod so that it can ship in lockstep with the leaf agent without a
// chicken-and-egg release. It can be promoted to its own module later if
// downstream consumers need to depend on it independently.
package hub

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"path/filepath"
	"sync"
	"time"

	libfossil "github.com/danmestas/libfossil"
	natsserver "github.com/nats-io/nats-server/v2/server"
)

// Config configures NewHub.
type Config struct {
	// RepoPath is the absolute path to hub.fossil. Created if absent.
	RepoPath string

	// BootstrapUser is the libfossil user created at hub bootstrap.
	// Default: "hub".
	BootstrapUser string

	// NATSStoreDir is the JetStream storage directory. When empty, defaults
	// to filepath.Dir(RepoPath) + "/nats-store".
	NATSStoreDir string

	// FossilHTTPPort is the port to serve fossil HTTP on. 0 = auto-pick.
	FossilHTTPPort int

	// NATSClientPort is the embedded NATS server's client port.
	// 0 = auto-pick.
	NATSClientPort int

	// NATSLeafPort is the leafnode listener port for remote agents.
	// 0 = auto-pick.
	NATSLeafPort int
}

// Hub hosts an EdgeSync hub in-process. Construct one via NewHub, then call
// ServeHTTP in a goroutine. Use Stop to tear everything down.
type Hub struct {
	repo       *Repo
	server     *natsserver.Server
	httpListener net.Listener
	httpServer *http.Server

	httpAddr     string
	natsURL      string
	leafUpstream string

	stopOnce sync.Once
}

// NewHub bootstraps the hub repo if absent, applies SQLite tunings, binds
// the HTTP listener (so HTTPAddr is immediately available), and starts the
// embedded NATS server. Does not yet serve HTTP — call ServeHTTP for that.
func NewHub(ctx context.Context, cfg Config) (*Hub, error) {
	if cfg.RepoPath == "" {
		return nil, errors.New("hub: Config.RepoPath is required")
	}
	if cfg.BootstrapUser == "" {
		cfg.BootstrapUser = "hub"
	}
	if cfg.NATSStoreDir == "" {
		cfg.NATSStoreDir = filepath.Join(filepath.Dir(cfg.RepoPath), "nats-store")
	}

	libfossilRepo, err := openOrCreateRepo(cfg.RepoPath, cfg.BootstrapUser)
	if err != nil {
		return nil, err
	}
	applySQLiteTuning(libfossilRepo)
	repo := newRepoFromHandle(libfossilRepo)

	httpAddr := fmt.Sprintf("127.0.0.1:%d", cfg.FossilHTTPPort)
	httpLn, err := net.Listen("tcp", httpAddr)
	if err != nil {
		repo.Close()
		return nil, fmt.Errorf("hub: bind HTTP listener on %s: %w", httpAddr, err)
	}

	natsServer, err := startNATS(cfg)
	if err != nil {
		httpLn.Close()
		repo.Close()
		return nil, err
	}

	leafAddr := ""
	if varz, vErr := natsServer.Varz(&natsserver.VarzOptions{}); vErr == nil && varz.LeafNode.Port > 0 {
		leafAddr = fmt.Sprintf("nats-leaf://127.0.0.1:%d", varz.LeafNode.Port)
	}

	h := &Hub{
		repo:         repo,
		server:       natsServer,
		httpListener: httpLn,
		httpAddr:     httpLn.Addr().String(),
		natsURL:      natsServer.ClientURL(),
		leafUpstream: leafAddr,
	}
	return h, nil
}

// ServeHTTP runs the fossil HTTP server (timeline UI + xfer endpoint)
// against the listener bound by NewHub. Blocks until ctx is cancelled or
// Stop is called.
func (h *Hub) ServeHTTP(ctx context.Context) error {
	mux := http.NewServeMux()
	mux.Handle("/", h.repo.handle.XferHandler())

	srv := &http.Server{Handler: mux}
	h.httpServer = srv

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(shutdownCtx)
	}()

	if err := srv.Serve(h.httpListener); err != nil && err != http.ErrServerClosed {
		return fmt.Errorf("hub: serve HTTP: %w", err)
	}
	return nil
}

// Stop tears down the embedded NATS server and any HTTP listeners. Safe to
// call multiple times.
func (h *Hub) Stop() error {
	var firstErr error
	h.stopOnce.Do(func() {
		if h.httpServer != nil {
			shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			if err := h.httpServer.Shutdown(shutdownCtx); err != nil && err != http.ErrServerClosed {
				firstErr = fmt.Errorf("hub: shutdown HTTP: %w", err)
			}
		} else if h.httpListener != nil {
			h.httpListener.Close()
		}
		if h.server != nil {
			h.server.Shutdown()
			h.server.WaitForShutdown()
		}
		if h.repo != nil {
			if err := h.repo.Close(); err != nil && firstErr == nil {
				firstErr = err
			}
		}
	})
	return firstErr
}

// NATSURL returns the embedded NATS server's client URL.
func (h *Hub) NATSURL() string { return h.natsURL }

// LeafUpstream returns the leafnode listener URL, suitable for passing to
// remote agents as their NATS leaf upstream. Empty if no leaf port is
// configured.
func (h *Hub) LeafUpstream() string { return h.leafUpstream }

// HTTPAddr returns the host:port the fossil HTTP server is bound to.
func (h *Hub) HTTPAddr() string { return h.httpAddr }

// Repo returns the underlying fossil-repo handle the hub serves from. Use
// this to share the handle with another component in the same process —
// e.g. a coexisting CLI command path that needs user/commit/read access
// without re-opening the file. Don't call Close on the returned *Repo
// while the Hub is still serving; use Hub.Stop for teardown.
func (h *Hub) Repo() *Repo { return h.repo }

func openOrCreateRepo(path, bootstrapUser string) (*libfossil.Repo, error) {
	if r, err := libfossil.Open(path); err == nil {
		return r, nil
	}
	r, err := libfossil.Create(path, libfossil.CreateOpts{User: bootstrapUser})
	if err != nil {
		return nil, fmt.Errorf("hub: create repo at %s: %w", path, err)
	}
	return r, nil
}

func startNATS(cfg Config) (*natsserver.Server, error) {
	opts := &natsserver.Options{
		Host:      "127.0.0.1",
		Port:      portOrAuto(cfg.NATSClientPort),
		NoLog:     true,
		NoSigs:    true,
		JetStream: true,
		StoreDir:  cfg.NATSStoreDir,
	}
	opts.LeafNode.Host = "127.0.0.1"
	opts.LeafNode.Port = portOrAuto(cfg.NATSLeafPort)

	srv, err := natsserver.NewServer(opts)
	if err != nil {
		return nil, fmt.Errorf("hub: create NATS server: %w", err)
	}
	srv.Start()
	if !srv.ReadyForConnections(5 * time.Second) {
		srv.Shutdown()
		return nil, fmt.Errorf("hub: NATS server not ready within 5s")
	}
	return srv, nil
}

func portOrAuto(p int) int {
	if p == 0 {
		return -1 // nats-server convention for "auto-pick"
	}
	return p
}

// applySQLiteTuning sets a 30s SQLite busy_timeout so concurrent operations
// retry on SQLITE_BUSY rather than failing fast.
//
// Mirrors leaf/agent's tuning. Notably does NOT cap MaxOpenConns: hub
// repos serve concurrent clones, and capping the pool deadlocks libfossil's
// clone path (issue #120).
func applySQLiteTuning(r *libfossil.Repo) {
	_, _ = r.DB().Exec(`PRAGMA busy_timeout = 30000`)
}
