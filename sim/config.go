package sim

import (
	"flag"
	"time"

	libsync "github.com/danmestas/go-libfossil/sync"
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
	Observer       libsync.Observer
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
