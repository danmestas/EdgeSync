# Private Branch Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Fossil's private artifact sync to go-libfossil — private blobs are excluded from normal sync and only transferred when both peers have `x` capability and the client opts in via `pragma send-private`.

**Architecture:** Three layers: content helpers (`IsPrivate`/`MakePrivate`/`MakePublic`), auth capability check (`CanSyncPrivate` for `x`), and sync protocol changes (client filters igot/file/gimme; server handles `pragma send-private`, `private` card, and igot filtering). The `private` table already exists in the schema.

**Tech Stack:** Go, SQLite, xfer card protocol

**Spec:** `docs/superpowers/specs/2026-03-25-private-branch-sync-design.md`
**Worktree:** `.worktrees/cdg-117`
**Branch:** `feature/cdg-117-handle-private-branch-cards`

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `go-libfossil/content/private.go` | Create | `IsPrivate`, `MakePrivate`, `MakePublic` |
| `go-libfossil/content/private_test.go` | Create | Unit tests for private helpers |
| `go-libfossil/auth/auth.go` | Modify | Add `CanSyncPrivate` |
| `go-libfossil/auth/auth_test.go` | Modify | Add `TestCanSyncPrivate` |
| `go-libfossil/sync/session.go` | Modify | Add `Private` to `SyncOpts`, `nextIsPrivate` to session |
| `go-libfossil/sync/handler.go` | Modify | `syncPrivate`/`nextIsPrivate` on handler, pragma handling, private card dispatch, igot/clone filtering, MakePrivate/MakePublic on receive |
| `go-libfossil/sync/handler_test.go` | Modify | Handler tests for private sync |
| `go-libfossil/sync/client.go` | Modify | `sendUnclustered`/`sendAllClusters`/`buildFileCards`/`buildGimmeCards`/`processResponse` private filtering; new `sendPrivate()` |
| `dst/scenario_test.go` | Modify | DST private sync scenario |
| `sim/equivalence_test.go` | Modify | Equivalence test against real `fossil serve` |

---

### Task 1: Content helpers — `IsPrivate`, `MakePrivate`, `MakePublic`

**Files:**
- Create: `go-libfossil/content/private.go`
- Create: `go-libfossil/content/private_test.go`

- [ ] **Step 1: Write failing tests**

In `go-libfossil/content/private_test.go`:

```go
package content

import (
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

func TestIsPrivate_NotPrivate(t *testing.T) {
	r := testutil.NewTestRepo(t)
	rid, _, _ := blob.Store(r.DB(), []byte("public blob"))
	if IsPrivate(r.DB(), int64(rid)) {
		t.Error("blob should not be private by default")
	}
}

func TestMakePrivate_ThenIsPrivate(t *testing.T) {
	r := testutil.NewTestRepo(t)
	rid, _, _ := blob.Store(r.DB(), []byte("will be private"))
	if err := MakePrivate(r.DB(), int64(rid)); err != nil {
		t.Fatalf("MakePrivate: %v", err)
	}
	if !IsPrivate(r.DB(), int64(rid)) {
		t.Error("blob should be private after MakePrivate")
	}
}

func TestMakePrivate_Idempotent(t *testing.T) {
	r := testutil.NewTestRepo(t)
	rid, _, _ := blob.Store(r.DB(), []byte("double private"))
	MakePrivate(r.DB(), int64(rid))
	if err := MakePrivate(r.DB(), int64(rid)); err != nil {
		t.Fatalf("second MakePrivate should not error: %v", err)
	}
}

func TestMakePublic_ClearsPrivate(t *testing.T) {
	r := testutil.NewTestRepo(t)
	rid, _, _ := blob.Store(r.DB(), []byte("private then public"))
	MakePrivate(r.DB(), int64(rid))
	if err := MakePublic(r.DB(), int64(rid)); err != nil {
		t.Fatalf("MakePublic: %v", err)
	}
	if IsPrivate(r.DB(), int64(rid)) {
		t.Error("blob should not be private after MakePublic")
	}
}

func TestMakePublic_NoopIfNotPrivate(t *testing.T) {
	r := testutil.NewTestRepo(t)
	rid, _, _ := blob.Store(r.DB(), []byte("never private"))
	if err := MakePublic(r.DB(), int64(rid)); err != nil {
		t.Fatalf("MakePublic on non-private should not error: %v", err)
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/content/ -run TestIsPrivate -v`
Expected: FAIL — `IsPrivate` not defined

