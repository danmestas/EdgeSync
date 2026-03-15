# go-libfossil Design Spec

Pure-Go reimplementation of libfossil's API surface. Lives at `EdgeSync/go-libfossil/` as its own Go module (`github.com/dmestas/edgesync/go-libfossil`), eventually extracted to a standalone repo.

## Constraints

- Pure Go, no CGo
- SQLite via `modernc.org/sqlite` with FTS5 enabled
- Vector search deferred to Phase P
- Every operation must produce `.fossil` files that `fossil rebuild --verify` accepts
- Behavioral equivalence validated by fossil CLI as test oracle
- Performance: compute ops within 3x of C libfossil, I/O ops within 5x
- Regression tracking: Go baselines recorded, no operation regresses beyond 10% from its own prior baseline
- Strict TDD red-green — failing test first, then implement, then validate

## Reference Source

- Amalgamation: `~/fossil/libfossil-amalgamation/libfossil.{c,h}` (46K + 24K lines, ~778 exported functions)
- Full source: `~/projects/EdgeSync/libfossil/checkout/src/` (52K lines across 60+ files)
- Headers: `~/projects/EdgeSync/libfossil/checkout/include/fossil-scm/`
- Fossil CLI: `/opt/homebrew/bin/fossil` (v2.28)
- Existing delta port: `~/projects/EdgeSync/pkg/delta/delta.go` (copy-and-adapt into new module, do not import EdgeSync)

## Project Structure

```
go-libfossil/
  go.mod
  go.sum
  README.md
  Makefile
  types.go              # FslID, FslSize, result code constants
  errors.go             # error types matching FSL_RC_* codes
  julian.go             # Julian day <-> time.Time conversion
  db/
    db.go               # open, close, exec, prepared statement cache
    schema.go           # repo schema creation (repo1 static + repo2 transient)
    db_test.go
  hash/
    hash.go             # SHA1 + SHA3-256 content addressing
    hash_test.go
  delta/
    delta.go            # delta encode/decode
    delta_test.go
  blob/
    blob.go             # store, retrieve, exists
    compress.go         # zlib compress/decompress
    blob_test.go
  content/
    content.go          # delta chain expansion, phantom tracking, verification
    content_test.go
  repo/
    repo.go             # create, open, close, verify
    repo_test.go
  deck/                 # Phase B
    deck.go
    deck_test.go
  checkout/             # Phase B
    checkout.go
    checkout_test.go
  tag/                  # Phase C
  branch/               # Phase D
  diff/                 # Phase E
  merge/                # Phase F
  annotate/             # Phase G
  wiki/                 # Phase H
  ticket/               # Phase I
  forum/                # Phase J
  search/               # Phase K
  glob/                 # Phase L
  xfer/                 # Phase M
  config/               # Phase N
  zip/                  # Phase O
  testutil/
    testutil.go         # fossil CLI wrappers, temp repo creation, validation helpers
  bench/
    bench_test.go       # Go benchmarks
    cbench/             # C benchmark runner against libfossil amalgamation, outputs JSON
    Makefile            # builds cbench from amalgamation, runs both harnesses, compares
```

Package dependency graph (not strictly linear):
- root types (no deps)
- db -> root types
- hash (no deps)
- delta (no deps)
- blob -> db, hash, delta
- content -> db, blob, delta
- repo -> db, blob, content, hash

No circular imports. testutil depends on repo (for OpenWithGo) and shells out to `fossil` binary.

## Design Decisions

### Type widening: FslID is int64, not int32

C libfossil defines `fsl_id_t` as `int32_t` for historical reasons. We use `int64` because:
- SQLite stores INTEGER as up to 8 bytes; Go's `database/sql` naturally returns `int64`
- Avoids artificial limits for large repos
- No behavioral difference since Fossil repo RIDs fit in int32 today but int64 is future-proof

### FslSize uses int64, not uint64

C libfossil defines `fsl_size_t` as `uint64_t`, but the `blob.size` column stores `-1` for phantom blobs. Using `int64` in Go allows representing the phantom sentinel without unsafe casting. The `PhantomSize` constant is defined as `-1`.

