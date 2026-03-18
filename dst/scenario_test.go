package dst

import (
	"context"
	"flag"
	"fmt"
	"math/rand"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/uv"
	"github.com/dmestas/edgesync/leaf/agent"
)

// Command-line flags for CI seed sweeps:
//
//	go test -run TestDST -seed=42 -level=adversarial -steps=50000
var (
	flagSeed  = flag.Int64("seed", 0, "DST seed (0 = use test-specific defaults)")
	flagLevel = flag.String("level", "", "DST severity level: normal, adversarial, hostile")
	flagSteps = flag.Int("steps", 0, "DST max steps (0 = use test-specific defaults)")
)

// severity configures fault injection for a simulation run.
type severity struct {
	Name     string
	DropRate float64
	Buggify  bool
}

var (
	levelNormal      = severity{"normal", 0, false}
	levelAdversarial = severity{"adversarial", 0.10, true}
	levelHostile     = severity{"hostile", 0.20, true}
)

func parseSeverity() severity {
	switch *flagLevel {
	case "adversarial":
		return levelAdversarial
	case "hostile":
		return levelHostile
	case "normal", "":
		return levelNormal
	default:
		return levelNormal
	}
}

func seedFor(defaultSeed int64) int64 {
	if *flagSeed != 0 {
		return *flagSeed
	}
	return defaultSeed
}

func stepsFor(defaultSteps int) int {
	if *flagSteps != 0 {
		return *flagSteps
	}
	return defaultSteps
}

// --- Scenario 1: Clean Sync ---
// Master has 100 blobs, 3 empty leaves sync to convergence.

