# Checkout Sim Integration Tests

**Date**: 2026-03-24
**Ticket**: CDG-162
**Branch**: `feature/cdg-162-checkout-sim-tests` from main, in worktree

## Problem

The checkout package (CDG-152) has 78 unit tests using MemStorage but no integration tests that exercise real NATS, real Fossil, and real disk I/O. Need to verify the checkout → sync → checkout round-trip works end-to-end.

## Solution

Add `sim/checkout_test.go` with 4 integration tests that exercise checkout operations in realistic scenarios with real infrastructure.

## File

Single file: `sim/checkout_test.go`

No new packages. Uses existing `checkout`, `manifest`, `repo`, `sync`, `verify`, `agent` packages plus the `fossil` binary.

## Test 1: Checkout After Clone via NATS

**Goal:** Prove that after cloning through a real NATS leaf agent, checkout can extract files correctly.

**Flow:**
1. Start embedded NATS server
2. Leaf A: create repo with 2 checkins (initial + child with 3 files), start agent with `ServeNATSEnabled: true`
3. Leaf B: create empty repo with same project-code, start agent with `Pull: true`
4. Wait for convergence (Leaf B blob count matches Leaf A)
5. Stop both agents
6. Crosslink on Leaf B (`manifest.Crosslink` — sync transfers blobs but doesn't auto-populate derived tables like event/mlink/plink. Crosslink parses manifest blobs and rebuilds these tables, making pulled checkins visible to checkout operations.)
7. `checkout.Create(repo, dir, CreateOpts{})`, get version, `Extract(rid, ExtractOpts{})`
8. Read files from disk, assert byte-identical to Leaf A's content

**Key assertion:** Every file Leaf A committed is byte-identical on Leaf B's disk after extract. Comparison source: the original `[]manifest.File` content from the seed checkin (known at test setup time).

## Test 2: Commit → Sync → Update Round-Trip

**Goal:** Prove the full edit cycle: commit on A, sync to B, update B's checkout.

**Flow:**
1. Start embedded NATS
2. Leaf A: create repo with initial checkin (2 files), create checkout, extract. Start agent.
3. Leaf B: sync from A (wait for convergence), crosslink, create checkout, extract. Start agent.
4. Stop Leaf A's agent. On Leaf A: modify a file on disk (`os.WriteFile`), open checkout, `ScanChanges(ScanHash)`, `Commit(CommitOpts{...})`, close checkout. Restart agent.
5. Wait for Leaf B to receive the new commit (convergence)
6. Stop Leaf B's agent. Crosslink. Open checkout.
7. `CalcUpdateVersion()` — should return the new checkin RID (non-zero)
8. `Update(UpdateOpts{})` — writes updated files to disk
9. Read Leaf B's files, assert they match Leaf A's committed content

**Key assertions:**
- `CalcUpdateVersion` returns non-zero (newer version exists)
- After Update, Leaf B's working directory matches Leaf A's commit
- No merge conflicts (clean fast-forward update)

## Test 3: Concurrent Edit + Sync

**Goal:** Prove checkout operations don't corrupt the repo when sync is running concurrently.

**Flow:**
1. Start embedded NATS
2. Leaf A: create repo with initial checkin, start agent (push + serve)
3. Leaf B: create repo with same project-code and same initial content, create checkout, extract, start agent (push + pull)
4. Seed 5 additional checkins on Leaf A (to keep sync active on B)
5. While Leaf B's agent is actively syncing:
   - Write a new file to Leaf B's checkout directory
   - Open checkout (separate `repo.Open` handle — agent has its own)
   - `Manage` the new file, `ScanChanges`, `Commit`, close checkout
6. Wait for convergence (both repos have all blobs)
7. Stop both agents
8. Verify:
   - Leaf B's commit exists in its blob table
   - `verify.Verify` clean on both repos (no corruption)
   - Leaf A eventually received Leaf B's commit (bidirectional sync converged)

**Key assertions:**
- Commit succeeds while sync is running (no DB lock errors)
- `verify.Verify` clean on both repos
- Leaf B's commit syncs to Leaf A

**SQLite concurrency note:** Agent opens its own `*repo.Repo`, checkout opens a separate one. WAL mode allows concurrent readers + serialized writers. Checkout's Commit and agent's sync both write — they serialize via SQLite's write lock. This test proves that works without corruption. The test setup must ensure WAL mode is active (`PRAGMA journal_mode=WAL` — this is the default in our driver's `DefaultPragmas()` but should be verified).

