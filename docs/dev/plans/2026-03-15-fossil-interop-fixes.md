# Fossil Interop Fixes Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix 4 interop bugs preventing the Go sync engine from successfully syncing against a real Fossil 2.28 server.

**Architecture:** Targeted fixes to the sync protocol (version pragma, phantom eviction), blob storage (format detection), and agent config (auth cleanup). No new packages or files — all changes to existing code.

**Tech Stack:** Go 1.23, Fossil xfer protocol, zlib compression

**Spec:** `docs/superpowers/specs/2026-03-15-fossil-interop-fixes-design.md`

---

## File Structure

### Modified Files

| File | Change |
|------|--------|
| `go-libfossil/sync/client.go:24-27` | Fix version pragma to numeric format |
| `go-libfossil/sync/client.go:195-270` | Add phantom age tracking after card loop |
| `go-libfossil/sync/session.go:43-76` | Add `phantomAge` field to session struct |
| `go-libfossil/blob/blob.go:105-122` | Detect compressed vs uncompressed in Load() |
| `leaf/agent/config.go:13-65` | Remove `NoLogin` field and `User: "anonymous"` default |
| `leaf/agent/agent.go:212-225` | Remove NoLogin logic from runSync() |
| `sim/harness.go` | Remove `NoLogin: true` from agent configs |

---

## Chunk 1: Version Pragma + Blob Load + Phantom Eviction + Auth Cleanup

### Task 1: Fix version pragma to numeric format

**Files:**
- Modify: `go-libfossil/sync/client.go:24-27`
- Test: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Read the current pragma code**

Read `go-libfossil/sync/client.go` lines 20-30 to see the current pragma card construction.

- [ ] **Step 2: Write a test that verifies numeric pragma values**

Add to `go-libfossil/sync/sync_test.go`:

```go
func TestBuildRequestPragmaNumericVersion(t *testing.T) {
	s, _ := newTestSession(t, SyncOpts{Pull: true, ServerCode: "sc", ProjectCode: "pc"})
	msg, err := s.buildRequest(0)
	if err != nil {
		t.Fatalf("buildRequest: %v", err)
	}
	pragmas := cardsByType(msg, xfer.CardPragma)
	if len(pragmas) == 0 {
		t.Fatal("no pragma cards")
	}
	p := pragmas[0].(*xfer.PragmaCard)
	if p.Name != "client-version" {
		t.Fatalf("pragma name = %q", p.Name)
	}
	// Version must be numeric (Fossil uses atoi), >= 20000 for Fossil 2.0+ compat
	if len(p.Values) < 1 {
		t.Fatal("pragma client-version has no values")
	}
	var version int
	if _, err := fmt.Sscanf(p.Values[0], "%d", &version); err != nil {
		t.Fatalf("pragma version %q is not numeric: %v", p.Values[0], err)
	}
	if version < 20000 {
		t.Fatalf("pragma version %d < 20000 (Fossil 2.0 minimum)", version)
	}
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `go test ./go-libfossil/sync/ -run TestBuildRequestPragmaNumericVersion -v -count=1`
Expected: FAIL — current value `"go-libfossil/0.1"` is not numeric.

- [ ] **Step 4: Fix the pragma value**

In `go-libfossil/sync/client.go`, change the pragma card construction (around line 24-27):

```go
// Before:
cards = append(cards, &xfer.PragmaCard{
    Name:   "client-version",
    Values: []string{"go-libfossil/0.1"},
})

// After:
cards = append(cards, &xfer.PragmaCard{
    Name:   "client-version",
    Values: []string{"22800", "20260315", "120000"},
})
```

- [ ] **Step 5: Run test to verify it passes**

Run: `go test ./go-libfossil/sync/ -run TestBuildRequestPragmaNumericVersion -v -count=1`
Expected: PASS

- [ ] **Step 6: Run all sync tests**

Run: `go test ./go-libfossil/sync/ -v -count=1`
Expected: All pass. The existing `TestBuildRequestHasPragmaClientVersion` test checks the pragma exists but should still pass with the new values.

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/client.go go-libfossil/sync/sync_test.go
git commit -m "sync: fix version pragma to numeric format for Fossil 2.0+ compat"
```

---

### Task 2: Blob Load format detection

**Files:**
- Modify: `go-libfossil/blob/blob.go:105-122`
- Test: `go-libfossil/blob/blob_test.go`

- [ ] **Step 1: Read the current Load function**

Read `go-libfossil/blob/blob.go` lines 105-122.

- [ ] **Step 2: Write tests for both compressed and uncompressed blobs**

Add to `go-libfossil/blob/blob_test.go`:

```go
func TestLoadUncompressedBlob(t *testing.T) {
	// Simulate a Fossil-native uncompressed blob:
	// blob.size == len(blob.content) means uncompressed.
	r := testutil.CreateTestRepo(t)
	defer r.Close()

	content := []byte("uncompressed test blob content")
	// Insert directly with size == len(content) and raw (uncompressed) content.
	result, err := r.DB().Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		"deadbeefdeadbeefdeadbeefdeadbeefdeadbeef", len(content), content,
	)
	if err != nil {
		t.Fatalf("insert: %v", err)
	}
	rid, _ := result.LastInsertId()

	loaded, err := Load(r.DB(), libfossil.FslID(rid))
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if !bytes.Equal(loaded, content) {
		t.Fatalf("loaded %d bytes, want %d", len(loaded), len(content))
	}
}

func TestLoadCompressedBlob(t *testing.T) {
	// Normal path: blob.size > len(blob.content), content is zlib-compressed.
	r := testutil.CreateTestRepo(t)
	defer r.Close()

	content := []byte("compressed test blob content that should be stored with zlib")
	rid, _, err := Store(r.DB(), content)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	loaded, err := Load(r.DB(), rid)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if !bytes.Equal(loaded, content) {
		t.Fatalf("loaded content doesn't match original")
	}
}
```

Note: Check if `testutil.CreateTestRepo` exists or if tests use a different helper. Read `go-libfossil/blob/blob_test.go` first to see the existing test patterns.

- [ ] **Step 3: Run tests — the uncompressed test should fail**

Run: `go test ./go-libfossil/blob/ -run TestLoadUncompressedBlob -v -count=1`
Expected: FAIL — current `Load()` always calls `Decompress()` on raw data.

- [ ] **Step 4: Fix Load() to detect compression**

In `go-libfossil/blob/blob.go`, replace the `Load` function:

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

	// Fossil convention: if stored bytes == declared size, content is uncompressed.
	// If stored bytes < declared size, content is zlib-compressed.
	if int64(len(content)) == size {
		return content, nil
	}
	return Decompress(content)
}
```

- [ ] **Step 5: Run both tests**

Run: `go test ./go-libfossil/blob/ -run "TestLoadUncompressed|TestLoadCompressed" -v -count=1`
Expected: Both PASS.

- [ ] **Step 6: Run all blob tests**

Run: `go test ./go-libfossil/blob/ -v -count=1`
Expected: All pass.

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/blob/blob.go go-libfossil/blob/blob_test.go
git commit -m "blob: detect compressed vs uncompressed content in Load()"
```

---

### Task 3: Phantom eviction after 3 rounds

**Files:**
- Modify: `go-libfossil/sync/session.go` (session struct + newSession)
- Modify: `go-libfossil/sync/client.go` (processResponse)
- Test: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Read the session struct and processResponse**

Read `go-libfossil/sync/session.go` lines 43-76 (session struct, newSession).
Read `go-libfossil/sync/client.go` lines 195-270 (processResponse, convergence check).

- [ ] **Step 2: Write a test for phantom eviction**

Add to `go-libfossil/sync/sync_test.go`:

```go
func TestPhantomEvictionAfterThreeRounds(t *testing.T) {
	// Server sends igot for a blob but never delivers it as a file card.
	// After 3 rounds of gimme without delivery, the phantom should be evicted.
	s, _ := newTestSession(t, SyncOpts{
		Pull: true, ServerCode: "sc", ProjectCode: "pc",
	})

	// Simulate: server sends igot for an artifact we don't have.
	undeliverableUUID := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	serverResp := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.IGotCard{UUID: undeliverableUUID},
			&xfer.CookieCard{Value: "c"},
		},
	}

	// Process 3 rounds where the phantom is never resolved.
	for round := 0; round < 3; round++ {
		done, err := s.processResponse(serverResp)
		if err != nil {
			t.Fatalf("round %d: %v", round, err)
		}
		if done {
			t.Fatalf("round %d: should not converge yet", round)
		}
		// After first round, phantom exists.
		if round == 0 {
			if len(s.phantoms) != 1 {
				t.Fatalf("round %d: expected 1 phantom, got %d", round, len(s.phantoms))
			}
		}
	}

	// After 3 rounds of non-delivery, phantom should be evicted.
	// Process one more response to trigger convergence check.
	emptyResp := &xfer.Message{Cards: []xfer.Card{&xfer.CookieCard{Value: "c"}}}
	done, err := s.processResponse(emptyResp)
	if err != nil {
		t.Fatalf("final round: %v", err)
	}
	if len(s.phantoms) != 0 {
		t.Fatalf("phantom should be evicted, got %d", len(s.phantoms))
	}
	// With phantom evicted and no other work, should converge.
	_ = done // convergence depends on unsent table too
}
```

- [ ] **Step 3: Run test to verify it fails**

Run: `go test ./go-libfossil/sync/ -run TestPhantomEvictionAfterThreeRounds -v -count=1`
Expected: FAIL — no eviction logic exists yet.

- [ ] **Step 4: Add phantomAge field to session**

