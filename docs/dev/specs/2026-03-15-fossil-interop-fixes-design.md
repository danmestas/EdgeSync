# Fossil Interop Fixes

**Date:** 2026-03-15
**Status:** Draft
**Scope:** sync protocol, blob storage, agent config

## Problem

The `sim/` integration harness running against real Fossil 2.28 revealed 3 interop bugs that prevent sync from working. An xfer trace captured the exact failure modes.

## Trace Summary

- **Round 0:** Leaf sends push+pull+igot. Server responds with gimme cards (wants blobs) + igot for its own blob.
- **Round 1:** Leaf sends file cards. Server responds with `"Fossil version 2.0 or later required"` error, re-gimmes the same blobs, never delivers its own blob.
- **Round 2+:** Same pattern indefinitely. Convergence impossible.

## Fix 1: Version Pragma (Critical)

**Root cause:** We send `pragma client-version go-libfossil/0.1`. Fossil parses this with `atoi()` → 0. Fossil checks `remoteVersion < 20000` and refuses to send SHA3 blobs or accept file cards from clients claiming to be pre-2.0.

**Fix:** Change the pragma value from `"go-libfossil/0.1"` to numeric format matching Fossil's convention:

```
pragma client-version 22800 20260315 120000
```

- `22800` = Fossil 2.28 protocol compatibility level
- `20260315` = build date (YYYYMMDD as integer)
- `120000` = build time (HHMMSS as integer)

**File:** `go-libfossil/sync/client.go:24-27`

**Change:**
```go
// Before:
&xfer.PragmaCard{Name: "client-version", Values: []string{"go-libfossil/0.1"}}

// After:
&xfer.PragmaCard{Name: "client-version", Values: []string{"22800", "20260315", "120000"}}
```

**Impact:** Unblocks file card acceptance. Server will accept pushed blobs and serve SHA3 blobs. This is the single highest-leverage fix — convergence should start working once file cards are accepted.

## Fix 2: Stale Phantom Eviction (Convergence)

**Root cause:** The server sends `igot` for a blob each round, we send `gimme`, but the server never delivers the file card. This creates a permanent phantom that blocks convergence because `len(s.phantoms) > 0` prevents the convergence check from returning true.

**Why it happens:** The blob may be a control artifact, or the server withholds it due to permissions. Once Fix 1 is applied, this may self-resolve (the server was refusing SHA3 blobs to our "old" client). However, defensive handling is still needed for any unresolvable artifacts.

**Fix:** Track how many consecutive rounds each phantom has been gimme'd without delivery. After 3 rounds, evict it.

**File:** `go-libfossil/sync/session.go` (session struct), `go-libfossil/sync/client.go` (processResponse)

**Changes:**

Add to session struct:
```go
phantomAge map[string]int // UUID -> consecutive rounds gimme'd without delivery
```

Initialize in `newSession()`:
```go
phantomAge: make(map[string]int),
```

In `processResponse()`, after the card processing loop, before the convergence check:
```go
// Track which phantoms were resolved this round.
resolved := make(map[string]bool)
// (FileCard/CFileCard handlers already delete from s.phantoms —
//  collect resolved UUIDs there)

// Age unresolved phantoms; evict after 3 rounds.
for uuid := range s.phantoms {
    s.phantomAge[uuid]++
    if s.phantomAge[uuid] >= 3 {
        delete(s.phantoms, uuid)
        delete(s.phantomAge, uuid)
    }
}
// Clean up ages for resolved phantoms.
for uuid := range resolved {
    delete(s.phantomAge, uuid)
}
```

**Impact:** Prevents any single unresolvable artifact from blocking convergence forever. The happy path (phantoms resolved within 1-2 rounds) is unaffected.

## Fix 3: Auth Protocol (Full User/Password Support)

**Root cause:** The `computeLogin()` function in `auth.go` is actually correct for normal users — it computes `SHA1(project-code/user/password)` which matches what Fossil stores in `user.pw`. The real issue was with the `anonymous` user, which has a random cleartext password we don't know.

**Fix:** Rather than implementing special anonymous auth handling, simplify the auth contract:

1. **If User and Password are both set:** Send login card with proper SHA1 auth (existing `computeLogin` logic).
2. **If either is empty:** Skip login card. Sync runs as unauthenticated "nobody".

**Files:** `leaf/agent/config.go`, `leaf/agent/agent.go`, `go-libfossil/sync/client.go`

**Changes:**

In `agent/config.go` `applyDefaults()`:
- Remove `c.User = "anonymous"` default. Empty User means no auth.

In `agent/agent.go` `runSync()`:
- Remove `NoLogin` field and its logic. The existing check in `buildRequest()` already skips login when `s.opts.User == ""`.

In `sim/harness.go`:
- Remove `NoLogin: true` from all agent.Config blocks.
- Keep User empty (no auth — sim uses `nobody` capabilities on server).

**Impact:** Authenticated sync works for real users with passwords. Anonymous/unauthenticated sync works by leaving User empty. The `NoLogin` workaround is removed.

## Fix 4: Blob Format Detection in Load()

**Root cause:** `blob.Load()` always calls `Decompress()`. Blobs created by Fossil's C binary (like the initial manifest from `fossil new`) may be stored uncompressed. Decompressing uncompressed data fails with "zlib: invalid header".

**Fossil's convention:** `blob.size` = uncompressed size. If `len(content) == size`, data is uncompressed. If `len(content) < size`, data is zlib-compressed.

**File:** `go-libfossil/blob/blob.go:105-122`

**Change:**
```go
func Load(q db.Querier, rid libfossil.FslID) ([]byte, error) {
    var content []byte
    var size int64
    err := q.QueryRow("SELECT content, size FROM blob WHERE rid=?", rid).Scan(&content, &size)
    if err != nil {
        return nil, fmt.Errorf("blob.Load query: %w", err)
    }
    if size == -1 {
        return nil, fmt.Errorf("blob.Load: rid %d is a phantom", rid)
    }
    if content == nil {
        return nil, fmt.Errorf("blob.Load: rid %d has NULL content", rid)
    }

    // Detect compression: if stored bytes == declared size, content is uncompressed.
    // If stored bytes < declared size, content is zlib-compressed.
    if int64(len(content)) == size {
        return content, nil
    }
    return Decompress(content)
}
```

**Impact:** Invariant checker can read blobs from both Go-created repos and Fossil-native repos.

## Testing

1. **Unit tests:** Each fix has a targeted unit test.
   - Version pragma: verify the pragma card has numeric values.
   - Phantom eviction: mock transport that never delivers a gimme'd blob; verify convergence after 3 rounds.
   - Auth: existing `TestComputeLogin` still passes; new test for empty-user-skips-login.
   - Blob Load: test with both compressed and uncompressed content.

2. **Sim test:** After all fixes, `go test ./sim/ -run TestSimulation -sim.seed=42` should show successful sync with convergence.

3. **DST tests:** Verify `go test ./dst/ -run TestDST` still passes (MockFossil path unaffected).

4. **Existing tests:** `go test ./go-libfossil/sync/ ./leaf/agent/ ./bridge/bridge/` all pass.

## Implementation Order

1. Version pragma fix (unblocks everything)
2. Blob Load format detection (unblocks invariant checking)
3. Phantom eviction (ensures convergence)
4. Auth cleanup (remove NoLogin, fix defaults)
5. Run sim test to validate
6. Commit

## Dependencies

No new dependencies. All changes are in existing packages.
