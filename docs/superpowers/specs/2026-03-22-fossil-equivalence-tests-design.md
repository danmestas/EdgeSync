# Spec: Fossil-Equivalent Validation Tests

**Date:** 2026-03-22
**Branch:** fix/checkin-mark-file-blobs-unsent (PR #19)
**Status:** Draft

## Problem

The file blob marking bug (`manifest.Checkin` stored file blobs but didn't mark them for sync) slipped through the entire test suite because every test layer that exercises sync bypasses `manifest.Checkin()`:

| Test Layer | How blobs are created | Gap |
|-----------|----------------------|-----|
| `sim/serve_test.go` | `SeedLeaf()` → `blob.Store()` | Skips Checkin entirely |
| `dst/scenario_test.go` | `MockFossil.StoreArtifact()` → `blob.Store()` | Skips Checkin entirely |
| `dst/e2e_test.go` | Direct `blob.Store()` in WithTx | Skips Checkin entirely |
| `go-libfossil/manifest/manifest_test.go` | `manifest.Checkin()` | No sync verification |
| `go-libfossil/sync/sync_test.go` | Pre-stored blobs | Assumes blobs already marked |

No test bridges the gap: **create a real commit with files → sync → verify files are readable on the other side**.

## Design

### Principle

The leaf agent should be treated as a **fully functional fossil equivalent** for its implemented feature set. We validate this claim by round-tripping through the actual `fossil` binary: create commits via `manifest.Checkin()`, sync between repos, then `fossil open` + verify file contents on disk.

### File

`sim/equivalence_test.go` — all tests and helpers in one file.

### Topologies

Three sync topologies, each proving a different path through the system:

**Topology A — Leaf → Fossil (via clone):**
1. Create a leaf repo, call `manifest.Checkin()` with files
2. Serve the leaf repo over HTTP via `sync.ServeHTTP`
3. `fossil clone` from the leaf's HTTP endpoint into a fresh repo
4. `fossil open` the cloned repo into a temp dir
5. Assert file contents on disk match what was checked in

Note: `fossil clone` handles crosslinking internally — no explicit `manifest.Crosslink` call needed.

This tests: Checkin marks blobs correctly → ServeHTTP serves them → fossil binary can consume them.

**Topology B — Fossil → Leaf:**
1. Create a fossil repo with `fossil init`, `fossil open`, write files, `fossil commit`
2. Start `fossil serve` on the repo (with `nobody` user granted read capabilities — `fossil user new nobody "" cghijknorswz -R <repo>`)
3. Create a leaf repo via `sync.Clone` from the fossil serve endpoint
4. `fossil open` the leaf's repo into a temp dir
5. Assert file contents match

Note: `sync.Clone` calls `manifest.Crosslink` internally, so the cloned repo is ready for `fossil open`. The `nobody` user with capabilities is required because `sync.Clone` connects unauthenticated.

This tests: fossil binary produces valid repos → leaf's Clone correctly ingests them → files are intact.

**Topology C — Leaf → Leaf:**
1. Create leaf A repo, `manifest.Checkin()` with files
2. Create leaf B repo (empty, same project/server codes)
3. Serve leaf A over HTTP via `sync.ServeHTTP`
4. `sync.Sync` leaf B from leaf A via `HTTPTransport`
5. Run `manifest.Crosslink` on leaf B's repo (needed because `sync.Sync`, unlike `sync.Clone`, does not crosslink — leaf B has blobs but no event/mlink/leaf table entries)
6. `fossil open` leaf B's repo into a temp dir
7. Assert file contents match

This tests: Checkin marks blobs → Go sync engine transfers them → receiving repo is a valid fossil repo.

### Commit Complexity

Each topology runs three sub-tests:

| Sub-test | Checkin | Validates |
|----------|---------|-----------|
| `single_file` | 1 commit, 1 file (`hello.txt` → `"hello world"`) | Base case: manifest + 1 file blob |
| `multi_file` | 1 commit, 3 files | All file blobs marked, not just the first |
| `commit_chain` | 2 commits (parent→child), child adds a file | Parent-child chain, both commits' files accessible at tip |

For `commit_chain`, `fossil open` checks out the tip (child commit). Verify both the parent's original files and the child's new file are present.

### Helpers

**`fossilCheckout(t, repoPath string) string`**
- Copies repo to a temp path (fossil open modifies the DB)
- Runs `fossil open <repo>` in a temp working directory
- Returns the working directory path
- `t.Cleanup` removes it

**`assertFiles(t, dir string, expected map[string]string)`**
- For each entry: reads file at `dir/name`, compares to expected content
- Fails with clear diff on mismatch or missing file

**`fossilInit(t) string`** (for Topology B)
- Runs `fossil init` into a temp file
- Returns the repo path

**`fossilCommit(t, repoDir string, files map[string]string)`** (for Topology B)
- Writes files to the working directory
- Runs `fossil add` + `fossil commit`

### What This Would Have Caught

The original bug: `manifest.Checkin` stored file blobs via `blob.Store()` but didn't mark them as unclustered/unsent.

- Topology A: `fossil clone` would succeed (manifest blob was marked), but `fossil open` would fail or produce empty files — file blobs never arrived because they weren't marked for sync.
- Topology C: `sync.Sync` would transfer the manifest but not the file blobs. `fossil open` would fail with missing blob errors.

The test failure: `assertFiles` would report "expected hello.txt to contain 'hello world', got: file not found" or fossil would error with "missing content for artifact".

### Dependencies

- `fossil` binary on PATH (same requirement as existing `sim/serve_test.go`)
- `sync.ServeHTTP` for leaf HTTP serving
- `manifest.Checkin` for creating commits
- `manifest.Crosslink` for post-sync event linkage (Topology C only — `sync.Sync` does not crosslink; `sync.Clone` and `fossil clone` handle it internally)
- `sync.Sync` + `HTTPTransport` for leaf→leaf
- `sync.Clone` + `HTTPTransport` for fossil→leaf

### Test Gating

These tests require the `fossil` binary. Use the same skip pattern as `sim/serve_test.go`:

```go
if _, err := exec.LookPath("fossil"); err != nil {
    t.Skip("fossil binary not found")
}
```

### Non-Goals

- Testing UV sync through fossil binary (UV has its own test coverage)
- Testing delta manifests specifically (Checkin tests cover this)
- Adding a `Commit()` API to the agent (agent is a sync daemon, not a commit tool)
- Replacing existing sim/DST tests (those test fault tolerance; these test correctness)
