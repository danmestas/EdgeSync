package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/dmestas/edgesync/leaf/agent"
)

type SyncStartCmd struct {
	NATSUrl      string        `help:"NATS server URL" default:"nats://localhost:4222"`
	PollInterval time.Duration `help:"Sync poll interval" default:"5s"`
	Push         bool          `help:"Enable push" default:"true" negatable:""`
	Pull         bool          `help:"Enable pull" default:"true" negatable:""`
}

func (c *SyncStartCmd) Run(g *Globals) error {
	if g.Repo == "" {
		return fmt.Errorf("repository required (use -R <path>)")
	}

	a, err := agent.New(agent.Config{
		RepoPath:     g.Repo,
		NATSUrl:      c.NATSUrl,
		PollInterval: c.PollInterval,
		Push:         c.Push,
		Pull:         c.Pull,
	})
	if err != nil {
		return fmt.Errorf("agent: %w", err)
	}

	if err := a.Start(); err != nil {
		return fmt.Errorf("start: %w", err)
	}

	log.Printf("edgesync sync agent running (repo=%s nats=%s poll=%s)", g.Repo, c.NATSUrl, c.PollInterval)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	log.Printf("shutting down...")
	return a.Stop()
}
