package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"time"

	"github.com/danmestas/EdgeSync/sim"
)

func main() {
	startSeed := flag.Int64("start-seed", 1, "starting seed")
	dataDir := flag.String("data-dir", ".", "directory for state and failures")
	flag.Parse()

	stateFile := filepath.Join(*dataDir, "last-seed.txt")
	failDir := filepath.Join(*dataDir, "failures")
	os.MkdirAll(failDir, 0o755)

	seed := *startSeed
	if data, err := os.ReadFile(stateFile); err == nil {
		fmt.Sscanf(string(data), "%d", &seed)
		seed++
	}

	var totalRuns, totalFails int
	batchStart := time.Now()

	for {
		numLeaves := int(seed%4) + 2

		for _, severity := range []sim.Level{sim.LevelNormal, sim.LevelAdversarial, sim.LevelHostile} {
			cfg := sim.SimConfig{
				Seed:           seed,
				NumLeaves:      numLeaves,
				BlobsPerLeaf:   5,
				MaxBlobSize:    4096,
				FaultDuration:  20 * time.Second,
				QuiesceTimeout: 60 * time.Second,
				Severity:       severity,
				KeepOnFailure:  false,
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

		os.WriteFile(stateFile, []byte(fmt.Sprintf("%d", seed)), 0o644)

		if seed%100 == 0 {
			elapsed := time.Since(batchStart)
			rate := float64(totalRuns) / elapsed.Hours()
			log.Printf("STATS: %d runs, %d failures (%.1f%%), %.0f runs/hour",
				totalRuns, totalFails, 100*float64(totalFails)/float64(totalRuns), rate)
		}

		seed++
	}
}
