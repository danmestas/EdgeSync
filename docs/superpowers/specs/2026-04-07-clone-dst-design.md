# Clone DST: Deterministic Fault Injection for Clone()

**Date:** 2026-04-07
**Target repos:** go-libfossil (Buggify plumbing + fault sites), EdgeSync (DST test)
**Linear:** EDG-14

## Problem

`Clone()` uses its own transport loop, not `Tick()`. The DST framework can't inject faults mid-clone — no testing of phantom handling, pagination, or error cleanup under adversarial conditions. Clone is the most fragile sync operation (sequential delivery, phantom resolution, repo-deletion-on-failure), yet it has zero fault-injection coverage.

## Design Decisions

**Clone is not an agent concern.** Clone is a one-shot operation, not a continuous loop. The DST test calls `libfossil.Clone()` directly with a Buggify checker — no changes to Agent, Tick(), or the poll loop.

**Handler-backed transport, not MockFossil or SimNetwork.** The DST test wraps `HandleSyncWithOpts` in a `Transport` closure, passing server-side Buggify via `HandleOpts`. Simple, in-process, fully deterministic. No real `fossil serve`, no NATS, no network.

**Phase-targeted faults, not transport-level.** Clone has three phases with distinct failure modes. Buggify sites target each phase specifically. Transport-level faults (partitions, latency) are already proven for Sync via the shared transport layer — diminishing returns for Clone.

## Changes

### go-libfossil: Buggify Plumbing

**Add `Buggify` to `CloneOpts`** (`fossil.go`):

```go
type CloneOpts struct {
    User        string
    Password    string
    ProjectCode string
    ServerCode  string
    Observer    SyncObserver
    Buggify     BuggifyChecker  // NEW — zero-value = no faults
}
```

**Thread to internal `cloneSession`** (`internal/sync/clone.go`):

Internal `CloneOpts` gets a `Buggify BuggifyChecker` field. `cloneSession` stores it. `Clone()` in `fossil.go` passes `opts.Buggify` through to the internal call, same pattern as `Sync()`.

### go-libfossil: Fault Sites

Six sites across three phases. Rates follow existing conventions (2-10%).

**Pagination phase (seqno > 0):**

| Site | Location | Rate | Effect |
|------|----------|------|--------|
| `clone.emitCloneBatch.truncate` | handler.go, `emitCloneBatch()` | 10% | Truncate batch to half. Forces extra pagination rounds. |
| `clone.processResponse.dropFile` | clone.go, `processResponse()` | 5% | Skip storing a received file, creating a phantom. Tests phantom-from-pagination recovery. |

**Phantom resolution phase (seqno = 0):**

| Site | Location | Rate | Effect |
|------|----------|------|--------|
| `clone.buildRequest.dropGimme` | clone.go, `buildRequest()` | 5% | Skip last gimme card. Tests re-request on next round. |
| `clone.processResponse.dropSeqNo` | clone.go, `processResponse()` | 5% | Ignore `CloneSeqNoCard(0)` completion signal. Forces extra round before convergence. |

**Error cleanup:**

| Site | Location | Rate | Effect |
|------|----------|------|--------|
| `clone.processResponse.corruptHash` | clone.go, `processResponse()` | 2% | Flip byte in file content before storing. Tests hash verification catches corruption and repo file is deleted. |
| `clone.buildRequest.badLogin` | clone.go, `buildRequest()` | 5% | Corrupt login nonce. Tests auth failure recovery. |

### EdgeSync: DST Test

New file `dst/clone_test.go`.

**handlerTransport** — wraps `HandleSyncWithOpts` as a `Transport`:

```go
type handlerTransport struct {
    repo *libfossil.Repo
    opts libfossil.HandleOpts
}

func (t *handlerTransport) RoundTrip(ctx context.Context, payload []byte) ([]byte, error) {
    return t.repo.HandleSyncWithOpts(ctx, payload, t.opts)
}
```

**TestCloneDST** — single-seed test:

1. Create server repo, seed with 50 blobs across multiple checkins
2. Build `handlerTransport` with server-side Buggify (seed + 1000)
3. Call `libfossil.Clone()` with client-side Buggify (seed + 2000)
4. On success: assert blob convergence, crosslinks valid, no phantoms
5. On failure (corruptHash): assert repo file deleted, error mentions hash mismatch

**TestCloneDSTSeedSweep** — run across 20 seeds as subtests. Different seeds exercise different fault combinations deterministically.

**Invariant checks:**

After successful clone:
- Blob convergence: every blob in server repo exists in cloned repo with matching UUID and content
- Crosslinks valid: checkin events and mlink entries exist
- No phantoms: `SELECT COUNT(*) FROM phantom` = 0

After failed clone (expected from corruptHash):
- Repo file deleted: `os.Stat(clonePath)` returns `os.ErrNotExist`
- Error contains hash mismatch indication

## What Does NOT Change

- **Agent** — no clone mode, no EventClone, no Tick() changes
- **SimNetwork / PeerNetwork / FaultSchedule / MockFossil** — untouched
- **Existing clone tests** — `TestCloneViaHandler*` in go-libfossil stay as-is (unit tests without faults)
- **Public `Clone()` return type** — same `(*Repo, *CloneResult, error)`
- **Wire protocol** — no new cards, no format changes

## Testing

The DST test *is* the deliverable. Success criteria:

1. `TestCloneDSTSeedSweep` passes across 20 seeds — clone converges or fails cleanly under faults
2. Existing `TestCloneViaHandler` and `TestCloneViaHandlerMultipleCheckins` still pass (no regression from Buggify plumbing)
3. Existing DST sync tests still pass (no handler changes affect sync)