### Context decomposition

C libfossil organizes around a monolithic `fsl_cx` context object holding DB connections, caches, error state, and config. In Go, this is decomposed:
- `repo.Repo` is the top-level handle (owns the DB connection, wraps cross-cutting state)
- `db.DB` holds the connection and prepared statement cache
- Error propagation uses standard Go error returns, not context-stored error state
- Transaction scoping: `repo.Repo` provides `WithTx(func(tx) error)` that threads the transaction through blob/content operations

### Timestamps: Julian day numbers

Fossil stores all timestamps as Julian day numbers (float64). The root package provides `JulianToTime(float64) time.Time` and `TimeToJulian(time.Time) float64`. The D-card datetime format, `event.mtime`, and `rcvfrom.mtime` all use Julian days.

## Testing Architecture

### Layer 1: Unit Tests

Standard Go table-driven tests. Every exported function gets tests. Naming: `TestDeltaApply`, `TestBlobStore`, etc.

### Layer 2: Fossil CLI Validation

Integration tests using `testutil` package that wraps the `fossil` binary:

```go
package testutil

func NewTestRepo(t *testing.T) *TestRepo
func (r *TestRepo) FossilRebuild(t *testing.T)
func (r *TestRepo) FossilArtifact(t *testing.T, uuid string) []byte
func (r *TestRepo) FossilSQL(t *testing.T, sql string) string  // uses fossil sql -R <path>
func (r *TestRepo) OpenWithGo(t *testing.T) *repo.Repo
```

Pattern: perform operation with Go, then `fossil rebuild --verify`, then query via `fossil` CLI to confirm semantic correctness. Naming: `TestDeltaApply_FossilValidation`.

Note: `fossil sql` requires `-R <path>` flag to operate on a repo file without a checkout directory. All testutil methods pass this flag.

### Layer 3: Benchmarks

Each operation gets a Go benchmark (`BenchmarkDeltaApply`) and a corresponding C benchmark calling libfossil.

C benchmark harness (`bench/cbench/`):
- Built from the libfossil amalgamation (`~/fossil/libfossil-amalgamation/`)
- Compiled via `bench/Makefile` using the system C compiler
- Each benchmark function outputs JSON: `{"op": "delta_apply", "input_size": 10000, "ns": 4523}`
- Go harness parses this output and compares against `go test -bench` results

Results written to `bench/baseline.json` for regression tracking.

Thresholds:
- Compute (delta, hash): Go within 3x of C
- I/O (SQLite queries): Go within 5x of C
- Regression: no operation regresses beyond 10% from its own Go baseline

## Validation Protocol

### Per-generation (every code change):

1. `go vet ./...` — no issues
2. `go test ./...` — all pass
3. `go test -race ./...` — no data races
4. All `_FossilValidation` integration tests pass

### Per-package completion:

1. All per-generation checks
2. `go test -bench=. ./...` — benchmarks recorded to baseline
3. C benchmark counterpart run, relative threshold checked
4. `go test -cover ./...` — coverage reported

### Per-phase completion:

1. All per-package checks
2. Full round-trip integration test: Go creates repo -> fossil operates on it -> Go reads it back
3. Benchmark summary: all operations within threshold
4. `fossil rebuild --verify` on every test repo produced during the suite

No failing test is commented out or deferred. Fix before writing new code.

## Phase A: Repository Fundamentals

Build order:

### A1. Root Package (types.go, errors.go, julian.go)

