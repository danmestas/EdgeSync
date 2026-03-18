# go-libfossil Phase B Design Spec: Checkout Operations

Manifest parsing, serialization, and checkin operations for go-libfossil. Builds on Phase A (repo fundamentals) to enable programmatic creation and reading of Fossil checkins.

## Scope

Manifest-only with explicit file lists. No working-directory management (no vfile table, no `fossil open`-style checkout). The caller provides file names and content directly. Working directory tracking deferred to a future Phase B2 if needed.

## Constraints

All Phase A constraints carry forward:
- Pure Go, no CGo
- Every operation must produce `.fossil` files that `fossil rebuild` accepts
- Behavioral equivalence validated by fossil CLI as test oracle
- Strict TDD red-green
- Performance: compute ops within 3x of C, I/O ops within 5x

## Package Structure

### deck/ — Manifest Format Layer

Pure data structures and algorithms. No database dependency. Depends only on `crypto/md5`, standard library, and root `libfossil` package.

Responsibilities:
- Parse manifest text into a Deck struct
- Serialize a Deck to canonical manifest bytes with Z-card
- Compute and verify Z-card (MD5 checksum)
- Compute R-card (file manifest hash) via content-fetcher callback
- Fossil-encode/decode strings

### manifest/ — Checkin Operations Layer

Higher-level operations combining deck with database storage. Depends on `deck/`, `db/`, `blob/`, `content/`, `hash/`.

Responsibilities:
- Create checkins from explicit file lists
- Populate mlink, plink, event, leaf, unclustered, unsent tables
- Resolve file lists (including delta manifest -> baseline resolution)
- Parse stored manifests
- Walk checkin history

### Dependency Graph

```
deck (stdlib + root types only)
manifest -> deck, db, blob, content, hash
```

No circular imports. `deck` is intentionally database-free so it can be tested and used independently.

### Transaction Interface (Phase B prerequisite)

Phase A's `blob.Store`, `blob.Load`, `content.Expand` etc. accept `*db.DB`. Phase B's `Checkin()` must store file blobs and the manifest blob within a single transaction. To support this, Phase B introduces a `db.Querier` interface:

```go
// Querier is implemented by both *DB and *Tx.
type Querier interface {
    Exec(query string, args ...any) (sql.Result, error)
    QueryRow(query string, args ...any) *sql.Row
    Query(query string, args ...any) (*sql.Rows, error)
}
```

Phase B updates the blob and content function signatures to accept `db.Querier` instead of `*db.DB`. This is a backward-compatible change — existing callers passing `*db.DB` continue to work since `*db.DB` satisfies `Querier`.

### Package Naming Note

The parent design spec uses `checkout/` as the package name. This spec uses `manifest/` instead, reflecting the actual scope: manifest-only operations without working-directory checkout. If a future Phase B2 adds `fossil open`-style working directory management, it would live in a separate `checkout/` package.

### Hash Policy

All UUIDs in this spec are SHA1 (40 hex chars), matching Fossil's default hash policy. SHA3-256 support (64 hex chars) exists in the `hash/` package but is not exercised in Phase B. Parsing accepts both lengths via `hash.IsValidHash`.

## deck/ Data Model

### Deck Struct

```go
type Deck struct {
    Type ArtifactType
    A    *AttachmentCard // attachment info
    B    string          // baseline manifest UUID (empty = this is a baseline)
    C    string          // comment (decoded from fossil-encoding)
    D    time.Time       // timestamp
    E    *EventCard      // event datetime + UUID
    F    []FileCard      // file entries, sorted by name
    G    string          // forum thread root UUID
    H    string          // forum title (decoded)
    I    string          // forum in-reply-to UUID
    J    []TicketField   // ticket field updates
    K    string          // ticket UUID
    L    string          // wiki title (decoded)
    M    []string        // cluster member UUIDs, sorted
    N    string          // MIME type
    P    []string        // parent UUIDs (first = primary)
    Q    []CherryPick    // cherry-pick/backout entries
    R    string          // file manifest MD5 hash (32 hex chars)
    T    []TagCard       // tags, sorted by type+name+uuid
    U    string          // user login (decoded)
    W    []byte          // wiki/forum body content
    Z    string          // manifest checksum (computed during Marshal, not set by caller)
}
```

### ArtifactType Enum