In `go-libfossil/sync/session.go`, add to the session struct:

```go
phantomAge map[string]int // UUID -> consecutive rounds gimme'd without delivery
```

In `newSession()`, initialize it:

```go
phantomAge: make(map[string]int),
```

- [ ] **Step 5: Add eviction logic to processResponse**

In `go-libfossil/sync/client.go`, in `processResponse()`, after the card processing loop (after the `for _, card := range msg.Cards` block ends at ~line 239) and before the convergence check (~line 244), add:

```go
// Track which phantoms were resolved this round (received as file cards).
// FileCard/CFileCard handlers already delete from s.phantoms, so any UUID
// still in s.phantoms after the loop was NOT resolved.
for uuid := range s.phantoms {
	s.phantomAge[uuid]++
	if s.phantomAge[uuid] >= 3 {
		delete(s.phantoms, uuid)
		delete(s.phantomAge, uuid)
	}
}
// Clean up age entries for phantoms that were resolved (no longer in map).
for uuid := range s.phantomAge {
	if !s.phantoms[uuid] {
		delete(s.phantomAge, uuid)
	}
}
```

- [ ] **Step 6: Run the eviction test**

Run: `go test ./go-libfossil/sync/ -run TestPhantomEvictionAfterThreeRounds -v -count=1`
Expected: PASS

- [ ] **Step 7: Run all sync tests**

Run: `go test ./go-libfossil/sync/ -v -count=1`
Expected: All pass.

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/sync/session.go go-libfossil/sync/client.go go-libfossil/sync/sync_test.go
git commit -m "sync: evict stale phantoms after 3 rounds without delivery"
```

---

### Task 4: Auth cleanup — remove NoLogin, fix defaults

**Files:**
- Modify: `leaf/agent/config.go:13-65`
- Modify: `leaf/agent/agent.go:212-225`
- Modify: `sim/harness.go`
- Test: `leaf/agent/agent_test.go`

- [ ] **Step 1: Read the files**

Read `leaf/agent/config.go`, `leaf/agent/agent.go` lines 210-225, and grep for `NoLogin` in `sim/harness.go`.

- [ ] **Step 2: Remove NoLogin from config.go**

In `leaf/agent/config.go`:
- Remove the `NoLogin bool` field and its doc comment.
- Remove `c.User = "anonymous"` from `applyDefaults()`. Leave the `if c.User == ""` block empty or remove it entirely — empty User means no auth.

- [ ] **Step 3: Remove NoLogin logic from agent.go**

In `leaf/agent/agent.go` `runSync()`, remove the `NoLogin` conditional:

```go
// Remove this block:
user := a.config.User
if a.config.NoLogin {
    user = ""
}

// Replace with just using a.config.User directly:
opts := sync.SyncOpts{
    Push:        a.config.Push,
    Pull:        a.config.Pull,
    ProjectCode: a.projectCode,
    ServerCode:  a.serverCode,
    User:        a.config.User,
    Password:    a.config.Password,
    Buggify:     a.config.Buggify,
}
```

- [ ] **Step 4: Remove NoLogin from sim/harness.go**

Remove `NoLogin: true,` from all agent.Config blocks in `sim/harness.go` (there should be 3 occurrences: StartAgents, restartLeaf, and RunSimulation).

- [ ] **Step 5: Build and test**

Run: `go build ./leaf/agent/ ./sim/ && go test ./leaf/agent/ -v -count=1`
Expected: Build succeeds, all agent tests pass.

- [ ] **Step 6: Commit**

```bash
git add leaf/agent/config.go leaf/agent/agent.go sim/harness.go
git commit -m "agent: remove NoLogin workaround, empty User means no auth"
```

---

### Task 5: Validate with sim test

**Files:** None (validation only)

- [ ] **Step 1: Run all unit tests**

Run: `go test ./go-libfossil/sync/ ./go-libfossil/blob/ ./leaf/agent/ ./bridge/bridge/ -count=1`
Expected: All pass.

- [ ] **Step 2: Run DST tests**

Run: `go test ./dst/ -run TestDST -count=1`
Expected: All pass (MockFossil path unaffected).

- [ ] **Step 3: Run sim test against real Fossil**

Run: `go test ./sim/ -run TestSimulation -sim.seed=42 -v -timeout=120s`
Expected: Sync should now exchange blobs with the Fossil server. Look for:
- No "Fossil version 2.0 or later required" errors
- No "login failed" errors
- `rounds=N` with N < 100 (convergence achieved)
- Invariant results (may still have issues with the Fossil-native initial blob, but leaf blobs should converge)

- [ ] **Step 4: Commit validation results**

If the sim test passes or shows meaningful progress, commit:
```bash
git add -A
git commit -m "sim: validate Fossil interop fixes — sync converges against real Fossil"
```

If there are remaining issues, document them but still commit the fixes — they're independently correct.
