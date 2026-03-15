# Integration Simulation Testing Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an integration simulation layer (`sim/`) that tests EdgeSync's leaf-bridge-Fossil sync path using real NATS, a TCP fault proxy, and a real Fossil server, complementing the existing deterministic `dst/` package.

**Architecture:** TCP fault proxy sits between leaf NATS clients and an embedded NATS server. Bridge connects directly to NATS. Real `fossil server` process per run. Seed-driven fault schedules control partitions, latency, and restarts. Invariant checkers verify blob convergence and content integrity after quiescence.

**Tech Stack:** Go 1.23, `nats-server/v2` (embedded), `nats.go`, `modernc.org/sqlite`, `fossil` CLI

**Spec:** `docs/superpowers/specs/2026-03-15-deterministic-sim-testing-design.md`

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `sim/config.go` | `SimConfig` struct, `Level` type (Normal/Adversarial/Hostile), defaults, flag parsing |
| `sim/harness.go` | `Harness` struct: setup/teardown lifecycle, Fossil server process management, embedded NATS, agent/bridge creation |
| `sim/seed.go` | Blob seeding: insert random blobs into leaf repos with unclustered/unsent entries |
| `sim/invariants.go` | Shared invariant checkers: blob convergence, content integrity, no duplicates (reusable by `dst/` too) |
| `sim/proxy.go` | `FaultProxy`: TCP proxy with per-connection latency, drop, partition support |
| `sim/schedule.go` | `FaultSchedule`: seed-driven fault schedule generation and execution |
| `sim/sim_test.go` | `TestSimulation` entry point, flag-driven seed/severity/leaves, short mode |
| `sim/buggify.go` | `Buggify` struct (goroutine-safe), `NewBuggify()`, `Check()` |
| `sim/cmd/soak/main.go` | Soak runner binary: persistent state, failure archive, stats |
| `.github/workflows/sim.yml` | CI workflow: 16 seeds x 3 severity levels |

### Modified Files

| File | Change |
|------|--------|
| `go-libfossil/sync/session.go:21-28` | Add `*Buggify` field to `SyncOpts` |
| `leaf/agent/config.go:13-42` | Add `Buggify` field to `Config` |
| `leaf/agent/agent.go:212` | Thread `Buggify` to `SyncOpts` in `runSync()`, add BUGGIFY site |
| `bridge/bridge/config.go:10-19` | Add `Buggify` field to `Config` |
| `bridge/bridge/bridge.go:90` | Thread `Buggify` to `HandleRequest()` path |
| `go-libfossil/sync/client.go:103,171,261` | Add BUGGIFY sites at `buildFileCards`, `buildLoginCard`, `handleFileCard` |
| `go-libfossil/sync/client.go:303` | Remove existing `simio.Buggify()` call, replace with DI-based check |
| `leaf/agent/agent.go:125` | Remove existing `simio.Buggify()` call, replace with DI-based check |

---

## Chunk 1: Phase 1 — Harness Skeleton (Clean Sync)

### Task 1: SimConfig and Level type

**Files:**
- Create: `sim/config.go`

- [ ] **Step 1: Write the config file with SimConfig, Level, defaults, and flag registration**

```go
package sim

import (
	"flag"
	"time"
)

// Level controls fault injection severity.
type Level int

const (
	LevelNormal      Level = iota // No faults
	LevelAdversarial              // 5-15% fault rate
	LevelHostile                  // 20-30% fault rate
)

func (l Level) String() string {
	switch l {
	case LevelNormal:
		return "normal"
	case LevelAdversarial:
		return "adversarial"
	case LevelHostile:
		return "hostile"
	}
	return "unknown"
}

func ParseLevel(s string) Level {
	switch s {
	case "adversarial":
		return LevelAdversarial
	case "hostile":
		return LevelHostile
	default:
		return LevelNormal
	}
}

// SimConfig configures a simulation run.
type SimConfig struct {
	Seed           int64
	NumLeaves      int
	BlobsPerLeaf   int
	MaxBlobSize    int
	FaultDuration  time.Duration
	QuiesceTimeout time.Duration
	Severity       Level
	KeepOnFailure  bool
}

func (c *SimConfig) applyDefaults() {
	if c.NumLeaves == 0 {
		c.NumLeaves = 2
	}
	if c.BlobsPerLeaf == 0 {
		c.BlobsPerLeaf = 5
	}
	if c.MaxBlobSize == 0 {
		c.MaxBlobSize = 4096
	}
	if c.FaultDuration == 0 {
		c.FaultDuration = 20 * time.Second
	}
	if c.QuiesceTimeout == 0 {
		c.QuiesceTimeout = 60 * time.Second
	}
}

// Test flags — registered once.
var (
	flagSeed     = flag.Int64("sim.seed", 1, "simulation PRNG seed")
	flagSeeds    = flag.String("sim.seeds", "", "seed range e.g. 1-16")
	flagSeverity = flag.String("sim.severity", "normal", "normal|adversarial|hostile")
	flagLeaves   = flag.Int("sim.leaves", 2, "number of leaf agents")
)
```

- [ ] **Step 2: Verify it compiles**

Run: `go build ./sim/`
Expected: Success (no errors)

- [ ] **Step 3: Commit**

```bash
git add sim/config.go
git commit -m "sim: add SimConfig, Level type, and test flags"
```

---

### Task 2: Fossil server process helper

**Files:**
- Create: `sim/harness.go`

- [ ] **Step 1: Write Harness struct with Fossil server lifecycle**

Reference the existing pattern from `bridge/bridge/integration_test.go:22-65` which starts `fossil server --port=<N> <repoPath>` and polls for TCP readiness.

```go
package sim

import (
	"context"
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

// Harness orchestrates a simulation run: Fossil server, embedded NATS,
// bridge, leaf agents, and cleanup.
type Harness struct {
	Config SimConfig

	tmpDir     string
	fossilCmd  *exec.Cmd
	fossilURL  string
	fossilRepo string // path to fossil server's repo file
	nats       *natsserver.Server
	natsURL    string
	bridge     *bridge.Bridge
	leaves     []*agent.Agent
	leafPaths  []string // repo paths for reopening after stop
}

// New creates a Harness. Call Setup() to start all components.
func NewHarness(cfg SimConfig) *Harness {
	cfg.applyDefaults()
	return &Harness{Config: cfg}
}

// SetupInfra starts infrastructure: temp dir, fossil server, embedded NATS,
// and creates leaf repos. Call StartAgents() after seeding blobs.
func (h *Harness) SetupInfra() error {
	var err error

	// 1. Temp directory.
	h.tmpDir, err = os.MkdirTemp("", "sim-*")
	if err != nil {
		return fmt.Errorf("sim: tmpdir: %w", err)
	}

	// 2. Create fossil server repo.
	h.fossilRepo = filepath.Join(h.tmpDir, "server.fossil")
	create := exec.Command("fossil", "new", h.fossilRepo)
	if out, err := create.CombinedOutput(); err != nil {
		return fmt.Errorf("sim: fossil new: %s: %w", out, err)
	}

	// 3. Start fossil server on random port.
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

	// 4. Embedded NATS.
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

	// 5. Create leaf repos (but don't start agents yet — seed blobs first).
	rng := simio.NewSeededRand(h.Config.Seed)
	for i := range h.Config.NumLeaves {
		repoPath := filepath.Join(h.tmpDir, fmt.Sprintf("leaf-%d.fossil", i))
		r, err := repo.Create(repoPath, "simuser", rng)
		if err != nil {
			return fmt.Errorf("sim: repo create leaf-%d: %w", i, err)
		}

		// Set project-code to match the bridge's "sim-project" so NATS subjects align.
		r.DB().Exec("UPDATE config SET value='sim-project' WHERE name='project-code'")

		r.Close() // Close so SeedLeaf and agent.New() can open it later
		h.leafPaths = append(h.leafPaths, repoPath)
	}

	return nil
}

// StartAgents starts the bridge and all leaf agents. Call after seeding blobs
// into leaf repos via SeedLeaf().
func (h *Harness) StartAgents() error {
	var err error

	// Bridge.
	h.bridge, err = bridge.New(bridge.Config{
		NATSUrl:     h.natsURL,
		FossilURL:   h.fossilURL,
		ProjectCode: "sim-project",
	})
	if err != nil {
		return fmt.Errorf("sim: bridge new: %w", err)
	}
	if err := h.bridge.Start(); err != nil {
		return fmt.Errorf("sim: bridge start: %w", err)
	}

	// Leaf agents — connect through proxy if available, else direct to NATS.
	natsTarget := h.natsURL
	if h.proxy != nil {
		natsTarget = h.proxy.URL()
	}
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

// Teardown stops all components and cleans up.
func (h *Harness) Teardown() error {
	// Stop leaves.
	for i, a := range h.leaves {
		if err := a.Stop(); err != nil {
			fmt.Fprintf(os.Stderr, "sim: stop leaf-%d: %v\n", i, err)
		}
	}

	// Stop bridge.
	if h.bridge != nil {
		h.bridge.Stop()
	}

	// Stop NATS.
	if h.nats != nil {
		h.nats.Shutdown()
	}

	// Stop fossil server.
	if h.fossilCmd != nil && h.fossilCmd.Process != nil {
		h.fossilCmd.Process.Kill()
		h.fossilCmd.Wait()
	}

	// Clean up temp dir (keep on failure).
	if !h.Config.KeepOnFailure && h.tmpDir != "" {
		os.RemoveAll(h.tmpDir)
	}

	return nil
}

// FossilRepoPath returns the path to the fossil server's repo file.
func (h *Harness) FossilRepoPath() string {
	return h.fossilRepo
}

// LeafPaths returns the repo file paths for all leaves.
func (h *Harness) LeafPaths() []string {
	return h.leafPaths
}

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

// waitForTCP polls until a TCP connection succeeds or timeout.
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
```

