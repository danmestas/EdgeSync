# go-libfossil Phase D Design Spec: Sync Engine

Transport-agnostic sync state machine for Fossil's xfer protocol. Drives the multi-round igot/gimme/file convergence loop. Client role implemented, server role stubbed for future. Clone mode stubbed for future.

## Scope

Build a sync engine that can push and pull artifacts between a local repo and a remote Fossil repository. The engine produces/consumes `xfer.Message` values via a `Transport` interface — it doesn't know or care whether the transport is HTTP, NATS, or a test mock.

**In scope:** Client-side sync (push/pull), convergence loop, phantom tracking, login/auth card generation, file/cfile sending and receiving, igot/gimme negotiation, max-send budgeting, cookie caching, pragma exchange, HTTPTransport for integration testing.
**Out of scope:** Server-side sync logic (stubbed interface only), clone mode (stubbed), config/reqconfig sync, unversioned file sync, private artifact handling. These are future work.

## Constraints

All Phase A-C constraints carry forward:
- Pure Go, no CGo
- Behavioral equivalence validated by fossil CLI as test oracle
- Strict TDD red-green
- Performance: compute ops within 3x of C, I/O ops within 5x
- Race detector clean
- `fossil server` is an acceptable test dependency

## Package Structure

New package: `go-libfossil/sync/`

### Dependency Graph

```
sync -> xfer, blob, content, repo, db, hash, root types
sync uses net/http (stdlib) for HTTPTransport
```

First package that bridges the wire format layer (`xfer/`) with the storage layer (`blob/`, `content/`). No external dependencies beyond stdlib.

### File Structure

```
go-libfossil/
  sync/
    transport.go        # Transport interface, HTTPTransport, MockTransport
    client.go           # Client sync engine: buildRequest, processResponse
    session.go          # Sync() entry point, convergence loop driver
    auth.go             # Login card nonce/signature computation
    sync_test.go        # Unit tests with mock transport
    integration_test.go # Tests against fossil server
```

## Transport Interface

```go
// Transport sends an xfer request and returns the response.
// Implementations handle the actual I/O (HTTP, NATS, loopback, mock).
type Transport interface {
    Exchange(ctx context.Context, request *xfer.Message) (*xfer.Message, error)
}
```

Single method. `context.Context` for cancellation and timeouts.

### HTTPTransport

Provided in the sync package for integration testing. Speaks Fossil's HTTP `/xfer` protocol using stdlib `net/http`.

```go
type HTTPTransport struct {
    URL string // e.g. "http://localhost:8080"
}

func (t *HTTPTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error)
```

Flow:
1. `req.Encode()` produces zlib-compressed bytes
2. HTTP POST to `URL/xfer` with `Content-Type: application/x-fossil`
3. Read response body
4. `xfer.Decode()` decompresses and parses response
5. Return parsed `*xfer.Message`

### MockTransport

For unit tests. Accepts a function that maps request to response:

```go
type MockTransport struct {
    Handler func(req *xfer.Message) *xfer.Message
}

func (t *MockTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
    return t.Handler(req), nil
}
```

Tests configure Handler to return canned responses or to inspect/assert on request cards.

## Client Sync Engine

### Public API

```go
// SyncOpts configures a sync operation.
type SyncOpts struct {
    Push        bool
    Pull        bool
    ProjectCode string
    ServerCode  string
    User        string // empty = "anonymous"
    Password    string // empty = no auth
    MaxSend     int    // max bytes per round, 0 = 250000 default (matches Fossil's max-upload)
}

// SyncResult reports what happened during sync.
type SyncResult struct {
    Rounds     int
    FilesSent  int
    FilesRecvd int
    Errors     []string // error/message cards from server
}

// Sync runs a complete sync session.
// Returns when converged or on error.
func Sync(ctx context.Context, r *repo.Repo, t Transport, opts SyncOpts) (*SyncResult, error)
```

### Internal Architecture

The sync loop is split into testable units:

```go
// session holds per-sync mutable state.
type session struct {
    repo        *repo.Repo
    opts        SyncOpts
    result      SyncResult
    cookie      string           // cached cookie from server
    remoteHas   map[string]bool  // UUIDs the remote announced via igot
    phantoms    map[string]bool  // UUIDs we need but don't have
    pendingSend map[string]bool  // UUIDs the remote requested via gimme
}

// buildRequest constructs the outgoing xfer.Message for one round.
func (s *session) buildRequest(cycle int) (*xfer.Message, error)

// processResponse handles the incoming xfer.Message from one round.
// Returns done=true when convergence criteria are met.
func (s *session) processResponse(msg *xfer.Message) (done bool, err error)
```

### Convergence Loop (session.go)

