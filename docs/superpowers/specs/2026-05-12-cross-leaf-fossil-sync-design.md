# Cross-Leaf Fossil Sync Over NATS — EdgeSync PR Plan

**Date**: 2026-05-12
**Tracking**: EdgeSync #156
**Dependency**: libfossil `CreateOpts.ProjectCode` (separate upstream issue, expected `v0.6.1`)

## Context

EdgeSync issue #156 documents two gaps that block cross-process Fossil
sharing for sesh:

1. The hub's NATS sync subject is keyed by per-repo `project-code`.
   Each leaf creates a fresh repo → fresh code → siblings subscribe to
   different subjects → commits don't propagate.
2. The sync handler is subscribe-only — `Repo.Commit` never publishes
   anything, so peers only learn of new commits by explicit polling.

The fix is two coordinated changes, both EdgeSync-side (once the
upstream libfossil PR lands):

- **(a) Shared project-code.** New `Config.ProjectCode` for roots that
  want to declare their identity, plus `Config.SeedFromUpstream` for
  children that should inherit it by cloning at bootstrap time.
- **(b) Notification on commit.** `Repo.Commit` publishes a
  `{rid, uuid}` JSON message on a sibling `.commit` subject after the
  underlying libfossil commit succeeds. Subscribers on that subject
  pull via the existing `.sync` request/reply path.

## Goal

A single self-contained EdgeSync PR that, on green CI, makes the sesh
integration tests (`TestSubLeaf_DoesNotSyncToday`,
`TestHub_DoesNotAccumulateProjectCommitsToday`) flippable from
"asserts no propagation" to "asserts propagation within N seconds."

## In scope

- Bump `github.com/danmestas/libfossil` pin to `v0.6.1`
- `Config.ProjectCode` — explicit, optional, validated (re-uses
  upstream's `^[0-9a-f]{40}$` constraint via Create's error path)
- `Config.SeedFromUpstream` — URL of an upstream hub's HTTP xfer
  endpoint; when the local repo doesn't yet exist, clone before open
- Auto-publish on commit via the hub's `Repo.Commit` wrapper
- New `.commit`-subject subscriber in the hub that triggers an xfer
  pull on receipt
- Integration test: two `NewHub` instances, shared project-code, on a
  bridged NATS topology; commit on A appears on B within ~2s

## Out of scope (deferred)

- NATS-native clone (would need upstream `HandleSync` clone support —
  CDG-148; HTTP seed is the fast path).
- Leaf agent changes — its existing sync already works for its tier;
  any cross-leaf propagation it needs comes via the hub. Filed as a
  follow-up if integration testing reveals a gap.
- Updating `leaf/` submodule pin — separate change, not required for
  this PR to ship.
- Multi-responder `.sync` race-condition hardening (see Q3 below).

## Design

### 1. `Config.ProjectCode`

```go
type Config struct {
    // ... existing fields ...

    // ProjectCode optionally pins the repo's project-code on first
    // create. When the repo at RepoPath already exists, this value
    // (if non-empty) must match the on-disk project-code or NewHub
    // returns an error — guards against silent topology drift where
    // a caller fixes their config but an old repo is still around.
    // Empty preserves current behavior: generate on create, accept
    // whatever's on disk on open.
    ProjectCode string
}
```

Threading: `openOrCreateRepo(path, bootstrapUser, projectCode)`. On the
create branch, passes through to `libfossil.Create(path, CreateOpts{
User: bootstrapUser, ProjectCode: projectCode})`. On the open branch,
if `projectCode != ""`, read `Config("project-code")` and compare;
error on mismatch.

### 2. `Config.SeedFromUpstream`

```go
type Config struct {
    // SeedFromUpstream, when set and RepoPath doesn't yet exist on
    // disk, clones from this URL before opening. The cloned repo
    // carries the upstream's project-code, so children declaring a
    // SeedFromUpstream don't need to also set ProjectCode.
    //
    // URL is an EdgeSync hub HTTP xfer endpoint
    // (e.g. "http://hub.local:8080/"). NATS-native clone is deferred
    // until libfossil HandleSync supports the clone protocol.
    SeedFromUpstream string
}
```

If both `ProjectCode` and `SeedFromUpstream` are set and the clone
result's project-code disagrees with `ProjectCode`, fail.

### 3. Auto-publish on commit

In `hub.Repo.Commit` (file: `hub/repo.go:129`), after the underlying
`libfossil.Repo` commit returns successfully:

- If the parent `Hub` has a NATS client and a `commitSubject`,
  publish a JSON message: `{"rid": <int64>, "uuid": "<lowercase hex>"}`.
- Best-effort: log publish failures, do not fail the commit. The repo
  is the source of truth; downstream is eventually consistent.

This requires `hub.Repo` to know about the Hub's NATS client. Two
shapes:

- **(a)** Add a back-reference: `Repo.publish func(rid, uuid)`,
  wired by `NewHub` when constructing the wrapper.
- **(b)** Move the auto-publish responsibility to a Hub-level method
  that wraps Commit (`Hub.Commit(ctx, opts) (RevID, error)`).

Lean (a) — keeps the existing `Hub.Repo()` API surface unchanged and
lets direct callers of `Repo.Commit` benefit from publish without
going through Hub. `OpenRepo` (the standalone path) leaves
`publish == nil`, which the wrapper treats as "no-op."

