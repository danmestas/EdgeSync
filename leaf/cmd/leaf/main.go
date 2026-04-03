package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	libsync "github.com/danmestas/go-libfossil/sync"
	"github.com/dmestas/edgesync/leaf/agent"
	"github.com/dmestas/edgesync/leaf/telemetry"

	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
)

func main() {
	repoPath := flag.String("repo", envOrDefault("LEAF_REPO", ""), "path to Fossil repository file (required)")
	natsURL := flag.String("nats", envOrDefault("LEAF_NATS_URL", "nats://localhost:4222"), "NATS server URL")
	poll := flag.Duration("poll", 5*time.Second, "poll interval")
	user := flag.String("user", envOrDefault("LEAF_USER", ""), "Fossil user name")
	password := flag.String("password", envOrDefault("LEAF_PASSWORD", ""), "Fossil user password")
	push := flag.Bool("push", true, "enable push")
	pull := flag.Bool("pull", true, "enable pull")
	serveHTTP := flag.String("serve-http", envOrDefault("LEAF_SERVE_HTTP", ""), "HTTP listen address (e.g. :8080) to serve fossil clone/sync")
	serveNATS := flag.Bool("serve-nats", false, "enable NATS request/reply listener for leaf-to-leaf sync")
	uv := flag.Bool("uv", false, "enable unversioned file sync (wiki, forum, attachments)")
	iroh := flag.Bool("iroh", envBool("LEAF_IROH"), "enable iroh sidecar for peer-to-peer sync")
	irohKeyPath := flag.String("iroh-key", envOrDefault("LEAF_IROH_KEY", ""), "path to iroh Ed25519 keypair (default: <repo>.iroh-key)")

	var irohPeers stringSlice
	if v := os.Getenv("LEAF_IROH_PEERS"); v != "" {
		for _, p := range strings.Split(v, ",") {
			if p = strings.TrimSpace(p); p != "" {
				irohPeers = append(irohPeers, p)
			}
		}
	}
	flag.Var(&irohPeers, "iroh-peer", "remote iroh EndpointId to sync with (repeatable)")
	var verboseFlag bool
	flag.BoolVar(&verboseFlag, "verbose", false, "log sync round details to stderr (no OTel required)")
	flag.BoolVar(&verboseFlag, "v", false, "alias for --verbose")
	autosyncFlag := flag.String("autosync", "off", "autosync mode: on, off, pullonly")
	allowFork := flag.Bool("allow-fork", false, "bypass fork and lock checks")
	overrideLock := flag.Bool("override-lock", false, "ignore lock conflicts (implies allow-fork)")

	// OTel flags (fall back to standard OTEL_* env vars)
	otelEndpoint := flag.String("otel-endpoint", envOrDefault("OTEL_EXPORTER_OTLP_ENDPOINT", ""), "OTel OTLP endpoint")
	otelHeaders := flag.String("otel-headers", envOrDefault("OTEL_EXPORTER_OTLP_HEADERS", ""), "OTel OTLP headers (key=value,key=value)")
	otelServiceName := flag.String("otel-service-name", envOrDefault("OTEL_SERVICE_NAME", "edgesync-leaf"), "OTel service name")

	flag.Parse()

	if *repoPath == "" {
		fmt.Fprintln(os.Stderr, "error: --repo is required (or set LEAF_REPO)")
		flag.Usage()
		os.Exit(1)
	}

	// Setup telemetry
	ctx := context.Background()
	telCfg := telemetry.TelemetryConfig{
		ServiceName: *otelServiceName,
		Endpoint:    *otelEndpoint,
		Headers:     parseHeaders(*otelHeaders),
	}
	shutdown, err := telemetry.Setup(ctx, telCfg)
	if err != nil {
		slog.Error("telemetry setup failed", "error", err)
		os.Exit(1)
	}

	if *otelEndpoint == "" {
		slog.Info("telemetry disabled: no OTEL_EXPORTER_OTLP_ENDPOINT or --otel-endpoint set")
	} else {
		slog.Info("telemetry enabled", "endpoint", *otelEndpoint, "service", *otelServiceName)
	}

	// Create observer (nil-safe: if no endpoint, OTel uses no-op providers)
	var obs libsync.Observer = telemetry.NewOTelObserver(nil, nil)
	if verboseFlag {
		obs = telemetry.NewVerboseObserver(os.Stderr, obs)
	}

	cfg := agent.Config{
		RepoPath:         *repoPath,
		NATSUrl:          *natsURL,
		PollInterval:     *poll,
		User:             *user,
		Password:         *password,
		Push:             *push,
		Pull:             *pull,
		UV:               *uv,
		ServeHTTPAddr:    *serveHTTP,
		ServeNATSEnabled: *serveNATS,
		Observer:         obs,
		IrohEnabled:      *iroh,
		IrohPeers:        irohPeers,
		IrohKeyPath:      *irohKeyPath,
		Autosync:         parseAutosyncMode(*autosyncFlag),
		AllowFork:        *allowFork,
		OverrideLock:     *overrideLock,
	}

	a, err := agent.New(cfg)
	if err != nil {
		slog.Error("agent init failed", "error", err)
		os.Exit(1)
	}

	if err := a.Start(); err != nil {
		slog.Error("agent start failed", "error", err)
		os.Exit(1)
	}

	logAttrs := []any{"repo", *repoPath, "nats", *natsURL, "poll", *poll}
	if *iroh {
		logAttrs = append(logAttrs, "iroh", true, "iroh_peers", len(irohPeers))
	}
	slog.Info("leaf-agent started", logAttrs...)
	awaitSignals(a, shutdown)
}

// awaitSignals blocks until SIGINT/SIGTERM (clean shutdown) or SIGUSR1 (manual sync).
func awaitSignals(a *agent.Agent, shutdown func(context.Context) error) {
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)

	for sig := range sigCh {
		switch sig {
		case syscall.SIGUSR1:
			slog.Info("SIGUSR1 received, triggering sync")
			a.SyncNow()
		case syscall.SIGINT, syscall.SIGTERM:
			slog.Info("shutdown signal received", "signal", sig.String())
			if err := a.Stop(); err != nil {
				slog.Error("agent stop error", "error", err)
			}
			shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			if err := shutdown(shutdownCtx); err != nil {
				slog.Error("telemetry shutdown error", "error", err)
			}
			cancel()
			os.Exit(0)
		}
	}
}

// parseHeaders parses "key=value,key=value" into a map.
func parseHeaders(s string) map[string]string {
	if s == "" {
		return nil
	}
	headers := make(map[string]string)
	for _, pair := range strings.Split(s, ",") {
		k, v, ok := strings.Cut(pair, "=")
		if ok {
			headers[strings.TrimSpace(k)] = strings.TrimSpace(v)
		}
	}
	return headers
}

// parseAutosyncMode converts a CLI string to an AutosyncMode value.
func parseAutosyncMode(s string) agent.AutosyncMode {
	switch strings.ToLower(s) {
	case "on":
		return agent.AutosyncOn
	case "pullonly":
		return agent.AutosyncPullOnly
	default:
		return agent.AutosyncOff
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

// envBool returns true if the named env var is set to "1", "true", or "yes".
func envBool(key string) bool {
	switch strings.ToLower(os.Getenv(key)) {
	case "1", "true", "yes":
		return true
	}
	return false
}

// stringSlice implements flag.Value for repeatable string flags.
type stringSlice []string

func (s *stringSlice) String() string { return fmt.Sprintf("%v", *s) }
func (s *stringSlice) Set(v string) error {
	*s = append(*s, v)
	return nil
}
