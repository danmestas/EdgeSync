# Clone Implementation — go-libfossil/sync

**Date:** 2026-03-18
**Branch:** feat/clone
**Scope:** Implement `sync.Clone()` — full repository clone from a remote Fossil server using clone protocol version 3.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Scope | Core clone only (no config transfer, no UV sync) | Essential functionality first; config/UV can be added later |
| Repo lifecycle | Clone creates the repo | Matches Fossil: create → seed → delete project-code → clone rounds → receive project-code from server |
| Gimme timing | Only after cloneSeqno reaches 1 | Matches Fossil xfer.c:2284-2285 |
| Clone protocol version | Version 3 (compressed file cards) | Current Fossil default |

## API

```go
// CloneResult reports what happened during a clone.
type CloneResult struct {
    Rounds      int
    BlobsRecvd  int
    ProjectCode string
    ServerCode  string
}

// Clone creates a new repository at path by cloning from a remote Fossil server.
// The path must not already exist. On success, returns the opened repo.
// On failure, the partially-created repo file is deleted.
func Clone(ctx context.Context, path string, t Transport, opts CloneOpts) (*repo.Repo, *CloneResult, error)
```

`CloneOpts` (already defined in stubs.go):
```go
type CloneOpts struct {
    ProjectCode string   // Usually empty — server provides it
    ServerCode  string   // Usually empty — server provides it
    User        string
    Password    string
    Version     int      // Protocol version (default 3)
}
```

## Fossil Clone Protocol (Version 3)

### Wire Format

**Client request (each round):**
```
pragma client-version 22800 20260315 120000
clone 3 <seqno>
login <user> <nonce> <signature>    # if credentials provided
cookie <value>                       # if cached from prior round
gimme <uuid>                         # phantoms, only after seqno==1
# <random-hex>                       # nonce uniqueness
```

**Server response:**
```
push <server-code> <project-code>    # tells client the codes
file <uuid> <size> <flags>\n<content>  # blobs in rid order
cfile <uuid> <delta-src> <size> <flags>\n<content>  # delta blobs
clone_seqno <next>                   # 0 = all sent, >0 = resume here
cookie <value>                       # session cookie
pragma server-version <ver> <date> <time>
error <message>                      # on auth failure or other error
message <text>                       # informational
```

### Sequential Delivery

The server sends blobs in rid order (1, 2, 3, ...). Each round:
1. Client sends `clone 3 <seqno>` where seqno is the next rid to receive
2. Server sends blobs from seqno up to its send budget or time limit
3. Server responds with `clone_seqno <next>` — the next rid to request
4. When `clone_seqno 0`, all blobs have been sent

### Convergence Rules (matching Fossil xfer.c:2995-3004)

Continue if:
- `clone_seqno > 0` (more sequential blobs to deliver)
- Phantoms exist and files were received this round
- `nCycle == 1` (always go at least 2 rounds on clone)

Stop when:
- `clone_seqno == 0` AND no phantoms remain AND at least 2 rounds done
- Hard cap at `MaxRounds` (100)
- Context cancelled
- Error card received

## Repo Creation Sequence

Matches Fossil's `clone_cmd()` in `clone.c:234-268`:

1. **Create repo:** `repo.Create(path, rng)` — creates DB with full schema
2. **Clear project-code:** `DELETE FROM config WHERE name='project-code'` — server will provide it
3. **Generate server-code:** Already done by `repo.Create` via `db.SeedConfig`
4. **Run clone rounds** against the transport
5. **On first `push` card:** Store project-code and server-code from server:
   ```sql
   REPLACE INTO config(name,value,mtime) VALUES('project-code', ?, julianday('now'))
   REPLACE INTO config(name,value,mtime) VALUES('server-code', ?, julianday('now'))
   ```
6. **On success:** Return opened `*repo.Repo` + `*CloneResult`
7. **On failure:** Close repo, `os.Remove(path)`, return error

## Implementation Structure

### New File: `go-libfossil/sync/clone.go`

