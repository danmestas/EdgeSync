# Fossil Interop Test Suite — Design Spec

**Date:** 2026-03-25
**Status:** Draft
**Ticket:** CDG-182 (parent), follow-on hardening
**Location:** `sim/interop_test.go`

## Problem

EdgeSync's test suite was entirely self-referential — Go-created data verified by Go code. Three bugs survived undetected:

1. **Delta copy command args swapped** (`offset@count` instead of `count@offset`) — Create and Apply matched each other but not Fossil
2. **Delta checksum wrong algorithm** (Fletcher-like uint16 instead of big-endian uint32 word sum) — only visible on data > 256 bytes
3. **CFile usize validation rejected delta cards** — `usize` is the fully expanded content size, not the decompressed delta size

All three would have been caught instantly by cross-checking against the real `fossil` binary.

## Solution

A comprehensive interop test suite in `sim/interop_test.go` that uses the Fossil CLI as an oracle. Two tiers:

- **Tier 1 (fast):** Seed repos created per-test. Always runs, CI-safe, <15s total.
- **Tier 2 (large repo):** Uses the Fossil SCM repo (66K artifacts). Skipped in `-short` mode.

## Test Structure

```
TestInterop/
├── delta_codec/
│   ├── fossil_creates_we_apply
│   ├── we_create_fossil_applies
│   └── round_trip_large_payload
├── clone_from_us/
│   ├── single_commit
│   ├── commit_chain
│   └── with_uv_files
├── clone_from_fossil/
│   ├── single_commit
│   ├── commit_chain_with_deltas
│   └── expand_and_verify_all
├── incremental_sync/
│   ├── fossil_commits_we_pull
│   ├── we_push_fossil_pulls
│   └── bidirectional
├── hash_compat/
│   ├── sha1_vs_fossil
│   └── sha3_vs_fossil
└── large_repo/                     (Tier 2, skipped in -short)
    ├── clone_and_expand_all
    └── verify_hash_integrity
```

## Test Descriptions

### `delta_codec/`

Tests that our delta Create/Apply are bit-compatible with Fossil's delta.c.

**`fossil_creates_we_apply`**: Write source and target files to disk. Run `fossil test-delta-create` to produce a delta. Read the delta bytes, call `delta.Apply(source, fossilDelta)`, assert output == target.

**`we_create_fossil_applies`**: Call `delta.Create(source, target)`, write to disk. Run `fossil test-delta-apply`. Assert Fossil's output == target.

**`round_trip_large_payload`**: Same as above with 64KB+ payloads. Stresses the checksum algorithm (the uint16 bug only manifested above 256 bytes). Test with both directions.

### `clone_from_us/`

Tests that real `fossil clone` can consume our Go-served repos.

**`single_commit`**: Create a Go repo, store a checkin manifest + file blobs via `manifest.Checkin`. Serve with `sync.ServeHTTP`. Run `fossil clone` against it. Run `fossil rebuild` on the clone. Assert zero errors from rebuild. Run `fossil open` + verify file content matches.

**`commit_chain`**: Same but with 5+ sequential commits. Forces Fossil's `content_deltify` during rebuild, creating delta chains. Then `fossil rebuild` — if our blob format is wrong, rebuild catches it.

**`with_uv_files`**: Store UV files including binary content (random 4KB bytes). Clone, then `fossil uv list` + `fossil uv cat` to verify content. Tests UV wire format and binary encoding.

### `clone_from_fossil/`

Tests that our `sync.Clone` can consume real Fossil-served repos. This is where the cfile delta bug lived.

**`single_commit`**: `fossil init` + `fossil commit` + `fossil serve`. `sync.Clone` against it. Call `verifyAllBlobs` on the resulting repo.

**`commit_chain_with_deltas`**: `fossil init` + 5 sequential `fossil commit` calls. `fossil serve`. `sync.Clone`. `verifyAllBlobs`. This exercises the cfile path with delta-compressed artifacts.

**`expand_and_verify_all`**: Same as commit_chain but with larger files (4KB+) to ensure delta chains are created. After clone, expand every blob and verify hash matches UUID. Also run `fossil rebuild -R` on the Go-created repo file to let Fossil itself validate our storage format.

### `incremental_sync/`

Tests sync after initial clone — the production steady-state.

**`fossil_commits_we_pull`**: Clone from Fossil, then make 3 new `fossil commit`s on the Fossil side. Run `fossil sync` against our Go server (which has the clone). Verify the 3 new artifacts arrived and `verifyAllBlobs` passes.

