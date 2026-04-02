package sim

import (
	"context"
	"fmt"
	"math/rand"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/danmestas/go-libfossil/blob"
	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/simio"
	"github.com/danmestas/go-libfossil/sync"
)

func hasFossil() bool {
	_, err := exec.LookPath("fossil")
	return err == nil
}

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
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping simulation in short mode")
	}

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
				Observer:       testObserver,
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

// TestCloneFromFossilServer tests sync.Clone against a real Fossil server.
// It seeds blobs into a leaf, syncs them to the server via the normal
// agent/bridge path, then clones into a fresh repo and verifies all
// blobs arrived.
func TestCloneFromFossilServer(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping clone sim test in short mode")
	}

	// Set up infrastructure: Fossil server + NATS + 1 leaf.
	cfg := SimConfig{
		Seed:           42,
		NumLeaves:      1,
		BlobsPerLeaf:   5,
		MaxBlobSize:    4096,
		FaultDuration:  0, // No faults — we want clean sync first.
		QuiesceTimeout: 30 * time.Second,
		Observer:       testObserver,
	}
	h := NewHarness(cfg)
	if err := h.SetupInfra(); err != nil {
		t.Fatalf("SetupInfra: %v", err)
	}
	defer h.Teardown()

	// Seed blobs into the leaf repo.
	rng := rand.New(rand.NewSource(cfg.Seed))
	leafPath := h.LeafPaths()[0]
	lr, err := repo.Open(leafPath)
	if err != nil {
		t.Fatalf("open leaf: %v", err)
	}
	seededUUIDs, err := SeedLeaf(lr, rng, cfg.BlobsPerLeaf, cfg.MaxBlobSize)
	lr.Close()
	if err != nil {
		t.Fatalf("seed leaf: %v", err)
	}
	t.Logf("Seeded %d blobs into leaf", len(seededUUIDs))

	// Start agents so blobs sync from leaf → NATS → bridge → Fossil server.
	if err := h.StartAgents(); err != nil {
		t.Fatalf("StartAgents: %v", err)
	}

	// Wait for sync to converge.
	time.Sleep(cfg.QuiesceTimeout)

	// Stop agents — we just needed them to push blobs to the server.
	for _, a := range h.leaves {
		a.Stop()
	}
	h.bridge.Stop()

	// Verify server has the blobs.
	sr, err := repo.Open(h.FossilRepoPath())
	if err != nil {
		t.Fatalf("open server repo: %v", err)
	}
	serverBlobCount := 0
	for _, uuid := range seededUUIDs {
		if _, ok := blob.Exists(sr.DB(), uuid); ok {
			serverBlobCount++
		}
	}
	sr.Close()
	t.Logf("Server has %d/%d seeded blobs", serverBlobCount, len(seededUUIDs))

	if serverBlobCount == 0 {
		t.Fatal("server has no blobs — sync did not converge, cannot test clone")
	}

	// Clone from the Fossil server into a fresh repo.
	clonePath := filepath.Join(t.TempDir(), "cloned.fossil")
	transport := &sync.HTTPTransport{URL: h.FossilURL()}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	clonedRepo, result, err := sync.Clone(ctx, clonePath, transport, sync.CloneOpts{
		Env:      &simio.Env{Rand: simio.NewSeededRand(cfg.Seed + 999)},
		Observer: testObserver,
	})
	if err != nil {
		t.Fatalf("Clone failed: %v", err)
	}
	defer clonedRepo.Close()

	t.Logf("Clone completed: rounds=%d blobs=%d project-code=%s",
		result.Rounds, result.BlobsRecvd, result.ProjectCode)

	// Verify cloned repo has the seeded blobs.
	clonedBlobCount := 0
	for _, uuid := range seededUUIDs {
		if _, ok := blob.Exists(clonedRepo.DB(), uuid); ok {
			clonedBlobCount++
		}
	}

	if clonedBlobCount < serverBlobCount {
		t.Errorf("cloned repo has %d/%d blobs (server had %d)",
			clonedBlobCount, len(seededUUIDs), serverBlobCount)
	} else {
		t.Logf("Clone verified: %d/%d seeded blobs present", clonedBlobCount, len(seededUUIDs))
	}

	// Verify project-code matches.
	if result.ProjectCode == "" {
		t.Error("clone did not receive project-code from server")
	}
}
