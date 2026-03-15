// Command bridge runs the EdgeSync NATS-to-HTTP bridge.
//
// The bridge translates between NATS artifact messages and
// Fossil's HTTP /xfer card protocol. To the master Fossil server,
// the bridge looks like a normal Fossil client doing push/pull.
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
	masterURL := flag.String("master", "", "Fossil master server URL (e.g. https://fossil.example.com)")
	natsURL := flag.String("nats", "nats://localhost:4222", "NATS server URL")
	projectCode := flag.String("project", "", "Fossil project code")
	flag.Parse()

	if *masterURL == "" || *projectCode == "" {
		fmt.Fprintln(os.Stderr, "usage: bridge -master <fossil-url> -project <code> [-nats <url>]")
		os.Exit(1)
	}

	log.Printf("EdgeSync bridge starting")
	log.Printf("  master: %s", *masterURL)
	log.Printf("  nats:   %s", *natsURL)
	log.Printf("  project: %s", *projectCode)

	// TODO: Phase 5 — Connect to NATS, subscribe to project subjects
	// TODO: Phase 5 — On incoming artifacts from NATS, push to master via HTTP /xfer
	// TODO: Phase 5 — Periodically pull from master, publish new artifacts to NATS

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Printf("EdgeSync bridge shutting down")
}
