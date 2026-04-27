package cli

import (
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"github.com/danmestas/EdgeSync/bridge/bridge"
	libfossilcli "github.com/danmestas/libfossil/cli"
)

// BridgeCmd is the top-level command group for the NATS-to-Fossil bridge.
type BridgeCmd struct {
	Serve BridgeServeCmd `cmd:"" help:"Start NATS-to-Fossil bridge"`
}

type BridgeServeCmd struct {
	NATSUrl   string `help:"NATS server URL" default:"nats://localhost:4222"`
	FossilURL string `required:"" help:"Fossil server HTTP URL"`
	Project   string `required:"" help:"Project code for NATS subject"`
}

func (c *BridgeServeCmd) Run(g *libfossilcli.Globals) error {
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
