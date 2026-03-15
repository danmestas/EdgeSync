// Package agent implements the EdgeSync leaf agent that syncs a local
// Fossil repository via NATS messaging.
package agent

import (
	"errors"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/simio"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
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

	// SubjectPrefix is the NATS subject prefix (default "fossil").
	// The full subject is "<SubjectPrefix>.<project-code>.sync".
	SubjectPrefix string

	// Clock controls time operations (timer, sleep). Nil defaults to real time.
	// Set to a SimClock for deterministic simulation testing.
	Clock simio.Clock

	// Buggify is an optional fault injection checker. Nil in production.
	Buggify libsync.BuggifyChecker
}

// applyDefaults fills in zero-valued fields with sensible defaults.
func (c *Config) applyDefaults() {
	if c.NATSUrl == "" {
		c.NATSUrl = "nats://localhost:4222"
	}
	if c.PollInterval == 0 {
		c.PollInterval = 5 * time.Second
	}
	if c.User == "" {
		c.User = "anonymous"
	}
	if !c.Push && !c.Pull {
		c.Push = true
		c.Pull = true
	}
	if c.SubjectPrefix == "" {
		c.SubjectPrefix = "fossil"
	}
	if c.Clock == nil {
		c.Clock = simio.RealClock{}
	}
}

// validate checks that required fields are present after defaults are applied.
func (c *Config) validate() error {
	if c.RepoPath == "" {
		return errors.New("agent: config: RepoPath is required")
	}
	return nil
}