- [ ] **Step 3: Write implementation**

In `go-libfossil/content/private.go`:

```go
package content

import (
	"github.com/dmestas/edgesync/go-libfossil/db"
)

// IsPrivate returns true if the blob with the given rid is in the private table.
func IsPrivate(q db.Querier, rid int64) bool {
	if q == nil {
		panic("content.IsPrivate: q must not be nil")
	}
	var n int
	err := q.QueryRow("SELECT 1 FROM private WHERE rid=?", rid).Scan(&n)
	return err == nil
}

// MakePrivate inserts the rid into the private table (no-op if already present).
func MakePrivate(q db.Querier, rid int64) error {
	if q == nil {
		panic("content.MakePrivate: q must not be nil")
	}
	_, err := q.Exec("INSERT OR IGNORE INTO private(rid) VALUES(?)", rid)
	return err
}

// MakePublic removes the rid from the private table (no-op if not present).
func MakePublic(q db.Querier, rid int64) error {
	if q == nil {
		panic("content.MakePublic: q must not be nil")
	}
	_, err := q.Exec("DELETE FROM private WHERE rid=?", rid)
	return err
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/content/ -run "TestIsPrivate|TestMakePrivate|TestMakePublic" -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/content/private.go go-libfossil/content/private_test.go
git commit -m "feat(content): add IsPrivate, MakePrivate, MakePublic helpers"
```

---

### Task 2: Auth — `CanSyncPrivate`

**Files:**
- Modify: `go-libfossil/auth/auth.go:24` (add after `CanPushUV`)
- Modify: `go-libfossil/auth/auth_test.go` (add test)

- [ ] **Step 1: Write failing test**

Append to `go-libfossil/auth/auth_test.go`:

```go
func TestCanSyncPrivate(t *testing.T) {
	if !CanSyncPrivate("x") { t.Error("CanSyncPrivate(x) should be true") }
	if CanSyncPrivate("oi") { t.Error("CanSyncPrivate(oi) should be false") }
	// 'x' is NOT implied by 'a' (admin) or 's' (setup)
	if CanSyncPrivate("as") { t.Error("CanSyncPrivate(as) should be false — x must be explicit") }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/auth/ -run TestCanSyncPrivate -v`
Expected: FAIL — `CanSyncPrivate` not defined

- [ ] **Step 3: Write implementation**

Add to `go-libfossil/auth/auth.go` after line 24:

```go
func CanSyncPrivate(caps string) bool { return HasCapability(caps, 'x') }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/auth/ -run TestCanSyncPrivate -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/auth/auth.go go-libfossil/auth/auth_test.go
git commit -m "feat(auth): add CanSyncPrivate for 'x' capability"
```

---

### Task 3: SyncOpts + session state

**Files:**
- Modify: `go-libfossil/sync/session.go:36` (add `Private` to SyncOpts)
- Modify: `go-libfossil/sync/session.go:52-79` (add `nextIsPrivate` to session)

- [ ] **Step 1: Add `Private` field to `SyncOpts`**

In `go-libfossil/sync/session.go`, after line 36 (`UV bool`), add:

```go
	Private                 bool              // enable private artifact sync
```

- [ ] **Step 2: Add `nextIsPrivate` to `session` struct**

In the `session` struct (after the `nGimmeRcvd` field at line 72), add:

```go
	nextIsPrivate       bool // true when a PrivateCard precedes the next file/cfile
```

- [ ] **Step 3: Verify compilation**

Run: `cd .worktrees/cdg-117 && go build -buildvcs=false ./go-libfossil/sync/`
Expected: builds clean

- [ ] **Step 4: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/session.go
git commit -m "feat(sync): add Private to SyncOpts, nextIsPrivate to session"
```

---

### Task 4: Server — `pragma send-private` + handler state

**Files:**
- Modify: `go-libfossil/sync/handler.go:78-98` (add `syncPrivate`/`nextIsPrivate` to handler struct)
- Modify: `go-libfossil/sync/handler.go:211-258` (handle pragma in `handleControlCard`)
- Modify: `go-libfossil/sync/handler_test.go` (add tests)

- [ ] **Step 1: Write failing tests**

Add to `go-libfossil/sync/handler_test.go`:

```go
func TestHandlerPragmaSendPrivate_Accepted(t *testing.T) {
	r := setupSyncTestRepo(t)
	// Grant 'x' capability to nobody
	r.DB().Exec("UPDATE user SET cap='oix' WHERE login='nobody'")

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
	)

	// Should NOT have an error card about private
	for _, c := range resp.Cards {
		if e, ok := c.(*xfer.ErrorCard); ok {
			if e.Message == "not authorized to sync private content" {
				t.Error("should not get auth error with 'x' capability")
			}
		}
	}
}

