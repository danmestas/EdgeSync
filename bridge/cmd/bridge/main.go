package main

import (
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/dmestas/edgesync/bridge/bridge"
)

func main() {
	fossilURL := flag.String("fossil", envOrDefault("BRIDGE_FOSSIL_URL", ""), "Fossil HTTP server URL (required)")
	projectCode := flag.String("project", envOrDefault("BRIDGE_PROJECT_CODE", ""), "Fossil project code (required)")
	natsURL := flag.String("nats", envOrDefault("BRIDGE_NATS_URL", "nats://localhost:4222"), "NATS server URL")
	prefix := flag.String("prefix", envOrDefault("BRIDGE_PREFIX", "fossil"), "NATS subject prefix")
	flag.Parse()

	if *fossilURL == "" || *projectCode == "" {
		fmt.Fprintln(os.Stderr, "Usage: bridge --fossil <url> --project <code> [--nats <url>] [--prefix <prefix>]")
		fmt.Fprintln(os.Stderr, "")
		fmt.Fprintln(os.Stderr, "Environment variables: BRIDGE_FOSSIL_URL, BRIDGE_PROJECT_CODE, BRIDGE_NATS_URL, BRIDGE_PREFIX")
		flag.PrintDefaults()
		os.Exit(1)
	}

	cfg := bridge.Config{
		NATSUrl:       *natsURL,
		FossilURL:     *fossilURL,
		ProjectCode:   *projectCode,
		SubjectPrefix: *prefix,
	}

	b, err := bridge.New(cfg)
	if err != nil {
		slog.Error("bridge init failed", "error", err)
		os.Exit(1)
	}

	if err := b.Start(); err != nil {
		slog.Error("bridge start failed", "error", err)
		os.Exit(1)
	}

	// Wait for SIGINT or SIGTERM.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	s := <-sig
	slog.Info("received signal, shutting down", "signal", s.String())

	if err := b.Stop(); err != nil {
		slog.Error("bridge stop failed", "error", err)
		os.Exit(1)
	}
}

func envOrDefault(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
