# Server-Side IGot Filtering (remoteHas)

**Date:** 2026-04-07
**Target repo:** go-libfossil
**Linear:** EDG-16

## Problem

The go-libfossil sync handler (`sync/handler.go`) emits igot cards for every non-shunned, non-private blob in the repository on every pull response. For a repo with 10K blobs, that means 10K igot cards per round-trip — even if the client already has 9,999 of them.

Fossil's C implementation solves this with a per-request `onremote` temp table: when the server receives igot cards from the client (announcing what the client has), it records those rids, then filters its own igot emission to exclude them. go-libfossil's handler currently discards this information — `handleIGot()` checks whether the blob exists locally and may emit a gimme, but never records the fact that the client already has it.

## Fossil Reference

In `xfer.c`, the server-side flow within a single HTTP round-trip:

1. `CREATE TEMP TABLE onremote(rid INTEGER PRIMARY KEY)` at request start (line 1321)
2. When an igot card arrives from the client, `remote_has(rid)` inserts into `onremote` (line 1471)
3. `send_unclustered()` and `send_all_clusters()` add `NOT EXISTS(SELECT 1 FROM onremote WHERE rid=blob.rid)` to their queries (lines 1011–1012, 1056–1057)
4. `DROP TABLE onremote` after the round-trip completes (line 1927)

The cookie card (`xfer.c:1678–1679`) is defined in the protocol spec but the server-side implementation is a stub — `/* Process the cookie */` with no code. Fossil relies entirely on per-round igot filtering, not cross-session cookies.

## Design

### Approach: Go map on handler struct

Add a `remoteHas map[string]bool` field to the `handler` struct. This is the Go equivalent of Fossil's `onremote` temp table, adapted to go-libfossil's UUID-string conventions (Fossil uses integer rids).

**Why a Go map instead of a SQL temp table:** go-libfossil's igot queries select UUID strings. Using a SQL temp table would require either text joins (slow) or an extra rid lookup per igot card (extra round-trip per card). Filtering in Go after the scan is simpler. The wire bandwidth savings from not emitting redundant igot cards is the real win — not avoiding the SQL scan.

### Changes

**1. Handler struct** (`sync/handler.go`)

Add one field:

```go
type handler struct {
    // ...existing fields...
    remoteHas map[string]bool // UUIDs the client announced via igot
}
```

Lazily initialized — stays `nil` until the first igot card is processed. Zero overhead for clone requests and push-only sessions.

**2. handleIGot()** (`sync/handler.go`, currently line 332)

After confirming the blob exists locally, record the UUID:

```go
func (h *handler) handleIGot(c *xfer.IGotCard) error {
    if c == nil {
        panic("handler.handleIGot: c must not be nil")
    }
    if !h.pullOK {
        return nil
    }
    _, exists := blob.Exists(h.repo.DB(), c.UUID)
    if exists {
        if h.remoteHas == nil {
            h.remoteHas = make(map[string]bool)
        }
        h.remoteHas[c.UUID] = true
        return nil
    }
    if c.IsPrivate && !h.syncPrivate {
        return nil
    }
    h.resp = append(h.resp, &xfer.GimmeCard{UUID: c.UUID})
    return nil
}
```

This mirrors Fossil's `remote_has(rid)` call at `xfer.c:1471` — same trigger point (igot card for a known blob), same semantics (record it so we skip it in emission).

**3. emitIGots()** (`sync/handler.go`, currently line 447)

Add a filter in the collection loop:

```go
for rows.Next() {
    var uuid string
    if err := rows.Scan(&uuid); err != nil {
        return err
    }
    if h.remoteHas[uuid] {
        continue
    }
    uuids = append(uuids, uuid)
}
```

`nil` map lookup returns `false`, so this is safe without a nil check.

**4. emitPrivateIGots()** (`sync/handler.go`, currently line 493)

Same one-line filter in the collection loop:

```go
if h.remoteHas[uuid] {
    continue
}
```

**5. sendAllClusters()** (`sync/handler.go`, currently line 527)

Same one-line filter in the collection loop:

```go
if h.remoteHas[uuid] {
    continue
}
```

### What does NOT change

- **Client side:** `sync/client.go` already maintains `session.remoteHas` populated from server igots, and `sendUnclustered()` / `sendAllClusters()` already filter against it. No changes needed.
- **Cookie card:** Remains a stub matching upstream Fossil. Client parses, caches, and echoes; server ignores. `session.cookie` field stays for forward compatibility.
- **HandleOpts / SyncOpts:** No new fields. The optimization is internal to the handler.
- **SyncResult:** No new fields. The optimization is transparent to callers.
- **Wire protocol:** No changes. Same cards, same format. Fewer igot cards in responses is the only observable difference.

## Testing

### Unit test: TestHandlerIGotFiltersEmit

In `sync/handler_test.go` (or `sync/sync_test.go`):

1. Create a repo with N blobs (e.g., 5 checkins).
2. Build an xfer request containing: `pull` card + igot cards for a known subset of the repo's UUIDs.
3. Call `HandleSyncWithOpts()` with that request.
4. Decode the response and collect the igot cards.
5. Assert: response igot UUIDs do NOT include any UUID from the client's igot set.
6. Assert: response igot UUIDs DO include all blobs not in the client's igot set.

### Unit test: TestHandlerIGotFilterPrivate

Same as above but with private blobs and `pragma send-private`. Assert private igots are also filtered when the client announces them.

### DST

Existing DST tests run multi-round sync sessions. After this change:
- Convergence still occurs (correctness).
- The BUGGIFY truncation in `emitIGots` operates on a now-smaller list — still exercised.
- No new DST-specific test needed unless we want to assert payload size reduction (optional follow-up).

### Interop (sim)

The sim tests running `fossil clone` / `fossil sync` against the Go server continue to pass. Fossil's client sends igot cards; the server now filters its response. The optimization is automatic and protocol-compatible.

## Migration

None. No schema changes, no config changes, no new dependencies. The change is internal to the handler's per-request lifecycle.

## Scope Boundary

**In scope (this PR):**
- `handler` struct field
- `handleIGot()` population
- `emitIGots()`, `emitPrivateIGots()`, `sendAllClusters()` filtering
- Unit tests

**Out of scope:**
- Cross-session cookie persistence (cookie is a stub — nothing to persist)
- Client-side changes (already correct)
- Payload size metrics/observability (follow-up)
- EDG-16 ticket may be narrowed or closed after this lands — the Fossil-faithful optimization is the `onremote` pattern, not cookies
