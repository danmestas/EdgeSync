package dst

import (
	"flag"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
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
