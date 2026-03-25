package agent

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/nats-io/nats.go"
)

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
	Result *sync.SyncResult // non-nil when Type == ActionSynced
	Err    error            // non-nil on sync failure
}

// Agent manages the lifecycle of a leaf sync daemon: it opens a Fossil repo,
// connects to NATS, and periodically syncs via the xfer protocol.
type Agent struct {
	config      Config
	clock       simio.Clock
	repo        *repo.Repo
	conn        *nats.Conn // nil when created via NewFromParts
	transport   sync.Transport
	projectCode string
	serverCode  string
	cancel      context.CancelFunc
	done        chan struct{}
	syncNow     chan struct{} // buffer 1
}

// New creates a new Agent from the given configuration.
// It opens the Fossil repo, reads project-code and server-code from
// the config table, connects to NATS, and builds the NATSTransport.
func New(cfg Config) (*Agent, error) {
	cfg.applyDefaults()
	if err := cfg.validate(); err != nil {
		return nil, err
	}

	r, err := repo.Open(cfg.RepoPath)
	if err != nil {
		return nil, fmt.Errorf("agent: open repo: %w", err)
	}

	var projectCode string
	err = r.DB().QueryRow("SELECT value FROM config WHERE name=?", "project-code").Scan(&projectCode)
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("agent: read project-code: %w", err)
	}

	var serverCode string
	err = r.DB().QueryRow("SELECT value FROM config WHERE name=?", "server-code").Scan(&serverCode)
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("agent: read server-code: %w", err)
	}

	natsOpts := []nats.Option{nats.Name("edgesync-leaf")}
	if cfg.CustomDialer != nil {
		natsOpts = append(natsOpts, nats.SetCustomDialer(cfg.CustomDialer))
	}
	if cfg.Logger != nil {
		natsOpts = append(natsOpts, nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
			if err != nil {
				cfg.Logger("NATS disconnected: " + err.Error())
			}
		}))
		natsOpts = append(natsOpts, nats.ReconnectHandler(func(_ *nats.Conn) {
			cfg.Logger("NATS reconnected")
		}))
	}
	nc, err := nats.Connect(cfg.NATSUrl, natsOpts...)
	if err != nil {
		r.Close()
		return nil, fmt.Errorf("agent: nats connect: %w", err)
	}
	if cfg.Logger != nil {
		cfg.Logger("connected to NATS: " + cfg.NATSUrl)
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
func NewFromParts(cfg Config, r *repo.Repo, t sync.Transport, projectCode, serverCode string) *Agent {
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
// drives events directly. In production, pollLoop converts channel
// selects into Tick calls.
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
func (a *Agent) Repo() *repo.Repo {
	return a.repo
}

// Start launches the background poll loop and any configured server
// listeners. Call Stop to shut everything down.
func (a *Agent) Start() error {
	a.logf("starting agent...")
	ctx, cancel := context.WithCancel(context.Background())
	a.cancel = cancel
	a.done = make(chan struct{})
	go a.pollLoop(ctx)

	// Server listeners
	if a.config.ServeHTTPAddr != "" {
		go func() {
			if err := a.serveHTTP(ctx); err != nil {
				slog.Error("serve-http stopped", "error", err)
			}
		}()
	}
	if a.config.ServeNATSEnabled && a.conn != nil {
		go func() {
			subject := a.config.SubjectPrefix + "." + a.projectCode + ".sync"
			if err := ServeNATS(ctx, a.conn, subject, a.repo, sync.HandleSync); err != nil {
				slog.Error("serve-nats stopped", "error", err)
			}
		}()
	}

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
	if a.conn != nil {
		a.conn.Close()
	}
	if a.repo != nil {
		return a.repo.Close()
	}
	return nil
}

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

		act := a.Tick(ctx, ev)
		if act.Err != nil {
			a.logf("sync error: %v", act.Err)
			slog.ErrorContext(ctx, "sync error", "error", act.Err)
			continue
		}
		if act.Result != nil {
			a.logf("sync done: ↑%d ↓%d rounds=%d", act.Result.FilesSent, act.Result.FilesRecvd, act.Result.Rounds)
			slog.InfoContext(ctx, "sync done", "rounds", act.Result.Rounds, "sent", act.Result.FilesSent, "recv", act.Result.FilesRecvd, "errors", len(act.Result.Errors))
			for _, e := range act.Result.Errors {
				a.logf("sync warning: %s", e)
				slog.WarnContext(ctx, "sync protocol error", "detail", e)
			}
			if a.config.PostSyncHook != nil {
				a.config.PostSyncHook(act.Result)
			}
			// Update peer registry after successful sync.
			if err := a.updatePeerRegistryAfterSync(); err != nil {
				a.logf("warning: updatePeerRegistryAfterSync: %v", err)
			}
		}
	}
}

// runSync performs one sync cycle against the transport.
func (a *Agent) runSync(ctx context.Context) (*sync.SyncResult, error) {
	opts := sync.SyncOpts{
		Push:        a.config.Push,
		Pull:        a.config.Pull,
		ProjectCode: a.projectCode,
		ServerCode:  a.serverCode,
		User:        a.config.User,
		Password:    a.config.Password,
		Buggify:     a.config.Buggify,
		UV:          a.config.UV,
		Private:     a.config.Private,
		Observer:    a.config.Observer,
	}
	return sync.Sync(ctx, a.repo, a.transport, opts)
}