- [ ] **Step 2: Verify it compiles**

Run: `go build ./sim/`
Expected: Success

- [ ] **Step 3: Commit**

```bash
git add sim/harness.go
git commit -m "sim: add Harness with Fossil server, NATS, bridge, and leaf lifecycle"
```

---

### Task 3: Blob seeding

**Files:**
- Create: `sim/seed.go`

- [ ] **Step 1: Write seed.go with blob seeding logic**

This follows the pattern from `dst/e2e_test.go:99-111` — `blob.Store()` + manual INSERT into `unclustered` and `unsent`.

```go
package sim

import (
	"fmt"
	"math/rand"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// SeedResult records what was seeded into a leaf repo.
type SeedResult struct {
	LeafIndex int
	UUIDs     []string
}

// SeedLeaf inserts random blobs into a leaf repo, marking them in
// unclustered and unsent so the sync protocol will push them.
func SeedLeaf(r *repo.Repo, rng *rand.Rand, count, maxSize int) ([]string, error) {
	var uuids []string

	err := r.WithTx(func(tx *db.Tx) error {
		for range count {
			size := rng.Intn(maxSize) + 1
			data := make([]byte, size)
			rng.Read(data)

			rid, uuid, err := blob.Store(tx, data)
			if err != nil {
				return fmt.Errorf("blob.Store: %w", err)
			}

			if _, err := tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
				return fmt.Errorf("insert unclustered: %w", err)
			}
			if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid); err != nil {
				return fmt.Errorf("insert unsent: %w", err)
			}

			uuids = append(uuids, uuid)
		}
		return nil
	})

	return uuids, err
}
```

- [ ] **Step 2: Verify it compiles**

Run: `go build ./sim/`
Expected: Success

- [ ] **Step 3: Commit**

```bash
git add sim/seed.go
git commit -m "sim: add SeedLeaf for inserting test blobs into leaf repos"
```

---

### Task 4: Invariant checkers (shared, reusable)

**Files:**
- Create: `sim/invariants.go`

- [ ] **Step 1: Write invariant checkers**

These resolve UUID→rid per-repo as documented in the spec (line 155), using `content.Expand(querier, rid)`.

```go
package sim

import (
	"bytes"
	"fmt"
	"sort"
	"strings"

	"github.com/dmestas/edgesync/go-libfossil/content"
	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// InvariantResult describes a single invariant check outcome.
type InvariantResult struct {
	Name    string
	Passed  bool
	Details string
}

// CheckBlobConvergence verifies all repos contain the same set of blob UUIDs.
func CheckBlobConvergence(repos []*repo.Repo, labels []string) InvariantResult {
	sets := make([]map[string]bool, len(repos))
	for i, r := range repos {
		sets[i] = make(map[string]bool)
		rows, err := r.DB().Query("SELECT uuid FROM blob WHERE size >= 0")
		if err != nil {
			return InvariantResult{Name: "blob_convergence", Details: fmt.Sprintf("%s: query: %v", labels[i], err)}
		}
		for rows.Next() {
			var uuid string
			rows.Scan(&uuid)
			sets[i][uuid] = true
		}
		rows.Close()
	}

	// Compare all sets to the first.
	ref := sets[0]
	var diffs []string
	for i := 1; i < len(sets); i++ {
		for uuid := range ref {
			if !sets[i][uuid] {
				diffs = append(diffs, fmt.Sprintf("%s missing %s", labels[i], uuid))
			}
		}
		for uuid := range sets[i] {
			if !ref[uuid] {
				diffs = append(diffs, fmt.Sprintf("%s has extra %s", labels[i], uuid))
			}
		}
	}

	if len(diffs) == 0 {
		return InvariantResult{Name: "blob_convergence", Passed: true,
			Details: fmt.Sprintf("all %d repos have %d blobs", len(repos), len(ref))}
	}
	sort.Strings(diffs)
	return InvariantResult{Name: "blob_convergence", Details: strings.Join(diffs, "\n")}
}

// CheckContentIntegrity verifies expanded content is byte-identical across repos.
func CheckContentIntegrity(repos []*repo.Repo, labels []string) InvariantResult {
	// Collect all UUIDs from first repo.
	rows, err := repos[0].DB().Query("SELECT uuid FROM blob WHERE size >= 0")
	if err != nil {
		return InvariantResult{Name: "content_integrity", Details: fmt.Sprintf("query: %v", err)}
	}
	var uuids []string
	for rows.Next() {
		var uuid string
		rows.Scan(&uuid)
		uuids = append(uuids, uuid)
	}
	rows.Close()

	var diffs []string
	for _, uuid := range uuids {
		// Resolve UUID→rid and expand in each repo.
		var refContent []byte
		for i, r := range repos {
			var rid libfossil.FslID
			err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", uuid).Scan(&rid)
			if err != nil {
				diffs = append(diffs, fmt.Sprintf("%s: %s not found", labels[i], uuid))
				continue
			}
			data, err := content.Expand(r.DB(), rid)
			if err != nil {
				diffs = append(diffs, fmt.Sprintf("%s: expand %s: %v", labels[i], uuid, err))
				continue
			}
			if i == 0 {
				refContent = data
			} else if !bytes.Equal(data, refContent) {
				diffs = append(diffs, fmt.Sprintf("%s: %s content differs (%d vs %d bytes)", labels[i], uuid, len(data), len(refContent)))
			}
		}
	}

	if len(diffs) == 0 {
		return InvariantResult{Name: "content_integrity", Passed: true,
			Details: fmt.Sprintf("all %d blobs match across %d repos", len(uuids), len(repos))}
	}
	sort.Strings(diffs)
	return InvariantResult{Name: "content_integrity", Details: strings.Join(diffs, "\n")}
}

// CheckNoDuplicates verifies no UUID appears more than once in any repo.
func CheckNoDuplicates(repos []*repo.Repo, labels []string) InvariantResult {
	var diffs []string
	for i, r := range repos {
		rows, err := r.DB().Query("SELECT uuid, COUNT(*) as cnt FROM blob GROUP BY uuid HAVING cnt > 1")
		if err != nil {
			diffs = append(diffs, fmt.Sprintf("%s: query: %v", labels[i], err))
			continue
		}
		for rows.Next() {
			var uuid string
			var cnt int
			rows.Scan(&uuid, &cnt)
			diffs = append(diffs, fmt.Sprintf("%s: %s appears %d times", labels[i], uuid, cnt))
		}
		rows.Close()
	}

	if len(diffs) == 0 {
		return InvariantResult{Name: "no_duplicates", Passed: true}
	}
	return InvariantResult{Name: "no_duplicates", Details: strings.Join(diffs, "\n")}
}
```

