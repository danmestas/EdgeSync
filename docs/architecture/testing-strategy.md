# Testing & Validation Strategy

## Test Tiers

| Tier | Location | Infrastructure | Speed | What It Proves |
|------|----------|---------------|-------|----------------|
| Unit | `libfossil/*/` | None | <1s | Package-level correctness |
| DST (deterministic sim) | `dst/` | In-process only | Fast (thousands of seeds in CI) | Protocol logic, convergence, message ordering |
| Integration sim | `sim/` | Real NATS + fault proxy + real Fossil | ~15-60s per run | NATS integration, HTTP encoding, production code paths |
| Equivalence | `sim/equivalence_test.go` | Fossil CLI | <15s | Round-trip: Checkin -> sync -> fossil open -> verify files |
| Checkout integration | `sim/checkout_test.go` | Real NATS, optional Fossil CLI | <30s | Checkout -> sync -> update round-trip, concurrent access safety |
| Interop (Tier 1) | `sim/interop_test.go` | Fossil CLI as oracle | <15s | Bit-compatibility with Fossil (delta, hash, clone, sync) |
| Interop (Tier 2) | `sim/interop_test.go` | Fossil SCM repo (66K artifacts) | Minutes | Deep delta chains (depth 2547), large-scale blob expansion |

## Deterministic Simulation (`dst/`)

Single-threaded, fully deterministic. Same seed produces identical execution.

**Core abstractions:**

- `SimNetwork` -- in-memory message routing with drop/partition (bridge mode, through MockFossil)
- `PeerNetwork` -- leaf-to-leaf direct sync
- `MockFossil` -- in-process Go Fossil server, delegates to `HandleSyncWithOpts`
- `simio.SimClock` -- deterministic time
- `simio.Env` -- injectable clock, rand, environment
- `BuggifyChecker` -- interface for application-level fault injection

**Decision:** DST bypasses real NATS and HTTP entirely. Constructors use `NewFromParts()` (not production `New()`). This is the fast inner loop; `sim/` is the complementary outer loop.

## Integration Simulation (`sim/`)

Uses production constructors (`agent.New()`, `bridge.New()`), real embedded NATS, real `fossil server` process, and a TCP fault proxy.

**Fault proxy** sits between leaf NATS clients and embedded NATS server:

| Fault | Description |
|-------|-------------|
| Latency injection | Hold bytes for N ms before forwarding |
| Message drop | Close connection mid-exchange |
| Partition | Block all traffic for leaf X for N seconds |
| Asymmetric partition | One-directional traffic block |
| Bridge restart | `Stop()` then `bridge.New()` with same config |
| Leaf restart | `Stop()` then `agent.New()` with same repo path |

**Severity levels** (TigerBeetle VOPR model):

| Level | Fault Rate | Purpose |
|-------|-----------|---------|
| Normal | 0% | Baseline multi-leaf convergence |
| Adversarial | 5-15% | Latency spikes, occasional partitions |
| Hostile | 20-30% | Asymmetric partitions, mid-sync restarts |

**Seed controls:** fault schedule, leaf count/config, content generation, BUGGIFY site enablement. Goroutine scheduling and OS timing are NOT controlled (structurally deterministic, not fully deterministic).

**Invariants checked after quiescence:**

1. Blob convergence -- identical UUID sets across all repos
2. Content integrity -- byte-identical expanded content (per-repo rid via `content.Expand`)
3. Liveness -- convergence within timeout after faults heal
4. No duplicate blobs -- each UUID appears exactly once per repo
5. Table sync integrity -- PK hashes and mtimes match across peers
6. Tombstone convergence -- all peers agree on which extension table rows are tombstones

## BUGGIFY

Goroutine-safe fault injection inside application code. Complements the fault proxy (infra faults) with application-level edge cases.

**Design decisions:**

- Standalone `Buggify` struct with mutex-protected RNG (not on `simio.Env`)
- Threaded via `*Buggify` field on `SyncOpts`, `agent.Config`, `bridge.Config`
- `nil` in production (nil-safe `Check()` returns false)
- 25% of sites enabled per run, decided at seed initialization