```go
type ArtifactType int

const (
    Checkin    ArtifactType = iota
    Wiki
    Ticket
    Event
    Cluster
    ForumPost
    Attachment
    Control
)
```

### Card Types

```go
type FileCard struct {
    Name    string // filename (decoded)
    UUID    string // content hash (empty = deleted in delta manifest)
    Perm    string // "" (regular), "x" (executable), "w" (writable), "l" (symlink)
    OldName string // original name if renamed (decoded)
}

type TagCard struct {
    Type  TagType // '+' singleton, '*' propagating, '-' anti-tag
    Name  string
    UUID  string  // target artifact, or "*" for self
    Value string
}

type TagType byte

const (
    TagSingleton   TagType = '+'
    TagPropagating TagType = '*'
    TagCancel      TagType = '-'
)

type CherryPick struct {
    IsBackout bool
    Target    string // UUID
    Baseline  string // UUID (optional)
}

type AttachmentCard struct {
    Filename string
    Target   string // wiki page or ticket UUID
    Source   string // blob UUID (empty = delete attachment)
}

type EventCard struct {
    Date time.Time
    UUID string
}

type TicketField struct {
    Name  string
    Value string // empty = clear field
}
```

## Card Reference

All 20 card types that can appear in manifests:

| Card | Syntax | Multiplicity | Used In |
|------|--------|-------------|---------|
| A | `A <filename> <target> ?<source>?` | 0-1 | Attachment |
| B | `B <uuid>` | 0-1 | Checkin (delta only) |
| C | `C <encoded-comment>` | 0-1 | Checkin, Wiki, Event, Attachment |
| D | `D <datetime>` | 1 | All except Cluster |
| E | `E <datetime> <uuid>` | 0-1 | Event |
| F | `F <name> ?<uuid>? ?<perm>? ?<old-name>?` | 0-N | Checkin |
| G | `G <uuid>` | 0-1 | ForumPost |
| H | `H <encoded-title>` | 0-1 | ForumPost |
| I | `I <uuid>` | 0-1 | ForumPost |
| J | `J <encoded-name> ?<value>?` | 0-N | Ticket |
| K | `K <uuid>` | 0-1 | Ticket |
| L | `L <encoded-title>` | 0-1 | Wiki |
| M | `M <uuid>` | 1-N | Cluster |
| N | `N <mimetype>` | 0-1 | Wiki, ForumPost |
| P | `P <uuid> ...` | 0-1 | Checkin, Event |
| Q | `Q (+\|-)<uuid> ?<uuid>?` | 0-N | Checkin |
| R | `R <md5-hash>` | 0-1 | Checkin |
| T | `T (+\|*\|-)<name> <uuid> ?<value>?` | 0-N | Checkin, Control |
| U | `U <encoded-login>` | 0-1 | All except Cluster |
| W | `W <size>\n<content>\n` | 0-1 | Wiki, ForumPost |
| Z | `Z <md5-hash>` | 1 | All (always last) |

## Serialization Rules

### Card Ordering

Cards emitted in strict ASCII order. For checkin manifests:

```
B C D F K L N P Q R T U W Z
```

F-cards sorted alphabetically by filename within the F section. T-cards sorted lexically by `type+name+uuid`. Z-card always last.

General ordering for all types: A B C D E F G H I J K L M N P Q R T U W Z — cards simply appear in this order, absent cards are skipped.

### D-Card Format

`YYYY-MM-DDTHH:MM:SS.SSS` — uppercase `T` separator in output (matching both fossil CLI and libfossil). D-card always includes 3-digit milliseconds (`.000` if zero) because omitting them would change the manifest hash. E-card (Event) timestamps omit milliseconds. Parsing accepts both `T` and `t` for robustness.

### Fossil-Encoding

Three escape sequences:
- `\s` = space (0x20)
- `\n` = newline (0x0A)
- `\\` = backslash (0x5C)

Applied to: C-card comment, F-card filename, F-card old-name, L-card title, U-card login, H-card title, J-card field name.

### W-Card Format

```
W <size>\n
<exactly size bytes of content>\n
```

The size is decimal. Content is raw bytes (not fossil-encoded).

## Z-Card Computation

The Z-card is an MD5 checksum of the entire manifest text preceding it.

