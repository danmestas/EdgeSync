package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/dmestas/edgesync/leaf/agent"
)

func main() {
	repoPath := flag.String("repo", envOrDefault("LEAF_REPO", ""), "path to Fossil repository file (required)")
	natsURL := flag.String("nats", envOrDefault("LEAF_NATS_URL", "nats://localhost:4222"), "NATS server URL")
	poll := flag.Duration("poll", 5*time.Second, "poll interval")
	user := flag.String("user", envOrDefault("LEAF_USER", ""), "Fossil user name")
	password := flag.String("password", envOrDefault("LEAF_PASSWORD", ""), "Fossil user password")
	push := flag.Bool("push", true, "enable push")
	pull := flag.Bool("pull", true, "enable pull")

	flag.Parse()

	if *repoPath == "" {
		fmt.Fprintln(os.Stderr, "error: --repo is required (or set LEAF_REPO)")
		flag.Usage()
		os.Exit(1)
	}

	cfg := agent.Config{
		RepoPath:     *repoPath,
		NATSUrl:      *natsURL,
		PollInterval: *poll,
		User:         *user,
		Password:     *password,
		Push:         *push,
		Pull:         *pull,
	}

	a, err := agent.New(cfg)
	if err != nil {
		log.Fatalf("agent: init: %v", err)
	}

	if err := a.Start(); err != nil {
		log.Fatalf("agent: start: %v", err)
	}

	log.Printf("leaf-agent started: repo=%s nats=%s poll=%v", *repoPath, *natsURL, *poll)

	// Signal handling: SIGUSR1 triggers immediate sync, SIGINT/SIGTERM stops.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)

	for sig := range sigCh {
		switch sig {
		case syscall.SIGUSR1:
			log.Println("leaf-agent: SIGUSR1 received, triggering sync")
			a.SyncNow()
		case syscall.SIGINT, syscall.SIGTERM:
			log.Printf("leaf-agent: %v received, shutting down", sig)
			if err := a.Stop(); err != nil {
				log.Printf("leaf-agent: stop error: %v", err)
			}
			os.Exit(0)
		}
	}
}

// envOrDefault returns the value of the environment variable named key,
// or fallback if the variable is empty or unset.
func envOrDefault(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