**`we_push_fossil_pulls`**: Clone from Fossil. Store 3 new blobs in the Go repo. Run `fossil sync` from the Fossil side. Verify `fossil artifact` can retrieve the new blobs. Run `fossil rebuild`.

**`bidirectional`**: Both sides commit after initial clone. Sync until convergence. Both sides `fossil rebuild`. `verifyAllBlobs` on Go side.

### `hash_compat/`

Tests that our hash functions produce identical output to Fossil's.

**`sha1_vs_fossil`**: Generate 10 random payloads (various sizes: 0, 1, 255, 256, 1024, 64KB). For each: compute `hash.SHA1(data)`, write data to file, run `fossil sha1sum`, assert match.

**`sha3_vs_fossil`**: Same with `hash.SHA3` and `fossil sha3sum`. This is currently untested — SHA3 was only validated against Go test vectors, never against Fossil.

### `large_repo/`

Tier 2 tests using the real Fossil SCM repo. Skipped in `-short` mode.

**`clone_and_expand_all`**: If `testdata/fossil.fossil` doesn't exist, clone from `fossil-scm.org/home` (skips if network unavailable). Open the repo with `db.Open`. Call `verifyAllBlobs` — expand every non-phantom blob via `content.Expand`, hash it, assert matches UUID. This is 66K blobs including chains up to depth 2547.

**`verify_hash_integrity`**: Same repo. For a random sample of 500 blobs, also compare expanded content against `fossil artifact -R` output. This cross-checks our Expand + Decompress against Fossil's own expansion.

## Shared Helpers

### `verifyAllBlobs(t *testing.T, d *db.DB)`

The most important function in the suite. Opens a repo and verifies every non-phantom blob:

```
SELECT rid, uuid, size FROM blob WHERE size >= 0 AND content IS NOT NULL
```

For each row:
1. `content.Expand(d, rid)` — walks delta chain, decompresses
2. `hash.SHA1(expanded)` or `hash.SHA3(expanded)` based on UUID length
3. Assert computed hash == stored UUID

Fails with details: rid, uuid, chain depth, expected vs actual hash.

### `verifyWithFossilRebuild(t *testing.T, repoPath string)`

Runs `fossil rebuild -R <repoPath>` and asserts zero errors. Fossil's rebuild recomputes every content hash from scratch — the ultimate integrity check.

### `fossilExec(t *testing.T, args ...string) string`

Shared helper to run Fossil CLI commands. Skips test if `fossil` not in PATH. Returns stdout, fails test on non-zero exit.

### `cloneFossilSCMRepo(t *testing.T) string`

Returns path to `testdata/fossil.fossil`, cloning from upstream if needed. Skips if offline or clone fails.

## Tier 2 Self-Provisioning

The large repo tests use `testdata/fossil.fossil` (gitignored by `*.fossil` pattern). On first run:

1. Check if file exists
2. If not, attempt `fossil clone https://fossil-scm.org/home testdata/fossil.fossil`
3. If clone fails (offline, timeout), skip the test gracefully
4. File persists across runs (~45MB)

## Integration with CI

- `make test` runs with `-short` — Tier 1 only, <15s additional
- `make test-full` (or equivalent) runs without `-short` — includes Tier 2
- Pre-commit hook runs Tier 1 only (keeps the ~8s budget)
- All tests require `fossil` binary in PATH (skip gracefully if absent)

## What This Catches

| Bug class | How caught |
|---|---|
| Delta format mismatch | `delta_codec/fossil_creates_we_apply` |
| Checksum algorithm wrong | `delta_codec/round_trip_large_payload` + `verifyAllBlobs` |
| CFile usize for deltas | `clone_from_fossil/commit_chain_with_deltas` |
| SHA3 hash mismatch | `hash_compat/sha3_vs_fossil` |
| Blob compression format | `verifyWithFossilRebuild` after every clone |
| Deep delta chain corruption | `large_repo/clone_and_expand_all` |
| Incremental sync corruption | `incremental_sync/bidirectional` |
| UV binary content encoding | `clone_from_us/with_uv_files` |

## Non-Goals

- Performance benchmarking (separate concern, CDG-137)
- Concurrent/stress testing (covered by DST)
- Network fault injection (covered by sim fault proxy)
- WASM compatibility (separate test tier)