Algorithm:
1. Serialize all cards (B through W) into a buffer
2. MD5-hash all bytes in the buffer
3. Append `Z <32-hex-lowercase>\n`

Verification (on raw manifest bytes):
1. Manifest must be at least 35 bytes
2. Last 35 bytes must be `Z <32-hex>\n`
3. MD5-hash bytes `[0 : len-35]`
4. Compare against the 32 hex chars in the Z-card

The Z-card is an internal integrity check. The manifest's blob UUID (SHA1/SHA3 of the complete bytes including Z-card) is its external identity.

## R-Card Computation

The R-card is an MD5 hash over all file names, sizes, and contents.

Algorithm:
1. If no F-cards: return `d41d8cd98f00b204e9800998ecf8427e` (MD5 of empty string)
2. Sort F-cards alphabetically by filename
3. Initialize MD5 context
4. For each F-card (sorted order):
   - Feed: `<filename>` (filename bytes only, no null terminator)
   - Fetch file content by UUID (via callback)
   - Feed: ` <size-decimal>\n` (space, size in decimal ASCII, newline)
   - Feed: `<raw-file-content-bytes>`
5. Finalize MD5, return 32 hex lowercase

The content-fetcher callback `func(uuid string) ([]byte, error)` keeps `deck` database-free. The `manifest` package provides the callback backed by `blob.Load` + `content.Expand`.

**Parsing note:** The R-card has 0-1 multiplicity. `Parse()` must accept manifests without an R-card (e.g., manifests from older Fossil versions or non-checkin artifact types). `Marshal()` for checkin manifests always emits the R-card when `ComputeR` has been called and `Deck.R` is set.

## Delta vs Baseline Manifests

### Baseline Manifest
- No B-card
- F-cards list every file in the checkin
- R-card computed over all files

### Delta Manifest
- Has B-card pointing to the baseline manifest's UUID
- F-cards list only changes relative to baseline:
  - `F <name> <uuid> [perm]` — file added or modified
  - `F <name>` (no UUID) — file deleted
  - Files not mentioned are unchanged from baseline
- R-card still computed over the full resolved file set

### Creating Delta Manifests

When `CheckinOpts.Delta` is true and a parent exists:
1. Load parent manifest
2. If parent is a delta manifest, follow its B-card to find the actual baseline
3. Set B-card to that baseline's UUID
4. Compare caller's file list against baseline's file list
5. Emit F-cards only for additions, modifications, and deletions
6. If the delta would be larger than a baseline, fall back to baseline

## deck/ API

```go
// Parse parses raw manifest bytes into a Deck.
// Validates card ordering and Z-card integrity.
func Parse(data []byte) (*Deck, error)

// VerifyZ checks the Z-card on raw manifest bytes without full parsing.
func VerifyZ(data []byte) error

// Marshal serializes the Deck to canonical manifest format.
// Computes and appends the Z-card. Caller must not set d.Z.
func (d *Deck) Marshal() ([]byte, error)

// ComputeR computes the R-card MD5 hash for a checkin manifest.
// getContent resolves a file UUID to its raw content bytes.
func (d *Deck) ComputeR(getContent func(uuid string) ([]byte, error)) (string, error)

// FossilEncode encodes a string using Fossil's escaping rules.
func FossilEncode(s string) string

// FossilDecode decodes a Fossil-encoded string.
func FossilDecode(s string) string
```

## manifest/ API

```go
// CheckinOpts configures a new checkin.
type CheckinOpts struct {
    Files   []File
    Comment string
    User    string
    Parent  FslID      // 0 for initial checkin
    Delta   bool       // produce delta manifest if parent exists
    Time    time.Time  // zero value = time.Now(); explicit for deterministic tests
}

type File struct {
    Name    string
    Content []byte
    Perm    string // "", "x", "l"
}

// Checkin creates a new checkin in the repository.
// Stores file blobs, builds manifest, populates event/mlink/plink/leaf tables.
func Checkin(r *repo.Repo, opts CheckinOpts) (FslID, string, error)

// GetManifest loads and parses a manifest from the repository by RID.
func GetManifest(r *repo.Repo, rid FslID) (*deck.Deck, error)

// FileEntry describes a file in a resolved checkin.
type FileEntry struct {
    Name string
    UUID string
    Perm string
}

// ListFiles returns the complete file list for a checkin.
// Resolves delta manifests against their baseline.
func ListFiles(r *repo.Repo, rid FslID) ([]FileEntry, error)

// LogOpts configures history walking.
type LogOpts struct {
    Start FslID // 0 = latest leaf
    Limit int   // max entries, 0 = no limit
}

// LogEntry describes one checkin in the history.
type LogEntry struct {
    RID     FslID
    UUID    string
    Comment string
    User    string
    Time    time.Time
    Parents []string
}

// Log walks the checkin DAG via the plink table.
func Log(r *repo.Repo, opts LogOpts) ([]LogEntry, error)
```

