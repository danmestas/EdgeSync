package sim

import (
	"fmt"
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

	// Wait for quiescence.
	time.Sleep(cfg.QuiesceTimeout)

	// Stop leaves and bridge before invariant checking.
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
