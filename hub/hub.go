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
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sync"
	"time"

	libfossil "github.com/danmestas/libfossil"
	natsserver "github.com/nats-io/nats-server/v2/server"
	"github.com/nats-io/nats.go"
)

// Config configures NewHub.
type Config struct {
	// RepoPath is the absolute path to hub.fossil. Created if absent.
	RepoPath string

	// ServerName is the NATS server identity. Surfaces in leafnode
	// handshakes, varz, and gossip. Empty leaves nats-server's
	// auto-generated random name.
	ServerName string

	// BootstrapUser is the libfossil user created at hub bootstrap.
	// Default: "hub".
	BootstrapUser string

	// NATSStoreDir is the JetStream storage directory. When empty, defaults
	// to filepath.Dir(RepoPath) + "/messaging" — implementation-agnostic on
	// disk; the contents underneath are still NATS JetStream format.
	NATSStoreDir string

	// FossilHTTPPort is the port to serve fossil HTTP on. 0 = auto-pick.
	FossilHTTPPort int

	// NATSClientPort is the embedded NATS server's client port.
	// 0 = auto-pick.
	NATSClientPort int

	// NATSLeafPort is the leafnode listener port for remote agents.
	// 0 = auto-pick.
	NATSLeafPort int

	// LeafUpstream is an optional upstream leafnode URL to solicit a
	// connection toward (e.g. "nats-leaf://hub.example:7422"). When set,
	// this hub acts as a leaf of that upstream — subjects published locally
	// are forwarded upstream and vice versa. Empty = no upstream solicit.
	LeafUpstream string

	// NobodyCaps grants the libfossil-pre-populated 'nobody' user these
	// caps after hub bootstrap. Empty leaves nobody at libfossil's defaults
	// (no caps; unauthenticated requests are rejected). Set to "gio" to
	// allow unauthenticated clone/pull/push, the typical hub deployment.
	NobodyCaps string

	// DisableFossilSyncOverNATS turns off the hub's NATS subscriber for the
	// fossil sync subject. By default, NewHub connects a local NATS client
	// to the embedded server and subscribes to "fossil.<project-code>.sync"
	// so peer leaf agents can push xfer requests to the hub over NATS
	// (alongside the HTTP path). Set to true for read-only HTTP-only
	// mirrors that don't accept NATS pushes.
	//
	// Inverted-name (Disable*) shape over the more obvious ServeFossilSyncOverNATS
	// because Go bool zero-value is false; the production default is "enabled,"
	// which maps cleanly to "DisableX defaults false."
	DisableFossilSyncOverNATS bool

	// FossilSyncSubjectPrefix overrides the NATS subject prefix used by the
	// fossil sync subscriber. Empty defaults to "fossil" (matching the
	// leaf-agent default). The full subscribed subjects are
	// "<prefix>.<project-code>.sync" (xfer request/reply) and
	// "<prefix>.<project-code>.commit" (notification on new commits).
	FossilSyncSubjectPrefix string

	// ProjectCode optionally pins the repo's project-code on first create
	// (passed to libfossil.CreateOpts). When non-empty, must be 40-char
	// lowercase hex (^[0-9a-f]{40}$).
	//
	// When the repo at RepoPath already exists, this value (if non-empty)
	// must match the on-disk project-code or NewHub returns an error —
	// guards against silent topology drift when a caller updates their
	// config but an old repo lingers.
	//
	// Empty preserves current behavior: generate on create, accept whatever's
	// on disk on open. Set this in topologies where multiple hubs/leaves
	// must share a project-code so commits propagate over the same NATS
	// sync subject.
	ProjectCode string

	// SeedFromUpstream, when set and RepoPath doesn't yet exist on disk,
	// clones from this URL before opening. The cloned repo carries the
	// upstream's project-code, so children declaring a SeedFromUpstream
	// don't typically also need to set ProjectCode (though setting both
	// and asserting they match is allowed and cross-checked).
	//
	// URL is an EdgeSync hub HTTP xfer endpoint (e.g. "http://hub.local:8080/").
	// NATS-native clone is deferred until libfossil HandleSync supports the
	// clone protocol upstream.
	SeedFromUpstream string

	// CheckpointInterval is how often the hub runs a PASSIVE WAL checkpoint
	// against hub.fossil while serving. PASSIVE never blocks readers or
	// writers, so the file stays readable by vanilla fossil tooling
	// (fossil ui / fossil info / fossil timeline) without coordinating
	// with the hub. Zero defaults to defaultCheckpointInterval.
	//
	// No "disable" knob: vanilla-fossil readability is the contract this
	// Config exists to uphold. Set a long interval if you really want to
	// dilute the cadence; it should never be off.
	CheckpointInterval time.Duration
}