- [ ] **Step 2: Verify it compiles**

Run: `go build ./sim/`
Expected: Success

- [ ] **Step 3: Commit**

```bash
git add sim/invariants.go
git commit -m "sim: add shared invariant checkers (convergence, integrity, duplicates)"
```

---

### Task 5: Phase 1 integration test — clean sync with invariant checking

**Files:**
- Create: `sim/sim_test.go`

- [ ] **Step 1: Write the test that starts the full harness, seeds blobs, waits, checks invariants**

```go
package sim

import (
	"math/rand"
	"os/exec"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

func hasFossil() bool {
	_, err := exec.LookPath("fossil")
	return err == nil
}

func TestSimulationCleanSync(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping simulation in short mode")
	}

	cfg := SimConfig{
		Seed:           *flagSeed,
		NumLeaves:      *flagLeaves,
		BlobsPerLeaf:   3,
		MaxBlobSize:    1024,
		QuiesceTimeout: 30 * time.Second,
		Severity:       LevelNormal,
		KeepOnFailure:  true,
	}

	h := NewHarness(cfg)
	if err := h.SetupInfra(); err != nil {
		t.Fatalf("SetupInfra: %v", err)
	}
	defer func() {
		h.Config.KeepOnFailure = t.Failed()
		h.Teardown()
		if t.Failed() && h.tmpDir != "" {
			t.Logf("Temp dir preserved: %s", h.tmpDir)
		}
	}()

	// Seed blobs into each leaf repo BEFORE starting agents.
	rng := rand.New(rand.NewSource(cfg.Seed))
	var allSeeded []SeedResult
	for i, path := range h.LeafPaths() {
		r, err := repo.Open(path)
		if err != nil {
			t.Fatalf("open leaf-%d for seeding: %v", i, err)
		}
		uuids, err := SeedLeaf(r, rng, cfg.BlobsPerLeaf, cfg.MaxBlobSize)
		r.Close()
		if err != nil {
			t.Fatalf("seed leaf-%d: %v", i, err)
		}
		allSeeded = append(allSeeded, SeedResult{LeafIndex: i, UUIDs: uuids})
		t.Logf("Seeded %d blobs into leaf-%d", len(uuids), i)
	}

	// Start agents (leaves will push seeded blobs, pull from server).
	if err := h.StartAgents(); err != nil {
		t.Fatalf("StartAgents: %v", err)
	}

	// Wait for quiescence: sleep long enough for agents to complete sync cycles.
	// In Phase 1 (no faults), convergence should happen within a few poll intervals.
	time.Sleep(cfg.QuiesceTimeout)

	// Stop leaves and bridge before invariant checking.
	// Agent.Stop() closes the repo DB, so we reopen for read-only checks.
	for _, a := range h.leaves {
		a.Stop()
	}
	h.bridge.Stop()

	// Reopen repos for invariant checking.
	var repos []*repo.Repo
	var labels []string

	sr, err := repo.Open(h.FossilRepoPath())
	if err != nil {
		t.Fatalf("reopen server repo: %v", err)
	}
	defer sr.Close()
	repos = append(repos, sr)
	labels = append(labels, "server")

	for i, path := range h.LeafPaths() {
		r, err := repo.Open(path)
		if err != nil {
			t.Fatalf("reopen leaf-%d repo: %v", i, err)
		}
		defer r.Close()
		repos = append(repos, r)
		labels = append(labels, fmt.Sprintf("leaf-%d", i))
	}

	// Check invariants.
	results := []InvariantResult{
		CheckBlobConvergence(repos, labels),
		CheckContentIntegrity(repos, labels),
		CheckNoDuplicates(repos, labels),
	}

	for _, r := range results {
		if r.Passed {
			t.Logf("PASS: %s — %s", r.Name, r.Details)
		} else {
			t.Errorf("FAIL: %s\n%s", r.Name, r.Details)
		}
	}
}
```

- [ ] **Step 2: Run the test**

Run: `go test ./sim/ -run TestSimulationCleanSync -v -timeout=120s`
Expected: Leaves pull blobs from server, invariants pass. If fossil binary is missing, test is skipped.

- [ ] **Step 3: Debug and fix any issues**

Common issues: agent.New() may need project-code to match bridge's "sim-project". The fossil server's repo won't have a project-code set by `fossil new`. May need to INSERT project-code/server-code into the fossil server's config table, or use the existing sync protocol's auto-negotiation.

- [ ] **Step 4: Commit**

```bash
git add sim/sim_test.go
git commit -m "sim: add Phase 1 clean sync integration test with invariant checking"
```

---

---

## Chunk 2: Phase 2 — Fault Proxy

### Task 7: TCP fault proxy

**Files:**
- Create: `sim/proxy.go`

- [ ] **Step 1: Write the FaultProxy with per-connection forwarding**

