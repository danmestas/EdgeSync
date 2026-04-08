package main

import (
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/danmestas/go-libfossil/cli"
	"github.com/dmestas/edgesync/leaf/agent"
)

type SyncStartCmd struct {
	NATSUrl      string        `help:"NATS server URL" default:"nats://localhost:4222"`
	PollInterval time.Duration `help:"Sync poll interval" default:"5s"`
	Push         bool          `help:"Enable push" default:"true" negatable:""`
	Pull         bool          `help:"Enable pull" default:"true" negatable:""`
	UV           bool          `help:"Sync unversioned files" default:"false"`
}

func (c *SyncStartCmd) Run(g *cli.Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}

	a, err := agent.New(agent.Config{
		RepoPath:     g.Repo,
		NATSUpstream: c.NATSUrl,
		PollInterval: c.PollInterval,
		Push:         c.Push,
		Pull:         c.Pull,
		UV:           c.UV,
	})
	if err != nil {
		return fmt.Errorf("agent: %w", err)
	}

	if err := a.Start(); err != nil {
		return fmt.Errorf("start: %w", err)
	}

	slog.Info("edgesync sync agent running", "repo", g.Repo, "nats", c.NATSUrl, "poll", c.PollInterval)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	slog.Info("shutting down")
	return a.Stop()
}