// defaultCheckpointInterval is the period between background PASSIVE
// checkpoints when Config.CheckpointInterval is zero.
const defaultCheckpointInterval = 10 * time.Second

// Hub hosts an EdgeSync hub in-process. Construct one via NewHub, then call
// ServeHTTP in a goroutine. Use Stop to tear everything down.
//
// While serving, the hub runs a PASSIVE WAL checkpoint on hub.fossil every
// Config.CheckpointInterval (default 10s) so vanilla fossil tooling can read
// the file. After Stop returns nil, the on-disk hub.fossil is self-contained
// — libfossil's Close runs a TRUNCATE checkpoint, leaving no WAL/SHM
// sidecar.
type Hub struct {
	repo         *Repo
	server       *natsserver.Server
	httpListener net.Listener
	httpServer   *http.Server

	natsClient    *nats.Conn
	natsSyncSub   *nats.Subscription
	natsCommitSub *nats.Subscription
	syncSubject   string
	commitSubject string

	httpAddr string
	natsURL  string
	leafURL  string

	checkpointStop chan struct{}
	checkpointDone chan struct{}

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
		cfg.NATSStoreDir = filepath.Join(filepath.Dir(cfg.RepoPath), "messaging")
	}

	if cfg.SeedFromUpstream != "" {
		if _, statErr := os.Stat(cfg.RepoPath); errors.Is(statErr, os.ErrNotExist) {
			if err := seedFromUpstream(ctx, cfg.RepoPath, cfg.SeedFromUpstream, cfg.BootstrapUser, cfg.ProjectCode); err != nil {
				return nil, err
			}
		}
	}

	libfossilRepo, err := openOrCreateRepo(cfg.RepoPath, cfg.BootstrapUser, cfg.ProjectCode)
	if err != nil {
		return nil, err
	}
	applySQLiteTuning(libfossilRepo)
	repo := newRepoFromHandle(libfossilRepo)

	if cfg.NobodyCaps != "" {
		if err := repo.SetUserCaps("nobody", cfg.NobodyCaps); err != nil {
			repo.Close()
			return nil, fmt.Errorf("hub: apply NobodyCaps: %w", err)
		}
	}

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
		leafURL:      leafAddr,
	}

	if !cfg.DisableFossilSyncOverNATS {
		if err := h.startFossilSyncSubscriber(cfg); err != nil {
			natsServer.Shutdown()
			natsServer.WaitForShutdown()
			httpLn.Close()
			repo.Close()
			return nil, err
		}
		if err := h.startCommitSubscriber(cfg); err != nil {
			if h.natsSyncSub != nil {
				_ = h.natsSyncSub.Unsubscribe()
			}
			if h.natsClient != nil {
				h.natsClient.Close()
			}
			natsServer.Shutdown()
			natsServer.WaitForShutdown()
			httpLn.Close()
			repo.Close()
			return nil, err
		}
	}

	h.startCheckpointLoop(cfg.CheckpointInterval)

	return h, nil
}

// startCheckpointLoop runs PASSIVE WAL checkpoints against the hub repo on a
// ticker so vanilla fossil tooling can read hub.fossil while the hub is
// serving. PASSIVE never blocks readers or writers; checkpoint errors are
// non-fatal (the next tick retries, and Hub.Stop's repo.Close runs a
// TRUNCATE checkpoint anyway).
func (h *Hub) startCheckpointLoop(interval time.Duration) {
	if interval <= 0 {
		interval = defaultCheckpointInterval
	}
	h.checkpointStop = make(chan struct{})
	h.checkpointDone = make(chan struct{})
	go func() {
		defer close(h.checkpointDone)
		t := time.NewTicker(interval)
		defer t.Stop()
		for {
			select {
			case <-h.checkpointStop:
				return
			case <-t.C:
				_ = h.repo.handle.Checkpoint(libfossil.CheckpointPassive)
			}
		}
	}()
}

