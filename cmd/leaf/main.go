// Command leaf runs the EdgeSync leaf agent.
//
// The leaf agent watches a local Fossil repository for changes,
// publishes new artifacts to NATS, and subscribes for incoming
// artifacts from other nodes.
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	repoPath := flag.String("repo", "", "path to Fossil repository database")
	natsURL := flag.String("nats", "nats://localhost:4222", "NATS server URL")
	flag.Parse()

	if *repoPath == "" {
		fmt.Fprintln(os.Stderr, "usage: leaf -repo <path-to-repo.fossil> [-nats <url>]")
		os.Exit(1)
	}

	log.Printf("EdgeSync leaf agent starting")
	log.Printf("  repo: %s", *repoPath)
	log.Printf("  nats: %s", *natsURL)

	// TODO: Phase 2 — Open repo DB
	// TODO: Phase 3 — Connect to NATS, set up JetStream consumer
	// TODO: Phase 4 — Watch for repo changes, publish igot/file messages
	// TODO: Phase 4 — Subscribe for gimme requests, respond with artifacts

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Printf("EdgeSync leaf agent shutting down")
}
