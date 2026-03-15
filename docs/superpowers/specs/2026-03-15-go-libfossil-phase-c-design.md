# go-libfossil Phase C Design Spec: Xfer Card Protocol Codec

Wire-format codec for Fossil's xfer sync protocol. Parses and encodes individual cards and full zlib-compressed xfer messages. No sync logic, no HTTP transport, no database dependency — pure format layer.

## Scope

Encode and decode all 19 card types used in Fossil's `/xfer` sync protocol, plus full message-level zlib compression/decompression. Callers (future leaf agent, bridge, sync engine) use this package to read and write xfer payloads.

**In scope:** Card types, binary payload handling, zlib message compression, typed card structs, Fossil string encoding/decoding for applicable fields.
**Out of scope:** Sync state machine, convergence logic, HTTP client, authentication computation, phantom tracking. These are Phase D.

## Constraints

All Phase A/B constraints carry forward:
- Pure Go, no CGo
- Behavioral equivalence validated by fossil CLI as test oracle
- Strict TDD red-green
- Performance: compute ops within 3x of C, I/O ops within 5x
- Race detector clean

## Package Structure

Single new package: `go-libfossil/xfer/`

No database dependency. Depends on standard library (`bytes`, `compress/zlib`, `encoding/hex`, `fmt`, `io`, `strconv`, `strings`) and the `deck` package for `FossilEncode`/`FossilDecode` (shared string escaping logic).

### Dependency Graph

```
xfer -> deck (for FossilEncode/FossilDecode only)
```

Intentionally database-free — same isolation principle as `deck/`. The dependency on `deck` is narrow: only the two string encoding functions.

**Naming note:** This package defines `xfer.FileCard` which is distinct from `deck.FileCard`. Go's package system handles this naturally — callers use `xfer.FileCard` vs `deck.FileCard`. These are unrelated types for different protocol layers.

### File Structure

```
go-libfossil/
  xfer/
    card.go          # CardType enum, Card interface, all typed card structs
    encode.go        # EncodeCard — Card -> wire bytes
    decode.go        # DecodeCard — wire bytes -> Card (handles binary payloads)
    message.go       # Message struct, Encode/Decode with zlib
    card_test.go     # Unit tests for individual cards
    message_test.go  # Message-level round-trip and integration tests
```

## Card Data Model

### CardType Enum

```go
type CardType int

const (
    CardFile       CardType = iota // file — raw artifact transfer
    CardCFile                      // cfile — zlib-compressed artifact transfer
    CardIGot                       // igot — announce possession
    CardGimme                      // gimme — request artifact
    CardLogin                      // login — authentication
    CardPush                       // push — announce push capability
    CardPull                       // pull — announce pull capability
    CardCookie                     // cookie — state optimization hint
    CardClone                      // clone — full clone mode
    CardCloneSeqNo                 // clone_seqno — next sequence number
    CardConfig                     // config — configuration transfer
    CardReqConfig                  // reqconfig — request configuration
    CardPrivate                    // private — next file/cfile is private
    CardUVFile                     // uvfile — unversioned file transfer
    CardUVGimme                    // uvgimme — request unversioned file
    CardUVIGot                     // uvigot — announce unversioned file state
    CardPragma                     // pragma — meta-information
    CardError                      // error — fatal error
    CardMessage                    // message — informational message
    CardUnknown                    // unknown — forward compatibility
)
```

### Card Interface

```go
type Card interface {
    Type() CardType
}
```

All typed card structs implement this interface. Callers use type switches to handle specific card types.

### Fossil String Encoding

Several card fields use Fossil's string encoding where `\s` = space, `\n` = newline, `\\` = backslash. The `deck.FossilEncode` and `deck.FossilDecode` functions handle this.

Fields that use Fossil encoding:
- `login` USER field
- `error` MESSAGE field
- `message` MESSAGE field

Decode defossilizes these fields; encode fossilizes them. Other fields (UUIDs, codes, numeric args) are never encoded.

### Typed Card Structs

#### Artifact Transfer