### 4. `.commit` subscriber

New method `startCommitSubscriber(cfg Config)`, invoked from `NewHub`
right after `startFossilSyncSubscriber`. Subscribes to
`<prefix>.<project-code>.commit`. On message:

- Decode `{rid, uuid}`.
- If our local repo already has that uuid (cheap `Config` /
  manifest lookup), drop.
- Else trigger an xfer pull on `<prefix>.<project-code>.sync`, using
  a NATS transport adapter. This is the request/reply path that
  already exists on the receive side — we just need a *requester*.

The requester is the new plumbing in this PR. Likely a small adapter
that implements `libfossil.Transport` over NATS request/reply, then
calling `h.repo.handle.Sync(ctx, SyncOpts{...})` against it. See
Q2 below — there may be reusable bits in `leaf/agent`.

### 5. Integration test

`hub/cross_leaf_test.go` (new):

- `cfgA, cfgB`: distinct `RepoPath`, same `ProjectCode`,
  `LeafUpstream` wired so B solicits a connection to A (or both join a
  third NATS for the test — see Q1).
- `hubA, _ := NewHub(ctx, cfgA)` + serve.
- `hubB, _ := NewHub(ctx, cfgB)` + serve. SeedFromUpstream points at A.
- Commit on A: `rid, _ := hubA.Repo().Commit(ctx, ...)`.
- Assert eventually (≤2s, 50ms poll) that B has a commit with the
  same uuid.
- Reverse direction: commit on B, assert A sees it.

Skip via `t.Skip` if the host can't open two ephemeral ports — same
pattern as existing hub tests.

## Open questions

- **Q1 — Test topology.** Two hubs, one NATS bridge? Easiest:
  `hubB` configured with `LeafUpstream: hubA.LeafURL()` so the
  embedded NATS servers are linked. Alternative: spin a third
  standalone NATS server, both connect as clients. Lean leaf-link —
  matches production topology and exercises the leafnode path.

- **Q2 — Pull-via-NATS machinery.** `leaf/agent/code_artifact.go`
  has `Agent.Sync` / `Agent.SyncTo` that pull from a hub URL — but
  it's HTTP, not NATS request/reply on a subject. I haven't found a
  reusable NATS-pull adapter; likely need to write one in `hub/`
  (`natsTransport` implementing `libfossil.Transport.Send`). Confirm
  this isn't already done somewhere I missed before I write it from
  scratch.

- **Q3 — `.sync` multi-responder race.** If multiple peers subscribe
  to the same project-code's `.sync` subject (which is exactly what
  this PR enables), `nc.Subscribe` puts them all in the deliver-set
  and they all respond. `nc.Request()` returns the first reply;
  if the first responder is a peer that doesn't have the data yet,
  the puller sees a stale/empty response. For the 2-node test this
  isn't a problem (only one responder has the data). For 3+
  topologies we'll want either queue-group routing or per-peer
  reply subjects. Flag for a follow-up, don't block this PR.

## File touch list

- `hub/hub.go` — Config fields, `startCommitSubscriber`, hook into
  `NewHub`, `openOrCreateRepo` signature.
- `hub/repo.go` — `Repo.publish` hook, `Commit` augmentation.
- `hub/clone.go` *(new)* — `seedFromUpstream` helper invoked by
  `NewHub` before `openOrCreateRepo` when the path is absent.
- `hub/nats_transport.go` *(new, if Q2 lands here)* —
  `libfossil.Transport` over NATS request/reply.
- `hub/cross_leaf_test.go` *(new)* — integration test.
- `go.mod` — libfossil pin → `v0.6.1`.
- `docs/architecture/sync-protocol.md` — note the new subjects
  (`.sync` and `.commit`) and the propagation model.

## Tasks (in order)

1. **Blocked**: upstream libfossil `CreateOpts.ProjectCode` lands and
   `v0.6.1` is cut.
2. Bump `go.mod` pin; `go mod tidy`; verify compile.
3. `Config.ProjectCode` plumbing through `openOrCreateRepo`; tests for
   mismatch on existing repo.
4. `Config.SeedFromUpstream` + `hub/clone.go`; test against an HTTP
   xfer endpoint (stand up a transient hub in the test).
5. `Repo.publish` hook + `Commit` augmentation; unit test that a
   commit produces a `.commit` message with correct payload.
6. `startCommitSubscriber`; integration test the receive side
   (publish a `.commit`, observe an xfer pull).
7. NATS Transport adapter (Q2 outcome).
8. Cross-leaf integration test (the gold-path one).
9. `docs/architecture/sync-protocol.md` update.
10. `make` / `go test ./...` / replicate CI locally per global policy.
11. Push, open PR, wait for human merge.

## Sesh side (out of this PR, follow-up by sesh-maintainer)

Once this PR ships and sesh bumps its EdgeSync pin:

- Set `Config.ProjectCode` from a deterministic seed
  (e.g. SHA1 of project root path, lowercased hex, first 40 chars) on
  the sesh project.repo.
- Set `Config.SeedFromUpstream` on the hub.repo so the hub inherits
  the project-code at bootstrap.
- Flip the two integration tests in `sesh#11` from
  `TestSubLeaf_DoesNotSyncToday` to
  `TestSubLeaf_SyncsToday` (and the hub equivalent).