## Table Population During Checkin

When `Checkin()` runs, it populates these tables within a transaction:

1. **blob**: Manifest stored as a blob (via `blob.Store`)
2. **filename**: Insert-or-lookup for each file path, returns fnid
3. **mlink**: One row per F-card: `(mid=manifest_rid, fnid, fid=file_blob_rid, pmid=parent_manifest_rid, pid=parent_file_rid)`
4. **plink**: One row per parent: `(pid=parent_rid, cid=manifest_rid, isprim=1 for first parent, mtime=checkin_time)`
5. **event**: `(type='ci', mtime=julian_time, objid=manifest_rid, user=user, comment=comment)`
6. **leaf**: Add manifest_rid; remove parent_rid (no longer a leaf)
7. **unclustered**: Add manifest_rid
8. **unsent**: Add manifest_rid

## File Structure

```
go-libfossil/
  deck/
    deck.go          # Deck struct, ArtifactType, card subtypes
    parse.go         # Parse() — manifest text -> Deck
    marshal.go       # Marshal() — Deck -> canonical bytes with Z-card
    rcard.go         # ComputeR() — R-card MD5
    zcard.go         # VerifyZ() — Z-card verification
    encode.go        # FossilEncode / FossilDecode
    deck_test.go     # Unit tests (no database)
  manifest/
    manifest.go      # Checkin(), GetManifest()
    files.go         # ListFiles() — resolve delta -> baseline
    log.go           # Log() — walk plink DAG
    manifest_test.go # Integration tests with fossil CLI
```

## Testing Architecture

### Layer 1: deck Unit Tests (no database, no fossil binary)

- Parse known-good manifests (extracted from EdgeSync repo)
- Round-trip: parse -> marshal -> parse, byte-exact match
- Z-card verification on raw bytes
- R-card computation with mock content-fetcher
- Fossil-encoding/decoding round-trips
- Card ordering validation (reject out-of-order)
- All artifact types: checkin, wiki, ticket, event manifests
- Edge cases: empty comment, no parent (initial checkin), delta manifest with file deletions

### Layer 2: manifest Integration Tests (database + fossil CLI oracle)

- Checkin creation: create repo, checkin files, `fossil rebuild` passes
- `fossil artifact` round-trip: Go creates checkin, fossil retrieves manifest, Go parses it back
- Multi-checkin history: 3+ sequential checkins, `fossil timeline -R` shows all
- Delta manifests: checkin with parent, verify B-card, verify ListFiles resolves full list
- ListFiles validation: compare against `fossil ls --r <uuid> -R <path>`
- Reverse direction: `fossil new` + `fossil commit` -> Go parses manifests correctly
- Log walking: compare against `fossil timeline`

### Layer 3: Benchmarks

- `BenchmarkParse` — realistic manifest (~50 F-cards)
- `BenchmarkMarshal` — serialize a populated deck
- `BenchmarkCheckin` — full checkin flow
- `BenchmarkListFiles` — delta -> baseline resolution

## Phase B Exit Criteria

1. Parse any Fossil manifest (all 20 card types) into a Deck and marshal back to byte-exact output
2. Z-card computation and verification passes on all test manifests
3. R-card computation matches Fossil's output for known checkins
4. Programmatic checkins pass `fossil rebuild` on Go-created repos
5. Delta manifests produced when parent exists, B-card correctly references baseline
6. `fossil artifact <uuid>` returns manifests that Go parses identically
7. ListFiles resolves delta manifests against baselines, matches `fossil ls --r`
8. Multi-checkin repos: 3+ sequential checkins, `fossil timeline` shows correct history
9. Log output matches `fossil timeline` ordering
10. Round-trip: Go creates -> fossil reads/writes -> Go reads back
11. All benchmarks recorded, all tests green including race detector