```go
// FileCard represents a "file" card — raw uncompressed artifact.
// Wire: file UUID SIZE \n CONTENT
// Wire: file UUID DELTASRC SIZE \n CONTENT (delta variant)
// No trailing newline after CONTENT.
type FileCard struct {
    UUID     string // artifact hash
    DeltaSrc string // base artifact hash (empty if full content)
    Content  []byte // raw artifact bytes
}

// CFileCard represents a "cfile" card — zlib-compressed artifact.
// Wire: cfile UUID USIZE CSIZE \n ZCONTENT
// Wire: cfile UUID DELTASRC USIZE CSIZE \n ZCONTENT (delta variant)
// No trailing newline after ZCONTENT (unless last byte happens to be \n).
// Decode decompresses; encode compresses. Caller always works with raw bytes.
type CFileCard struct {
    UUID     string // artifact hash
    DeltaSrc string // base artifact hash (empty if full content)
    USize    int    // uncompressed size (for verification)
    Content  []byte // raw artifact bytes (decompressed)
}
```

#### Negotiation

```go
// IGotCard represents an "igot" card — announce possession of artifact.
// Wire: igot UUID
// Wire: igot UUID 1 (private variant)
type IGotCard struct {
    UUID      string
    IsPrivate bool
}

// GimmeCard represents a "gimme" card — request an artifact.
// Wire: gimme UUID
type GimmeCard struct {
    UUID string
}
```

#### Authentication & Capability

```go
// LoginCard represents a "login" card.
// Wire: login USER NONCE SIGNATURE
// USER is Fossil-encoded (defossilized on decode, fossilized on encode).
type LoginCard struct {
    User      string // decoded (plain text with spaces/newlines)
    Nonce     string
    Signature string
}

// PushCard represents a "push" card.
// Wire: push SERVERCODE PROJECTCODE
type PushCard struct {
    ServerCode  string
    ProjectCode string
}

// PullCard represents a "pull" card.
// Wire: pull SERVERCODE PROJECTCODE
type PullCard struct {
    ServerCode  string
    ProjectCode string
}

// CookieCard represents a "cookie" card.
// Wire: cookie VALUE
type CookieCard struct {
    Value string
}
```

#### Clone

```go
// CloneCard represents a "clone" card.
// Wire: clone
// Wire: clone VERSION
// Wire: clone VERSION SEQNO
// Version 3 means the server uses cfile (compressed) cards and the response
// is sent uncompressed (Content-Type: application/x-fossil-uncompressed).
type CloneCard struct {
    Version int // 0 if unspecified; 3 = compressed cards, uncompressed response
    SeqNo   int // 0 if unspecified; resume point for interrupted clones
}

// CloneSeqNoCard represents a "clone_seqno" card.
// Wire: clone_seqno N
// SeqNo 0 means all artifacts have been sent.
type CloneSeqNoCard struct {
    SeqNo int
}
```

#### Configuration

```go
// ConfigCard represents a "config" card — key-value transfer with binary payload.
// Wire: config NAME SIZE \n CONTENT \n
// Trailing newline after CONTENT.
type ConfigCard struct {
    Name    string
    Content []byte
}

// ReqConfigCard represents a "reqconfig" card.
// Wire: reqconfig NAME
type ReqConfigCard struct {
    Name string
}
```

#### Private Content

```go
// PrivateCard represents a "private" card — flags next file/cfile as private.
// Wire: private
type PrivateCard struct{}
```

#### Unversioned Files

```go
// UVFileCard represents a "uvfile" card — unversioned file transfer.
// Wire: uvfile NAME MTIME HASH SIZE FLAGS \n CONTENT
// FLAGS is always present. No trailing newline after CONTENT.
//
// FLAGS bits:
//   0x0001 = deleted (SIZE=0, HASH="-", no content)
//   0x0004 = content omitted (header only, no binary payload)
//
// When FLAGS indicates deletion or content-omitted, Content is nil.
type UVFileCard struct {
    Name    string
    MTime   int64   // Unix timestamp (NOT Julian day)
    Hash    string  // "-" means deleted
    Size    int     // uncompressed content size; 0 for deleted
    Flags   int
    Content []byte  // nil when deleted or content-omitted
}

// UVGimmeCard represents a "uvgimme" card.
// Wire: uvgimme NAME
type UVGimmeCard struct {
    Name string
}

// UVIGotCard represents a "uvigot" card.
// Wire: uvigot NAME MTIME HASH SIZE
type UVIGotCard struct {
    Name  string
    MTime int64   // Unix timestamp (NOT Julian day)
    Hash  string  // "-" means deleted
    Size  int
}
```

#### Meta & Errors