// startFossilSyncSubscriber connects a local NATS client to the embedded
// server and subscribes to "<prefix>.<project-code>.sync", dispatching
// incoming xfer payloads to the hub repo's HandleSync. Invoked by NewHub
// when Config.DisableFossilSyncOverNATS is false.
func (h *Hub) startFossilSyncSubscriber(cfg Config) error {
	projectCode, err := h.repo.handle.Config("project-code")
	if err != nil {
		return fmt.Errorf("hub: read project-code for fossil-sync subscriber: %w", err)
	}
	if projectCode == "" {
		return fmt.Errorf("hub: project-code is empty; refusing to start fossil-sync subscriber")
	}

	prefix := cfg.FossilSyncSubjectPrefix
	if prefix == "" {
		prefix = "fossil"
	}
	subject := prefix + "." + projectCode + ".sync"

	nc, err := nats.Connect(h.natsURL,
		nats.Name("edgesync-hub"),
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2*time.Second),
		nats.RetryOnFailedConnect(true),
		nats.Timeout(5*time.Second),
		nats.NoEcho(), // we publish on .commit + .sync ourselves; don't self-loop
	)
	if err != nil {
		return fmt.Errorf("hub: connect local NATS for fossil-sync subscriber: %w", err)
	}

	sub, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		respBytes, hsErr := h.repo.handle.HandleSync(context.Background(), msg.Data)
		if hsErr != nil {
			// Empty response signals failure to the caller without hanging.
			_ = msg.Respond([]byte{})
			return
		}
		_ = msg.Respond(respBytes)
	})
	if err != nil {
		nc.Close()
		return fmt.Errorf("hub: subscribe %s: %w", subject, err)
	}

	// Flush so the server has registered the subscription before NewHub
	// returns. Tests that publish immediately after NewHub depend on this.
	if err := nc.FlushTimeout(2 * time.Second); err != nil {
		_ = sub.Unsubscribe()
		nc.Close()
		return fmt.Errorf("hub: flush fossil-sync subscription: %w", err)
	}

	h.natsClient = nc
	h.natsSyncSub = sub
	h.syncSubject = subject
	return nil
}

// commitNotification is the JSON payload published on the .commit subject
// when a new commit lands. Peers decode it and pull the manifest via the
// .sync subject.
type commitNotification struct {
	RID  int64  `json:"rid"`
	UUID string `json:"uuid"`
}