```go
package sim

import (
	"io"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// FaultProxy is a TCP proxy between leaf NATS clients and the embedded NATS server.
// It can inject latency, drop connections, and partition specific leaves.
// Supports both global partitions ("*") and per-leaf partitions by label.
type FaultProxy struct {
	listener   net.Listener
	upstream   string // embedded NATS address
	mu         sync.Mutex
	latency    time.Duration          // global latency added to forwarding
	partitions map[string]bool        // "*" for global, or leaf labels
	leafByAddr map[string]string      // remote addr -> leaf label (set by RegisterLeaf)
	closed     atomic.Bool
	conns      map[net.Conn]struct{}  // active connections (for DropConnections)
}

// NewFaultProxy creates a proxy that forwards to the given upstream address.
func NewFaultProxy(upstream string) (*FaultProxy, error) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	p := &FaultProxy{
		listener:   l,
		upstream:   upstream,
		partitions: make(map[string]bool),
		leafByAddr: make(map[string]string),
		conns:      make(map[net.Conn]struct{}),
	}
	go p.acceptLoop()
	return p, nil
}

// Addr returns the proxy's listen address (for leaf agent config).
func (p *FaultProxy) Addr() string {
	return p.listener.Addr().String()
}

// URL returns the proxy's NATS-compatible URL.
func (p *FaultProxy) URL() string {
	return "nats://" + p.Addr()
}

// SetLatency sets the forwarding delay for all connections.
func (p *FaultProxy) SetLatency(d time.Duration) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.latency = d
}

// RegisterLeaf associates a remote address with a leaf label for per-leaf partitions.
// Call after each leaf connects (the harness can discover addrs from NATS connection info).
func (p *FaultProxy) RegisterLeaf(remoteAddr, label string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.leafByAddr[remoteAddr] = label
}

// Partition blocks traffic. Use "*" for global, or a leaf label for per-leaf.
func (p *FaultProxy) Partition(target string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.partitions[target] = true
}

// Heal removes a partition. Use "*" for global, or a leaf label.
func (p *FaultProxy) Heal(target string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	delete(p.partitions, target)
}

// HealAll removes all partitions.
func (p *FaultProxy) HealAll() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.partitions = make(map[string]bool)
}

// DropConnections closes all active proxy connections (simulates network blip).
func (p *FaultProxy) DropConnections() {
	p.mu.Lock()
	conns := p.conns
	p.conns = make(map[net.Conn]struct{})
	p.mu.Unlock()
	for c := range conns {
		c.Close()
	}
}

// Close shuts down the proxy listener and all connections.
func (p *FaultProxy) Close() error {
	p.closed.Store(true)
	p.DropConnections()
	return p.listener.Close()
}

func (p *FaultProxy) acceptLoop() {
	for {
		clientConn, err := p.listener.Accept()
		if err != nil {
			if p.closed.Load() {
				return
			}
			continue
		}
		go p.handleConn(clientConn)
	}
}

func (p *FaultProxy) handleConn(client net.Conn) {
	p.mu.Lock()
	p.conns[client] = struct{}{}
	p.mu.Unlock()

	defer func() {
		client.Close()
		p.mu.Lock()
		delete(p.conns, client)
		p.mu.Unlock()
	}()

	// Check global or per-leaf partition.
	p.mu.Lock()
	partitioned := p.partitions["*"]
	if !partitioned {
		if label, ok := p.leafByAddr[client.RemoteAddr().String()]; ok {
			partitioned = p.partitions[label]
		}
	}
	p.mu.Unlock()
	if partitioned {
		return // drop connection
	}

	upstream, err := net.Dial("tcp", p.upstream)
	if err != nil {
		return
	}
	defer upstream.Close()

	p.mu.Lock()
	p.conns[upstream] = struct{}{}
	p.mu.Unlock()

	// Bidirectional forwarding with latency injection.
	done := make(chan struct{}, 2)
	forward := func(dst, src net.Conn) {
		defer func() { done <- struct{}{} }()
		buf := make([]byte, 32*1024)
		for {
			n, err := src.Read(buf)
			if n > 0 {
				p.mu.Lock()
				lat := p.latency
				partitioned := p.partitions["*"]
				p.mu.Unlock()

				if partitioned {
					return
				}
				if lat > 0 {
					time.Sleep(lat)
				}
				if _, werr := dst.Write(buf[:n]); werr != nil {
					return
				}
			}
			if err != nil {
				return
			}
		}
	}
	go forward(upstream, client)
	go forward(client, upstream)
	<-done
}
```

- [ ] **Step 2: Write a basic proxy test**

Create `sim/proxy_test.go`:

```go
package sim

import (
	"net"
	"testing"
	"time"
)

func TestFaultProxyForwards(t *testing.T) {
	// Start a simple echo server.
	echoL, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer echoL.Close()
	go func() {
		for {
			c, err := echoL.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				buf := make([]byte, 1024)
				n, _ := c.Read(buf)
				c.Write(buf[:n])
			}()
		}
	}()

	proxy, err := NewFaultProxy(echoL.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer proxy.Close()

	// Connect through proxy.
	conn, err := net.Dial("tcp", proxy.Addr())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()

	conn.Write([]byte("hello"))
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 1024)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatal(err)
	}
	if string(buf[:n]) != "hello" {
		t.Fatalf("got %q, want %q", buf[:n], "hello")
	}
}

func TestFaultProxyPartition(t *testing.T) {
	echoL, _ := net.Listen("tcp", "127.0.0.1:0")
	defer echoL.Close()
	go func() {
		for {
			c, err := echoL.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				buf := make([]byte, 1024)
				n, _ := c.Read(buf)
				c.Write(buf[:n])
			}()
		}
	}()

	proxy, _ := NewFaultProxy(echoL.Addr().String())
	defer proxy.Close()

	proxy.Partition("*")

	conn, err := net.DialTimeout("tcp", proxy.Addr(), 1*time.Second)
	if err != nil {
		// Connection refused or timeout — partition is working.
		return
	}
	defer conn.Close()

	conn.Write([]byte("hello"))
	conn.SetReadDeadline(time.Now().Add(1 * time.Second))
	buf := make([]byte, 1024)
	_, err = conn.Read(buf)
	if err == nil {
		t.Fatal("expected read to fail during partition")
	}
}
```

- [ ] **Step 3: Run proxy tests**

Run: `go test ./sim/ -run TestFaultProxy -v`
Expected: Both tests pass.

- [ ] **Step 4: Commit**

```bash
git add sim/proxy.go sim/proxy_test.go
git commit -m "sim: add TCP FaultProxy with latency, partition, and connection drop"
```

---

### Task 8: Fault schedule generation and execution

**Files:**
- Create: `sim/schedule.go`

- [ ] **Step 1: Write the fault schedule generator and executor**

```go
package sim

import (
	"fmt"
	"math/rand"
	"sort"
	"strings"
	"time"
)

// FaultType identifies a kind of fault.
type FaultType int

const (
	FaultPartition     FaultType = iota
	FaultHealPartition
	FaultLatency
	FaultHealLatency
	FaultDropConns
	FaultBridgeRestart
	FaultLeafRestart
	FaultHealAll
)

func (f FaultType) String() string {
	switch f {
	case FaultPartition:
		return "partition"
	case FaultHealPartition:
		return "heal-partition"
	case FaultLatency:
		return "latency"
	case FaultHealLatency:
		return "heal-latency"
	case FaultDropConns:
		return "drop-connections"
	case FaultBridgeRestart:
		return "bridge-restart"
	case FaultLeafRestart:
		return "leaf-restart"
	case FaultHealAll:
		return "heal-all"
	}
	return "unknown"
}

// FaultEvent is a scheduled fault.
type FaultEvent struct {
	Time    time.Duration // offset from simulation start
	Type    FaultType
	Target  string        // e.g. "leaf-0", "bridge", "*"
	Param   time.Duration // e.g. latency amount
}

func (e FaultEvent) String() string {
	s := fmt.Sprintf("t=%s %s", e.Time, e.Type)
	if e.Target != "" {
		s += " " + e.Target
	}
	if e.Param > 0 {
		s += fmt.Sprintf(" (%s)", e.Param)
	}
	return s
}

// FaultSchedule is a time-ordered list of fault events.
type FaultSchedule struct {
	Events []FaultEvent
}

// String returns a human-readable schedule log.
func (s *FaultSchedule) String() string {
	var lines []string
	for _, e := range s.Events {
		lines = append(lines, e.String())
	}
	return strings.Join(lines, "\n")
}

// GenerateSchedule creates a fault schedule from a seed and severity level.
func GenerateSchedule(seed int64, severity Level, faultDuration time.Duration, numLeaves int) *FaultSchedule {
	if severity == LevelNormal {
		return &FaultSchedule{} // No faults
	}

	rng := rand.New(rand.NewSource(seed))
	var events []FaultEvent

	// Determine number of fault events based on severity.
	var numFaults int
	switch severity {
	case LevelAdversarial:
		numFaults = 2 + rng.Intn(4) // 2-5 faults
	case LevelHostile:
		numFaults = 5 + rng.Intn(6) // 5-10 faults
	}

	for range numFaults {
		// Random time offset within fault duration.
		offset := time.Duration(rng.Int63n(int64(faultDuration)))

		// Pick a fault type based on severity.
		var ft FaultType
		switch severity {
		case LevelAdversarial:
			switch rng.Intn(3) {
			case 0:
				ft = FaultPartition
			case 1:
				ft = FaultLatency
			case 2:
				ft = FaultDropConns
			}
		case LevelHostile:
			switch rng.Intn(5) {
			case 0:
				ft = FaultPartition
			case 1:
				ft = FaultLatency
			case 2:
				ft = FaultDropConns
			case 3:
				ft = FaultBridgeRestart
			case 4:
				ft = FaultLeafRestart
			}
		}

		target := fmt.Sprintf("leaf-%d", rng.Intn(numLeaves))
		if ft == FaultBridgeRestart {
			target = "bridge"
		}

		param := time.Duration(0)
		if ft == FaultLatency {
			param = time.Duration(100+rng.Intn(900)) * time.Millisecond
		}

		events = append(events, FaultEvent{
			Time:   offset,
			Type:   ft,
			Target: target,
			Param:  param,
		})

		// Auto-heal partitions and latency after a random duration.
		if ft == FaultPartition {
			healOffset := offset + time.Duration(1+rng.Intn(5))*time.Second
			if healOffset > faultDuration {
				healOffset = faultDuration
			}
			events = append(events, FaultEvent{
				Time:   healOffset,
				Type:   FaultHealPartition,
				Target: target,
			})
		}
		if ft == FaultLatency {
			healOffset := offset + time.Duration(1+rng.Intn(3))*time.Second
			if healOffset > faultDuration {
				healOffset = faultDuration
			}
			events = append(events, FaultEvent{
				Time:   healOffset,
				Type:   FaultHealLatency,
			})
		}
	}

	// Always heal all at the end of the fault duration.
	events = append(events, FaultEvent{
		Time: faultDuration,
		Type: FaultHealAll,
	})

	// Sort by time.
	sort.Slice(events, func(i, j int) bool {
		return events[i].Time < events[j].Time
	})

	return &FaultSchedule{Events: events}
}
```

