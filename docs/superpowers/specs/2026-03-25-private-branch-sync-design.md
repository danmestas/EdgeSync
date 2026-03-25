# Private Branch Sync — Design Spec

**Ticket:** CDG-117
**Date:** 2026-03-25
**Status:** Approved

## Summary

Port Fossil's private artifact sync to go-libfossil. Private artifacts are blobs listed in the `private` table. They are excluded from normal sync and only transferred when both peers have the `x` capability and the client explicitly opts in via `pragma send-private`.

## Fossil Reference

Fossil's `xfer.c` implements private sync with these mechanics:

- **`private` table** — `CREATE TABLE private(rid INTEGER PRIMARY KEY)`. Tracks which blobs are private.
- **`private` card** — a bare `private\n` line that precedes a `file` or `cfile` card, marking the next payload as private content.
- **`igot UUID 1`** — the second argument `1` signals the artifact is private. Peers without `x` capability treat these as "don't request this."
- **`pragma send-private`** — client requests the server to include private artifacts in its responses.
- **`x` capability** — required to sync private content. Not implied by `a` (admin) or `s` (setup) — must be explicitly granted.
- **`content_is_private(rid)`** — checks the `private` table.
- **`content_make_private(rid)` / `content_make_public(rid)`** — inserts/removes from the `private` table. Receiving a public artifact that was previously private makes it public.

### Key behaviors from Fossil C source

1. **`send_file`**: If artifact is private and `syncPrivate` is false, emit `igot UUID 1` instead of sending file content. If `syncPrivate` is true, prepend `private\n` before the `file` card.
2. **`send_unsent`**: `SELECT rid FROM unsent EXCEPT SELECT rid FROM private` — never proactively send private blobs in normal sync.
3. **`send_unclustered`**: Excludes `NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)` from igot emission.
4. **`send_all_clusters`**: Same private exclusion.
5. **`request_phantoms`**: Excludes private phantoms when `syncPrivate` is false.
6. **`send_private`**: Separate function that emits `igot UUID 1` for all private blobs when `syncPrivate` is true.
7. **Server `private` card**: Sets `nextIsPrivate` flag. If peer lacks `x` capability, emits error.
8. **Server `pragma send-private`**: Checks `x` capability, sets `syncPrivate` on the session.
9. **Server `igot` with IsPrivate**: If authorized, creates private phantom. If not authorized, marks as private and doesn't request.
10. **Client `igot` with IsPrivate**: If pulling private, creates private phantom. Otherwise marks existing blob as private and ignores.
11. **Client `private` card**: Sets `nextIsPrivate`. Next file/cfile is stored and marked private.
12. **Delta safety**: Never send a delta against a private artifact (the base might not exist on the peer).

## Design

### 1. Content helpers — `content/private.go`

New file in `go-libfossil/content/`:

```go
// IsPrivate returns true if the blob with the given rid is in the private table.
func IsPrivate(db *db.DB, rid int64) bool

// MakePrivate inserts the rid into the private table (no-op if already present).
func MakePrivate(db *db.DB, rid int64) error

// MakePublic removes the rid from the private table (no-op if not present).
func MakePublic(db *db.DB, rid int64) error
```

### 2. Auth — `auth/auth.go`

New capability check:

```go
func CanSyncPrivate(caps string) bool { return HasCapability(caps, 'x') }
```

### 3. Client-side — `sync/`

#### SyncOpts

New field:

```go
type SyncOpts struct {
    // ... existing fields ...
    Private bool // enable private artifact sync (requires 'x' capability)
}
```

#### First round — `pragma send-private`

When `opts.Private` is true, emit `pragma send-private\n` on the first round (same pattern as `pragma uv-hash`).

#### `sendUnclustered`

Add exclusion filter when `!s.opts.Private`:

```sql
SELECT uuid FROM unclustered JOIN blob USING(rid)
WHERE size >= 0
  AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)
```

#### `sendPrivate`

New function, called after `sendUnclustered` in `buildRequest` when `s.opts.Private`. Emits `igot UUID 1` for all private blobs:

