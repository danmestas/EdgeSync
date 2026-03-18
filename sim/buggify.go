package sim

import (
	"math/rand"
	"sync"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
)

var _ libsync.BuggifyChecker = (*Buggify)(nil)

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
	sites map[string]bool
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
	if site == "" {
		panic("buggify.Check: site must not be empty")
	}
	if probability < 0 || probability > 1 {
		panic("buggify.Check: probability must be in [0,1]")
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