- [ ] **Step 2: Write a test for schedule generation**

Add to `sim/proxy_test.go` (or create `sim/schedule_test.go`):

```go
func TestGenerateScheduleNormal(t *testing.T) {
	s := GenerateSchedule(42, LevelNormal, 20*time.Second, 2)
	if len(s.Events) != 0 {
		t.Fatalf("normal severity should have 0 events, got %d", len(s.Events))
	}
}

func TestGenerateScheduleAdversarial(t *testing.T) {
	s := GenerateSchedule(42, LevelAdversarial, 20*time.Second, 2)
	if len(s.Events) == 0 {
		t.Fatal("adversarial should have events")
	}
	// Last event should be heal-all.
	last := s.Events[len(s.Events)-1]
	if last.Type != FaultHealAll {
		t.Fatalf("last event should be heal-all, got %s", last.Type)
	}
	t.Log(s.String())
}

func TestGenerateScheduleDeterministic(t *testing.T) {
	s1 := GenerateSchedule(99, LevelHostile, 20*time.Second, 3)
	s2 := GenerateSchedule(99, LevelHostile, 20*time.Second, 3)
	if s1.String() != s2.String() {
		t.Fatal("same seed should produce same schedule")
	}
}
```

- [ ] **Step 3: Run tests**

Run: `go test ./sim/ -run TestGenerateSchedule -v`
Expected: All pass.

- [ ] **Step 4: Commit**

```bash
git add sim/schedule.go sim/schedule_test.go
git commit -m "sim: add seed-driven fault schedule generation"
```

---

### Task 9: Integrate fault proxy and schedule into harness

**Files:**
- Modify: `sim/harness.go`
- Modify: `sim/sim_test.go`

- [ ] **Step 1: Add proxy to Harness and per-leaf proxy support**

Add a `proxy` field to `Harness`. In `SetupInfra()`, create one `FaultProxy` per leaf. Update leaf agent creation to connect through the proxy instead of directly to NATS. For simplicity in this phase, use a single shared proxy (all leaves share one proxy endpoint). Per-leaf proxies can come later.

Add fields:
```go
proxy    *FaultProxy
schedule *FaultSchedule
```

In `SetupInfra()`, after NATS starts:
```go
// Extract NATS host:port from URL for proxy upstream.
natsHost := strings.TrimPrefix(h.natsURL, "nats://")
h.proxy, err = NewFaultProxy(natsHost)
```

In `StartAgents()`, point leaves at `h.proxy.URL()` instead of `h.natsURL`.

- [ ] **Step 2: Add ExecuteSchedule method to Harness**

```go
func (h *Harness) ExecuteSchedule(schedule *FaultSchedule, t *testing.T) {
    start := time.Now()
    for _, ev := range schedule.Events {
        delay := ev.Time - time.Since(start)
        if delay > 0 {
            time.Sleep(delay)
        }
        t.Logf("FAULT: %s", ev)
        switch ev.Type {
        case FaultPartition:
            h.proxy.Partition(ev.Target) // per-leaf: e.g. "leaf-0"
        case FaultHealPartition:
            h.proxy.Heal(ev.Target)
        case FaultLatency:
            h.proxy.SetLatency(ev.Param)
        case FaultHealLatency:
            h.proxy.SetLatency(0)
        case FaultDropConns:
            h.proxy.DropConnections()
        case FaultBridgeRestart:
            h.restartBridge(t)
        case FaultLeafRestart:
            h.restartLeaf(ev.Target, t)
        case FaultHealAll:
            h.proxy.HealAll()
            h.proxy.SetLatency(0)
        }
    }
}

// restartBridge stops the bridge and creates a new one with same config.
// Bridge.Stop() drains and closes the NATS connection permanently,
// so a new Bridge must be created via bridge.New().
func (h *Harness) restartBridge(t *testing.T) {
    t.Helper()
    if err := h.bridge.Stop(); err != nil {
        t.Logf("bridge stop: %v", err)
    }
    var err error
    h.bridge, err = bridge.New(bridge.Config{
        NATSUrl:     h.natsURL,
        FossilURL:   h.fossilURL,
        ProjectCode: "sim-project",
    })
    if err != nil {
        t.Logf("bridge restart new: %v", err)
        return
    }
    if err := h.bridge.Start(); err != nil {
        t.Logf("bridge restart start: %v", err)
    }
}

// restartLeaf stops a leaf agent and creates a new one with the same repo path.
// Agent.Stop() closes the repo, so agent.New() reopens it.
func (h *Harness) restartLeaf(target string, t *testing.T) {
    t.Helper()
    // Parse leaf index from target string (e.g. "leaf-0" -> 0).
    var idx int
    fmt.Sscanf(target, "leaf-%d", &idx)
    if idx < 0 || idx >= len(h.leaves) {
        t.Logf("restartLeaf: invalid target %s", target)
        return
    }
    if err := h.leaves[idx].Stop(); err != nil {
        t.Logf("leaf-%d stop: %v", idx, err)
    }
    natsTarget := h.natsURL
    if h.proxy != nil {
        natsTarget = h.proxy.URL()
    }
    a, err := agent.New(agent.Config{
        RepoPath:      h.leafPaths[idx],
        NATSUrl:       natsTarget,
        User:          "simuser",
        Push:          true,
        Pull:          true,
        PollInterval:  2 * time.Second,
        SubjectPrefix: "fossil",
    })
    if err != nil {
        t.Logf("leaf-%d restart new: %v", idx, err)
        return
    }
    if err := a.Start(); err != nil {
        t.Logf("leaf-%d restart start: %v", idx, err)
        return
    }
    h.leaves[idx] = a
}
```

- [ ] **Step 3: Add TestSimulationWithFaults test**