Port from `config.h` and `core.h`:
- `FslID` (int64) — database record IDs (deliberately widened from C's int32, see Design Decisions)
- `FslSize` (int64) — sizes, with -1 sentinel for phantoms (see Design Decisions)
- `PhantomSize` constant (-1)
- Result code constants matching `FSL_RC_*`
- Error types wrapping result codes
- `JulianToTime` / `TimeToJulian` conversion functions

### A2. db

Port from `db.c`. Thin wrapper over `modernc.org/sqlite`:
- Open/close fossil repo database files
- `PRAGMA application_id=252006673` set on creation (identifies file as Fossil repo)
- Prepared statement cache
- Transaction helpers (begin, commit, rollback)
- Create full repo schema matching Fossil's expectations:
  - **repo1 (static)**: `blob`, `delta`, `rcvfrom`, `user`, `config`, `shun`, `private`, `reportfmt`, `concealed` — plus seed INSERT into `rcvfrom` and required indexes
  - **repo2 (transient)**: `filename`, `mlink`, `plink`, `leaf`, `event`, `phantom`, `orphan`, `unclustered`, `unsent`, `tag`, `tagxref`, `backlink`, `attachment`, `cherrypick` — plus seed tag rows (1-11) and indexes
  - Schema SQL sourced from `libfossil/checkout/src/schema_repo1_cstr.c` and `schema_repo2_cstr.c`, embedded as Go string constants
- Validation: create schema with Go, query with `fossil sql -R <path>`

### A3. hash

Port from `sha1.c` and `sha3.c`:
- `SHA1([]byte) string` — returns lowercase hex
- `SHA3([]byte) string` — SHA3-256, lowercase hex
- Fossil's specific formatting (no prefix, lowercase)
- Validation: compare output to `fossil sha1sum` / `fossil sha3sum`

### A4. delta

Port from `delta.c` (~670 lines in libfossil). Copy-and-adapt existing EdgeSync `pkg/delta/delta.go` into new module:
- `Create(origin, target []byte) []byte`
- `Apply(origin, delta []byte) ([]byte, error)`
- `Apply` must be byte-exact: given the same origin and delta bytes, output must match C exactly
- `Create` must produce valid deltas that fossil can apply; byte-exactness with C is a stretch goal, not a hard requirement (the algorithm is deterministic but internal parameters like hash table sizing may produce functionally equivalent but different output)
- Validation: create with Go and apply with fossil (and vice versa); verify round-trip correctness

### A5. blob

Port from parts of `content.c` and `repo.c`:
- `Store(db, content []byte) (FslID, error)` — hash, zlib compress, insert into `blob` table
- `Load(db, rid FslID) ([]byte, error)` — fetch, decompress
- `Exists(db, uuid string) (FslID, bool)`
- Delta storage: store as delta referencing a source blob
- Phantom blobs: insert with `size = -1` and no content, for referenced-but-missing artifacts
- Validation: store with Go, retrieve with `fossil artifact <uuid>`

### A6. content

Port from `content.c`:
- Expand delta chains (walk `delta` table, apply deltas recursively)
- Phantom blob tracking (referenced but not yet received)
- Content verification (re-hash, confirm integrity)
- Validation: mixed full-text and delta blobs, `fossil rebuild --verify` passes

### A7. repo

Port from `repo.c`:
- `Create(path string) (*Repo, error)` — new `.fossil` file with full schema, application_id pragma
- `Open(path string) (*Repo, error)` — open existing, verify application_id
- `Close()` — clean shutdown, finalize prepared statements
- `Verify() error` — internal consistency check
- `WithTx(func(tx) error) error` — transaction scoping for multi-step operations
- Validation: `fossil new` -> Go opens; Go creates -> `fossil rebuild --verify`; round-trip both directions

### Phase A Exit Criteria

- Programmatically create a `.fossil` repo with complete schema
- `fossil` recognizes it as a valid Fossil repo (application_id, schema tables all present)
- Store content as blobs (full-text and delta-compressed)
- Retrieve and verify content
- Phantom blobs tracked correctly
- `fossil rebuild --verify` passes on Go-created repos
- All benchmarks within threshold
- All tests green including race detector

## Phase B: Checkout Operations

### B1. deck

Port from `deck.c`. Manifest parsing and serialization.

The parser must handle ALL card types that can appear in manifests. Fossil defines 20+ card types:
`A` (attachment), `B` (baseline manifest), `C` (comment), `D` (datetime), `E` (event timestamp), `F` (file), `G` (forum thread), `H` (forum title), `I` (forum in-reply-to), `J` (ticket field), `K` (ticket UUID), `L` (wiki title), `M` (forum parent), `N` (forum mimetype), `P` (parent), `Q` (cherry-pick), `R` (repo hash), `T` (tag), `U` (user), `W` (wiki text), `Z` (checksum).

Phase B implements full parsing for all card types (so any manifest can be round-tripped), but only constructs checkin manifests (using B, C, D, F, P, R, T, U, Z cards).

Critical details:
- **B-card (baseline manifest)**: Required for delta manifests. Fossil creates delta manifests by default after the first checkin on a branch. Without B-card support, checkout operations fail for most repos. A delta manifest references a baseline and only lists changed files.
- **T-card (tags)**: `fossil commit` embeds T-cards in checkin manifests. Must parse and preserve for round-trip fidelity even though tag package is Phase C.
- **Z-card (checksum)**: MD5 hash of the entire manifest text up to and including "Z ". Must be computed exactly — Fossil validates this.
- Serialization must produce byte-exact canonical manifest text (card ordering, whitespace, newlines must match Fossil's format) because the manifest hash is its UUID.

Validation: parse manifests from existing repos via `fossil artifact`, round-trip serialize -> parse -> serialize, byte-exact match.

### B2. checkout

Port from `checkin.c`, `checkout.c`, `vfile.c`:
- `Checkin(repo, files, comment, user, parent) (FslID, error)` — create manifest, store file blobs, record mlink, update event table. Produces delta manifests when a parent exists.
- `Checkout(repo, rid, dest) error` — extract files to directory. Must handle both baseline and delta manifests.
- `Ls(repo, rid) ([]FileEntry, error)` — list files in a checkin (resolve delta manifest against baseline)
- `Log(repo, opts) ([]LogEntry, error)` — walk checkin DAG
- R-card computation: hash of sorted F-card file hashes (for baseline manifests) or hash of baseline's R-card content plus changes (for delta manifests)
- Validation:
  - `fossil rebuild --verify` passes
  - `fossil ls --r <uuid>` matches Go file list
  - `fossil cat -r <uuid> <file>` returns correct content
  - `fossil timeline` shows Go-created checkins
  - Reverse: `fossil commit` -> Go reads and parses correctly
  - Multi-checkin test: create 3+ sequential checkins, verify delta manifests are produced and fossil can walk the full history

### Phase B Exit Criteria

- Create repos, commit files across multiple checkins with parent-child relationships
- Delta manifests correctly produced and resolvable
- Check files out, walk history
- Fossil CLI reads, rebuilds, and operates on Go-created repos indistinguishably from fossil-created ones
- Round-trip: Go creates -> fossil reads/writes -> Go reads back
- All benchmarks within threshold

## Future Work Roadmap

| Phase | Package | Scope | Depends On |
|-------|---------|-------|------------|
| C | tag | Create, apply, query tags and properties. Branch names are tags. | B |
| D | branch | Branch creation, listing, branch-point resolution. Thin layer over tag + deck. | C |
| E | diff | Unified diff generation between blob versions. Port of diff.c. | A, B |
| F | merge | 3-way merge, pivot resolution, conflict detection. Port of merge.c / merge3.c. | E, B |
| G | annotate | Line-by-line annotation (blame). Port of annotate.c. | E, B |
| H | wiki | Wiki artifact creation/parsing. W-card decks. | B |
| I | ticket | Ticket artifact creation/parsing. J-card decks. | B |
| J | forum | Forum post artifacts. Port of forum.c. | B |
| K | search | FTS5 full-text search over checkins, wiki, tickets. Port of search.c. | H, I, B |
| L | glob | Fossil glob-pattern matching for ignore rules. Port of glob.c. | A |
| M | xfer | Sync protocol card encoding/decoding. Port of xfer.c. | A, B |
| N | config | Repository and checkout config DB operations. Port of config.c. | A |
| O | zip | Generate ZIP/tarball archives from checkins. Port of zip.c. | B |
| P | vector | Vector search UDF registration via modernc. | K |
| Q | edgesync-integration | Replace EdgeSync pkg/ with go-libfossil imports. | M, B |

Critical path: A -> B -> C -> D -> F for full branching/merging. E can parallel C/D (depends on A+B). H/I/J are independent after B.

Each future phase gets its own brainstorm -> spec -> plan -> implement cycle when scheduled.
