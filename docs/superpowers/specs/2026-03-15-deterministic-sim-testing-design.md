# Deterministic Simulation Testing for EdgeSync

**Date:** 2026-03-15
**Status:** Draft
**Scope:** Leaf agent, bridge, and sync engine

## Problem

EdgeSync synchronizes Fossil repos across leaf nodes via NATS messaging through a bridge to an unmodified Fossil server. The sync protocol involves multi-round convergence, message ordering dependencies, and concurrent push/pull from multiple leaves. Traditional unit and integration tests cannot exercise the combinatorial space of message orderings, partial failures, and concurrent operations that occur in production.

## Goals

1. Verify that multiple leaf nodes converge to identical repo state after syncing through the bridge, regardless of message ordering and timing.
2. Test resilience to infrastructure failures: NATS partitions, message drops, latency spikes, leaf/bridge restarts mid-sync.
3. Run thousands of randomized scenarios automatically to find bugs that targeted tests miss.
4. Provide enough failure context (fault schedule, sync history, invariant diffs) to debug issues without exact replay.

## Non-Goals

- Replacing the existing `dst/` package (the deterministic single-threaded simulation stays as the fast, reproducible inner loop).
- Performance benchmarking.
- Testing Fossil server internals.

## Relationship to Existing `dst/` Package

The project already has a `dst/` package that provides **deterministic single-threaded simulation** using `NewFromParts()` constructors, `SimNetwork` (in-memory message routing with drop/partition), `MockFossil` (in-process Go Fossil server), and `SimClock`. It bypasses real NATS and HTTP entirely.

The new `sim/` package is a **complementary integration simulation layer** that tests a different surface:

| | `dst/` (existing) | `sim/` (new) |
|---|---|---|
| NATS | Simulated (`SimTransport`) | Real (embedded NATS + fault proxy) |
| Fossil server | Mock (`MockFossil`) | Real (`fossil server` process) |
| Constructors | `NewFromParts()` | `New()` (production path) |
| Determinism | Fully deterministic (same seed = same execution) | Structurally deterministic (same fault schedule, non-deterministic goroutine timing) |
| Speed | Fast (thousands of seeds in CI) | Slower (process startup, real I/O) |
| Bug class | Protocol logic, convergence, message ordering | NATS integration, HTTP encoding, real Fossil interop, production code paths |

Both layers share invariant checking logic (blob convergence, content integrity). The `sim/` package should import and reuse invariant checkers rather than duplicating them.

**`dst/` is not modified by this spec.** It continues to serve as the fast inner-loop simulation. `sim/` adds the outer-loop integration simulation.

## Architecture

### Overview

```
┌──────────┐     ┌──────────────────┐     ┌──────────────┐     ┌──────────────┐
│  Leaf A   │────>│   Fault Proxy    │────>│  Embedded    │<────│   Bridge     │
│  Leaf B   │────>│  (seed-driven    │────>│  NATS Server │     │              │
│  Leaf C   │────>│   delay/drop/    │     │  (in-process)│     │  (real code) │
│  ...      │     │   partition)     │     └──────────────┘     └──────┬───────┘
└──────────┘     └──────────────────┘                                  │
                                                                  HTTP POST
                                                                       │
                                                              ┌────────v───────┐
                                                              │ fossil server  │
                                                              │ (fresh per run)│
                                                              └────────────────┘
```

### Components

- **Test harness** (`sim/`): Orchestrates setup, fault injection, quiescence detection, and invariant checking.
- **Fault proxy**: TCP proxy between leaf NATS clients and embedded NATS server. Intercepts connections and applies seed-controlled faults.
- **Leaf agents**: Real `Agent` code from `leaf/agent/`, created via `agent.New()` (production constructor, not `NewFromParts()`), configured to connect to the fault proxy address.
- **Bridge**: Real `Bridge` code from `bridge/bridge/`, created via `bridge.New()` (production constructor, not `NewFromParts()`), connects directly to embedded NATS.
- **Fossil server**: Real `fossil server` process on a temp repo, fresh per simulation run.

### What the Seed Controls

- Fault proxy behavior (which messages to delay/drop, partition timing)
- Number of leaves and their push/pull configuration
- Content generation (blob count, sizes, which leaf holds which blobs)
- Fault schedule (when partitions start/heal, when to restart components)
- BUGGIFY site enablement (which application-level fault sites are active)

### What Is Real (Not Controlled)