```
Clone(ctx, path, t, opts)
  ├── repo.Create(path, rng)
  ├── clearProjectCode(r)
  ├── cloneLoop(ctx, r, t, opts) → *CloneResult
  │     ├── buildCloneRequest(seqno, cycle) → *xfer.Message
  │     ├── t.Exchange(ctx, req) → *xfer.Message
  │     └── processCloneResponse(msg) → (seqno, done, error)
  │           ├── handlePushCard() — extract project/server codes
  │           ├── handleFileCard() — store blob (reuse from client.go)
  │           ├── handleCloneSeqnoCard() — update seqno
  │           ├── handleCookieCard() — cache cookie
  │           └── handleErrorCard() — abort
  └── cleanup on error: os.Remove(path)
```

### Clone Session State

```go
type cloneSession struct {
    repo        *repo.Repo
    opts        CloneOpts
    seqno       int          // next rid to request (starts at 1)
    projectCode string       // received from server
    serverCode  string       // received from server
    cookie      string       // session cookie
    blobsRecvd  int          // total blobs received
    phantoms    map[string]bool
    filesRecvdThisRound int
}
```

### Reused Components

| Component | From | Usage |
|-----------|------|-------|
| `Transport` + `HTTPTransport` | sync/transport.go | HTTP POST to Fossil server |
| `xfer.CloneCard` | xfer/card.go | Clone request card |
| `xfer.CloneSeqNoCard` | xfer/card.go | Clone seqno response card |
| `xfer.Message.Encode/Decode` | xfer/message.go | Zlib compression |
| `computeLogin()` | sync/auth.go | Login card generation |
| `blob.Store`, `blob.Compress` | blob/ | Blob storage |
| `hash.SHA1`, `hash.SHA3` | hash/ | UUID verification |
| `hash.IsValidHash` | hash/ | UUID format validation |

### File Card Handling

The existing `handleFileCard()` in client.go operates on a `*session` receiver. For clone, we need the same logic (hash verification, delta application, blob storage) but on a `cloneSession`. Two options:

1. Extract `storeReceivedFile(r *repo.Repo, uuid, deltaSrc string, payload []byte) error` as a package-level function callable by both session types
2. Duplicate the logic in clone.go

Option 1 is cleaner — extract the core blob-storage logic into a shared function.

### Login Card Handling

Fossil's clone skips the login card on the first round (`xfer.c:2407`). On subsequent rounds, if credentials are provided, a login card is sent. We match this behavior:
- Round 0: no login card
- Round 1+: login card if `opts.User != ""`

## Testing Strategy

### Unit Tests (mock transport)

1. **TestCloneBasic** — Mock returns push card + 5 file cards + clone_seqno 0. Verify: repo created, 5 blobs stored, project-code set, result.BlobsRecvd == 5.

2. **TestCloneMultiRound** — Mock returns clone_seqno 3 on round 1 (with 2 blobs), clone_seqno 0 on round 2 (with 1 blob). Verify: 2 rounds, 3 total blobs.

3. **TestCloneWithPhantoms** — Mock sends a cfile (delta) before its base arrives. Verify: phantom created, gimme sent on next round, base delivered, delta resolved.

4. **TestCloneErrorCleansUp** — Mock returns error card. Verify: repo file deleted, error returned.

5. **TestCloneAuthFailure** — Mock returns "not authorized to clone" error. Verify: repo file deleted, error contains auth message.

6. **TestCloneExistingPath** — Call Clone with a path that already exists. Verify: error returned immediately, no network calls.

### Integration Test (real Fossil server)

7. **TestIntegrationClone** — Clone from a real Fossil server (if available). Informational — does not fail CI. Verify: `fossil rebuild` passes on the cloned repo.

### CLI Integration

8. Add `edgesync repo clone <url> <path>` subcommand that calls `sync.Clone()`.

## What We're NOT Doing

- Config transfer (`reqconfig` / `config` cards)
- Unversioned file sync (`uvfile`, `uvgimme`, `uv-pull-only`)
- Private content
- `--nocompress` option
- Checkout opening after clone
- Clone over SSH or filesystem (HTTP only via Transport)
