package bridge

import (
	"fmt"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
)

// Config holds the settings for a Bridge instance.
type Config struct {
	NATSUrl       string // default "nats://localhost:4222"
	FossilURL     string // required (unless Upstream is set)
	ProjectCode   string // required
	SubjectPrefix string // default "fossil"

	// Upstream is the transport to the Fossil server. Nil defaults to
	// HTTPTransport using FossilURL. Set for simulation or testing.
	Upstream libsync.Transport
}

func (c *Config) applyDefaults() {
	if c.NATSUrl == "" {
		c.NATSUrl = "nats://localhost:4222"
	}
	if c.SubjectPrefix == "" {
		c.SubjectPrefix = "fossil"
	}
}

func (c *Config) validate() error {
	if c.FossilURL == "" && c.Upstream == nil {
		return fmt.Errorf("bridge: FossilURL is required (unless Upstream is set)")
	}
	if c.ProjectCode == "" {
		return fmt.Errorf("bridge: ProjectCode is required")
	}
	return nil
}