- Goroutine scheduling and NATS internal message delivery timing
- Fossil server response time
- OS-level scheduling

## Fault Proxy

The fault proxy is a TCP proxy that sits between leaf NATS clients and the embedded NATS server.

### How It Works

```
Leaf connects to proxy:port --> Proxy opens upstream to embedded NATS:port
                             --> All bytes flow through, but the proxy can:
                                1. Delay forwarding (add latency)
                                2. Drop the connection (simulate partition)
                                3. Close and reopen (simulate leaf restart)
                                4. Pause traffic in one direction (asymmetric partition)
```

### Fault Types

| Fault | Description | Severity Level |
|-------|-------------|----------------|
| Latency injection | Hold bytes for N ms before forwarding | Normal |
| Message drop | Close connection mid-exchange, leaf gets timeout | Adversarial |
| Partition | Block all traffic for leaf X for N seconds | Adversarial |
| Asymmetric partition | Leaf can send but not receive (or vice versa) | Hostile |
| Bridge restart | Harness calls Bridge.Stop() then creates a new Bridge via bridge.New() with same config (Stop() drains and closes the NATS connection permanently) | Hostile |
| Leaf restart | Harness calls Agent.Stop() then creates new Agent via agent.New() with same repo path (Stop() closes the repo; new agent reopens it) | Hostile |

### Fault Schedule

A seed-driven `*rand.Rand` generates a fault schedule at the start of each run. The schedule is logged in full so failures can be understood and manually reproduced.

Example schedule:

```
t=0s:   start all leaves + bridge (clean)
t=2s:   partition leaf-B from NATS for 5s
t=4s:   inject 500ms latency on leaf-A
t=7s:   heal partition on leaf-B
t=10s:  restart bridge
t=15s:  heal all faults
t=20s:  quiesce — wait for convergence
t=30s:  check invariants
```

### Severity Levels (TigerBeetle VOPR Model)

| Level | Description | Fault Rate |
|-------|-------------|-----------|
| Normal | No faults. Validates basic multi-leaf convergence. | 0% |
| Adversarial | Latency spikes, occasional partitions, message drops. | 5-15% |
| Hostile | Asymmetric partitions, mid-sync restarts, aggressive latency. | 20-30% |

## Invariant Checking

After all faults are healed, the harness enters a quiescence phase then checks invariants.

### Quiescence Detection

Poll each leaf's repo DB until no new blobs appear for 3 consecutive sync cycles. Timeout (default 60s) catches liveness violations.

### Invariant 1: Blob Convergence

Every blob UUID present in the Fossil server's repo must be present in every leaf repo, and vice versa.

```sql
-- From each repo:
SELECT uuid FROM blob WHERE size >= 0
-- All repos must produce identical UUID sets.
```

### Invariant 2: Content Integrity

For every shared UUID, the expanded content (after delta chain resolution) must be byte-identical across all repos. The harness resolves each UUID to its per-repo `rid` (via `SELECT rid FROM blob WHERE uuid=?`), then calls `content.Expand(querier, rid)` to walk the delta chain. Different repos will have different `rid` values for the same UUID.

### Invariant 3: Liveness

If all faults are healed, the system must converge within the quiesce timeout. Failure to converge is a liveness violation.

### Invariant 4: No Duplicate Blobs

Each UUID appears exactly once in each repo's blob table.

```sql
SELECT uuid, COUNT(*) FROM blob GROUP BY uuid HAVING COUNT(*) > 1
-- Must return zero rows.
```

### Failure Reporting

When an invariant fails, the harness logs:

1. The seed
2. The full fault schedule with timestamps
3. Which invariant failed
4. The diff (missing/extra UUIDs, differing content bytes)
5. Each leaf's sync history (rounds, files sent/received per round)

## Test Harness Orchestration

### Package Location

`sim/` at the project root.

### Simulation Run Lifecycle

