package dst

import (
	"container/heap"
	"context"
	"fmt"
	"math/rand"
	"path/filepath"
	"time"

	"github.com/dmestas/edgesync/bridge/bridge"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/leaf/agent"
)

// Simulator drives a deterministic simulation of multiple leaf agents
// syncing through a bridge to an upstream transport (mock Fossil master).
type Simulator struct {
	Seed    int64
	rng     *rand.Rand
	clock   *simio.SimClock
	network *SimNetwork
	events  EventQueue
	leaves  map[NodeID]*agent.Agent
	bridge  *bridge.Bridge

	// Config
	pollInterval time.Duration
	leafIDs      []NodeID // ordered for deterministic iteration
	buggify      bool

	// Stats
	Steps         int
	TotalSyncs    int
	TotalErrors   int
}

// SimConfig configures a simulation run.
type SimConfig struct {
	Seed         int64
	NumLeaves    int
	PollInterval time.Duration
	TmpDir       string         // directory for repo files
	Upstream     sync.Transport // mock Fossil master
	Buggify      bool           // enable BUGGIFY fault injection
	UV           bool           // sync unversioned files
}

// New creates a Simulator with the given configuration. It creates
// leaf repos, agents, a bridge, and the simulated network. All I/O is
// local SQLite — no NATS or HTTP connections are made.
func New(cfg SimConfig) (*Simulator, error) {
	if cfg.PollInterval == 0 {
		cfg.PollInterval = 5 * time.Second
	}
	if cfg.NumLeaves == 0 {
		cfg.NumLeaves = 2
	}

	rng := rand.New(rand.NewSource(cfg.Seed))
	clock := simio.NewSimClock()

	// Create bridge with the upstream (mock fossil) transport.
	b := bridge.NewFromParts(bridge.Config{
		ProjectCode: "sim-project",
	}, cfg.Upstream)

	// Create simulated network.
	netRng := rand.New(rand.NewSource(rng.Int63()))
	network := NewSimNetwork(netRng, b)

	if cfg.Buggify {
		simio.EnableBuggify(rng.Int63())
	}

	s := &Simulator{
		Seed:         cfg.Seed,
		rng:          rng,
		clock:        clock,
		network:      network,
		bridge:       b,
		leaves:       make(map[NodeID]*agent.Agent),
		pollInterval: cfg.PollInterval,
		buggify:      cfg.Buggify,
	}
	heap.Init(&s.events)

	// Create leaf agents.
	simRand := simio.NewSeededRand(rng.Int63())
	for i := range cfg.NumLeaves {
		id := NodeID(fmt.Sprintf("leaf-%d", i))

		repoPath := filepath.Join(cfg.TmpDir, fmt.Sprintf("%s.fossil", id))
		r, err := repo.Create(repoPath, "simuser", simRand)
		if err != nil {
			return nil, fmt.Errorf("dst: create repo for %s: %w", id, err)
		}

		var projCode, srvCode string
		r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
		r.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

		transport := network.Transport(id)

		a := agent.NewFromParts(agent.Config{
			Clock:        clock,
			PollInterval: cfg.PollInterval,
			UV:           cfg.UV,
		}, r, transport, projCode, srvCode)

		s.leaves[id] = a
		s.leafIDs = append(s.leafIDs, id)

		// Schedule initial timer event with a staggered start.
		offset := time.Duration(rng.Int63n(int64(cfg.PollInterval)))
		s.events.PushEvent(&Event{
			Time:   clock.Now().Add(offset),
			Type:   EvTimer,
			NodeID: id,
		})
	}

	return s, nil
}

// Clock returns the simulator's virtual clock.
func (s *Simulator) Clock() *simio.SimClock {
	return s.clock
}

// Network returns the simulated network for fault injection.
func (s *Simulator) Network() *SimNetwork {
	return s.network
}

// Leaf returns the agent for the given node ID.
func (s *Simulator) Leaf(id NodeID) *agent.Agent {
	return s.leaves[id]
}

// LeafIDs returns the ordered list of leaf node IDs.
func (s *Simulator) LeafIDs() []NodeID {
	return s.leafIDs
}

// ScheduleSyncNow injects a SyncNow event for the given leaf at the current time.
func (s *Simulator) ScheduleSyncNow(id NodeID) {
	s.events.PushEvent(&Event{
		Time:   s.clock.Now(),
		Type:   EvSyncNow,
		NodeID: id,
	})
}

// Step processes the next event in the queue. Returns false if the queue is empty.
func (s *Simulator) Step() (bool, error) {
	if s.events.Len() == 0 {
		return false, nil
	}

	ev := s.events.PopEvent()
	s.clock.AdvanceTo(ev.Time)

	leaf, ok := s.leaves[ev.NodeID]
	if !ok {
		return true, fmt.Errorf("dst: unknown node %s", ev.NodeID)
	}

	// Map simulation event to agent event.
	var agentEv agent.Event
	switch ev.Type {
	case EvTimer:
		agentEv = agent.EventTimer
	case EvSyncNow:
		agentEv = agent.EventSyncNow
	}

	// Execute the sync cycle.
	ctx := context.Background()
	act := leaf.Tick(ctx, agentEv)

	s.Steps++
	if act.Type == agent.ActionSynced {
		s.TotalSyncs++
		if act.Err != nil {
			s.TotalErrors++
		}
	}

	// Re-schedule the timer for this leaf.
	if ev.Type == EvTimer {
		s.events.PushEvent(&Event{
			Time:   s.clock.Now().Add(s.pollInterval),
			Type:   EvTimer,
			NodeID: ev.NodeID,
		})
	}

	return true, nil
}

// Run processes up to maxSteps events. Returns nil on success or the first
// invariant/error encountered.
func (s *Simulator) Run(maxSteps int) error {
	for i := 0; i < maxSteps; i++ {
		more, err := s.Step()
		if err != nil {
			return fmt.Errorf("step %d: %w", i, err)
		}
		if !more {
			break
		}
	}
	return nil
}

// RunUntil processes events until the clock reaches the given time.
func (s *Simulator) RunUntil(deadline time.Time) error {
	for s.events.Len() > 0 {
		// Peek at next event time without popping.
		next := s.events[0]
		if next.Time.After(deadline) {
			s.clock.AdvanceTo(deadline)
			break
		}
		_, err := s.Step()
		if err != nil {
			return err
		}
	}
	return nil
}

// Close cleans up all leaf agent repos and disables buggify.
func (s *Simulator) Close() error {
	if s.buggify {
		simio.DisableBuggify()
	}
	var firstErr error
	for _, a := range s.leaves {
		if err := a.Repo().Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}
