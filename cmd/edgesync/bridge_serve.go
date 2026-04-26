package main

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/danmestas/libfossil/cli"
	"github.com/danmestas/EdgeSync/bridge/bridge"
)

type BridgeServeCmd struct {
	NATSUrl   string `help:"NATS server URL" default:"nats://localhost:4222"`
	FossilURL string `required:"" help:"Fossil server HTTP URL"`
	Project   string `required:"" help:"Project code for NATS subject"`
}

func (c *BridgeServeCmd) Run(g *cli.Globals) error {
	b, err := bridge.New(bridge.Config{
		NATSUrl:     c.NATSUrl,
		FossilURL:   c.FossilURL,
		ProjectCode: c.Project,
	})
	if err != nil {
		return fmt.Errorf("bridge: %w", err)
	}

	if err := b.Start(); err != nil {
		return fmt.Errorf("start: %w", err)
	}

	log.Printf("edgesync bridge running (nats=%s fossil=%s project=%s)", c.NATSUrl, c.FossilURL, c.Project)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	log.Printf("shutting down...")
	return b.Stop()
}
