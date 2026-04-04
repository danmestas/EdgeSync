package agent

import (
	"context"
	"crypto/sha256"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/nats-io/nats.go"
)

// irohALPN is the ALPN protocol identifier for EdgeSync xfer over iroh.
const irohALPN = "/edgesync/xfer/1"

// syncTarget represents a single sync destination (NATS, iroh peer, etc.).
type syncTarget struct {
	transport libfossil.Transport
	label     string // "nats", "iroh:<endpoint>", etc.
}

// Event represents an input to the agent state machine.
type Event int

const (
	EventTimer   Event = iota // poll interval elapsed
	EventSyncNow              // manual sync trigger
	EventStop                 // shutdown requested
)

// ActionType classifies the result of processing an event.
type ActionType int

const (
	ActionNone    ActionType = iota
	ActionSynced                    // a sync round was executed
	ActionStopped                   // agent acknowledged stop
)

// Action is the result of processing a single event via Tick.
type Action struct {
	Type   ActionType
	Result *libfossil.SyncResult // non-nil when Type == ActionSynced
	Err    error                 // non-nil on sync failure
}

// Agent manages the lifecycle of a leaf sync daemon: it opens a Fossil repo,
// connects to NATS, and periodically syncs via the xfer protocol.
type Agent struct {
	config      Config
	clock       simio.Clock
	repo        *libfossil.Repo
	conn        *nats.Conn // nil when created via NewFromParts
	transport   libfossil.Transport
	syncTargets []syncTarget // unified list of sync destinations
	projectCode string
	serverCode  string
	cancel         context.CancelFunc
	done           chan struct{}
	syncNow        chan struct{} // buffer 1
	irohSidecar    *sidecar     // nil when iroh is disabled
	irohSock       string       // Unix socket path for iroh sidecar
	irohEndpointID string       // local iroh endpoint ID (set after sidecar ready)
}

// New creates a new Agent from the given configuration.
// It opens the Fossil repo, reads project-code and server-code from
// the config table, connects to NATS, and builds the NATSTransport.
func New(cfg Config) (*Agent, error) {
	cfg.applyDefaults()
	if err := cfg.validate(); err != nil {
		return nil, err
	}

	r, err := libfossil.Open(cfg.RepoPath)
	if err != nil {
		return nil, fmt.Errorf("agent: open repo: %w", err)
	}

	projectCode, err := r.Config("project-code")
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("agent: read project-code: %w", err)
	}

	serverCode, err := r.Config("server-code")
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("agent: read server-code: %w", err)
	}

	natsOpts := []nats.Option{
		nats.Name("edgesync-leaf"),
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2 * time.Second),
		nats.RetryOnFailedConnect(true),
	}
	if cfg.CustomDialer != nil {
		natsOpts = append(natsOpts, nats.SetCustomDialer(cfg.CustomDialer))
	}
	natsOpts = append(natsOpts, nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
		if err != nil {
			slog.Warn("NATS disconnected", "error", err)
			if cfg.Logger != nil {
				cfg.Logger("NATS disconnected: " + err.Error())
			}
		}
	}))
	natsOpts = append(natsOpts, nats.ReconnectHandler(func(_ *nats.Conn) {
		slog.Info("NATS reconnected", "url", cfg.NATSUrl)
		if cfg.Logger != nil {
			cfg.Logger("NATS reconnected")
		}
	}))
	nc, err := nats.Connect(cfg.NATSUrl, natsOpts...)
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("agent: nats connect: %w", err)
	}
	if nc.IsConnected() {
		slog.Info("connected to NATS", "url", cfg.NATSUrl)
		if cfg.Logger != nil {
			cfg.Logger("connected to NATS: " + cfg.NATSUrl)
		}
	} else {
		slog.Warn("NATS not yet reachable, will retry in background", "url", cfg.NATSUrl)
		if cfg.Logger != nil {
			cfg.Logger("warning: NATS not yet reachable at " + cfg.NATSUrl + ", will retry in background")
		}
	}

	transport := NewNATSTransport(nc, projectCode, 0, cfg.SubjectPrefix)

	a := &Agent{
		config:      cfg,
		clock:       cfg.Clock,
		repo:        r,
		conn:        nc,
		transport:   transport,
		projectCode: projectCode,
		serverCode:  serverCode,
		syncNow:     make(chan struct{}, 1),
	}

	// Initialize peer registry (log warnings, don't fail startup).
	if err := a.ensurePeerRegistry(); err != nil {
		a.logf("warning: ensurePeerRegistry: %v", err)
	}
	if err := a.seedPeerRegistry(); err != nil {
		a.logf("warning: seedPeerRegistry: %v", err)
	}

	return a, nil
}