```
1. SETUP
   - Create temp directory
   - Create Fossil server repo (fossil new)
   - Start fossil server on random port
   - Start embedded NATS server
   - Start fault proxy (fronting NATS)
   - Create N leaf repos (fossil new + seed content)
   - Start bridge (connects to NATS + Fossil server)
   - Start N leaf agents (connect to fault proxy)

2. SEED CONTENT
   - For each leaf, insert M random blobs via blob.Store()
   - Manually INSERT each blob's rid into both the unclustered and unsent tables
     (blob.Store() only writes to the blob table; the sync protocol discovers
     pushable artifacts via igot cards built from SELECT on unclustered;
     the unsent table prevents premature convergence detection — the sync
     loop checks unsent count before declaring done; matching the pattern
     in sync/sync_test.go and manifest/manifest.go)
   - Blob count and size driven by seed
   - Some leaves push-only, some pull-only, some both
   - Log initial state

3. FAULT PHASE
   - Execute fault schedule (generated from seed)
   - Faults fire at wall-clock offsets
   - Each fault logged with timestamp
   - Duration: configurable (default 20s)

4. HEAL + QUIESCE
   - Remove all faults
   - Wait for convergence
   - Timeout: configurable (default 60s)

5. CHECK INVARIANTS
   - Stop leaf agents and bridge (Agent.Stop() closes the repo DB)
   - Reopen each leaf repo via repo.Open() for read-only invariant queries
   - Open the Fossil server's repo DB directly for comparison
   - Blob convergence
   - Content integrity (resolve UUID to rid per-repo, then content.Expand())
   - No duplicates
   - Liveness (implicit in quiescence)
   - Log pass/fail + details
   - Close all reopened repo handles

6. TEARDOWN
   - Stop fault proxy
   - Stop NATS
   - Stop fossil server
   - Remove temp directory (keep on failure for debugging)
```

### Configuration

```go
type SimConfig struct {
    Seed           int64
    NumLeaves      int           // default 2
    BlobsPerLeaf   int           // default 5
    MaxBlobSize    int           // default 4096
    FaultDuration  time.Duration // default 20s
    QuiesceTimeout time.Duration // default 60s
    Severity       Level         // Normal, Adversarial, Hostile
    KeepOnFailure  bool          // preserve temp dir for debugging
}
```

### Running

```bash
# Single seed
go test ./sim/ -run TestSimulation -sim.seed=42

# Specific severity
go test ./sim/ -run TestSimulation -sim.seed=42 -sim.severity=hostile

# Multiple seeds with parallelism
go test ./sim/ -run TestSimulation -sim.seeds=1-16 -parallel=4

# Short mode for fast feedback
go test ./sim/ -run TestSimulation -sim.seed=42 -short
```

## BUGGIFY

BUGGIFY injects faults inside application code to exercise rare paths. It complements the fault proxy: the proxy tests infrastructure failures, BUGGIFY tests application-level edge cases.

### Guard Mechanism

The `Buggify` struct is a standalone, goroutine-safe component (uses a mutex-protected RNG) since multiple leaf agents run concurrently in `sim/`. It is **not** a field on `simio.Env` — it is threaded separately via config structs.

```go
type Buggify struct {
    mu      sync.Mutex
    rng     *rand.Rand      // Seeded at init
    sites   map[string]bool // Which sites are active this run
}

func NewBuggify(seed int64) *Buggify {
    rng := rand.New(rand.NewSource(seed))
    // ... decide site enablement from rng
}

func (b *Buggify) Check(site string, probability float64) bool {
    if b == nil {
        return false // nil-safe: production passes nil
    }
    b.mu.Lock()
    defer b.mu.Unlock()
    if !b.sites[site] {
        return false
    }
    return b.rng.Float64() < probability
}
```

**Injection path:** Add a `*Buggify` field to `SyncOpts`, `agent.Config`, and `bridge.Config` (all three need modification). In production, `nil` is passed and `Check()` is a no-op. In both `dst/` and `sim/` test harnesses, a seeded `*Buggify` instance is created and passed through config. This is separate from the `simio.Env` injection path — `Env` handles clock and randomness, `Buggify` handles fault injection.

### Per-Run Site Enablement

Each BUGGIFY site is enabled or disabled for the entire run (decided by seed at startup). 25% of sites are enabled per run. Site enablement is decided during `NewBuggify()` initialization.

### Initial BUGGIFY Sites

| Location | Fault | Probability | What It Tests |
|----------|-------|-------------|---------------|
| `sync/client.go` `buildFileCards()` | Skip a file card | 5% | Multi-round convergence |
| `sync/client.go` `handleFileCard()` | Reject a valid blob | 3% | Retry logic |
| `sync/client.go` `buildRequest()` | Set maxSend to minimum | 10% | Round budget logic |
| `leaf/agent/agent.go` `runSync()` | Return early after 1 round | 5% | Partial sync recovery |
| `bridge/bridge/bridge.go` `handleMessage()` | Respond with empty message | 3% | Leaf timeout + retry |
| `sync/client.go` `buildLoginCard()` | Use wrong nonce | 2% | Auth failure retry |