```
func Sync(ctx, repo, transport, opts):
    s = newSession(repo, opts)
    for cycle = 0; ; cycle++:
        if ctx.Done(): return cancelled
        if cycle > MaxRounds: return error("exceeded max rounds")

        req = s.buildRequest(cycle)
        resp = transport.Exchange(ctx, req)
        done = s.processResponse(resp)

        if done: break
    return s.result
```

**MaxRounds** safety limit: 100 (prevents infinite loops on misbehaving servers).

### buildRequest (client.go)

Per-round request construction:

1. **Every cycle includes these header cards:**
   - `pragma client-version <version> <date> <time>`
   - `push <servercode> <projectcode>` (if Push)
   - `pull <servercode> <projectcode>` (if Pull)
   - `login <user> <nonce> <signature>` (if User is set — see Auth section for nonce ordering)
   - `cookie <value>` (if cached from previous round)

2. **Every cycle includes these data cards:**
   - `igot <uuid>` for each artifact in the `unclustered` table that isn't in `remoteHas`. No count limit — limited only by MaxSend byte budget. Source is the `unclustered` DB table, filtered against `remoteHas` in-memory set.
   - `file <uuid> <size> \n <content>` for each UUID in `pendingSend` (UUIDs the server gimme'd in previous round) AND from the `unsent` table (new artifacts not yet pushed), respecting MaxSend budget
   - `gimme <uuid>` for each UUID in `phantoms` (artifacts we need, max 200 per round, dynamically scaled to `max(200, filesRecvdLastRound * 2)`)

3. **Budget management:**
   - Track cumulative bytes of file card content per round
   - Stop adding file cards when MaxSend exceeded
   - Remaining files carry over to next round via pendingSend

### processResponse (client.go)

Per-round response processing:

1. **For each card in response:**
   - `file`/`cfile`: If DeltaSrc is set, load the base artifact via `content.Expand`, apply `delta.Apply` to get full content. Store full artifact via `blob.Store(tx, fullContent)` — the returned UUID must match the card's UUID (verified via `hash.SHA1`). Increment FilesRecvd. Remove from phantoms if present.
   - `igot`: add UUID to `remoteHas`. If we don't have it and Pull is true, add to phantoms.
   - `gimme`: add UUID to `pendingSend` (send in next round).
   - `cookie`: cache the value for next round.
   - `error`: append to `result.Errors`. If the error indicates a fatal condition (e.g., "wrong project"), return error immediately.
   - `message`: append to `result.Errors` (informational).
   - `pragma`: process `server-version` (log it). Ignore others for now.
   - `push`: server acknowledges push permission.
   - Other cards: ignore (forward compatibility).

2. **Convergence check:**
   - `done = true` if ALL of:
     - No files were received this round (`filesRecvdThisRound == 0`)
     - No files were sent this round (`filesSentThisRound == 0`)
     - `phantoms` is empty (nothing left to request)
     - `pendingSend` is empty (nothing left to send)
     - No new artifacts in `unsent` table

   **Note:** This is a simplification of Fossil's convergence logic. Fossil also dynamically scales `mxPhantomReq` to `max(200, nFileRecv*2)` to avoid stalling when received artifacts create new phantoms. We implement this scaling in the gimme limit (see buildRequest above) for behavioral equivalence.

### Delta Optimization

When sending file cards, prefer delta encoding when a suitable base exists:

1. Look up the artifact's delta source in the `delta` table
2. If a delta source exists and the remote has it (`remoteHas`), send as `cfile` with DeltaSrc
3. Otherwise send as full `file` card

When receiving, the decoder handles both `file` and `cfile` transparently (Phase C handles decompression). If a received artifact has DeltaSrc, apply the delta via `delta.Apply` before storing.

## Authentication (auth.go)

Login card generation per Fossil's protocol:

```go
// computeLogin produces a LoginCard for the given credentials and payload.
func computeLogin(user, password, projectCode string, payload []byte) *xfer.LoginCard
```

Algorithm:
1. Append a random comment line (`# <random-hex>\n`) to the payload for nonce uniqueness (prevents replay attacks — Fossil does this at xfer.c lines 2391-2396)
2. Compute `nonce = SHA1(payload)` (SHA1 hex of the full payload including the random comment)
3. Compute `sharedSecret = SHA1(projectCode + "/" + user + "/" + password)` — Fossil's shared secret derivation (note: includes user login in the hash, per sha1.c line 433)
4. Compute `signature = SHA1(nonce + sharedSecret)`
5. Return `LoginCard{User: user, Nonce: nonce, Signature: signature}`

Special cases:
- User "anonymous" or "nobody": nonce is computed but signature is empty or ignored by server
- Empty User: default to "anonymous"

**Ordering constraint:** The nonce depends on the rest of the payload, so `buildRequest` must:
1. Assemble all non-login cards first
2. Append a random comment line
3. Encode those cards to bytes
4. Compute the login card from those bytes
5. Prepend the login card to the final message

## Server Role Stub

For future Phase E peer-to-peer or self-hosted server:

```go
// ServerHandler processes an incoming sync request and produces a response.
// NOT IMPLEMENTED in Phase D — placeholder for future work.
type ServerHandler interface {
    HandleSync(ctx context.Context, r *repo.Repo, request *xfer.Message) (*xfer.Message, error)
}
```

No implementation. Just the interface definition so the design is documented.

## Clone Mode Stub

```go
// CloneOpts configures a clone operation.
// NOT IMPLEMENTED in Phase D — placeholder for future work.
type CloneOpts struct {
    ProjectCode string
    ServerCode  string
    User        string
    Password    string
    Version     int // clone protocol version (3 = compressed)
}

// Clone performs a full repository clone from a remote.
// NOT IMPLEMENTED — returns error.
func Clone(ctx context.Context, r *repo.Repo, t Transport, opts CloneOpts) error {
    return fmt.Errorf("sync.Clone: not yet implemented")
}
```

## Testing Architecture

### Layer 1: Unit Tests (mock transport)

**buildRequest tests:**
- Push-only produces push card, no pull
- Pull-only produces pull card, no push
- Push+pull produces both
- Login card included when User is set, omitted for anonymous
- IGot cards emitted for unclustered artifacts (no count limit, budget-limited)
- File cards emitted for unsent table artifacts on push
- File cards emitted for pendingSend UUIDs
- Gimme cards emitted for phantoms (max 200)
- MaxSend budget stops adding file cards when exceeded
- Cookie card included when cached
- Pragma client-version always present

**processResponse tests:**
- File card stored via blob.Store, UUID verified
- CFile card decompressed and stored correctly
- IGot card adds to remoteHas
- IGot for unknown UUID added to phantoms (when Pull)
- Gimme card adds to pendingSend
- Cookie card cached
- Error card appended to result.Errors
- Fatal error returns immediately
- Convergence: returns done when nothing exchanged and no pending

**Session tests:**
- Single-round sync (small repo, everything fits in one round)
- Multi-round sync (enough artifacts to require 2+ rounds)
- Push convergence: sends files until server stops gimme-ing
- Pull convergence: requests files until no more igot cards
- MaxRounds exceeded returns error
- Context cancellation stops sync
- Delta optimization: sends cfile when delta base available

**Auth tests:**
- Nonce is SHA1 of payload (including random comment line)
- Shared secret includes projectCode + user + password
- Signature matches expected value for known inputs
- Anonymous user produces valid login card
- Random comment line ensures unique nonce per call

### Layer 2: Integration Tests (fossil server)

Test helper spins up `fossil server`:

```go
func startFossilServer(t *testing.T, repoDir string) (url string, cleanup func())
```

Starts `fossil server --port <random> --repolist <dir>` as a subprocess, waits for it to accept connections, returns URL. Cleanup kills the process.

**Integration tests:**
- **Push test:** Create repo with Go (`manifest.Checkin`), push to fossil server via HTTPTransport. Verify: `fossil timeline -R <server-repo>` shows checkins.
- **Pull test:** Create repo with `fossil new` + `fossil commit`, pull into Go repo via HTTPTransport. Verify: `content.Verify` passes, `manifest.GetManifest` returns correct data.
- **Bidirectional test:** Push from Go, commit on server with `fossil commit`, pull back. Verify both sides have all artifacts.
- **Multi-round test:** Create enough artifacts (~300) to require multiple rounds. Verify convergence.
- **Empty repo sync:** Push to empty remote, verify. Pull from empty remote, verify no-op.

### Layer 3: Benchmarks

- `BenchmarkBuildRequest` — repo with 1000 unclustered artifacts
- `BenchmarkProcessResponse` — message with 50 file cards
- `BenchmarkFullSync` — end-to-end with mock transport, 100 artifacts push+pull

## Phase D Exit Criteria

1. `Sync()` successfully pushes artifacts from a Go-created repo to a `fossil server` instance
2. `Sync()` successfully pulls artifacts from a `fossil server` instance into a Go repo
3. Bidirectional sync: push, modify on server, pull back — all artifacts present on both sides
4. Multi-round convergence works for repos with >200 artifacts
5. Login authentication produces correct nonce/signature (verified against fossil server)
6. MaxSend budget limits per-round payload size
7. Delta optimization sends cfile when delta base available on remote
8. Mock transport unit tests cover all buildRequest/processResponse paths
9. Context cancellation and MaxRounds safety limit work correctly
10. Server role interface and clone function are defined but return "not implemented"
11. All benchmarks recorded, all tests green including race detector
12. Performance within 3x of C for compute, 5x for I/O