// NewFromParts creates an Agent from pre-built components without performing
// any I/O. Used by tests and the deterministic simulation harness.
func NewFromParts(cfg Config, r *libfossil.Repo, t libfossil.Transport, projectCode, serverCode string) *Agent {
	cfg.applyDefaults()
	return &Agent{
		config:      cfg,
		clock:       cfg.Clock,
		repo:        r,
		transport:   t,
		projectCode: projectCode,
		serverCode:  serverCode,
		syncNow:     make(chan struct{}, 1),
	}
}

// Tick processes a single event and returns the resulting action.
// This is the core state machine entry point. In simulation, the caller
// drives events directly via the primary transport. In production,
// pollLoop iterates all syncTargets.
func (a *Agent) Tick(ctx context.Context, ev Event) Action {
	switch ev {
	case EventTimer:
		// BUGGIFY: skip a timer-triggered sync to test stale-state behavior.
		if a.config.Buggify != nil && a.config.Buggify.Check("agent.runSync.earlyReturn", 0.05) {
			return Action{Type: ActionNone}
		}
		result, err := a.runSync(ctx)
		return Action{Type: ActionSynced, Result: result, Err: err}
	case EventSyncNow:
		result, err := a.runSync(ctx)
		return Action{Type: ActionSynced, Result: result, Err: err}
	case EventStop:
		return Action{Type: ActionStopped}
	default:
		return Action{Type: ActionNone}
	}
}

func (a *Agent) logf(format string, args ...any) {
	if a.config.Logger != nil {
		a.config.Logger(fmt.Sprintf(format, args...))
	}
}

// Repo returns the agent's Fossil repository (for invariant checking in simulation).
func (a *Agent) Repo() *libfossil.Repo {
	return a.repo
}

// Start launches the background poll loop and any configured server
// listeners. Call Stop to shut everything down.
func (a *Agent) Start() error {
	a.logf("starting agent...")
	ctx, cancel := context.WithCancel(context.Background())
	a.cancel = cancel

	if err := a.startNATS(ctx); err != nil {
		cancel()
		return err
	}
	if err := a.startHTTPServer(ctx); err != nil {
		cancel()
		return err
	}
	if err := a.startIrohSidecar(ctx); err != nil {
		cancel()
		return err
	}

	a.done = make(chan struct{})
	go a.pollLoop(ctx)
	return nil
}

// startNATS launches the NATS serve listener if configured.
func (a *Agent) startNATS(ctx context.Context) error {
	// Add NATS as the primary sync target (always present when agent is built via New).
	if a.transport != nil {
		a.syncTargets = append(a.syncTargets, syncTarget{
			transport: a.transport,
			label:     "nats",
		})
	}

	if a.config.ServeNATSEnabled && a.conn != nil {
		go func() {
			subject := a.config.SubjectPrefix + "." + a.projectCode + ".sync"
			if err := ServeNATS(ctx, a.conn, subject, a.repo); err != nil {
				slog.Error("serve-nats stopped", "error", err)
			}
		}()
	}
	return nil
}

// startHTTPServer launches the public HTTP server if ServeHTTPAddr is set.
func (a *Agent) startHTTPServer(ctx context.Context) error {
	if a.config.ServeHTTPAddr == "" {
		return nil
	}
	go func() {
		if err := a.serveHTTP(ctx); err != nil {
			slog.Error("serve-http stopped", "error", err)
		}
	}()
	return nil
}