```go
// PragmaCard represents a "pragma" card.
// Wire: pragma NAME [VALUE...]
type PragmaCard struct {
    Name   string
    Values []string // remaining args after name
}

// ErrorCard represents an "error" card — fatal sync error.
// Wire: error ENCODED_MESSAGE
// MESSAGE is a single Fossil-encoded token. Defossilized on decode.
type ErrorCard struct {
    Message string // decoded plain text
}

// MessageCard represents a "message" card — informational.
// Wire: message ENCODED_MESSAGE
// MESSAGE is a single Fossil-encoded token. Defossilized on decode.
type MessageCard struct {
    Message string // decoded plain text
}

// UnknownCard represents an unrecognized card type.
// Returned by DecodeCard for forward compatibility — not an error.
type UnknownCard struct {
    Command string
    Args    []string
}
```

## Trailing Newline Rules

Binary payload handling varies by card type. The exact rules:

| Card | After header `\n` | After CONTENT |
|------|-------------------|---------------|
| file | yes | NO trailing `\n` |
| cfile | yes | NO trailing `\n` (content may end with `\n` coincidentally) |
| config | yes | YES trailing `\n` |
| uvfile | yes | NO trailing `\n` |

The decoder must handle each case correctly. The encoder must emit the exact format.

## Decode API

### Card-Level Decode

```go
// DecodeCard reads one card from the reader.
// Handles binary payloads for file/cfile/config/uvfile cards.
// Returns io.EOF when reader is exhausted.
// Skips comment lines (starting with #) and empty lines.
func DecodeCard(r *bytes.Reader) (Card, error)
```

**Decode flow:**
1. Read bytes until `\n` to get the header line
2. Skip if empty or starts with `#`
3. Split on first space to get command word
4. Dispatch to type-specific parser based on command word
5. For payload cards: parse SIZE from args, read exactly SIZE bytes from reader
6. For `config`: also consume the trailing `\n` after content
7. For `cfile`: zlib-decompress the payload (standard zlib/RFC 1950, not gzip), verify decompressed length == USize
8. For `error`/`message`/`login.User`: apply `deck.FossilDecode`
9. For unknown command words: return `UnknownCard` (not an error)
10. Return typed card struct

### Arg Count Validation

Each card type has a known arg count range. DecodeCard validates:

| Card | Min Args | Max Args | Has Payload |
|------|----------|----------|-------------|
| file | 2 | 3 | yes (last arg is SIZE) |
| cfile | 3 | 4 | yes (last arg is CSIZE) |
| igot | 1 | 2 | no |
| gimme | 1 | 1 | no |
| login | 3 | 3 | no |
| push | 2 | 2 | no |
| pull | 2 | 2 | no |
| cookie | 1 | 1 | no |
| clone | 0 | 2 | no |
| clone_seqno | 1 | 1 | no |
| config | 2 | 2 | yes (last arg is SIZE) |
| reqconfig | 1 | 1 | no |
| private | 0 | 0 | no |
| uvfile | 5 | 5 | conditional (see FLAGS) |
| uvgimme | 1 | 1 | no |
| uvigot | 4 | 4 | no |
| pragma | 1 | unlimited | no |
| error | 1 | 1 | no (Fossil-encoded token) |
| message | 1 | 1 | no (Fossil-encoded token) |

## Encode API

### Card-Level Encode

```go
// EncodeCard writes one card to the buffer.
// Handles binary payloads for file/cfile/config/uvfile cards.
func EncodeCard(w *bytes.Buffer, c Card) error
```

**Encode flow:**
1. Type-switch on the card
2. Write command word and args separated by spaces
3. For `error`/`message`: apply `deck.FossilEncode` to message text
4. For `login`: apply `deck.FossilEncode` to User field
5. For payload cards: see trailing newline rules table above
6. For `cfile`: zlib-compress (standard zlib/RFC 1950) the content, compute CSIZE, write header with USIZE and CSIZE, then compressed bytes
7. For non-payload cards: write args, `\n`

### CFile Compression Detail

- **Decode:** reads CSIZE bytes of zlib data, decompresses using `compress/zlib` (standard zlib, RFC 1950), verifies decompressed length == USIZE, stores raw bytes in `CFileCard.Content`
- **Encode:** compresses `CFileCard.Content` with `compress/zlib`, computes CSIZE from compressed output length, writes USIZE (from `len(Content)`) and CSIZE in header

The caller never handles compressed bytes directly.

## Message API

```go
// Message is a sequence of cards forming one xfer request or response.
type Message struct {
    Cards []Card
}

// Encode serializes all cards and zlib-compresses the result.
// This is what gets sent as the HTTP body with Content-Type: application/x-fossil.
func (m *Message) Encode() ([]byte, error)

// EncodeUncompressed serializes without zlib compression.
// Used for clone protocol v3 or debugging.
func (m *Message) EncodeUncompressed() ([]byte, error)

// Decode zlib-decompresses the input and decodes all cards.
func Decode(data []byte) (*Message, error)

// DecodeUncompressed decodes without decompression.
func DecodeUncompressed(data []byte) (*Message, error)
```