```go
func TestSimulationWithFaults(t *testing.T) {
    if !hasFossil() { t.Skip("fossil not found") }
    if testing.Short() { t.Skip("skipping simulation in short mode") }

    cfg := SimConfig{
        Seed:           *flagSeed,
        NumLeaves:      *flagLeaves,
        BlobsPerLeaf:   3,
        MaxBlobSize:    1024,
        FaultDuration:  10 * time.Second,
        QuiesceTimeout: 30 * time.Second,
        Severity:       ParseLevel(*flagSeverity),
    }

    h := NewHarness(cfg)
    if err := h.SetupInfra(); err != nil {
        t.Fatalf("SetupInfra: %v", err)
    }
    defer func() {
        h.Config.KeepOnFailure = t.Failed()
        h.Teardown()
    }()

    // Seed blobs into leaf repos before starting agents.
    rng := rand.New(rand.NewSource(cfg.Seed))
    for i, path := range h.LeafPaths() {
        r, err := repo.Open(path)
        if err != nil { t.Fatalf("open leaf-%d: %v", i, err) }
        _, err = SeedLeaf(r, rng, cfg.BlobsPerLeaf, cfg.MaxBlobSize)
        r.Close()
        if err != nil { t.Fatalf("seed leaf-%d: %v", i, err) }
    }

    // Start agents (through fault proxy).
    if err := h.StartAgents(); err != nil {
        t.Fatalf("StartAgents: %v", err)
    }

    // Generate and log fault schedule.
    schedule := GenerateSchedule(cfg.Seed, cfg.Severity, cfg.FaultDuration, cfg.NumLeaves)
    t.Logf("Fault schedule:\n%s", schedule)

    // Execute fault schedule (blocks until complete).
    h.ExecuteSchedule(schedule, t)

    // Quiesce — wait for convergence after faults healed.
    t.Log("Quiescing...")
    time.Sleep(cfg.QuiesceTimeout)

    // Stop and check invariants.
    for _, a := range h.leaves { a.Stop() }
    h.bridge.Stop()

    var repos []*repo.Repo
    var labels []string
    sr, _ := repo.Open(h.FossilRepoPath())
    defer sr.Close()
    repos = append(repos, sr)
    labels = append(labels, "server")
    for i, path := range h.LeafPaths() {
        r, _ := repo.Open(path)
        defer r.Close()
        repos = append(repos, r)
        labels = append(labels, fmt.Sprintf("leaf-%d", i))
    }

    report := &SimReport{
        Seed:       cfg.Seed,
        Severity:   cfg.Severity,
        NumLeaves:  cfg.NumLeaves,
        Schedule:   schedule,
        Invariants: []InvariantResult{
            CheckBlobConvergence(repos, labels),
            CheckContentIntegrity(repos, labels),
            CheckNoDuplicates(repos, labels),
        },
    }
    t.Log(report)
    if report.Failed() { t.Fail() }
}
```

- [ ] **Step 4: Run the test with adversarial severity**

Run: `go test ./sim/ -run TestSimulationWithFaults -sim.severity=adversarial -v -timeout=120s`
Expected: Faults fire, leaves recover after heal-all, invariants pass.

- [ ] **Step 5: Commit**

```bash
git add sim/harness.go sim/sim_test.go
git commit -m "sim: integrate fault proxy and schedule execution into harness"
```

---

## Chunk 3: Phase 3 — Rich Failure Reporting

### Task 10: Enhanced failure reporting

**Files:**
- Modify: `sim/invariants.go`
- Modify: `sim/sim_test.go`

- [ ] **Step 1: Add SimReport struct that bundles all failure context**

```go
// SimReport bundles all context needed to debug a simulation failure.
type SimReport struct {
	Seed          int64
	Severity      Level
	NumLeaves     int
	Schedule      *FaultSchedule
	Invariants    []InvariantResult
	SyncHistories map[string]string // per-leaf sync summaries (e.g. "leaf-0" -> "3 rounds, 5 sent, 2 received")
}

func (r *SimReport) Failed() bool {
	for _, inv := range r.Invariants {
		if !inv.Passed {
			return true
		}
	}
	return false
}

func (r *SimReport) String() string {
	var b strings.Builder
	fmt.Fprintf(&b, "Seed: %d\n", r.Seed)
	fmt.Fprintf(&b, "Severity: %s\n", r.Severity)
	fmt.Fprintf(&b, "Leaves: %d\n", r.NumLeaves)
	if r.Schedule != nil && len(r.Schedule.Events) > 0 {
		fmt.Fprintf(&b, "\nFault Schedule:\n%s\n", r.Schedule)
	}
	if len(r.SyncHistories) > 0 {
		fmt.Fprintf(&b, "\nSync Histories:\n")
		for label, hist := range r.SyncHistories {
			fmt.Fprintf(&b, "  %s: %s\n", label, hist)
		}
	}
	fmt.Fprintf(&b, "\nInvariants:\n")
	for _, inv := range r.Invariants {
		status := "PASS"
		if !inv.Passed {
			status = "FAIL"
		}
		fmt.Fprintf(&b, "  %s: %s\n", status, inv.Name)
		if inv.Details != "" {
			fmt.Fprintf(&b, "    %s\n", inv.Details)
		}
	}
	return b.String()
}
```

- [ ] **Step 2: Update sim_test.go to use SimReport for failure output**

When invariants fail, `t.Errorf` the full report:
```go
report := &SimReport{
    Seed:       cfg.Seed,
    Severity:   cfg.Severity,
    NumLeaves:  cfg.NumLeaves,
    Schedule:   schedule,
    Invariants: results,
}
if report.Failed() {
    t.Errorf("Simulation failed:\n%s", report)
}
```

- [ ] **Step 3: Run a test and verify failure output format**

Run: `go test ./sim/ -run TestSimulation -v -timeout=120s`
Expected: On failure, see seed, schedule, and invariant diffs in output.

- [ ] **Step 4: Commit**

```bash
git add sim/invariants.go sim/sim_test.go
git commit -m "sim: add SimReport for rich failure context on invariant violations"
```

---

## Chunk 4: Phase 4 — BUGGIFY

### Task 11: Buggify struct

**Files:**
- Create: `sim/buggify.go`

- [ ] **Step 1: Write the goroutine-safe Buggify struct**

Note: The existing `simio/buggify.go` has a global `Buggify()` function. The new struct-based approach in `sim/buggify.go` is designed for concurrent use in `sim/` where multiple agents run in goroutines. The existing global API in `simio/` continues to work for `dst/` (single-threaded).

```go
package sim

import (
	"math/rand"
	"sync"
)

// AllSites lists all BUGGIFY site names for enablement decisions.
var AllSites = []string{
	"sync.buildFileCards.skip",
	"sync.handleFileCard.reject",
	"sync.buildRequest.minBudget",
	"agent.runSync.earlyReturn",
	"bridge.handleMessage.emptyReply",
	"sync.buildLoginCard.badNonce",
}

// Buggify provides goroutine-safe, seed-controlled fault injection.
// Pass nil in production — Check() is nil-safe and returns false.
type Buggify struct {
	mu    sync.Mutex
	rng   *rand.Rand
	sites map[string]bool // active sites for this run
}

// NewBuggify creates a Buggify instance. 25% of sites are enabled per run.
func NewBuggify(seed int64) *Buggify {
	rng := rand.New(rand.NewSource(seed))
	sites := make(map[string]bool)
	for _, name := range AllSites {
		if rng.Float64() < 0.25 {
			sites[name] = true
		}
	}
	return &Buggify{
		rng:   rng,
		sites: sites,
	}
}

// Check returns true if the named site should fire. Nil-safe.
func (b *Buggify) Check(site string, probability float64) bool {
	if b == nil {
		return false
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if !b.sites[site] {
		return false
	}
	return b.rng.Float64() < probability
}

// ActiveSites returns the list of sites enabled for this run (for logging).
func (b *Buggify) ActiveSites() []string {
	if b == nil {
		return nil
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	var active []string
	for name, enabled := range b.sites {
		if enabled {
			active = append(active, name)
		}
	}
	return active
}
```

- [ ] **Step 2: Write buggify tests**

Create `sim/buggify_test.go`:

```go
package sim

import "testing"

func TestBuggifyNilSafe(t *testing.T) {
	var b *Buggify
	if b.Check("anything", 1.0) {
		t.Fatal("nil Buggify should always return false")
	}
}

func TestBuggifyDeterministic(t *testing.T) {
	b1 := NewBuggify(42)
	b2 := NewBuggify(42)

	for i := 0; i < 100; i++ {
		r1 := b1.Check("sync.buildFileCards.skip", 0.5)
		r2 := b2.Check("sync.buildFileCards.skip", 0.5)
		if r1 != r2 {
			t.Fatalf("iteration %d: same seed should produce same result", i)
		}
	}
}

func TestBuggifySiteEnablement(t *testing.T) {
	b := NewBuggify(42)
	active := b.ActiveSites()
	t.Logf("Active sites for seed 42: %v", active)
	// With 6 sites and 25% chance, expect ~1-2 active on average.
	// Not asserting exact count since it's seed-dependent.
}
```

- [ ] **Step 3: Run tests**

Run: `go test ./sim/ -run TestBuggify -v`
Expected: All pass.

- [ ] **Step 4: Commit**

```bash
git add sim/buggify.go sim/buggify_test.go
git commit -m "sim: add goroutine-safe Buggify struct with per-run site enablement"
```

---

### Task 12: Thread Buggify through SyncOpts

**Files:**
- Modify: `go-libfossil/sync/session.go:21-28`

- [ ] **Step 1: Read current SyncOpts**

Read `go-libfossil/sync/session.go` to see the current struct.

- [ ] **Step 2: Add Buggify field**

Add a `Buggify` interface field. To avoid `sim` importing `sync` and vice versa, define a minimal interface in the sync package:

```go
// BuggifyChecker is an optional fault injection interface.
// Pass nil in production.
type BuggifyChecker interface {
	Check(site string, probability float64) bool
}
```

Add to `SyncOpts`:
```go
type SyncOpts struct {
	Push, Pull              bool
	ProjectCode, ServerCode string
	User, Password          string
	MaxSend                 int
	Env                     *simio.Env
	Buggify                 BuggifyChecker // nil in production
}
```

- [ ] **Step 3: Verify existing tests still pass**

Run: `go test ./go-libfossil/sync/ -v`
Expected: All existing tests pass (Buggify is nil, no behavior change).

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/sync/session.go
git commit -m "sync: add BuggifyChecker interface and Buggify field to SyncOpts"
```

---

### Task 13: Migrate existing global Buggify calls and add new BUGGIFY sites

**Important context:** The codebase already has `simio.Buggify()` calls at `go-libfossil/sync/client.go:303` and `leaf/agent/agent.go:125`. These use a global mutex-protected RNG via `simio.EnableBuggify(seed)`. The DI-based `BuggifyChecker` approach replaces these globals. This task removes the old calls and adds new DI-based ones at the same (and additional) locations.

**Files:**
- Modify: `go-libfossil/sync/client.go:103,171,261`

- [ ] **Step 0: Remove existing global simio.Buggify() calls**

In `go-libfossil/sync/client.go:303`, find the existing `simio.Buggify(0.03)` call and remove it.
In `leaf/agent/agent.go:125`, find the existing `simio.Buggify(0.05)` call and remove it.
These will be replaced by DI-based checks below and in Task 14.

Verify: `go test ./go-libfossil/sync/ ./leaf/agent/ -v` — existing tests should still pass since `simio.Buggify()` returns false when disabled (default).

- [ ] **Step 1: Add BUGGIFY site in buildFileCards (line ~103)**

Inside `buildFileCards()`, after the loop that builds file cards, add:

```go
// BUGGIFY: skip a file card to test multi-round convergence.
if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.buildFileCards.skip", 0.05) && len(cards) > 0 {
	cards = cards[:len(cards)-1]
}
```

- [ ] **Step 2: Add BUGGIFY site in handleFileCard (line ~261)**

At the top of `handleFileCard()`:

```go
if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.handleFileCard.reject", 0.03) {
	return fmt.Errorf("buggify: rejected file %s", uuid)
}
```

- [ ] **Step 3: Add BUGGIFY site in buildRequest (line ~15)**

Near the top of `buildRequest()`. Note: `s.maxSend` is a session-level field set once in `newSession()` (session.go:52), not a local variable. This override forces a tiny byte budget for the remainder of the session, causing more sync rounds:

```go
if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.buildRequest.minBudget", 0.10) {
	s.maxSend = 1024 // minimum budget to force more rounds
}
```

- [ ] **Step 4: Add BUGGIFY site in buildLoginCard (line ~171)**

Inside `buildLoginCard()`:

```go
if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.buildLoginCard.badNonce", 0.02) {
	// Corrupt nonce to test auth failure retry.
	payload = append(payload, []byte("BUGGIFY")...)
}
```

- [ ] **Step 5: Verify existing tests still pass**

Run: `go test ./go-libfossil/sync/ -v`
Expected: All pass (Buggify is nil in all existing tests).

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/sync/client.go
git commit -m "sync: add BUGGIFY sites in buildFileCards, handleFileCard, buildRequest, buildLoginCard"
```

---

### Task 14: Thread Buggify through agent and bridge configs

**Files:**
- Modify: `leaf/agent/config.go:13-42`
- Modify: `leaf/agent/agent.go:212`
- Modify: `bridge/bridge/config.go:10-19`
- Modify: `bridge/bridge/bridge.go:90`

- [ ] **Step 0: Add compile-time interface assertion to sim/buggify.go**

```go
import libsync "github.com/dmestas/edgesync/go-libfossil/sync"

var _ libsync.BuggifyChecker = (*Buggify)(nil)
```

- [ ] **Step 1: Add Buggify to agent.Config**

In `leaf/agent/config.go` (line ~41, before closing brace of Config struct):

```go
Buggify sync.BuggifyChecker // nil in production
```

Thread it through to `SyncOpts` in `runSync()` (line ~212):

```go
opts := sync.SyncOpts{
	// ... existing fields ...
	Buggify: a.config.Buggify,
}
```

Add BUGGIFY site in `runSync()`:

```go
if a.config.Buggify != nil && a.config.Buggify.Check("agent.runSync.earlyReturn", 0.05) {
	return &sync.SyncResult{Rounds: 1}, nil
}
```

- [ ] **Step 2: Add Buggify to bridge.Config and handleMessage**

In `bridge/bridge/config.go`, add:
```go
Buggify libsync.BuggifyChecker // nil in production
```

In `bridge/bridge/bridge.go` `handleMessage()` (line ~90), add at the top. Use the existing `respondEmpty` pattern (encodes an empty `xfer.Message{}`) rather than raw nil bytes:
```go
if b.config.Buggify != nil && b.config.Buggify.Check("bridge.handleMessage.emptyReply", 0.03) {
	emptyMsg := &xfer.Message{}
	data, _ := emptyMsg.Encode()
	msg.Respond(data)
	return
}
```

- [ ] **Step 3: Verify all tests pass**

Run: `go test ./leaf/agent/ ./bridge/bridge/ -v`
Expected: All pass (Buggify is nil).

- [ ] **Step 4: Commit**

```bash
git add leaf/agent/agent.go bridge/bridge/config.go bridge/bridge/bridge.go
git commit -m "agent,bridge: thread Buggify through configs and add BUGGIFY sites"
```

---

### Task 15: Wire Buggify into simulation harness

**Files:**
- Modify: `sim/harness.go`
- Modify: `sim/sim_test.go`

- [ ] **Step 1: Create Buggify in Harness and pass to agent/bridge configs**

In `Harness.StartAgents()`, create a `*Buggify` and pass it:

```go
h.buggify = NewBuggify(h.Config.Seed + 1000) // offset seed to avoid correlation with fault schedule

// When creating agents:
agent.Config{
	// ... existing fields ...
	Buggify: h.buggify,
}

// When creating bridge:
bridge.Config{
	// ... existing fields ...
	Buggify: h.buggify,
}
```