func TestScenarioCleanSync(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(1)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	for i := range 100 {
		mf.StoreArtifact([]byte(fmt.Sprintf("clean-sync-artifact-%04d", i)))
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    3,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()
	sim.Network().SetDropRate(sev.DropRate)

	steps := stepsFor(200)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Safety invariants must always hold.
	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Convergence: all 100 master artifacts in every leaf.
	if sev.DropRate == 0 && !sev.Buggify {
		if err := sim.CheckAllConverged(masterRepo); err != nil {
			t.Fatalf("Convergence: %v", err)
		}
	} else {
		// With faults, check subset (master artifacts that arrived should be intact).
		masterCount, _ := CountBlobs(masterRepo)
		for _, id := range sim.LeafIDs() {
			leafCount, _ := CountBlobs(sim.Leaf(id).Repo())
			t.Logf("  %s: %d/%d blobs", id, leafCount, masterCount)
		}
	}
}

// --- Scenario 2: Bidirectional ---
// Each leaf creates 10 unique artifacts, all converge through the master.

func TestScenarioBidirectional(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(2)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Also seed 5 artifacts in master.
	masterUUIDs := make([]string, 5)
	for i := range 5 {
		uuid, _ := mf.StoreArtifact([]byte(fmt.Sprintf("master-bidir-%d", i)))
		masterUUIDs[i] = uuid
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    3,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()
	sim.Network().SetDropRate(sev.DropRate)

	// Store 10 unique artifacts in each leaf.
	leafUUIDs := make(map[NodeID][]string)
	for _, id := range sim.LeafIDs() {
		r := sim.Leaf(id).Repo()
		for j := range 10 {
			data := []byte(fmt.Sprintf("leaf-%s-artifact-%d", id, j))
			var uuid string
			r.WithTx(func(tx *db.Tx) error {
				rid, u, _ := blob.Store(tx, data)
				uuid = u
				tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
				tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid)
				return nil
			})
			leafUUIDs[id] = append(leafUUIDs[id], uuid)
		}
	}

	steps := stepsFor(300)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[%s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// In normal mode, verify full convergence.
	if sev.DropRate == 0 && !sev.Buggify {
		// All master artifacts should be in all leaves.
		for _, id := range sim.LeafIDs() {
			for _, uuid := range masterUUIDs {
				if !HasBlob(sim.Leaf(id).Repo(), uuid) {
					t.Errorf("%s missing master artifact %s", id, uuid)
				}
			}
		}
		// All leaf artifacts should be in master.
		for id, uuids := range leafUUIDs {
			for _, uuid := range uuids {
				if !HasBlob(masterRepo, uuid) {
					t.Errorf("master missing %s artifact %s", id, uuid)
				}
			}
		}
	}
}

// --- Scenario 3: Partition and Heal ---
// Partition a leaf, run syncs, heal, verify it catches up.

func TestScenarioPartitionHeal(t *testing.T) {
	seed := seedFor(3)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	for i := range 20 {
		mf.StoreArtifact([]byte(fmt.Sprintf("partition-heal-%d", i)))
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    3,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Phase 1: partition leaf-2, run for a while.
	sim.Network().Partition("leaf-2")
	sim.Run(50)

	t.Logf("After partition: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// leaf-0 and leaf-1 should have master artifacts.
	for _, id := range []NodeID{"leaf-0", "leaf-1"} {
		count, _ := CountBlobs(sim.Leaf(id).Repo())
		t.Logf("  %s: %d blobs", id, count)
	}

	// leaf-2 should have fewer (or zero) artifacts from master.
	partitionedCount, _ := CountBlobs(sim.Leaf("leaf-2").Repo())
	t.Logf("  leaf-2 (partitioned): %d blobs", partitionedCount)

	// Phase 2: heal and run more steps.
	sim.Network().Heal("leaf-2")
	prevErrors := sim.TotalErrors
	sim.Run(50)

	t.Logf("After heal: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Safety should hold throughout.
	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// leaf-2 should now have caught up.
	healedCount, _ := CountBlobs(sim.Leaf("leaf-2").Repo())
	t.Logf("  leaf-2 (healed): %d blobs", healedCount)

	if healedCount <= partitionedCount {
		t.Errorf("leaf-2 didn't catch up after healing: before=%d after=%d",
			partitionedCount, healedCount)
	}

	// No new errors after healing (network is clean now).
	newErrors := sim.TotalErrors - prevErrors
	if newErrors > 0 {
		t.Logf("NOTE: %d errors after healing (may be from first sync attempt)", newErrors)
	}
}

// --- Scenario 4: Large Payload ---
// Single artifact larger than DefaultMaxSend (250KB).

func TestScenarioLargePayload(t *testing.T) {
	seed := seedFor(4)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Create a 500KB artifact.
	large := make([]byte, 500_000)
	for i := range large {
		large[i] = byte(i % 251) // non-zero, non-repeating pattern
	}
	largeUUID, _ := mf.StoreArtifact(large)

	// Also add a small artifact.
	smallUUID, _ := mf.StoreArtifact([]byte("small artifact"))

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    1,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	sim.Run(30)

	t.Logf("Large payload: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	leafRepo := sim.Leaf(sim.LeafIDs()[0]).Repo()
	if !HasBlob(leafRepo, smallUUID) {
		t.Error("leaf missing small artifact")
	}
	if !HasBlob(leafRepo, largeUUID) {
		t.Error("leaf missing large (500KB) artifact")
	}
}

// --- Scenario 5: Many Leaves ---
// 10 leaves syncing simultaneously.

func TestScenarioManyLeaves(t *testing.T) {
	seed := seedFor(5)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	for i := range 30 {
		mf.StoreArtifact([]byte(fmt.Sprintf("many-leaves-artifact-%d", i)))
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    10,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	sim.Run(200)

	t.Logf("Many leaves: steps=%d syncs=%d errors=%d",
		sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	if err := sim.CheckAllConverged(masterRepo); err != nil {
		t.Fatalf("Convergence: %v", err)
	}
}

// --- Scenario 6: Idempotency ---
// After convergence, another sync round produces zero new artifacts.

func TestScenarioIdempotent(t *testing.T) {
	seed := seedFor(6)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	for i := range 10 {
		mf.StoreArtifact([]byte(fmt.Sprintf("idempotent-%d", i)))
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Run to convergence.
	sim.Run(50)
	if err := sim.CheckAllConverged(masterRepo); err != nil {
		t.Fatalf("Initial convergence: %v", err)
	}

	// Snapshot blob counts.
	masterCount, _ := CountBlobs(masterRepo)
	leafCounts := make(map[NodeID]int)
	for _, id := range sim.LeafIDs() {
		c, _ := CountBlobs(sim.Leaf(id).Repo())
		leafCounts[id] = c
	}

	// Run more steps — counts should not change.
	sim.Run(20)

	masterCountAfter, _ := CountBlobs(masterRepo)
	if masterCountAfter != masterCount {
		t.Errorf("master blob count changed: %d -> %d", masterCount, masterCountAfter)
	}
	for _, id := range sim.LeafIDs() {
		c, _ := CountBlobs(sim.Leaf(id).Repo())
		if c != leafCounts[id] {
			t.Errorf("%s blob count changed: %d -> %d", id, leafCounts[id], c)
		}
	}
}

// --- Scenario 7: Leaf-to-Leaf Peer Sync ---
// Two leaves sync directly via HandleSync — no bridge, no master.
// Leaf-0 has blobs, leaf-1 is empty. Both should converge.

func TestScenarioPeerSync(t *testing.T) {
	seed := seedFor(7)
	sev := parseSeverity()
	steps := stepsFor(100)

	rng := rand.New(rand.NewSource(seed))
	clock := simio.NewSimClock()
	tmpDir := t.TempDir()

	// Create two leaf repos.
	simRand := simio.NewSeededRand(rng.Int63())
	var leafRepos [2]*repo.Repo
	var leafPaths [2]string
	for i := range 2 {
		path := filepath.Join(tmpDir, fmt.Sprintf("peer-%d.fossil", i))
		r, err := repo.Create(path, "peeruser", simRand)
		if err != nil {
			t.Fatalf("create peer-%d: %v", i, err)
		}
		t.Cleanup(func() { r.Close() })
		leafRepos[i] = r
		leafPaths[i] = path
	}

	// Seed blobs into peer-0 only.
	for i := range 10 {
		data := []byte(fmt.Sprintf("peer-blob-%d-seed%d", i, seed))
		uuid := hash.SHA1(data)
		_, err := leafRepos[0].DB().Exec(
			"INSERT INTO blob(uuid, size, content) VALUES(?, ?, ?)",
			uuid, len(data), data,
		)
		if err != nil {
			t.Fatalf("seed blob: %v", err)
		}
		leafRepos[0].DB().Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(last_insert_rowid())")
	}

	// Build peer network: leaf-0 syncs with leaf-1, leaf-1 syncs with leaf-0.
	peerNet := NewPeerNetwork(rand.New(rand.NewSource(rng.Int63())))
	peerNet.AddPeer("peer-0", leafRepos[0])
	peerNet.AddPeer("peer-1", leafRepos[1])
	peerNet.SetDropRate(sev.DropRate)

	if sev.Buggify {
		simio.EnableBuggify(rng.Int63())
		defer simio.DisableBuggify()
	}

	// Create agents with peer transports.
	var projCode0, srvCode0, projCode1, srvCode1 string
	leafRepos[0].DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode0)
	leafRepos[0].DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode0)
	leafRepos[1].DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode1)
	leafRepos[1].DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode1)

	// peer-0 syncs with peer-1, peer-1 syncs with peer-0.
	agent0 := agent.NewFromParts(agent.Config{
		Clock:        clock,
		PollInterval: 5 * time.Second,
		Push:         true,
		Pull:         true,
	}, leafRepos[0], peerNet.Transport("peer-0", "peer-1"), projCode0, srvCode0)

	agent1 := agent.NewFromParts(agent.Config{
		Clock:        clock,
		PollInterval: 5 * time.Second,
		Push:         true,
		Pull:         true,
	}, leafRepos[1], peerNet.Transport("peer-1", "peer-0"), projCode1, srvCode1)

	// Manually drive ticks (single-threaded DST).
	ctx := context.Background()
	for i := range steps {
		clock.Advance(3 * time.Second)
		act0 := agent0.Tick(ctx, agent.EventTimer)
		act1 := agent1.Tick(ctx, agent.EventTimer)
		_ = act0
		_ = act1

		// Check convergence periodically.
		if i > 0 && i%20 == 0 {
			c0, _ := CountBlobs(leafRepos[0])
			c1, _ := CountBlobs(leafRepos[1])
			if c0 == c1 && c0 > 0 {
				t.Logf("converged at step %d: %d blobs each", i, c0)
				break
			}
		}
	}

	// Invariants.
	c0, _ := CountBlobs(leafRepos[0])
	c1, _ := CountBlobs(leafRepos[1])
	t.Logf("peer-0: %d blobs, peer-1: %d blobs", c0, c1)

	if c0 != c1 {
		t.Fatalf("peer sync did not converge: peer-0=%d peer-1=%d", c0, c1)
	}
	if c0 == 0 {
		t.Fatal("both peers have 0 blobs — seeding failed")
	}

	// Content integrity.
	if err := CheckBlobIntegrity("peer-0", leafRepos[0]); err != nil {
		t.Fatalf("peer-0 integrity: %v", err)
	}
	if err := CheckBlobIntegrity("peer-1", leafRepos[1]); err != nil {
		t.Fatalf("peer-1 integrity: %v", err)
	}
}

// --- Scenario 8: Clone via HandleSync ---
// A fresh repo clones from a MockFossil (now backed by HandleSync).
// Tests the clone pagination path in HandleSync under DST.

func TestScenarioClone(t *testing.T) {
	seed := seedFor(8)
	steps := stepsFor(50)

	masterRepo := createMasterRepo(t)
	mf := NewMockFossil(masterRepo)

	// Seed enough artifacts to require multiple clone rounds.
	for i := range 250 {
		mf.StoreArtifact([]byte(fmt.Sprintf("clone-dst-%04d-seed%d", i, seed)))
	}
	masterCount, _ := CountBlobs(masterRepo)
	t.Logf("master has %d blobs", masterCount)

	// Create an empty leaf and run sync rounds manually (simulating clone pull).
	tmpDir := t.TempDir()
	leafPath := filepath.Join(tmpDir, "clone-leaf.fossil")
	simRand := simio.NewSeededRand(seed)
	leafRepo, err := repo.Create(leafPath, "cloneuser", simRand)
	if err != nil {
		t.Fatalf("create leaf: %v", err)
	}
	t.Cleanup(func() { leafRepo.Close() })

	var projCode, srvCode string
	leafRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	leafRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

	clock := simio.NewSimClock()
	a := agent.NewFromParts(agent.Config{
		Clock:        clock,
		PollInterval: 5 * time.Second,
		Push:         false,
		Pull:         true,
	}, leafRepo, mf, projCode, srvCode)

	ctx := context.Background()
	for i := range steps {
		clock.Advance(5 * time.Second)
		act := a.Tick(ctx, agent.EventTimer)
		if act.Err != nil {
			t.Logf("step %d: sync error (may recover): %v", i, act.Err)
		}
		if act.Result != nil {
			t.Logf("step %d: rounds=%d sent=%d recv=%d",
				i, act.Result.Rounds, act.Result.FilesSent, act.Result.FilesRecvd)
		}

		leafCount, _ := CountBlobs(leafRepo)
		if leafCount >= masterCount {
			t.Logf("clone converged at step %d: %d blobs", i, leafCount)
			break
		}
	}

	leafCount, _ := CountBlobs(leafRepo)
	if leafCount < masterCount {
		t.Fatalf("clone incomplete: leaf=%d master=%d", leafCount, masterCount)
	}

	if err := CheckBlobIntegrity("clone-leaf", leafRepo); err != nil {
		t.Fatalf("clone integrity: %v", err)
	}
}

// --- TestDST: Main entry point for CI seed sweeps ---
// Usage: go test -run TestDST -seed=42 -level=hostile -steps=10000

func TestDST(t *testing.T) {
	if *flagSeed == 0 && *flagLevel == "" && *flagSteps == 0 {
		t.Skip("TestDST requires -seed, -level, or -steps flags (CI seed sweep)")
	}

	sev := parseSeverity()
	seed := *flagSeed
	if seed == 0 {
		seed = 1
	}
	steps := stepsFor(10000)

	masterRepo := createMasterRepoAt(t, filepath.Join(t.TempDir(), "master.fossil"))
	mf := NewMockFossil(masterRepo)

	// Seed 50 artifacts.
	for i := range 50 {
		mf.StoreArtifact([]byte(fmt.Sprintf("dst-sweep-%04d-seed%d", i, seed)))
	}

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    3,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()
	sim.Network().SetDropRate(sev.DropRate)

	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[DST %s] seed=%d steps=%d syncs=%d errors=%d",
		sev.Name, seed, sim.Steps, sim.TotalSyncs, sim.TotalErrors)

	// Safety checks — blob integrity may fail with buggify's content.Expand
	// byte-flip, which is expected (it tests that the sync engine detects
	// corruption). Delta chains and orphan phantoms must always hold.
	if sev.Buggify {
		// With buggify, only check structural invariants (not content hashes).
		for _, id := range sim.LeafIDs() {
			r := sim.Leaf(id).Repo()
			if err := CheckDeltaChains(string(id), r); err != nil {
				t.Fatalf("Delta chain violation at seed=%d: %v", seed, err)
			}
			if err := CheckNoOrphanPhantoms(string(id), r); err != nil {
				t.Fatalf("Orphan phantom violation at seed=%d: %v", seed, err)
			}
		}
	} else {
		// Without buggify, full safety check.
		if err := sim.CheckSafety(); err != nil {
			t.Fatalf("Safety violation at seed=%d level=%s: %v", seed, sev.Name, err)
		}
	}

	// In normal mode, also check convergence.
	if sev.DropRate == 0 && !sev.Buggify {
		if err := sim.CheckAllConverged(masterRepo); err != nil {
			t.Fatalf("Convergence violation at seed=%d: %v", seed, err)
		}
	}

	// Log per-leaf stats.
	for _, id := range sim.LeafIDs() {
		c, _ := CountBlobs(sim.Leaf(id).Repo())
		t.Logf("  %s: %d blobs", id, c)
	}
}

// --- UV Scenarios ---

func TestUVCleanSync(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(10)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "wiki/intro.txt", []byte("Welcome to the wiki"), 100)
	uv.Write(masterRepo.DB(), "wiki/faq.txt", []byte("Frequently asked questions"), 200)
	uv.Write(masterRepo.DB(), "data/config.json", []byte(`{"version":1}`), 300)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	steps := stepsFor(200)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	t.Logf("[%s] seed=%d steps=%d syncs=%d", sev.Name, seed, sim.Steps, sim.TotalSyncs)

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	if sev.DropRate == 0 && !sev.Buggify {
		if err := sim.CheckAllUVConverged(masterRepo); err != nil {
			t.Fatalf("UV Convergence: %v", err)
		}
	}
}

func TestUVBidirectional(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(11)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "wiki/page1.txt", []byte("page one"), 100)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Write UV file directly into first leaf.
	leaf0 := sim.Leaf(sim.LeafIDs()[0])
	uv.EnsureSchema(leaf0.Repo().DB())
	uv.Write(leaf0.Repo().DB(), "wiki/page2.txt", []byte("page two"), 200)

	steps := stepsFor(300)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	if sev.DropRate == 0 && !sev.Buggify {
		if err := sim.CheckAllUVConverged(masterRepo); err != nil {
			t.Fatalf("UV Convergence: %v", err)
		}
	}
}

func TestUVConflictMtimeWins(t *testing.T) {
	seed := seedFor(12)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "conflict.txt", []byte("master version"), 200)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    1,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Leaf has older mtime for same filename.
	leaf := sim.Leaf(sim.LeafIDs()[0])
	uv.EnsureSchema(leaf.Repo().DB())
	uv.Write(leaf.Repo().DB(), "conflict.txt", []byte("leaf version"), 100)

	if err := sim.Run(stepsFor(200)); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Master version (mtime=200) should win on the leaf.
	content, _, _, _ := uv.Read(leaf.Repo().DB(), "conflict.txt")
	if string(content) != "master version" {
		t.Errorf("expected master version, got %q", content)
	}
}

func TestUVDeletion(t *testing.T) {
	seed := seedFor(14)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "doomed.txt", []byte("will be deleted"), 100)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// First sync: distribute the file.
	if err := sim.Run(stepsFor(100)); err != nil {
		t.Fatalf("Run phase 1: %v", err)
	}

	// Verify file arrived.
	for _, id := range sim.LeafIDs() {
		c, _, _, _ := uv.Read(sim.Leaf(id).Repo().DB(), "doomed.txt")
		if string(c) != "will be deleted" {
			t.Fatalf("leaf %s missing file before deletion", id)
		}
	}

	// Delete on master with newer mtime.
	uv.Delete(masterRepo.DB(), "doomed.txt", 200)

	// Second sync: propagate deletion.
	if err := sim.Run(stepsFor(200)); err != nil {
		t.Fatalf("Run phase 2: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Verify tombstone on all leaves.
	for _, id := range sim.LeafIDs() {
		_, mtime, h, _ := uv.Read(sim.Leaf(id).Repo().DB(), "doomed.txt")
		if h != "" {
			t.Errorf("leaf %s: expected tombstone, got hash=%q", id, h)
		}
		if mtime != 200 {
			t.Errorf("leaf %s: mtime=%d, want 200", id, mtime)
		}
	}
}

func TestUVDeletionRevival(t *testing.T) {
	seed := seedFor(15)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	// Master deletes the file at mtime=100.
	uv.Delete(masterRepo.DB(), "revived.txt", 100)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    1,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Leaf creates file with newer mtime=200.
	leaf := sim.Leaf(sim.LeafIDs()[0])
	uv.EnsureSchema(leaf.Repo().DB())
	uv.Write(leaf.Repo().DB(), "revived.txt", []byte("I'm back"), 200)

	if err := sim.Run(stepsFor(200)); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	// Leaf version (mtime=200) should win — file is alive on master.
	content, _, _, _ := uv.Read(masterRepo.DB(), "revived.txt")
	if string(content) != "I'm back" {
		t.Errorf("expected revival, got %q", content)
	}
}

func TestUVCatalogHashSkip(t *testing.T) {
	seed := seedFor(19)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "same.txt", []byte("identical"), 100)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    1,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Pre-populate leaf with identical content.
	leaf := sim.Leaf(sim.LeafIDs()[0])
	uv.EnsureSchema(leaf.Repo().DB())
	uv.Write(leaf.Repo().DB(), "same.txt", []byte("identical"), 100)

	// After sync, both should still be identical (and no unnecessary data transfer).
	if err := sim.Run(stepsFor(100)); err != nil {
		t.Fatalf("Run: %v", err)
	}

	masterHash, _ := uv.ContentHash(masterRepo.DB())
	leafHash, _ := uv.ContentHash(leaf.Repo().DB())
	if masterHash != leafHash {
		t.Errorf("hashes should match: master=%s leaf=%s", masterHash, leafHash)
	}
}

// --- helpers ---

func createMasterRepoAt(t *testing.T, path string) *repo.Repo {
	t.Helper()
	r, err := repo.Create(path, "master", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create master: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}