**Message.Encode flow:**
1. Allocate `bytes.Buffer`
2. For each card, call `EncodeCard`
3. Zlib-compress the entire buffer (standard zlib, RFC 1950)
4. Return compressed bytes

**Decode flow:**
1. Zlib-decompress input to `[]byte`
2. Wrap in `bytes.Reader`
3. Loop `DecodeCard` until `io.EOF`
4. Return `&Message{Cards: cards}`

### Error Handling

- Malformed cards during decode return an error with the card line for context
- Truncated binary payloads (fewer bytes than SIZE) return an error
- Unknown card types produce `UnknownCard` (not an error)
- Zlib decompression failure returns an error
- CFile USize mismatch (decompressed size != USize) returns an error

## Testing Architecture

### Layer 1: Card Unit Tests (no fossil binary)

**Round-trip per card type:**
- Construct each of the 19 card types -> EncodeCard -> DecodeCard -> compare all fields
- Test both minimal and maximal arg variants (e.g., `file` with and without DeltaSrc)

**Binary payload cards:**
- `file`: verify SIZE matches content length on wire, no trailing newline
- `cfile`: verify content survives compress/decompress, USize matches
- `config`: verify NAME and content preserved, trailing newline present
- `uvfile`: verify all fields including FLAGS, content absent when deleted/omitted

**Fossil encoding:**
- `error`/`message`: round-trip messages with spaces and newlines
- `login`: round-trip usernames with spaces

**Edge cases:**
- Empty content (zero-length payload)
- Large payload (1MB+)
- UUID validation (40-char SHA1, 64-char SHA3)
- `clone` with 0, 1, and 2 args
- `igot` with and without private flag
- `pragma` with varying number of values
- `uvfile` with deletion flag (FLAGS=0x0001, no content)
- `uvfile` with content-omitted flag (FLAGS=0x0004, no content)

**Malformed input:**
- Truncated payload (SIZE says 100, only 50 bytes available)
- Missing required args
- Non-numeric SIZE
- Bad zlib data in cfile
- USize mismatch after decompression
- Empty line, comment line (should be skipped)
- Unknown card type (should produce UnknownCard)

### Layer 2: Message Tests

- Multi-card message round-trip (encode -> decode -> compare)
- Mixed card types in one message (file + igot + gimme + pragma)
- Zlib compression round-trip
- Uncompressed round-trip
- Empty message (zero cards)
- Message with only comments/empty lines

### Layer 3: Integration Tests (fossil CLI oracle)

- Use `fossil sync --httptrace` or proxy to capture real xfer traffic
- Alternatively: create a repo with known content, use `fossil push --once` to a local fossil server, capture the payload
- Decode captured payload with our codec
- Verify all cards parse correctly
- Re-encode and compare (may not be byte-identical due to compression level, but decompressed content must match)

### Layer 4: Benchmarks

- `BenchmarkDecodeMessage` — realistic payload (~50 cards: mix of igot, gimme, file, pragma)
- `BenchmarkEncodeMessage` — same payload
- `BenchmarkDecodeFileCard` — single file card with 1MB payload
- `BenchmarkEncodeFileCard` — same
- `BenchmarkDecodeCFileCard` — compressed file card with 1MB original
- `BenchmarkEncodeCFileCard` — same

## Phase C Exit Criteria

1. All 19 card types (+ UnknownCard) encode and decode correctly via round-trip tests
2. Binary payload cards (file, cfile, config, uvfile) handle exact byte counts with correct trailing newline behavior per type
3. CFile compression/decompression is transparent to callers (standard zlib/RFC 1950)
4. Fossil-encoded fields (login.User, error.Message, message.Message) are correctly decoded/encoded
5. UVFile correctly handles deletion (FLAGS 0x0001) and content-omitted (FLAGS 0x0004) cases
6. Message-level zlib encode/decode works for both compressed and uncompressed modes
7. Comment lines and empty lines are skipped during decode
8. Unknown card types produce UnknownCard (forward compatibility)
9. Malformed input produces clear errors (truncated payloads, bad sizes, bad zlib, USize mismatch)
10. Integration test decodes real fossil sync traffic correctly
11. All benchmarks recorded, all tests green including race detector
12. Performance within 3x of C for compute, 5x for I/O