```sql
SELECT uuid FROM private JOIN blob USING(rid) WHERE blob.size >= 0
```

#### `sendAllClusters`

Add same `NOT EXISTS(SELECT 1 FROM private ...)` filter.

#### `buildFileCards`

Exclude private blobs from the unsent table query: `SELECT rid FROM unsent EXCEPT SELECT rid FROM private` (matches Fossil's `send_unsent`).

Before emitting a `FileCard` for a private blob (from `pendingSend`), prepend a `PrivateCard`. Check `content.IsPrivate(db, rid)` when loading.

Delta safety: when building a delta, skip if the delta source is private UNLESS `opts.Private` is true. Fossil allows deltas against private sources when both peers are in private sync mode (xfer.c line ~453).

#### `buildGimmeCards`

When `!opts.Private`, exclude private phantoms:

```sql
AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)
```

#### `processResponse`

- **`PrivateCard`**: Set `s.nextIsPrivate = true`.
- **`FileCard` / `CFileCard`**: After storing, if `s.nextIsPrivate`, call `content.MakePrivate` and reset flag. If `!s.nextIsPrivate`, call `content.MakePublic` (a public artifact clears any prior private status).
- **`IGotCard` with `IsPrivate`**:
  - If `s.opts.Private`: create private phantom (request it), call `MakePrivate`.
  - If `!s.opts.Private`: if blob exists locally, call `content.MakePrivate`. Do NOT create phantom (don't request it).

### 4. Server-side — `sync/handler.go`

#### Handler state

New fields on the `handler` struct:

```go
syncPrivate    bool // true if pragma send-private was accepted
nextIsPrivate  bool // true if a private card precedes the next file/cfile
```

#### `pragma send-private`

In `handleControlCard`, handle the pragma:

```go
if c.Name == "send-private" {
    if auth.CanSyncPrivate(h.caps) {
        h.syncPrivate = true
    } else {
        h.resp = append(h.resp, &xfer.ErrorCard{
            Message: "not authorized to sync private content",
        })
    }
}
```

#### `private` card

In `handleDataCard`, handle `*xfer.PrivateCard`:

```go
case *xfer.PrivateCard:
    if !auth.CanSyncPrivate(h.caps) {
        h.resp = append(h.resp, &xfer.ErrorCard{
            Message: "not authorized to sync private content",
        })
        h.nextIsPrivate = false // ensure flag is NOT set on auth failure
    } else {
        h.nextIsPrivate = true
    }
```

#### File/CFile handling

After storing a received file, if `h.nextIsPrivate`:
- Call `content.MakePrivate(db, rid)`.
- Reset `h.nextIsPrivate = false`.

If not `nextIsPrivate`, call `content.MakePublic(db, rid)` — a public artifact replaces any prior private status.

#### `emitIGots`

Exclude private blobs:

```sql
SELECT uuid FROM blob
WHERE size >= 0
  AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)
```

When `h.syncPrivate`, also emit `igot UUID 1` for private blobs via a separate query on the `private` table.

#### `emitCloneBatch`

Same private exclusion. When `syncPrivate`, include private blobs with `private\n` prefix.

#### `handleIGotCard` (in `handleDataCard`)

Respect `IsPrivate` flag:
- If `IsPrivate` and `h.syncPrivate` (authorized): create phantom, mark private.
- If `IsPrivate` and `!h.syncPrivate`: mark as private if exists locally, don't create phantom.
- If not `IsPrivate` and blob exists: call `MakePublic` (in case it was previously private).

### 5. Testing

#### Unit tests

- `content/private_test.go` — `IsPrivate`, `MakePrivate`, `MakePublic` round-trip.
- `auth/auth_test.go` — `CanSyncPrivate` checks `'x'`.

#### Handler tests (`sync/handler_test.go`)

- `TestHandlerPrivateCardAccepted` — with `x` capability, private + file stores and marks private.
- `TestHandlerPrivateCardRejected` — without `x`, error card emitted.
- `TestHandlerPragmaSendPrivate` — with `x`, sets syncPrivate; without `x`, error.
- `TestHandlerIGotPrivate` — private igot creates private phantom when authorized.
- `TestEmitIGotsExcludesPrivate` — private blobs excluded from igot emission in normal sync.
- `TestEmitIGotsIncludesPrivateWhenAuthorized` — private blobs emitted as `igot UUID 1` when syncPrivate.

#### Client tests (`sync/client_test.go` or similar)

- `TestSyncPrivatePragmaSent` — `Private: true` emits `pragma send-private`.
- `TestSyncPrivateIGotEmission` — private blobs emitted as `igot UUID 1`.
- `TestSyncPrivateFileCardPrefix` — private file cards preceded by `PrivateCard`.
- `TestSyncExcludesPrivateByDefault` — `Private: false` excludes private from igot/gimme.

#### Integration tests

- `TestSyncPrivateArtifactTransition` — receive blob as private, then receive same UUID as public. Verify `MakePublic` clears private status.
- `TestSyncPrivateDeltaSource` — delta against private artifact is sent as full content when `Private: false`, but as delta when `Private: true`.
- `TestSyncMixedPrivatePublic` — sync with a mix of private and public artifacts. Verify igot filtering works correctly.

#### DST scenario (`dst/scenario_test.go`)

- `TestScenarioPrivateSync` — master has mix of public and private blobs. Two leaves sync: one with `x` capability gets all blobs, one without `x` gets only public blobs.

#### Sim equivalence test (`sim/equivalence_test.go`)

- `TestPrivateSyncAgainstFossilServe` — create Fossil repo with private branch, grant `x` to nobody, serve via `fossil serve`, sync with go-libfossil `Private: true`, verify private artifacts arrive.

#### Clone tests

- `TestCloneExcludesPrivate` — clone without `Private: true` excludes private artifacts.
- `TestCloneIncludesPrivateWhenAuthorized` — clone with `Private: true` and `x` capability includes private artifacts with `private\n` prefix.

### 6. Files changed

| File | Change |
|------|--------|
| `go-libfossil/content/private.go` | **New** — `IsPrivate`, `MakePrivate`, `MakePublic` |
| `go-libfossil/content/private_test.go` | **New** — unit tests |
| `go-libfossil/auth/auth.go` | Add `CanSyncPrivate` |
| `go-libfossil/auth/auth_test.go` | Add `TestCanSyncPrivate` |
| `go-libfossil/sync/session.go` | Add `Private` to `SyncOpts`, `nextIsPrivate` to session |
| `go-libfossil/sync/client.go` | Private filtering in `sendUnclustered`, `sendAllClusters`, `buildFileCards`, `buildGimmeCards`, `processResponse`; new `sendPrivate()` |
| `go-libfossil/sync/handler.go` | `syncPrivate`/`nextIsPrivate` fields, `pragma send-private`, `PrivateCard` handling, igot filtering, `MakePrivate`/`MakePublic` on receive |
| `go-libfossil/sync/handler_test.go` | Handler tests for private sync |
| `dst/scenario_test.go` | DST private sync scenario |
| `sim/equivalence_test.go` | Equivalence test against real Fossil |

### 7. Not in scope

- **Shun table** — CDG-166 covers shun/private exclusions in cluster generation. This ticket focuses on sync protocol handling.
- **Private branch creation UI** — creating private branches (tagging with `private`) is a separate concern from syncing them.
- **`content_deltify` for private** — Fossil re-deltifies after making content public. Deferred.
- **`remoteDate` backward compatibility** — Fossil checks `remoteDate>=20200413` before emitting `igot UUID 1` to avoid confusing older clients. go-libfossil always emits `igot UUID 1` for private artifacts, assuming all peers are go-libfossil or modern Fossil (2020+).
- **Observer/telemetry counters** — `RoundStats.PrivateIGotsSent` (matching Fossil's `nPrivIGot`) is deferred to a follow-up.