// startIrohSidecar launches the iroh sidecar process and registers iroh
// peers as sync targets. It always creates a dedicated callback listener
// for sidecar-to-agent xfer traffic (decoupled from the public HTTP server).
func (a *Agent) startIrohSidecar(ctx context.Context) error {
	if !a.config.IrohEnabled {
		return nil
	}

	binPath, err := a.findIrohBinary()
	if err != nil {
		return fmt.Errorf("agent: iroh: %w", err)
	}

	// Use a hash of the repo path to make the socket unique per agent instance,
	// so multiple agents in the same process (tests) don't collide.
	sockHash := fmt.Sprintf("%x", sha256.Sum256([]byte(a.config.RepoPath)))[:12]
	a.irohSock = fmt.Sprintf("/tmp/iroh-%s.sock", sockHash)

	// Always create a dedicated callback listener for sidecar-to-agent traffic.
	// This is internal plumbing -- not the agent's public HTTP server.
	callbackLn, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return fmt.Errorf("agent: iroh callback listener: %w", err)
	}
	callbackMux := http.NewServeMux()
	callbackMux.Handle("/", a.repo.XferHandler())
	callbackSrv := &http.Server{Handler: callbackMux}
	go func() {
		<-ctx.Done()
		callbackSrv.Close()
	}()
	go func() {
		if err := callbackSrv.Serve(callbackLn); err != nil && err != http.ErrServerClosed {
			slog.Error("iroh callback server stopped", "error", err)
		}
	}()
	callbackURL := fmt.Sprintf("http://127.0.0.1:%d", callbackLn.Addr().(*net.TCPAddr).Port)

	a.irohSidecar = &sidecar{
		binPath:     binPath,
		socketPath:  a.irohSock,
		keyPath:     a.config.IrohKeyPath,
		callbackURL: callbackURL,
		alpn:        irohALPN,
	}

	status, err := a.irohSidecar.Start(10 * time.Second)
	if err != nil {
		callbackSrv.Close()
		return fmt.Errorf("agent: iroh sidecar: %w", err)
	}
	a.irohEndpointID = status.EndpointID
	a.logf("iroh sidecar ready, endpoint_id=%s", status.EndpointID)

	// Register iroh peers as sync targets.
	for _, peerID := range a.config.IrohPeers {
		a.syncTargets = append(a.syncTargets, syncTarget{
			transport: NewIrohTransport(a.irohSock, peerID),
			label:     "iroh:" + peerID,
		})
	}

	// Monitor sidecar process liveness.
	go a.monitorSidecar(ctx)
	return nil
}

// Stop cancels the poll loop, waits for it to finish, closes the NATS
// connection, and closes the repo.
func (a *Agent) Stop() error {
	if a.cancel != nil {
		a.cancel()
	}
	if a.done != nil {
		<-a.done
	}
	if a.irohSidecar != nil {
		a.irohSidecar.shutdown()
	}
	if a.conn != nil {
		a.conn.Close()
	}
	if a.repo != nil {
		return a.repo.Close()
	}
	return nil
}

// IrohEndpointID returns the local iroh endpoint ID, or "" if iroh is not enabled
// or the sidecar hasn't started yet.
func (a *Agent) IrohEndpointID() string { return a.irohEndpointID }

// IrohSocketPath returns the Unix socket path for the iroh sidecar, or "" if iroh is not enabled.
func (a *Agent) IrohSocketPath() string { return a.irohSock }

// SyncNow triggers an immediate sync round. It is non-blocking: if a sync
// trigger is already pending, the call is a no-op.
func (a *Agent) SyncNow() {
	select {
	case a.syncNow <- struct{}{}:
	default:
	}
}

