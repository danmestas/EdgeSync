package cli

import (
	"context"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/danmestas/EdgeSync/hub"
	libfossilcli "github.com/danmestas/libfossil/cli"
)

// HubCmd is the top-level command group for the in-process EdgeSync hub.
type HubCmd struct {
	Serve HubServeCmd `cmd:"" help:"Run an embedded hub (NATS + fossil) at the machine level"`
}

type HubServeCmd struct {
	Name             string `help:"NATS server name (defaults to <hostname>-hub)"`
	StoreDir         string `help:"JetStream store dir (default sibling of repo)"`
	HTTPPort         int    `help:"Fossil HTTP port (0 = auto)" default:"0"`
	NATSClientPort   int    `help:"NATS client port (0 = auto)" default:"0"`
	NATSLeafPort     int    `help:"NATS leafnode port (0 = auto)" default:"0"`
	LeafUpstream     string `help:"Solicit leafnode connection to this URL (e.g. nats-leaf://host:port)"`
	NobodyCaps       string `help:"Caps for unauthenticated 'nobody' user (e.g. 'gio' for clone access)" default:"gio"`
	ProjectCode      string `help:"Pin the Fossil project-code (40-char lowercase hex). Share across hubs so they sync over the same NATS subject. Must match the on-disk project-code if the repo already exists."`
	SeedFromUpstream string `help:"Clone from an upstream EdgeSync hub HTTP xfer endpoint (e.g. http://hub.local:8080/) before opening. No-op if the repo already exists at the target path."`
}

func (c *HubServeCmd) Run(g *libfossilcli.Globals) error {
	if c.ProjectCode != "" && !validProjectCode(c.ProjectCode) {
		return fmt.Errorf("--project-code must be 40-char lowercase hex, got %q", c.ProjectCode)
	}

	name := c.Name
	if name == "" {
		host, err := os.Hostname()
		if err != nil {
			return fmt.Errorf("hostname: %w", err)
		}
		name = shortHost(host) + "-hub"
	}

	repoPath := g.Repo
	if repoPath == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return fmt.Errorf("home dir: %w", err)
		}
		repoPath = filepath.Join(home, ".edgesync", name, "hub.repo")
		if err := os.MkdirAll(filepath.Dir(repoPath), 0o755); err != nil {
			return fmt.Errorf("mkdir %s: %w", filepath.Dir(repoPath), err)
		}
	}

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	h, err := hub.NewHub(ctx, hub.Config{
		RepoPath:         repoPath,
		ServerName:       name,
		NATSStoreDir:     c.StoreDir,
		FossilHTTPPort:   c.HTTPPort,
		NATSClientPort:   c.NATSClientPort,
		NATSLeafPort:     c.NATSLeafPort,
		LeafUpstream:     c.LeafUpstream,
		NobodyCaps:       c.NobodyCaps,
		ProjectCode:      c.ProjectCode,
		SeedFromUpstream: c.SeedFromUpstream,
	})
	if err != nil {
		return fmt.Errorf("hub: %w", err)
	}

	slog.Info("edgesync hub running",
		"name", h.ServerName(),
		"repo", repoPath,
		"http", "http://"+h.HTTPAddr(),
		"nats", h.NATSURL(),
		"leaf_url", h.LeafURL(),
		"sync_subject", h.FossilSyncSubject(),
	)

	serveErr := make(chan error, 1)
	go func() { serveErr <- h.ServeHTTP(ctx) }()

	select {
	case <-ctx.Done():
	case err := <-serveErr:
		if err != nil {
			slog.Error("hub HTTP server stopped", "err", err)
		}
	}

	slog.Info("shutting down hub", "name", name)
	return h.Stop()
}

// shortHost trims a FQDN to its first label, mirroring `hostname -s`.
func shortHost(h string) string {
	for i := 0; i < len(h); i++ {
		if h[i] == '.' {
			return h[:i]
		}
	}
	return h
}

// validProjectCode reports whether s matches libfossil's project-code
// format: 40 lowercase hex characters. Matches `^[0-9a-f]{40}$`. Kept
// inline rather than importing regexp for a one-off cheap check.
func validProjectCode(s string) bool {
	if len(s) != 40 {
		return false
	}
	for _, r := range s {
		switch {
		case r >= '0' && r <= '9':
		case r >= 'a' && r <= 'f':
		default:
			return false
		}
	}
	return true
}