- [ ] **Step 2: Log active BUGGIFY sites in test output**

```go
t.Logf("BUGGIFY active sites: %v", h.buggify.ActiveSites())
```

- [ ] **Step 3: Run simulation with BUGGIFY**

Run: `go test ./sim/ -run TestSimulation -v -timeout=120s`
Expected: Active sites logged, simulation still passes (BUGGIFY sites are probabilistic).

- [ ] **Step 4: Commit**

```bash
git add sim/harness.go sim/sim_test.go
git commit -m "sim: wire Buggify into harness, pass to agent and bridge configs"
```

---

## Chunk 5: Phase 5 — CI Integration

### Task 16: GitHub Actions workflow

**Files:**
- Create: `.github/workflows/sim.yml`

- [ ] **Step 1: Write the CI workflow**

```yaml
name: Simulation Tests

on: [push, pull_request]

jobs:
  sim:
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        seed: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
        severity: [normal, adversarial, hostile]
    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'

      - name: Install Fossil
        run: sudo apt-get update && sudo apt-get install -y fossil

      - name: Run simulation
        run: |
          go test ./sim/ -run TestSimulation \
            -sim.seed=${{ matrix.seed }} \
            -sim.severity=${{ matrix.severity }} \
            -sim.leaves=3 \
            -timeout=120s \
            -v
```

- [ ] **Step 2: Commit**

```bash
git add .github/workflows/sim.yml
git commit -m "ci: add simulation test workflow (16 seeds x 3 severity levels)"
```

---

## Chunk 6: Phase 6 — Soak Runner

### Task 17: Soak runner binary

**Files:**
- Create: `sim/cmd/soak/main.go`

- [ ] **Step 1: Write the soak runner**

```go
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"time"

	"github.com/dmestas/edgesync/sim"
)

func main() {
	startSeed := flag.Int64("start-seed", 1, "starting seed")
	dataDir := flag.String("data-dir", ".", "directory for state and failures")
	flag.Parse()

	stateFile := filepath.Join(*dataDir, "last-seed.txt")
	failDir := filepath.Join(*dataDir, "failures")
	os.MkdirAll(failDir, 0o755)

	// Resume from last completed seed.
	seed := *startSeed
	if data, err := os.ReadFile(stateFile); err == nil {
		fmt.Sscanf(string(data), "%d", &seed)
		seed++ // start from next
	}

	var totalRuns, totalFails int
	batchStart := time.Now()

	for {
		numLeaves := int(seed%4) + 2 // 2-5 leaves

		for _, severity := range []sim.Level{sim.LevelNormal, sim.LevelAdversarial, sim.LevelHostile} {
			cfg := sim.SimConfig{
				Seed:           seed,
				NumLeaves:      numLeaves,
				BlobsPerLeaf:   5,
				MaxBlobSize:    4096,
				FaultDuration:  20 * time.Second,
				QuiesceTimeout: 60 * time.Second,
				Severity:       severity,
				KeepOnFailure:  true,
			}

			log.Printf("seed=%d severity=%s leaves=%d", seed, severity, numLeaves)

			report, err := sim.RunSimulation(cfg)
			totalRuns++

			if err != nil || (report != nil && report.Failed()) {
				totalFails++
				dir := filepath.Join(failDir, fmt.Sprintf("%d-%s", seed, severity))
				os.MkdirAll(dir, 0o755)

				var content string
				if err != nil {
					content = fmt.Sprintf("Error: %v\n", err)
				}
				if report != nil {
					content += report.String()
				}
				os.WriteFile(filepath.Join(dir, "report.txt"), []byte(content), 0o644)
				log.Printf("FAIL seed=%d severity=%s — saved to %s", seed, severity, dir)
			}
		}

		// Save checkpoint.
		os.WriteFile(stateFile, []byte(fmt.Sprintf("%d", seed)), 0o644)

		// Stats every 100 seeds.
		if seed%100 == 0 {
			elapsed := time.Since(batchStart)
			rate := float64(totalRuns) / elapsed.Hours()
			log.Printf("STATS: %d runs, %d failures (%.1f%%), %.0f runs/hour",
				totalRuns, totalFails, 100*float64(totalFails)/float64(totalRuns), rate)
		}

		seed++
	}
}
```

- [ ] **Step 2: Extract RunSimulation helper from test code**

The soak runner needs to call the simulation as a library function, not via `go test`. Add to `sim/harness.go`:

```go
// RunSimulation executes a full simulation run and returns the report.
func RunSimulation(cfg SimConfig) (*SimReport, error) {
	h := NewHarness(cfg)
	if err := h.SetupInfra(); err != nil {
		return nil, fmt.Errorf("setup infra: %w", err)
	}
	defer h.Teardown()

	// Seed blobs...
	// Start agents...
	// Generate and execute schedule...
	// Quiesce...
	// Check invariants...
	// Return report.
}
```

Refactor the test to call `RunSimulation()` too, so both paths share the same logic.

- [ ] **Step 3: Verify soak runner compiles**

Run: `go build ./sim/cmd/soak`
Expected: Binary builds successfully.

- [ ] **Step 4: Commit**

```bash
git add sim/cmd/soak/main.go sim/harness.go
git commit -m "sim: add soak runner binary with persistent state and failure archive"
```

---

### Task 18: Final cleanup and README

**Files:**
- Modify: `sim/sim_test.go`

- [ ] **Step 1: Unify TestSimulation as the single entry point**

Consolidate `TestSimulationCleanSync` and `TestSimulationWithFaults` into a single `TestSimulation` that uses flags:

```go
// parseSeeds parses "-sim.seeds=1-16" into a slice of seeds.
// Falls back to single *flagSeed if -sim.seeds is empty.
func parseSeeds() []int64 {
	if *flagSeeds != "" {
		var lo, hi int64
		if _, err := fmt.Sscanf(*flagSeeds, "%d-%d", &lo, &hi); err == nil && hi >= lo {
			seeds := make([]int64, 0, hi-lo+1)
			for s := lo; s <= hi; s++ {
				seeds = append(seeds, s)
			}
			return seeds
		}
	}
	return []int64{*flagSeed}
}

func TestSimulation(t *testing.T) {
	if !hasFossil() { t.Skip("fossil not found") }

	seeds := parseSeeds()
	for _, seed := range seeds {
		seed := seed
		t.Run(fmt.Sprintf("seed=%d", seed), func(t *testing.T) {
			t.Parallel()

			cfg := SimConfig{
				Seed:           seed,
				NumLeaves:      *flagLeaves,
				BlobsPerLeaf:   5,
				MaxBlobSize:    4096,
				Severity:       ParseLevel(*flagSeverity),
				FaultDuration:  20 * time.Second,
				QuiesceTimeout: 60 * time.Second,
			}
			if testing.Short() {
				cfg.FaultDuration = 5 * time.Second
				cfg.QuiesceTimeout = 15 * time.Second
				cfg.BlobsPerLeaf = 2
			}

			report, err := RunSimulation(cfg)
			if err != nil {
				t.Fatalf("RunSimulation: %v", err)
			}
			t.Log(report)
			if report.Failed() {
				t.Fail()
			}
		})
	}
}
```

- [ ] **Step 2: Run the full test suite**

Run: `go test ./sim/ -run TestSimulation -v -timeout=120s`
Run: `go test ./sim/ -run TestSimulation -sim.severity=adversarial -v -timeout=120s`
Run: `go test ./... -v` (verify nothing is broken)
Expected: All pass.

- [ ] **Step 3: Commit**

```bash
git add sim/sim_test.go
git commit -m "sim: unify TestSimulation entry point with flag-driven configuration"
```
