package sim

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"

	"github.com/dmestas/edgesync/bridge/bridge"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/leaf/agent"
)

// Harness manages the full lifecycle of a simulation environment:
// Fossil server process, embedded NATS, leaf repos, bridge, and agents.
type Harness struct {
	Config SimConfig

	tmpDir     string
	fossilCmd  *exec.Cmd
	fossilURL  string
	fossilRepo string
	nats       *natsserver.Server
	natsURL    string
	// proxy is nil until Phase 2 fault injection is wired in.
	// TODO: add after proxy.go exists
	// proxy *FaultProxy
	bridge    *bridge.Bridge
	leaves    []*agent.Agent
	leafPaths []string
}

// NewHarness creates a Harness with the given config, applying defaults.
func NewHarness(cfg SimConfig) *Harness {
	cfg.applyDefaults()
	return &Harness{Config: cfg}
}

// SetupInfra creates the temp directory, starts a Fossil server process,
// starts an embedded NATS server, and creates leaf repos with the
// project-code set to "sim-project". It does NOT start agents — call
// StartAgents after seeding blobs.
func (h *Harness) SetupInfra() error {
	var err error

	// Temp directory for all repos.
	h.tmpDir, err = os.MkdirTemp("", "sim-*")
	if err != nil {
		return fmt.Errorf("sim: tmpdir: %w", err)
	}

	// Create server Fossil repo.
	h.fossilRepo = filepath.Join(h.tmpDir, "server.fossil")
	create := exec.Command("fossil", "new", h.fossilRepo)
	if out, err := create.CombinedOutput(); err != nil {
		return fmt.Errorf("sim: fossil new: %s: %w", out, err)
	}

	// Start fossil server on a free port.
	port, err := freePort()
	if err != nil {
		return fmt.Errorf("sim: free port: %w", err)
	}
	h.fossilURL = fmt.Sprintf("http://127.0.0.1:%d", port)
	h.fossilCmd = exec.Command("fossil", "server",
		"--port", fmt.Sprintf("%d", port),
		"--localhost",
		h.fossilRepo,
	)
	if err := h.fossilCmd.Start(); err != nil {
		return fmt.Errorf("sim: fossil server start: %w", err)
	}
	if err := waitForTCP(fmt.Sprintf("127.0.0.1:%d", port), 5*time.Second); err != nil {
		return fmt.Errorf("sim: fossil server not ready: %w", err)
	}

	// Embedded NATS server.
	opts := &natsserver.Options{Port: -1}
	h.nats, err = natsserver.NewServer(opts)
	if err != nil {
		return fmt.Errorf("sim: nats new: %w", err)
	}
	h.nats.Start()
	if !h.nats.ReadyForConnections(5 * time.Second) {
		return fmt.Errorf("sim: nats not ready")
	}
	h.natsURL = h.nats.ClientURL()

	// Create leaf repos with deterministic RNG.
	rng := simio.NewSeededRand(h.Config.Seed)
	for i := range h.Config.NumLeaves {
		repoPath := filepath.Join(h.tmpDir, fmt.Sprintf("leaf-%d.fossil", i))
		r, err := repo.Create(repoPath, "simuser", rng)
		if err != nil {
			return fmt.Errorf("sim: repo create leaf-%d: %w", i, err)
		}
		// Set project-code so NATS subjects match the bridge.
		if _, err := r.DB().Exec("UPDATE config SET value='sim-project' WHERE name='project-code'"); err != nil {
			r.Close()
			return fmt.Errorf("sim: set project-code leaf-%d: %w", i, err)
		}
		r.Close()
		h.leafPaths = append(h.leafPaths, repoPath)
	}

	return nil
}

// StartAgents creates and starts the bridge and all leaf agents.
// Call this after seeding blobs into leaf repos.
func (h *Harness) StartAgents() error {
	var err error

	h.bridge, err = bridge.New(bridge.Config{
		NATSUrl:       h.natsURL,
		FossilURL:     h.fossilURL,
		ProjectCode:   "sim-project",
		SubjectPrefix: "fossil",
	})
	if err != nil {
		return fmt.Errorf("sim: bridge new: %w", err)
	}
	if err := h.bridge.Start(); err != nil {
		return fmt.Errorf("sim: bridge start: %w", err)
	}

	natsTarget := h.natsURL
	// TODO: add after proxy.go exists
	// if h.proxy != nil {
	// 	natsTarget = h.proxy.URL()
	// }
	for i, repoPath := range h.leafPaths {
		a, err := agent.New(agent.Config{
			RepoPath:      repoPath,
			NATSUrl:       natsTarget,
			User:          "simuser",
			Push:          true,
			Pull:          true,
			PollInterval:  2 * time.Second,
			SubjectPrefix: "fossil",
		})
		if err != nil {
			return fmt.Errorf("sim: agent new leaf-%d: %w", i, err)
		}
		if err := a.Start(); err != nil {
			return fmt.Errorf("sim: agent start leaf-%d: %w", i, err)
		}
		h.leaves = append(h.leaves, a)
	}

	return nil
}

// Teardown stops all agents, the bridge, NATS, and the fossil server,
// then removes the temp directory (unless KeepOnFailure is set).
func (h *Harness) Teardown() error {
	for i, a := range h.leaves {
		if err := a.Stop(); err != nil {
			fmt.Fprintf(os.Stderr, "sim: stop leaf-%d: %v\n", i, err)
		}
	}
	if h.bridge != nil {
		h.bridge.Stop()
	}
	// TODO: add after proxy.go exists
	// if h.proxy != nil {
	// 	h.proxy.Close()
	// }
	if h.nats != nil {
		h.nats.Shutdown()
	}
	if h.fossilCmd != nil && h.fossilCmd.Process != nil {
		h.fossilCmd.Process.Kill()
		h.fossilCmd.Wait()
	}
	if !h.Config.KeepOnFailure && h.tmpDir != "" {
		os.RemoveAll(h.tmpDir)
	}
	return nil
}

// FossilRepoPath returns the path to the server's Fossil repository.
func (h *Harness) FossilRepoPath() string { return h.fossilRepo }

// LeafPaths returns the paths to all leaf Fossil repositories.
func (h *Harness) LeafPaths() []string { return h.leafPaths }

// NATSUrl returns the embedded NATS server's client URL.
func (h *Harness) NATSUrl() string { return h.natsURL }

// FossilURL returns the Fossil HTTP server URL.
func (h *Harness) FossilURL() string { return h.fossilURL }

// freePort asks the OS for an available TCP port.
func freePort() (int, error) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return 0, err
	}
	port := l.Addr().(*net.TCPAddr).Port
	l.Close()
	return port, nil
}

// waitForTCP polls until a TCP connection succeeds or the timeout expires.
func waitForTCP(addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 500*time.Millisecond)
		if err == nil {
			conn.Close()
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("tcp %s not reachable after %s", addr, timeout)
}