**Why stop agent in Test 2 but not Test 3?** Test 2 stops the agent during commit to establish a clean baseline. Test 3 deliberately keeps the agent running to prove concurrent access is safe.

## Test 4: Fossil Interop (Both Directions)

**Goal:** Prove Go checkout reads Fossil-created repos AND Fossil reads Go-created checkouts.

**Requires:** `fossil` binary (skip if not available).

### Subtest A: Fossil → Go

1. `fossil new` repo
2. `fossil open`, write files, `fossil add`, `fossil commit`
3. Close Fossil checkout (`fossil close --force`)
4. `repo.Open` the Fossil-created repo
5. `checkout.Create`, get version, `Extract`
6. Read files — assert they match what Fossil committed
7. Modify a file on disk, `Manage`, `ScanChanges`, `Commit`
8. Close repo

### Subtest B: Go → Fossil

9. `fossil rebuild` on the repo (proves Go's commit is structurally valid)
10. `fossil open` in a new directory
11. Read files — assert they include Go's committed changes
12. `fossil artifact <go-commit-uuid>` — assert it parses as valid manifest

**Key assertions:**
- Go extracts Fossil-native files correctly
- Go's Commit survives `fossil rebuild` (no structural errors)
- Fossil can `open` a repo with Go-created checkins
- The Go-created manifest is valid Fossil format

## Shared Helpers

```go
// startNATS starts an embedded NATS server and returns its client URL.
// Cleanup is automatic via t.Cleanup.
func startNATS(t *testing.T) string

// createSeededRepo creates a repo with an initial checkin containing the
// given files. Returns the repo path (repo is closed after seeding).
func createSeededRepo(t *testing.T, dir, name string, files []manifest.File) string // returns path

// startLeafAgent creates and starts a leaf agent. Returns the agent
// (caller must defer Stop) and the repo path.
func startLeafAgent(t *testing.T, repoPath, natsURL string, opts agent.Config) *agent.Agent

// waitForBlobCount polls until the repo at path has at least minCount
// non-phantom blobs, or fails after timeout.
func waitForBlobCount(t *testing.T, repoPath string, minCount int, timeout time.Duration)

// readCheckoutFiles reads all files from a checkout directory into a
// map[relativePath]content. Skips .fslckout and _FOSSIL_.
func readCheckoutFiles(t *testing.T, dir string) map[string]string

// extractCheckout creates a checkout, extracts the current version, and
// returns the Checkout (caller must defer Close) and the checkout directory.
func extractCheckout(t *testing.T, r *repo.Repo, dir string) *checkout.Checkout
```

## Test Constraints

- Tests 1-3: no `fossil` binary required (pure Go + NATS)
- Test 4: skips if `fossil` binary not found
- Tests run as part of `make sim` (not `-short` mode)
- Convergence timeout: 30 seconds max per test
- Each test uses `t.TempDir()` for isolation
- Agents stopped via `defer agent.Stop()` + explicit stop before checkout operations

## Dependencies

- `go-libfossil/checkout` — Create, Extract, Commit, Update, ScanChanges, Manage
- `go-libfossil/manifest` — Checkin, Crosslink
- `go-libfossil/repo` — Create, Open
- `go-libfossil/verify` — Verify (corruption check in Test 3)
- `go-libfossil/sync` — sync protocol (used by agent)
- `leaf/agent` — leaf agent with NATS transport
- `nats-server` — embedded NATS
- `fossil` binary — interop tests

## Out of Scope

- DST-level fault injection (these are integration tests, not simulation)
- WASM/OPFS checkout (tested separately)
- Performance benchmarking
- Merge conflict resolution (Test 2 is a clean fast-forward)
