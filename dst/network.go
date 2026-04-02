package dst

import (
	"context"
	"fmt"
	"math/rand"

	"github.com/dmestas/edgesync/bridge/bridge"
	"github.com/danmestas/go-libfossil/xfer"
)

// SimNetwork simulates the network between leaf agents and the bridge.
// It supports message dropping, response truncation, and network partitions,
// controlled by a seeded PRNG for deterministic behavior.
type SimNetwork struct {
	rng          *rand.Rand
	bridge       *bridge.Bridge
	dropRate     float64         // probability of dropping a message [0, 1)
	truncateRate float64         // probability of truncating response cards [0, 1)
	partitions   map[NodeID]bool // partitioned nodes (messages to/from are dropped)
}

// NewSimNetwork creates a simulated network connected to the given bridge.
func NewSimNetwork(rng *rand.Rand, b *bridge.Bridge) *SimNetwork {
	return &SimNetwork{
		rng:        rng,
		bridge:     b,
		partitions: make(map[NodeID]bool),
	}
}

// SetDropRate sets the probability that any message is dropped entirely.
func (n *SimNetwork) SetDropRate(rate float64) {
	n.dropRate = rate
}

// SetTruncateRate sets the probability that a response is truncated
// (random suffix of cards dropped). Simulates partial delivery.
func (n *SimNetwork) SetTruncateRate(rate float64) {
	n.truncateRate = rate
}

// Partition isolates a node — all messages to/from it are dropped.
func (n *SimNetwork) Partition(id NodeID) {
	n.partitions[id] = true
}

// Heal removes a node from the partition set.
func (n *SimNetwork) Heal(id NodeID) {
	delete(n.partitions, id)
}

// HealAll removes all partitions.
func (n *SimNetwork) HealAll() {
	n.partitions = make(map[NodeID]bool)
}

// Transport returns a sync.Transport for the given node that routes
// messages through this simulated network to the bridge.
func (n *SimNetwork) Transport(nodeID NodeID) *SimTransport {
	return &SimTransport{network: n, nodeID: nodeID}
}

// exchange handles a message from a leaf to the bridge, applying fault injection.
func (n *SimNetwork) exchange(ctx context.Context, nodeID NodeID, req *xfer.Message) (*xfer.Message, error) {
	if n.partitions[nodeID] {
		return nil, fmt.Errorf("simnet: node %s is partitioned", nodeID)
	}
	if n.dropRate > 0 && n.rng.Float64() < n.dropRate {
		return nil, fmt.Errorf("simnet: message from %s dropped", nodeID)
	}
	resp, err := n.bridge.HandleRequest(ctx, req)
	if err != nil {
		return nil, err
	}
	// Truncate: drop a random suffix of response cards to simulate partial delivery.
	if n.truncateRate > 0 && len(resp.Cards) > 1 && n.rng.Float64() < n.truncateRate {
		keep := 1 + n.rng.Intn(len(resp.Cards)-1) // keep at least 1 card
		resp.Cards = resp.Cards[:keep]
	}
	return resp, nil
}

// SimTransport implements sync.Transport by routing through the SimNetwork.
type SimTransport struct {
	network *SimNetwork
	nodeID  NodeID
}

// Exchange sends a request through the simulated network to the bridge.
func (t *SimTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	return t.network.exchange(ctx, t.nodeID, req)
}