func TestHandlerPragmaSendPrivate_Rejected(t *testing.T) {
	r := setupSyncTestRepo(t)
	// nobody has default caps (no 'x')
	r.DB().Exec("UPDATE user SET cap='oi' WHERE login='nobody'")

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
	)

	errors := findCards[*xfer.ErrorCard](resp)
	found := false
	for _, e := range errors {
		if e.Message == "not authorized to sync private content" {
			found = true
		}
	}
	if !found {
		t.Error("expected 'not authorized to sync private content' error")
	}
}
```

Note: `handleReq` is defined in `handler_uv_test.go`. It calls `HandleSync` and returns the response. `findCards` is a generic helper in `handler_test.go`.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestHandlerPragmaSendPrivate" -v`
Expected: FAIL — pragma not handled yet (tests may pass vacuously if no error expected; adjust assertions)

- [ ] **Step 3: Add handler struct fields and pragma handling**

In `go-libfossil/sync/handler.go`, add to the `handler` struct (after `filesRecvd` at line 89):

```go
	syncPrivate   bool // true if pragma send-private was accepted
	nextIsPrivate bool // true if a private card precedes the next file/cfile
```

In `handleControlCard`, inside the `*xfer.PragmaCard` case (after the `req-clusters` handling around line 225), add:

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

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestHandlerPragmaSendPrivate" -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "feat(sync): handle pragma send-private with x capability check"
```

---

### Task 5: Server — `private` card + file/cfile MakePrivate/MakePublic

**Files:**
- Modify: `go-libfossil/sync/handler.go:260-286` (`handleDataCard` — add PrivateCard case)
- Modify: `go-libfossil/sync/handler.go:326-351` (`handleFile` — add MakePrivate/MakePublic)
- Modify: `go-libfossil/sync/handler_test.go` (add tests)

- [ ] **Step 1: Write failing tests**

Add to `go-libfossil/sync/handler_test.go`:

```go
func TestHandlerPrivateCardAccepted(t *testing.T) {
	r := setupSyncTestRepo(t)
	r.DB().Exec("UPDATE user SET cap='oix' WHERE login='nobody'")

	data := []byte("private content")
	uuid := hash.SHA1(data)

	resp := handleReq(t, r,
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
		&xfer.PrivateCard{},
		&xfer.FileCard{UUID: uuid, Content: data},
	)

	// No error cards
	for _, c := range resp.Cards {
		if e, ok := c.(*xfer.ErrorCard); ok {
			t.Errorf("unexpected error: %s", e.Message)
		}
	}

	// Verify blob is stored and marked private
	rid, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("blob should exist")
	}
	if !content.IsPrivate(r.DB(), int64(rid)) {
		t.Error("blob should be marked private")
	}
}

func TestHandlerPrivateCardRejected(t *testing.T) {
	r := setupSyncTestRepo(t)
	r.DB().Exec("UPDATE user SET cap='oi' WHERE login='nobody'") // no 'x'

	resp := handleReq(t, r,
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PrivateCard{},
		&xfer.FileCard{UUID: "0000000000000000000000000000000000000000", Content: []byte("x")},
	)

	errors := findCards[*xfer.ErrorCard](resp)
	found := false
	for _, e := range errors {
		if e.Message == "not authorized to sync private content" {
			found = true
		}
	}
	if !found {
		t.Error("expected 'not authorized to sync private content' error")
	}
}

