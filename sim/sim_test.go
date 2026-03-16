package sim

import (
	"fmt"
	"os/exec"
	"testing"
	"time"
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