| Site | Fault | Probability |
|------|-------|-------------|
| `buildFileCards()` | Skip a file card | 5% |
| `handleFileCard()` | Reject a valid blob | 3% |
| `buildRequest()` | Set maxSend to minimum | 10% |
| `runSync()` | Return early after 1 round | 5% |
| `handleMessage()` | Respond with empty message | 3% |
| `buildLoginCard()` | Use wrong nonce | 2% |
| `handleXDelete.reject` | Reject valid xdelete | 3% |
| `sendXDelete.corruptPKData` | Corrupt PKData JSON | 2% |
| `handleXIGot.skipXDelete` | Skip sending xdelete for tombstone | 5% |
| `handleXDeleteResponse.drop` | Drop received xdelete | 5% |
| `buildTableSendCards.skipXDelete` | Skip queued xdelete send | 3% |
| `clone.processResponse.dropFile` | Skip storing received file (creates phantom) | 5% |
| `clone.processResponse.corruptHash` | Flip byte in file content (tests verify-before-commit) | 2% |
| `clone.processResponse.dropSeqNo` | Ignore clone completion signal (forces extra round) | 5% |
| `clone.buildRequest.dropGimme` | Drop last gimme card (delays phantom resolution) | 5% |
| `clone.buildRequest.badLogin` | Corrupt login nonce (tests auth failure recovery) | 5% |
| `clone.emitCloneBatch.truncate` | Remove last file from clone batch (incomplete delivery) | 10% |

### Clone DST

Clone is exercised via `dst/clone_test.go` — calls `sync.Clone()` directly through `MockFossil` (handler-backed transport) with Buggify on both client and server. Not driven through Agent/Tick — clone is a one-shot operation, not a poll loop.

**Seed sweep:** 20 seeds, each producing a deterministic fault combination. ~50% converge successfully, ~50% trigger `corruptHash` and verify clean repo deletion.

**Invariants:** On success: blob convergence, no residual phantoms. On failure: repo file deleted, error indicates hash mismatch.

## Fossil Interop Tests

Use the Fossil CLI as an oracle to catch self-referential bugs (Go code validating Go code).

**Tier 1 (always runs):** delta codec cross-validation, clone in both directions, incremental sync, hash compatibility (SHA1 + SHA3). All tests skip gracefully if `fossil` not in PATH.

**Tier 2 (skipped in `-short`):** Clone the real Fossil SCM repo, expand all 66K blobs, verify hashes, cross-check random sample against `fossil artifact -R` output.

**Key helper -- `verifyAllBlobs`:** Expands every non-phantom blob via delta chain, recomputes hash, asserts match against stored UUID.

**Key helper -- `verifyWithFossilRebuild`:** Runs `fossil rebuild -R` as the ultimate structural integrity check.

## Equivalence & Checkout Tests

**Equivalence tests** validate three sync topologies:

- Leaf -> Fossil (via `fossil clone` from Go-served HTTP)
- Fossil -> Leaf (via `sync.Clone` from `fossil serve`)
- Leaf -> Leaf (via `sync.Sync` + `HTTPTransport`, requires `manifest.Crosslink` on receiver)

Each topology runs single-file, multi-file, and commit-chain sub-tests.

**Checkout integration tests** (4 tests, pure Go + NATS for tests 1-3):

1. Checkout after clone via NATS -- blob transfer then extract
2. Commit -> sync -> update round-trip -- full edit cycle
3. Concurrent edit + sync -- proves WAL-mode SQLite handles concurrent agent + checkout writes
4. Fossil interop both directions -- `fossil rebuild` validates Go commits, Go extracts Fossil commits

## CI Integration

- `make test` -- unit + DST + Tier 1 interop + sim serve tests (~15s)
- `make test-full` -- adds Tier 2 (large repo)
- Pre-commit hook -- `make test` only (~8s budget)
- GitHub Actions -- 16 seeds x 3 severity levels (48 sim runs per push, `fail-fast: false`)
- Soak runner (`sim/cmd/soak/`) -- continuous seed churn on Hetzner VPS, failure archive at `failures/<seed>/`

## Constraints

- `fossil` binary required for interop/equivalence tests (skip if absent)
- `go build -buildvcs=false` required (dual VCS)
- `HandleSyncWithOpts` with `HandleOpts{Buggify}` for DST; `HandleSync` in production
- `sync.Clone` crosslinks automatically; `sync.Sync` does not (caller must run `manifest.Crosslink`)
- Tier 2 self-provisions `testdata/fossil.fossil` from upstream on first run (~45MB, gitignored)