// pollLoop runs until the context is cancelled, converting channel events
// into Tick calls and logging results.
func (a *Agent) pollLoop(ctx context.Context) {
	defer close(a.done)
	a.logf("poll loop started, interval %s", a.config.PollInterval)
	for {
		var ev Event
		select {
		case <-ctx.Done():
			a.logf("poll loop stopped")
			return
		case <-a.clock.After(a.config.PollInterval):
			ev = EventTimer
		case <-a.syncNow:
			ev = EventSyncNow
			a.logf("manual sync triggered")
		}

		// BUGGIFY: skip a timer-triggered sync to test stale-state behavior.
		if ev == EventTimer && a.config.Buggify != nil && a.config.Buggify.Check("agent.runSync.earlyReturn", 0.05) {
			continue
		}

		// Iterate all sync targets uniformly.
		for _, target := range a.syncTargets {
			result, err := a.repo.Sync(ctx, target.transport, a.buildSyncOpts())
			if err != nil {
				a.logf("sync error [%s]: %v", target.label, err)
				slog.ErrorContext(ctx, "sync error", "target", target.label, "error", err)
				continue
			}
			a.logf("sync done [%s]: ↑%d ↓%d rounds=%d", target.label, result.FilesSent, result.FilesRecvd, result.Rounds)
			slog.DebugContext(ctx, "sync details",
				"target", target.label,
				"rounds", result.Rounds,
				"files_sent", result.FilesSent,
				"files_recv", result.FilesRecvd,
				"bytes_sent", result.BytesSent,
				"bytes_recv", result.BytesRecvd,
				"uv_sent", result.UVFilesSent,
				"uv_recv", result.UVFilesRecvd,
				"errors", len(result.Errors),
			)
			for _, e := range result.Errors {
				a.logf("sync warning [%s]: %s", target.label, e)
			}
			if a.config.PostSyncHook != nil {
				a.config.PostSyncHook(result)
			}
		}

		// Update peer registry once per poll cycle.
		if err := a.updatePeerRegistryAfterSync(); err != nil {
			a.logf("warning: updatePeerRegistryAfterSync: %v", err)
		}
	}
}

// buildSyncOpts constructs SyncOpts from the agent's config.
func (a *Agent) buildSyncOpts() libfossil.SyncOpts {
	return libfossil.SyncOpts{
		Push:        a.config.Push,
		Pull:        a.config.Pull,
		ProjectCode: a.projectCode,
		ServerCode:  a.serverCode,
		User:        a.config.User,
		Password:    a.config.Password,
		PeerID:      a.config.PeerID,
		Buggify:     a.config.Buggify,
		UV:          a.config.UV,
		XTableSync:  a.config.ServeNATSEnabled,
		Private:     a.config.Private,
		Observer:    a.config.Observer,
	}
}

// runSync performs one sync cycle against the transport.
func (a *Agent) runSync(ctx context.Context) (*libfossil.SyncResult, error) {
	return a.repo.Sync(ctx, a.transport, a.buildSyncOpts())
}

// monitorSidecar watches the sidecar process and logs if it exits unexpectedly.
func (a *Agent) monitorSidecar(ctx context.Context) {
	if a.irohSidecar == nil {
		return
	}
	select {
	case <-a.irohSidecar.exited:
		// Check if this was an expected shutdown.
		select {
		case <-ctx.Done():
			return
		default:
		}
		slog.Error("iroh sidecar exited unexpectedly", "error", a.irohSidecar.exitErr)
		a.logf("iroh sidecar died: %v — iroh sync will fail until restart", a.irohSidecar.exitErr)
	case <-ctx.Done():
		return
	}
}

// findIrohBinary looks for the iroh-sidecar binary. It checks, in order:
// 1. Config.IrohBinaryPath (explicit override)
// 2. Next to the leaf binary
// 3. In PATH
func (a *Agent) findIrohBinary() (string, error) {
	if a.config.IrohBinaryPath != "" {
		if _, err := os.Stat(a.config.IrohBinaryPath); err != nil {
			return "", fmt.Errorf("iroh-sidecar binary not found at configured path %q: %w", a.config.IrohBinaryPath, err)
		}
		return a.config.IrohBinaryPath, nil
	}

	exe, err := os.Executable()
	if err == nil {
		candidate := filepath.Join(filepath.Dir(exe), "iroh-sidecar")
		if _, err := os.Stat(candidate); err == nil {
			return candidate, nil
		}
	}

	path, err := exec.LookPath("iroh-sidecar")
	if err != nil {
		return "", fmt.Errorf("iroh-sidecar binary not found (not next to leaf binary, not in PATH)")
	}
	return path, nil
}