func TestHandlerPublicFileClarsPrivate(t *testing.T) {
	r := setupSyncTestRepo(t)
	r.DB().Exec("UPDATE user SET cap='oix' WHERE login='nobody'")

	data := []byte("was private now public")
	uuid := hash.SHA1(data)

	// First: store as private
	handleReq(t, r,
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
		&xfer.PrivateCard{},
		&xfer.FileCard{UUID: uuid, Content: data},
	)

	rid, _ := blob.Exists(r.DB(), uuid)
	if !content.IsPrivate(r.DB(), int64(rid)) {
		t.Fatal("should be private after first store")
	}

	// Second: receive same blob as public (no private card prefix)
	handleReq(t, r,
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.FileCard{UUID: uuid, Content: data},
	)

	if content.IsPrivate(r.DB(), int64(rid)) {
		t.Error("blob should be public after receiving without private card")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestHandlerPrivateCard|TestHandlerPublicFile" -v`
Expected: FAIL

- [ ] **Step 3: Implement PrivateCard handling + MakePrivate/MakePublic in handleFile**

In `handleDataCard`, add a new case before the existing `case *xfer.ReqConfigCard:`:

```go
	case *xfer.PrivateCard:
		if !auth.CanSyncPrivate(h.caps) {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: "not authorized to sync private content",
			})
			h.nextIsPrivate = false
		} else {
			h.nextIsPrivate = true
		}
		return nil
```

In `handleFile`, after the `storeReceivedFile` call succeeds and before `h.filesRecvd++`, add:

```go
	// Mark private/public based on preceding private card.
	rid, _ := blob.Exists(h.repo.DB(), uuid)
	if h.nextIsPrivate {
		content.MakePrivate(h.repo.DB(), int64(rid))
		h.nextIsPrivate = false
	} else {
		content.MakePublic(h.repo.DB(), int64(rid))
	}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestHandlerPrivateCard|TestHandlerPublicFile" -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "feat(sync): handle private card + MakePrivate/MakePublic on file receipt"
```

---

### Task 6: Server — `emitIGots` private filtering

**Files:**
- Modify: `go-libfossil/sync/handler.go:374-404` (`emitIGots`)
- Modify: `go-libfossil/sync/handler_test.go` (add tests)

- [ ] **Step 1: Write failing tests**

Add to `go-libfossil/sync/handler_test.go`:

```go
func TestEmitIGotsExcludesPrivate(t *testing.T) {
	r := setupSyncTestRepo(t)

	// Store a public and a private blob
	pubData := []byte("public blob")
	privData := []byte("private blob")
	pubUUID := hash.SHA1(pubData)
	privUUID := hash.SHA1(privData)
	blob.Store(r.DB(), pubData)
	privRid, _, _ := blob.Store(r.DB(), privData)
	content.MakePrivate(r.DB(), int64(privRid))

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
	)

	igots := findCards[*xfer.IGotCard](resp)
	for _, ig := range igots {
		if ig.UUID == privUUID {
			t.Error("private blob should be excluded from igot in normal sync")
		}
	}
	foundPub := false
	for _, ig := range igots {
		if ig.UUID == pubUUID {
			foundPub = true
		}
	}
	if !foundPub {
		t.Error("public blob should be in igot")
	}
}

func TestEmitIGotsIncludesPrivateWhenAuthorized(t *testing.T) {
	r := setupSyncTestRepo(t)
	r.DB().Exec("UPDATE user SET cap='oix' WHERE login='nobody'")

	privData := []byte("private blob for sync")
	privUUID := hash.SHA1(privData)
	privRid, _, _ := blob.Store(r.DB(), privData)
	content.MakePrivate(r.DB(), int64(privRid))

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
	)

	igots := findCards[*xfer.IGotCard](resp)
	found := false
	for _, ig := range igots {
		if ig.UUID == privUUID {
			found = true
			if !ig.IsPrivate {
				t.Error("private igot should have IsPrivate=true")
			}
		}
	}
	if !found {
		t.Error("private blob should be included in igot when syncPrivate")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestEmitIGots" -v`
Expected: FAIL — private blob appears in igot without filtering

- [ ] **Step 3: Modify `emitIGots` to filter private blobs**

In `handler.go`, replace the `emitIGots` query with:

```go
func (h *handler) emitIGots() error {
	// Emit igot for all non-phantom, non-private blobs.
	rows, err := h.repo.DB().Query(
		`SELECT uuid FROM blob WHERE size >= 0
		 AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)`,
	)
	if err != nil {
		return fmt.Errorf("handler: listing blobs: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		uuids = append(uuids, uuid)
	}
	if err := rows.Err(); err != nil {
		return err
	}

	// BUGGIFY: 10% chance truncate igot list to test multi-round convergence.
	if h.buggify != nil && h.buggify.Check("handler.emitIGots.truncate", 0.10) && len(uuids) > 1 {
		uuids = uuids[:len(uuids)/2]
	}

	for _, uuid := range uuids {
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}

	// If syncPrivate, also emit igot UUID 1 for private blobs.
	if h.syncPrivate {
		if err := h.emitPrivateIGots(); err != nil {
			return err
		}
	}

	return nil
}

func (h *handler) emitPrivateIGots() error {
	rows, err := h.repo.DB().Query(
		"SELECT b.uuid FROM private p JOIN blob b ON p.rid=b.rid WHERE b.size >= 0",
	)
	if err != nil {
		return fmt.Errorf("handler: listing private blobs: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return err
		}
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid, IsPrivate: true})
	}
	return rows.Err()
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestEmitIGots" -v`
Expected: PASS

- [ ] **Step 5: Run full sync test suite to check for regressions**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1`
Expected: All existing tests PASS

- [ ] **Step 6: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "feat(sync): filter private blobs from emitIGots, add emitPrivateIGots"
```

---

### Task 7: Server — `handleIGot` private awareness

**Files:**
- Modify: `go-libfossil/sync/handler.go:288-300` (`handleIGot`)
- Modify: `go-libfossil/sync/handler_test.go`

- [ ] **Step 1: Write failing test**

Add to `go-libfossil/sync/handler_test.go`:

```go
func TestHandlerIGotPrivate_Authorized(t *testing.T) {
	r := setupSyncTestRepo(t)
	r.DB().Exec("UPDATE user SET cap='oix' WHERE login='nobody'")

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "send-private"},
		&xfer.IGotCard{UUID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", IsPrivate: true},
	)

	// Should gimme the private artifact when authorized
	gimmes := findCards[*xfer.GimmeCard](resp)
	if len(gimmes) != 1 || gimmes[0].UUID != "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" {
		t.Errorf("expected gimme for private igot, got %d gimmes", len(gimmes))
	}
}

func TestHandlerIGotPrivate_Unauthorized(t *testing.T) {
	r := setupSyncTestRepo(t)
	r.DB().Exec("UPDATE user SET cap='oi' WHERE login='nobody'") // no 'x'

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.IGotCard{UUID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", IsPrivate: true},
	)

	// Should NOT gimme the private artifact — just ignore it
	gimmes := findCards[*xfer.GimmeCard](resp)
	if len(gimmes) != 0 {
		t.Errorf("should not gimme private artifact without x capability, got %d", len(gimmes))
	}
}

func TestHandlerIGotPublic_ClearsPrivate(t *testing.T) {
	r := setupSyncTestRepo(t)

	// Pre-store a blob and mark it private
	data := []byte("was private")
	uuid := hash.SHA1(data)
	rid, _, _ := blob.Store(r.DB(), data)
	content.MakePrivate(r.DB(), int64(rid))

	handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.IGotCard{UUID: uuid, IsPrivate: false},
	)

	if content.IsPrivate(r.DB(), int64(rid)) {
		t.Error("public igot should clear private status")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestHandlerIGotPrivate|TestHandlerIGotPublic" -v`
Expected: FAIL

- [ ] **Step 3: Modify `handleIGot` for private awareness**

Replace the `handleIGot` method:

```go
func (h *handler) handleIGot(c *xfer.IGotCard) error {
	if c == nil {
		panic("handler.handleIGot: c must not be nil")
	}
	if !h.pullOK {
		return nil
	}
	rid, exists := blob.Exists(h.repo.DB(), c.UUID)
	if exists {
		// Update private/public status based on igot flag.
		if c.IsPrivate {
			content.MakePrivate(h.repo.DB(), int64(rid))
		} else {
			content.MakePublic(h.repo.DB(), int64(rid))
		}
		return nil
	}
	// Blob doesn't exist locally.
	if c.IsPrivate && !h.syncPrivate {
		// Not authorized for private sync — don't request it.
		return nil
	}
	h.resp = append(h.resp, &xfer.GimmeCard{UUID: c.UUID})
	return nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestHandlerIGotPrivate|TestHandlerIGotPublic" -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "feat(sync): private-aware handleIGot with MakePrivate/MakePublic"
```

---

### Task 8: Server — `handleGimme` private card prefix + `emitCloneBatch` filtering

**Files:**
- Modify: `go-libfossil/sync/handler.go:302-324` (`handleGimme`)
- Modify: `go-libfossil/sync/handler.go:435+` (`emitCloneBatch`)

- [ ] **Step 1: Modify `handleGimme` to prepend private card**

In `handleGimme`, after the file card is appended (line ~321), add a private check:

```go
func (h *handler) handleGimme(c *xfer.GimmeCard) error {
	if c == nil {
		panic("handler.handleGimme: c must not be nil")
	}
	if h.buggify != nil && h.buggify.Check("handler.handleGimme.skip", 0.05) {
		return nil
	}
	rid, ok := blob.Exists(h.repo.DB(), c.UUID)
	if !ok {
		return nil
	}
	isPriv := content.IsPrivate(h.repo.DB(), int64(rid))
	if isPriv && !h.syncPrivate {
		return nil // don't send private content without authorization
	}
	data, err := content.Expand(h.repo.DB(), rid)
	if err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("expand %s: %v", c.UUID, err),
		})
		return nil
	}
	if isPriv {
		h.resp = append(h.resp, &xfer.PrivateCard{})
	}
	h.resp = append(h.resp, &xfer.FileCard{UUID: c.UUID, Content: data})
	h.filesSent++
	return nil
}
```

- [ ] **Step 2: Modify `emitCloneBatch` to filter/prefix private blobs**

In `emitCloneBatch`, after scanning each `(rid, uuid)` row and before appending the file card, add:

```go
		isPriv := content.IsPrivate(h.repo.DB(), int64(rid))
		if isPriv && !h.syncPrivate {
			continue // skip private blobs in clone when not authorized
		}
		// ... existing expand + file card code ...
		if isPriv {
			h.resp = append(h.resp, &xfer.PrivateCard{})
		}
```

Note: Be careful with the count/lastRID tracking — skipped private blobs should still advance the cursor but not count toward the batch limit. Read the existing `emitCloneBatch` code carefully and adjust the skip logic to maintain correct pagination.

- [ ] **Step 3: Run full handler test suite**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/handler.go
git commit -m "feat(sync): private card prefix in handleGimme + emitCloneBatch filtering"
```

---

### Task 9: Server — `sendAllClusters` private filtering

**Files:**
- Modify: `go-libfossil/sync/handler.go:409-433` (`sendAllClusters`)

- [ ] **Step 1: Add private exclusion to `sendAllClusters` query**

Add `AND NOT EXISTS (SELECT 1 FROM private WHERE rid = b.rid)` to the query in `sendAllClusters`:

```sql
SELECT b.uuid FROM tagxref tx
JOIN blob b ON tx.rid = b.rid
WHERE tx.tagid = 7
  AND NOT EXISTS (SELECT 1 FROM unclustered WHERE rid = b.rid)
  AND NOT EXISTS (SELECT 1 FROM phantom WHERE rid = b.rid)
  AND NOT EXISTS (SELECT 1 FROM private WHERE rid = b.rid)
  AND b.size >= 0
```

- [ ] **Step 2: Run tests**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1`
Expected: All PASS

- [ ] **Step 3: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/handler.go
git commit -m "feat(sync): exclude private blobs from sendAllClusters"
```

---

### Task 10: Client — `pragma send-private` + `sendUnclustered` filtering

**Files:**
- Modify: `go-libfossil/sync/client.go` (buildRequest, sendUnclustered, new sendPrivate)

- [ ] **Step 1: Add `pragma send-private` to `buildRequest`**

In `buildRequest`, after the UV pragma block (around line 78 `// UV: pragma uv-hash on first round`), add:

```go
	// Private: send pragma on first round
	if cycle == 0 && s.opts.Private {
		cards = append(cards, &xfer.PragmaCard{Name: "send-private"})
	}
```

- [ ] **Step 2: Add private exclusion to `sendUnclustered`**

Modify the query in `sendUnclustered` (around line 157) to exclude private blobs:

```sql
SELECT uuid FROM unclustered JOIN blob USING(rid)
WHERE size >= 0
  AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)
```

- [ ] **Step 3: Add `sendPrivate` function**

After `sendUnclustered`, add:

```go
// sendPrivate emits igot UUID 1 for all private blobs.
// Called when opts.Private is true.
func (s *session) sendPrivate() ([]xfer.Card, error) {
	rows, err := s.repo.DB().Query(
		"SELECT b.uuid FROM private p JOIN blob b ON p.rid=b.rid WHERE b.size >= 0",
	)
	if err != nil {
		return nil, fmt.Errorf("sendPrivate: %w", err)
	}
	defer rows.Close()

	var cards []xfer.Card
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		if s.remoteHas[uuid] {
			continue
		}
		cards = append(cards, &xfer.IGotCard{UUID: uuid, IsPrivate: true})
	}
	return cards, rows.Err()
}
```

- [ ] **Step 4: Call `sendPrivate` in `buildRequest`**

After the `sendUnclustered` call in `buildRequest` (around line 67), add:

```go
	// Private: emit igot UUID 1 for private blobs
	if s.opts.Private {
		privCards, err := s.sendPrivate()
		if err != nil {
			return nil, fmt.Errorf("buildRequest sendPrivate: %w", err)
		}
		s.igotSentThisRound += len(privCards)
		s.roundStats.IgotsSent += len(privCards)
		cards = append(cards, privCards...)
	}
```

- [ ] **Step 5: Run tests**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/client.go
git commit -m "feat(sync): client pragma send-private, sendUnclustered filtering, sendPrivate"
```

---

### Task 11: Client — `sendAllClusters`, `buildFileCards`, `buildGimmeCards` filtering

**Files:**
- Modify: `go-libfossil/sync/client.go`

- [ ] **Step 1: Add private exclusion to `sendAllClusters`**

Add `AND NOT EXISTS(SELECT 1 FROM private WHERE rid=blob.rid)` to the client's `sendAllClusters` query.

- [ ] **Step 2: Add private exclusion to `buildFileCards` unsent query**

In `buildFileCards`, if there is an unsent table query, modify it to:

```sql
SELECT rid FROM unsent EXCEPT SELECT rid FROM private
```

Also, when loading a file card for a blob in `pendingSend`, check `content.IsPrivate` and prepend `PrivateCard` if private. Only do this when `s.opts.Private` — if `!s.opts.Private`, skip private blobs entirely.

- [ ] **Step 3: Add private exclusion to `buildGimmeCards`**

When `!s.opts.Private`, filter private phantoms from the gimme list. Check `content.IsPrivate` for each phantom's rid before emitting a gimme card. Since phantoms may not have a rid in the private table, this check is best-effort — the server-side filtering is the primary gate.

- [ ] **Step 4: Run tests**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/client.go
git commit -m "feat(sync): client-side private filtering in sendAllClusters, buildFileCards, buildGimmeCards"
```

---

### Task 12: Client — `processResponse` private handling

**Files:**
- Modify: `go-libfossil/sync/client.go:360-530` (`processResponse`)

- [ ] **Step 1: Handle `PrivateCard` in processResponse**

In the `switch c := card.(type)` block in `processResponse`, add:

```go
		case *xfer.PrivateCard:
			s.nextIsPrivate = true
```

- [ ] **Step 2: Add MakePrivate/MakePublic after FileCard/CFileCard storage**

After the `handleFileCard` call for `FileCard` and `CFileCard`, add:

```go
			// Mark private/public based on preceding private card.
			if s.nextIsPrivate {
				rid, _ := blob.Exists(s.repo.DB(), c.UUID)
				content.MakePrivate(s.repo.DB(), int64(rid))
				s.nextIsPrivate = false
			} else {
				rid, exists := blob.Exists(s.repo.DB(), c.UUID)
				if exists {
					content.MakePublic(s.repo.DB(), int64(rid))
				}
			}
```

- [ ] **Step 3: Handle `IGotCard.IsPrivate` in processResponse**

Replace the existing `IGotCard` case entirely. The current code just tracks `remoteHas` and creates phantoms. The new code adds private awareness:

```go
		case *xfer.IGotCard:
			s.remoteHas[c.UUID] = true
			rid, exists := blob.Exists(s.repo.DB(), c.UUID)
			if c.IsPrivate {
				if exists {
					// Already have it — update private status
					content.MakePrivate(s.repo.DB(), int64(rid))
				} else if s.opts.Private && s.opts.Pull {
					// Authorized for private sync — create phantom to request it
					s.phantoms[c.UUID] = true
				}
				// else: not authorized for private sync — don't create phantom
			} else {
				// Public artifact
				if exists {
					content.MakePublic(s.repo.DB(), int64(rid))
				} else if s.opts.Pull {
					s.phantoms[c.UUID] = true
				}
			}
```

Note: The existing code's `remoteHas` tracking is preserved at the top. The existing phantom creation logic (`if s.opts.Pull && !exists`) is preserved in the public branch. Read the current implementation to verify no other logic (e.g., unsent table removal) needs preserving.

- [ ] **Step 4: Run tests**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/client.go
git commit -m "feat(sync): client processResponse private card + igot handling"
```

---

### Task 13: Integration test — private sync end-to-end

**Files:**
- Modify: `go-libfossil/sync/handler_test.go` or create `go-libfossil/sync/private_integration_test.go`

- [ ] **Step 1: Write integration test**

Test that a full sync session with `Private: true` transfers private blobs, and `Private: false` excludes them:

```go
func TestSyncPrivateEndToEnd(t *testing.T) {
	// Create server repo with mix of public and private blobs.
	// Create client repo.
	// Sync with Private: false — only public blobs arrive.
	// Sync with Private: true — all blobs arrive, private ones marked in private table.
}
```

This test uses `MockTransport` with `HandleSyncWithOpts` to run a full sync loop without HTTP.

- [ ] **Step 2: Write test for private→public transition**

```go
func TestSyncPrivateArtifactTransition(t *testing.T) {
	// Server has a blob marked private.
	// Client syncs with Private: true — gets it, marked private.
	// Server removes from private table (MakePublic).
	// Client syncs again — blob's private status is cleared.
}
```

- [ ] **Step 3: Run integration tests**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./go-libfossil/sync/ -run "TestSyncPrivate" -v`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
cd .worktrees/cdg-117
git add go-libfossil/sync/
git commit -m "test(sync): private sync integration tests"
```

---

### Task 14: DST scenario — private sync

**Files:**
- Modify: `dst/scenario_test.go`

- [ ] **Step 1: Write DST scenario**

Add `TestScenarioPrivateSync`:

- Master repo with 50 public + 10 private blobs.
- Leaf A: `SyncOpts{Private: true}`, nobody has `x` capability → gets all 60 blobs.
- Leaf B: `SyncOpts{Private: false}`, nobody without `x` → gets only 50 public blobs.
- Run sim, check convergence, verify blob counts per leaf.

Note: The DST uses `MockFossil` and the simulator. Study existing scenarios like `TestScenarioCleanSync` for the pattern. You'll need to seed blobs on the master and mark some as private via `content.MakePrivate`.

- [ ] **Step 2: Run DST**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./dst/ -run TestScenarioPrivateSync -v`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
cd .worktrees/cdg-117
git add dst/scenario_test.go
git commit -m "test(dst): add TestScenarioPrivateSync"
```

---

### Task 15: Sim equivalence test — against real `fossil serve`

**Files:**
- Modify: `sim/equivalence_test.go`

- [ ] **Step 1: Write equivalence test**

Add `TestPrivateSyncAgainstFossilServe`:

1. `fossil new` a repo.
2. `fossil user capabilities nobody oix -R <repo>` — grant `x`.
3. Create a commit (so the repo has content).
4. `fossil checkout`, create a file on a private branch: `fossil branch new private-branch trunk --private -R <repo>`.
5. `fossil serve` the repo.
6. Clone/sync with go-libfossil `Private: true` — verify private artifacts arrive.

Note: Creating a private branch via `fossil branch new --private` is the Fossil way. Study `fossilInit`/`fossilCommitFiles`/`startFossilServe` helpers in `sim/equivalence_test.go`.

- [ ] **Step 2: Run test**

Run: `cd .worktrees/cdg-117 && go test -buildvcs=false ./sim/ -run TestPrivateSyncAgainstFossilServe -v`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
cd .worktrees/cdg-117
git add sim/equivalence_test.go
git commit -m "test(sim): add TestPrivateSyncAgainstFossilServe"
```

---

### Task 16: Full test suite + pre-commit

- [ ] **Step 1: Run full test suite**

Run: `cd .worktrees/cdg-117 && make test`
Expected: All PASS

- [ ] **Step 2: Run DST full**

Run: `cd .worktrees/cdg-117 && make dst`
Expected: All PASS

- [ ] **Step 3: Push branch**

```bash
cd .worktrees/cdg-117
git push -u origin feature/cdg-117-handle-private-branch-cards
```
