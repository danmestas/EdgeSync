// Package agent implements the EdgeSync leaf agent that syncs a local
// Fossil repository via NATS messaging.
package agent

import (
	"errors"
	"os"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/nats-io/nats.go"
)

// Config holds all settings for a leaf agent instance.
type Config struct {
	// RepoPath is the path to the local Fossil repository file (required).
	RepoPath string

	// NATSUrl is the NATS server URL to connect to.
	NATSUrl string

	// PollInterval is how often the agent checks for local changes.
	PollInterval time.Duration

	// User is the Fossil user name sent during sync handshakes.
	User string

	// Password is the Fossil user password (optional).
	Password string

	// Push enables pushing local artifacts to the remote.
	Push bool

	// Pull enables pulling remote artifacts to local.
	Pull bool

	// UV enables syncing unversioned files (wiki, forum, attachments).
	UV bool

	// Private enables syncing private artifacts (requires 'x' capability).
	Private bool

	// PeerID uniquely identifies this agent in the peer registry.
	// Defaults to hostname if not set.
	PeerID string

	// SubjectPrefix is the NATS subject prefix (default "fossil").
	// The full subject is "<SubjectPrefix>.<project-code>.sync".
	SubjectPrefix string

	// Clock controls time operations (timer, sleep). Nil defaults to real time.
	// Set to a SimClock for deterministic simulation testing.
	Clock simio.Clock

	// Buggify is an optional fault injection checker. Nil in production.
	Buggify libfossil.BuggifyChecker

	// Observer receives telemetry callbacks during sync operations.
	// Nil defaults to no-op (no telemetry).
	Observer libfossil.SyncObserver

	// ServeHTTPAddr is the HTTP listen address (e.g. ":8080").
	// Empty means do not serve HTTP. When set, the leaf acts as a
	// Fossil-compatible HTTP sync server.
	ServeHTTPAddr string

	// ServeNATSEnabled starts a NATS request/reply listener on the
	// project sync subject. Enables leaf-to-leaf sync without a bridge.
	ServeNATSEnabled bool

	// CustomDialer overrides the default net.Dial for NATS connections.
	// Set to &wsdialer.WSDialer{URL: "ws://..."} for browser WebSocket.
	CustomDialer nats.CustomDialer

	// Logger receives human-readable agent lifecycle messages.
	// Nil means no logging beyond slog.
	Logger func(string)

	// PostSyncHook is called after each successful sync with the result.
	// Use for crosslinking received manifests or refreshing UI state.
	PostSyncHook func(result *libfossil.SyncResult)

	// IrohEnabled starts the iroh sidecar for peer-to-peer sync.
	IrohEnabled bool

	// IrohPeers is a list of remote EndpointIds to sync with.
	IrohPeers []string

	// IrohKeyPath is the path to the persistent Ed25519 keypair.
	// Defaults to "<repo-dir>.iroh-key" (adjacent to the repo file).
	IrohKeyPath string

	// ContentCacheSize is kept for config compatibility but is no longer
	// used directly — content caching is handled internally by go-libfossil.
	ContentCacheSize int64

	// Autosync controls automatic sync around commit (default: AutosyncOff).
	Autosync AutosyncMode

	// AllowFork bypasses fork and lock checks during commit.
	AllowFork bool

	// OverrideLock ignores lock conflicts during commit (implies AllowFork).
	OverrideLock bool
}

// applyDefaults fills in zero-valued fields with sensible defaults.
func (c *Config) applyDefaults() {
	if c.NATSUrl == "" {
		c.NATSUrl = "nats://localhost:4222"
	}
	if c.PollInterval == 0 {
		c.PollInterval = 5 * time.Second
	}
	// User left empty = no login card sent (unauthenticated "nobody" sync).
	// Set User + Password for authenticated sync.
	if !c.Push && !c.Pull {
		c.Push = true
		c.Pull = true
	}
	if c.PeerID == "" {
		if h, err := os.Hostname(); err == nil {
			c.PeerID = h
		} else {
			c.PeerID = "unknown"
		}
	}
	if c.SubjectPrefix == "" {
		c.SubjectPrefix = "fossil"
	}
	if c.Clock == nil {
		c.Clock = simio.RealClock{}
	}
	if c.IrohEnabled && c.IrohKeyPath == "" {
		c.IrohKeyPath = c.RepoPath + ".iroh-key"
	}
	if c.ContentCacheSize == 0 {
		c.ContentCacheSize = 32 << 20 // 32 MiB
	}
}

// validate checks that required fields are present after defaults are applied.
func (c *Config) validate() error {
	if c.RepoPath == "" {
		return errors.New("agent: config: RepoPath is required")
	}
	return nil
}