// startCommitSubscriber subscribes to "<prefix>.<project-code>.commit" and,
// on receipt, triggers an xfer pull from peers on the sibling .sync
// subject. Also wires the hub repo's publish hook so successful commits
// publish a notification on the .commit subject.
//
// Requires startFossilSyncSubscriber to have run first (uses h.natsClient
// and shares the project-code / prefix derivation).
func (h *Hub) startCommitSubscriber(cfg Config) error {
	projectCode, err := h.repo.handle.Config("project-code")
	if err != nil {
		return fmt.Errorf("hub: read project-code for commit subscriber: %w", err)
	}
	if projectCode == "" {
		return fmt.Errorf("hub: project-code is empty; refusing to start commit subscriber")
	}
	prefix := cfg.FossilSyncSubjectPrefix
	if prefix == "" {
		prefix = "fossil"
	}
	commitSubject := prefix + "." + projectCode + ".commit"
	syncSubject := prefix + "." + projectCode + ".sync"

	pullTransport := newNATSTransport(h.natsClient, syncSubject, 0)
	sub, err := h.natsClient.Subscribe(commitSubject, func(msg *nats.Msg) {
		var n commitNotification
		if jsonErr := json.Unmarshal(msg.Data, &n); jsonErr != nil {
			log.Printf("hub commit subscriber: decode notification: %v", jsonErr)
			return
		}
		// Pull is naturally idempotent — if we already have the uuid,
		// the sync round converges with zero rounds / zero blobs. So
		// we skip the up-front existence check and let the protocol
		// handle dedup.
		pullCtx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()
		if _, syncErr := h.repo.handle.Sync(pullCtx, pullTransport, libfossil.SyncOpts{
			Pull:        true,
			ProjectCode: projectCode,
			PeerID:      "edgesync-hub",
		}); syncErr != nil {
			log.Printf("hub commit subscriber: pull triggered by notification rid=%d uuid=%s: %v", n.RID, n.UUID, syncErr)
		}
	})
	if err != nil {
		return fmt.Errorf("hub: subscribe %s: %w", commitSubject, err)
	}
	if flushErr := h.natsClient.FlushTimeout(2 * time.Second); flushErr != nil {
		_ = sub.Unsubscribe()
		return fmt.Errorf("hub: flush commit subscription: %w", flushErr)
	}

	// Wire publish hook on the repo so future commits notify peers.
	nc := h.natsClient
	h.repo.publish = func(rid int64, uuid string) {
		payload, jsonErr := json.Marshal(commitNotification{RID: rid, UUID: uuid})
		if jsonErr != nil {
			log.Printf("hub commit publish: encode notification rid=%d uuid=%s: %v", rid, uuid, jsonErr)
			return
		}
		if pubErr := nc.Publish(commitSubject, payload); pubErr != nil {
			log.Printf("hub commit publish: publish to %s rid=%d uuid=%s: %v", commitSubject, rid, uuid, pubErr)
		}
	}

	h.natsCommitSub = sub
	h.commitSubject = commitSubject
	return nil
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
		if h.natsCommitSub != nil {
			_ = h.natsCommitSub.Unsubscribe()
		}
		if h.natsSyncSub != nil {
			_ = h.natsSyncSub.Unsubscribe()
		}
		if h.natsClient != nil {
			h.natsClient.Close()
		}
		if h.server != nil {
			h.server.Shutdown()
			h.server.WaitForShutdown()
		}
		if h.checkpointStop != nil {
			close(h.checkpointStop)
			<-h.checkpointDone
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

// ServerName returns the NATS server identity (Config.ServerName, or the
// auto-generated random name if Config.ServerName was empty).
func (h *Hub) ServerName() string { return h.server.Name() }

// LeafURL returns this hub's leafnode listener URL — the URL remote leaves
// dial as their upstream. Empty if no leaf port is configured. Distinct
// from Config.LeafUpstream, which is the input URL this hub itself solicits.
func (h *Hub) LeafURL() string { return h.leafURL }

// HTTPAddr returns the host:port the fossil HTTP server is bound to.
func (h *Hub) HTTPAddr() string { return h.httpAddr }

// NumLeafs returns the current count of active leafnode connections to
// this hub. Consumers can poll this to implement auto-shutdown behavior
// when the last remote leaf disconnects (e.g. sesh's "hub runs until the
// last session closes" lifecycle).
func (h *Hub) NumLeafs() int { return h.server.NumLeafNodes() }

// FossilSyncSubject returns the NATS subject the hub subscribes to for
// incoming fossil-sync xfer requests, or "" if Config.DisableFossilSyncOverNATS
// was set. Format: "<prefix>.<project-code>.sync".
func (h *Hub) FossilSyncSubject() string { return h.syncSubject }

// FossilCommitSubject returns the NATS subject the hub publishes on
// after a successful commit, and subscribes to for peer notifications.
// Empty if Config.DisableFossilSyncOverNATS was set. Format:
// "<prefix>.<project-code>.commit".
func (h *Hub) FossilCommitSubject() string { return h.commitSubject }

// Repo returns the underlying fossil-repo handle the hub serves from. Use
// this to share the handle with another component in the same process —
// e.g. a coexisting CLI command path that needs user/commit/read access
// without re-opening the file. Don't call Close on the returned *Repo
// while the Hub is still serving; use Hub.Stop for teardown.
func (h *Hub) Repo() *Repo { return h.repo }

func openOrCreateRepo(path, bootstrapUser, projectCode string) (*libfossil.Repo, error) {
	if r, err := libfossil.Open(path); err == nil {
		if projectCode != "" {
			onDisk, cfgErr := r.Config("project-code")
			if cfgErr != nil {
				_ = r.Close()
				return nil, fmt.Errorf("hub: read project-code from existing repo at %s: %w", path, cfgErr)
			}
			if onDisk != projectCode {
				_ = r.Close()
				return nil, fmt.Errorf("hub: existing repo at %s has project-code %q, Config.ProjectCode is %q (config drift)", path, onDisk, projectCode)
			}
		}
		return r, nil
	}
	r, err := libfossil.Create(path, libfossil.CreateOpts{
		User:        bootstrapUser,
		ProjectCode: projectCode,
	})
	if err != nil {
		return nil, fmt.Errorf("hub: create repo at %s: %w", path, err)
	}
	return r, nil
}

func startNATS(cfg Config) (*natsserver.Server, error) {
	opts := &natsserver.Options{
		Host:       "127.0.0.1",
		Port:       portOrAuto(cfg.NATSClientPort),
		ServerName: cfg.ServerName,
		NoLog:      true,
		NoSigs:     true,
		JetStream:  true,
		StoreDir:   cfg.NATSStoreDir,
	}
	opts.LeafNode.Host = "127.0.0.1"
	opts.LeafNode.Port = portOrAuto(cfg.NATSLeafPort)

	if cfg.LeafUpstream != "" {
		u, err := url.Parse(cfg.LeafUpstream)
		if err != nil {
			return nil, fmt.Errorf("hub: parse LeafUpstream %q: %w", cfg.LeafUpstream, err)
		}
		opts.LeafNode.Remotes = []*natsserver.RemoteLeafOpts{
			{URLs: []*url.URL{u}},
		}
	}

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