## CI Integration

### GitHub Actions

Every push runs 16 seeds across 3 severity levels (48 runs total):

```yaml
name: Simulation Tests
on: [push, pull_request]

jobs:
  sim:
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        seed: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
        severity: [normal, adversarial, hostile]
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install Fossil
        run: sudo apt-get install -y fossil
      - name: Run simulation
        run: |
          go test ./sim/ -run TestSimulation \
            -sim.seed=${{ matrix.seed }} \
            -sim.severity=${{ matrix.severity }} \
            -sim.leaves=3 \
            -timeout=120s \
            -v
```

`fail-fast: false` ensures all seeds run even if one fails.

## Hetzner Soak Runner

A continuous process on the Hetzner VPS that churns through seeds endlessly.

### Design

Located at `sim/cmd/soak/main.go`.

```
Loop forever:
  1. Pick next seed (sequential counter)
  2. Run all 3 severity levels
  3. On pass: log seed + duration, continue
  4. On fail: log full details, write to failures/ dir, continue
  5. Report stats every 100 seeds
```

### Features

- **Persistent state**: Writes last-completed seed to file, resumes after restart.
- **Failure archive**: Each failure gets `failures/<seed>/` with fault schedule, invariant diff, and sync logs.
- **Stats reporting**: Every 100 seeds, logs throughput (seeds/hour), pass rate, failure summary.
- **Node count scaling**: Varies leaf count per seed (`seed % 4 + 2`, giving 2-5 leaves).
- **Coolify deployment**: Runs as long-lived process, restarts on crash.

### Running

```bash
# On Hetzner VPS
cd /opt/edgesync
go build ./sim/cmd/soak
./soak -start-seed=1 -data-dir=/var/lib/edgesync-soak
```

### Investigating Failures

```bash
# On VPS: check failures
ls /var/lib/edgesync-soak/failures/

# On local Mac: reproduce
go test ./sim/ -run TestSimulation -sim.seed=<failing_seed> \
  -sim.severity=<level> -sim.leaves=<count> -v
```

## Incremental Rollout

Each phase is independently useful.

### Phase 1: Harness Skeleton

- `sim/` package with `SimConfig`, setup/teardown lifecycle
- Start Fossil server + embedded NATS + bridge + 2 leaves
- No faults, no proxy — clean sync only
- Verify blob convergence invariant
- **Value**: Automated multi-leaf convergence test (does not exist today)

### Phase 2: Fault Proxy

- TCP proxy between leaves and NATS
- Partition and latency injection
- Seed-driven fault schedule generation
- Fault logging
- **Value**: Starts finding ordering and partition bugs

### Phase 3: Invariant Checking

- Content integrity check via `content.Expand()`
- Duplicate blob detection
- Rich failure reporting (diff, sync history, fault schedule)
- **Value**: Catches delta application bugs and corruption

### Phase 4: BUGGIFY

- `Buggify` struct with `Check()` method, threaded via dependency injection
- Add `*Buggify` field to `SyncOpts`, `agent.Config`, and `bridge.Config`
- Initial BUGGIFY sites in sync session, agent, and bridge
- Per-run site enablement from seed
- **Value**: Exercises rare application-level code paths

### Phase 5: CI Integration

- GitHub Actions workflow with seed matrix
- 3 severity levels, 16 seeds per level
- **Value**: Regression prevention on every push

### Phase 6: Soak Runner

- `sim/cmd/soak/` binary
- Persistent state, failure archive, stats reporting
- Deploy to Hetzner via Coolify
- **Value**: Continuous deep exploration of the state space

## Dependencies

- `fossil` binary available on PATH (for `fossil new` and `fossil server`)
- `github.com/nats-io/nats-server/v2` (embedded NATS, already used in integration tests)
- `github.com/nats-io/nats.go` (already a dependency)
- `modernc.org/sqlite` (already a dependency)
- Existing `go-libfossil` packages: `blob`, `content`, `repo`, `db`
- Existing `leaf/agent` and `bridge/bridge` packages

## Open Questions

1. Should the soak runner report failures to a notification channel (e.g., email, Slack, or a simple webhook)?
2. Should simulation runs record NATS message traces for post-mortem analysis beyond the fault schedule log?
3. What's the right balance of fault duration vs quiesce timeout for CI (fast feedback vs thorough testing)?
