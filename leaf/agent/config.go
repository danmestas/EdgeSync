// Package agent implements the EdgeSync leaf agent that syncs a local
// Fossil repository via NATS messaging.
package agent

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	libfossil "github.com/danmestas/libfossil"
	"github.com/danmestas/libfossil/simio"
	"github.com/nats-io/nats.go"
)

// NATSRole determines how the embedded NATS server participates in the mesh.
type NATSRole string

const (
	NATSRolePeer NATSRole = "peer" // accepts + solicits (lower EndpointId solicits)
	NATSRoleHub  NATSRole = "hub"  // accepts only, never solicits
	NATSRoleLeaf NATSRole = "leaf" // solicits only, never accepts
)

// Config holds all settings for a leaf agent instance.
type Config struct {
	// RepoPath is the path to the local Fossil repository file (required).
	RepoPath string

	// NATSUpstream is an optional external NATS URL that the embedded server
	// joins as a leaf node. Empty means standalone mesh (no upstream).
	NATSUpstream string

	// NATSClientPort is the local embedded NATS client listener port. Zero
	// lets NATS choose a random localhost port.
	NATSClientPort int

	// NATSStoreDir is where JetStream persists streams and KV buckets.
	// Empty falls back to "<cwd>/.nats-store". Callers that want isolated
	// per-workspace state should pass an absolute path under the workspace
	// (e.g. "<workspace>/.agent-infra/nats-store").
	NATSStoreDir string

	// NATSRole determines how the embedded NATS server participates in the
	// mesh (peer, hub, or leaf). Default: peer.
	NATSRole NATSRole

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

	// IrohBinaryPath is the path to the iroh-sidecar binary.
	// Empty means auto-discover (next to leaf binary, then PATH).
	IrohBinaryPath string

	// ContentCacheSize is kept for config compatibility but is no longer
	// used directly — content caching is handled internally by libfossil.
	ContentCacheSize int64

	// NotifyEnabled starts the notify messaging service alongside sync.
	// Requires a notify.fossil repo adjacent to the main repo (or at NotifyRepoPath).
	NotifyEnabled bool

	// NotifyRepoPath is the path to the notify.fossil repo.
	// Defaults to "<repo-dir>/notify.fossil" if empty.
	NotifyRepoPath string

	// Autosync controls automatic sync around commit (default: AutosyncOff).
	Autosync AutosyncMode

	// AllowFork bypasses fork and lock checks during commit.
	AllowFork bool

	// OverrideLock ignores lock conflicts during commit (implies AllowFork).
	OverrideLock bool
}

// applyDefaults fills in zero-valued fields with sensible defaults.
func (c *Config) applyDefaults() {
	if c.NATSRole == "" {
		c.NATSRole = NATSRolePeer
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
	if c.NotifyEnabled && c.NotifyRepoPath == "" {
		c.NotifyRepoPath = filepath.Join(filepath.Dir(c.RepoPath), "notify.fossil")
	}
}

// validate checks that required fields are present after defaults are applied.
func (c *Config) validate() error {
	if c.RepoPath == "" {
		return errors.New("agent: config: RepoPath is required")
	}
	switch c.NATSRole {
	case NATSRolePeer, NATSRoleHub, NATSRoleLeaf:
		// valid
	default:
		return fmt.Errorf("agent: config: invalid NATSRole %q (must be peer, hub, or leaf)", c.NATSRole)
	}
	if c.NATSClientPort < 0 || c.NATSClientPort > 65535 {
		return fmt.Errorf("agent: config: NATSClientPort %d out of range", c.NATSClientPort)
	}
	return nil
}
