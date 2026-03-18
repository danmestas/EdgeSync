# ServerHandler Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking. Use @superpowers:tigerstyle for all go-libfossil library code.

**Goal:** Add a server-side sync handler to go-libfossil that lets a leaf agent accept clone and sync requests over swappable transports.

**Architecture:** A stateless `HandleSync` function processes incoming xfer messages card-by-card and produces responses. Shared blob I/O helpers are extracted from existing client code. Transport listeners (HTTP in sync package, NATS in leaf/agent) call the handler. The leaf agent runs both client (pollLoop) and server (listeners) concurrently.

**Tech Stack:** Go, SQLite (via go-libfossil/db), xfer card protocol, net/http, nats.go

---

## File Structure

### New files
| File | Responsibility |
|------|---------------|
| `go-libfossil/sync/blob_io.go` | Shared blob store/load/list helpers (extracted from client.go) |
| `go-libfossil/sync/blob_io_test.go` | Tests for shared helpers |
| `go-libfossil/sync/handler.go` | `HandleFunc` type, `HandleSync()` implementation |
| `go-libfossil/sync/handler_test.go` | Unit tests for all card handling |
| `go-libfossil/sync/serve_http.go` | `ServeHTTP()` — HTTP /xfer listener |
| `go-libfossil/sync/serve_http_test.go` | Integration test with real `fossil clone`/`fossil sync` |
| `leaf/agent/serve_nats.go` | `ServeNATS()` — NATS request/reply listener |
| `leaf/agent/serve_nats_test.go` | Test for NATS listener |
| `leaf/agent/serve_p2p.go` | `ServeP2P()` — stub |

### Modified files
| File | Change |
|------|--------|
| `go-libfossil/sync/client.go` | Replace inline blob I/O with calls to blob_io.go helpers |
| `leaf/agent/config.go` | Add `ServeHTTP`, `ServeNATS` fields |
| `leaf/agent/agent.go` | Start listeners in `Start()`, stop in `Stop()` |

---

## Chunk 1: Extract Shared Blob I/O

### Task 1: Extract storeBlob helper

**Files:**
- Create: `go-libfossil/sync/blob_io.go`
- Create: `go-libfossil/sync/blob_io_test.go`
- Modify: `go-libfossil/sync/client.go`

- [ ] **Step 1: Write failing test for storeBlob**

```go
// blob_io_test.go
package sync

import (
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

func TestStoreBlobFull(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("hello world")
	uuid := hash.SHA1(data)

	if err := storeBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("storeBlob: %v", err)
	}

	rid, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("blob not found after store")
	}
	if rid <= 0 {
		t.Fatalf("expected positive rid, got %d", rid)
	}
}

func TestStoreBlobBadHash(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("hello world")
	badUUID := "0000000000000000000000000000000000000000"

	err := storeBlob(r.DB(), badUUID, "", data)
	if err == nil {
		t.Fatal("expected error for bad hash")
	}
}

func TestStoreBlobDuplicate(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("hello world")
	uuid := hash.SHA1(data)

	if err := storeBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("first store: %v", err)
	}
	if err := storeBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("duplicate store should succeed: %v", err)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test ./sync/ -run TestStoreBlob -count=1 -v`
Expected: FAIL — `storeBlob` undefined

- [ ] **Step 3: Create blob_io.go with storeBlob**

Extract the body of `session.handleFileCard` from `client.go:296-374` into a package-level function. Remove the buggify check (that stays in the session wrapper). Add TigerStyle preconditions.

```go
// blob_io.go
package sync

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// storeBlob validates, compresses, and stores a received blob.
// Handles both full content and delta-compressed payloads.
// If the blob already exists, it ensures it is in the unclustered table.
func storeBlob(querier db.Querier, uuid, deltaSrc string, payload []byte) error {
	if uuid == "" {
		panic("sync.storeBlob: uuid must not be empty")
	}
	if payload == nil {
		panic("sync.storeBlob: payload must not be nil")
	}
	if !hash.IsValidHash(uuid) {
		return fmt.Errorf("sync: invalid UUID format: %s", uuid)
	}

	fullContent, err := resolvePayload(querier, deltaSrc, payload)
	if err != nil {
		return fmt.Errorf("sync.storeBlob: %w", err)
	}

	if err := verifyHash(uuid, fullContent); err != nil {
		return err
	}

	return insertBlob(querier, uuid, fullContent)
}

// resolvePayload expands a delta payload against its source, or returns
// the payload as-is for non-delta content.
func resolvePayload(querier db.Querier, deltaSrc string, payload []byte) ([]byte, error) {
	if deltaSrc == "" {
		return payload, nil
	}
	srcRid, ok := blob.Exists(querier, deltaSrc)
	if !ok {
		return nil, fmt.Errorf("delta source %s not found", deltaSrc)
	}
	baseContent, err := content.Expand(querier, srcRid)
	if err != nil {
		return nil, fmt.Errorf("expanding delta source %s: %w", deltaSrc, err)
	}
	applied, err := delta.Apply(baseContent, payload)
	if err != nil {
		return nil, fmt.Errorf("applying delta for %s: %w", deltaSrc, err)
	}
	return applied, nil
}

// verifyHash checks that the content hashes to the expected UUID.
func verifyHash(uuid string, content []byte) error {
	var computed string
	if len(uuid) > 40 {
		computed = hash.SHA3(content)
	} else {
		computed = hash.SHA1(content)
	}
	if computed != uuid {
		return fmt.Errorf("UUID mismatch: expected %s, got %s", uuid, computed)
	}
	return nil
}

// insertBlob compresses and stores content, adding to unclustered.
// If the blob already exists, it just ensures unclustered entry.
func insertBlob(querier db.Querier, uuid string, fullContent []byte) error {
	if rid, ok := blob.Exists(querier, uuid); ok {
		_, err := querier.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
		return err
	}
	compressed, err := blob.Compress(fullContent)
	if err != nil {
		return err
	}
	result, err := querier.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		uuid, len(fullContent), compressed,
	)
	if err != nil {
		return err
	}
	rid, err := result.LastInsertId()
	if err != nil {
		return err
	}
	_, err = querier.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
	return err
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test ./sync/ -run TestStoreBlob -count=1 -v`
Expected: PASS (3 tests)

- [ ] **Step 5: Refactor client.go to use storeBlob**

Replace the body of `session.handleFileCard` (`client.go:296-374`) with a call to `storeBlob` plus the buggify wrapper:

```go
func (s *session) handleFileCard(uuid, deltaSrc string, payload []byte) error {
	if err := storeBlob(s.repo.DB(), uuid, deltaSrc, payload); err != nil {
		return err
	}
	if s.opts.Buggify != nil && s.opts.Buggify.Check("sync.handleFileCard.reject", 0.03) {
		return fmt.Errorf("buggify: simulated storage failure for %s", uuid)
	}
	return nil
}
```

- [ ] **Step 6: Run full sync tests to verify no regression**

Run: `cd go-libfossil && go test ./sync/ -count=1 -v`
Expected: All existing tests PASS

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/blob_io.go go-libfossil/sync/blob_io_test.go go-libfossil/sync/client.go
git commit -m "sync: extract storeBlob into shared blob_io helper"
```

### Task 2: Extract loadBlob helper

**Files:**
- Modify: `go-libfossil/sync/blob_io.go`
- Modify: `go-libfossil/sync/blob_io_test.go`
- Modify: `go-libfossil/sync/client.go`

- [ ] **Step 1: Write failing test for loadBlob**

```go
// blob_io_test.go (append)
func TestLoadBlob(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("load me")
	uuid := hash.SHA1(data)

	if err := storeBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("store: %v", err)
	}

	card, size, err := loadBlob(r.DB(), uuid)
	if err != nil {
		t.Fatalf("loadBlob: %v", err)
	}
	if card.UUID != uuid {
		t.Fatalf("UUID mismatch: got %s", card.UUID)
	}
	if size != len(data) {
		t.Fatalf("size: got %d, want %d", size, len(data))
	}
	if string(card.Content) != string(data) {
		t.Fatalf("content mismatch")
	}
}

func TestLoadBlobNotFound(t *testing.T) {
	r := testutil.TempRepo(t)
	_, _, err := loadBlob(r.DB(), "0000000000000000000000000000000000000000")
	if err == nil {
		t.Fatal("expected error for missing blob")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test ./sync/ -run TestLoadBlob -count=1 -v`
Expected: FAIL — `loadBlob` undefined

- [ ] **Step 3: Add loadBlob to blob_io.go**

```go
// loadBlob loads a blob by UUID and returns it as a FileCard with its size.
// Expands delta chains via content.Expand.
func loadBlob(querier db.Querier, uuid string) (*xfer.FileCard, int, error) {
	if uuid == "" {
		panic("sync.loadBlob: uuid must not be empty")
	}
	rid, ok := blob.Exists(querier, uuid)
	if !ok {
		return nil, 0, fmt.Errorf("blob %s not found", uuid)
	}
	data, err := content.Expand(querier, rid)
	if err != nil {
		return nil, 0, fmt.Errorf("expanding blob %s: %w", uuid, err)
	}
	return &xfer.FileCard{UUID: uuid, Content: data}, len(data), nil
}
```

- [ ] **Step 4: Refactor client.go loadFileCard to use loadBlob**

```go
func (s *session) loadFileCard(uuid string) (*xfer.FileCard, int, error) {
	return loadBlob(s.repo.DB(), uuid)
}
```

- [ ] **Step 5: Run all sync tests**

Run: `cd go-libfossil && go test ./sync/ -count=1 -v`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/sync/blob_io.go go-libfossil/sync/blob_io_test.go go-libfossil/sync/client.go
git commit -m "sync: extract loadBlob into shared blob_io helper"
```

### Task 3: Add listBlobUUIDs and listBlobsFromRID helpers

**Files:**
- Modify: `go-libfossil/sync/blob_io.go`
- Modify: `go-libfossil/sync/blob_io_test.go`

- [ ] **Step 1: Write failing tests**

```go
func TestListBlobUUIDs(t *testing.T) {
	r := testutil.TempRepo(t)
	data1 := []byte("blob one")
	data2 := []byte("blob two")
	uuid1 := hash.SHA1(data1)
	uuid2 := hash.SHA1(data2)

	storeBlob(r.DB(), uuid1, "", data1)
	storeBlob(r.DB(), uuid2, "", data2)

	uuids, err := listBlobUUIDs(r.DB())
	if err != nil {
		t.Fatalf("listBlobUUIDs: %v", err)
	}
	// TempRepo creates initial blobs, so we check our two are present
	found := map[string]bool{}
	for _, u := range uuids {
		found[u] = true
	}
	if !found[uuid1] || !found[uuid2] {
		t.Fatalf("missing blobs: found=%v", found)
	}
}

func TestListBlobsFromRID(t *testing.T) {
	r := testutil.TempRepo(t)
	// Store 5 blobs
	for i := 0; i < 5; i++ {
		data := []byte(fmt.Sprintf("clone blob %d", i))
		uuid := hash.SHA1(data)
		storeBlob(r.DB(), uuid, "", data)
	}

	// Page 1: limit 3
	cards, lastRID, more, err := listBlobsFromRID(r.DB(), 0, 3)
	if err != nil {
		t.Fatalf("page 1: %v", err)
	}
	if len(cards) != 3 {
		t.Fatalf("page 1: got %d cards, want 3", len(cards))
	}
	if !more {
		t.Fatal("page 1: expected more=true")
	}
	if lastRID <= 0 {
		t.Fatalf("page 1: bad lastRID %d", lastRID)
	}

	// Page 2: from lastRID
	cards2, _, more2, err := listBlobsFromRID(r.DB(), lastRID, 100)
	if err != nil {
		t.Fatalf("page 2: %v", err)
	}
	if len(cards2) < 2 {
		t.Fatalf("page 2: got %d cards, want >= 2", len(cards2))
	}
	if more2 {
		t.Fatal("page 2: expected more=false (all blobs fetched)")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test ./sync/ -run "TestListBlob" -count=1 -v`
Expected: FAIL — undefined functions

- [ ] **Step 3: Implement listBlobUUIDs and listBlobsFromRID**

```go
// DefaultCloneBatchSize is the number of blobs sent per clone round.
const DefaultCloneBatchSize = 200

// listBlobUUIDs returns all non-phantom blob UUIDs in the repo.
func listBlobUUIDs(querier db.Querier) ([]string, error) {
	rows, err := querier.Query("SELECT uuid FROM blob WHERE size >= 0")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		uuids = append(uuids, uuid)
	}
	return uuids, rows.Err()
}

// listBlobsFromRID returns a paginated batch of blobs for clone.
// Returns file cards, the last rid in the batch, and whether more blobs remain.
func listBlobsFromRID(querier db.Querier, afterRID, limit int) ([]xfer.FileCard, int, bool, error) {
	if limit <= 0 {
		panic("sync.listBlobsFromRID: limit must be positive")
	}
	// Fetch limit+1 to detect whether more remain.
	rows, err := querier.Query(
		"SELECT rid, uuid FROM blob WHERE rid > ? AND size >= 0 ORDER BY rid LIMIT ?",
		afterRID, limit+1,
	)
	if err != nil {
		return nil, 0, false, err
	}
	defer rows.Close()

	var cards []xfer.FileCard
	var lastRID int
	for rows.Next() {
		var rid int
		var uuid string
		if err := rows.Scan(&rid, &uuid); err != nil {
			return nil, 0, false, err
		}
		if len(cards) >= limit {
			// We got limit+1 rows — more exist.
			return cards, lastRID, true, nil
		}
		data, err := content.Expand(querier, libfossil.FslID(rid))
		if err != nil {
			return nil, 0, false, fmt.Errorf("expanding rid %d (%s): %w", rid, uuid, err)
		}
		cards = append(cards, xfer.FileCard{UUID: uuid, Content: data})
		lastRID = rid
	}
	if err := rows.Err(); err != nil {
		return nil, 0, false, err
	}
	return cards, lastRID, false, nil
}
```

Note: add `libfossil "github.com/dmestas/edgesync/go-libfossil"` to imports for `FslID`.

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test ./sync/ -run "TestListBlob" -count=1 -v`
Expected: PASS

- [ ] **Step 5: Run full test suite**

Run: `cd go-libfossil && go test ./sync/ -count=1`
Expected: All PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/sync/blob_io.go go-libfossil/sync/blob_io_test.go
git commit -m "sync: add listBlobUUIDs and listBlobsFromRID for handler/clone"
```

---

## Chunk 2: HandleSync Implementation

### Task 4: HandleSync — pull, push, igot, gimme, file cards

**Files:**
- Create: `go-libfossil/sync/handler.go`
- Create: `go-libfossil/sync/handler_test.go`

- [ ] **Step 1: Write failing tests for pull and igot/gimme**

```go
// handler_test.go
package sync

import (
	"context"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// helper: find all cards of a given type
func findCards[T xfer.Card](msg *xfer.Message) []T {
	var out []T
	for _, c := range msg.Cards {
		if tc, ok := c.(T); ok {
			out = append(out, tc)
		}
	}
	return out
}

func TestHandlePull(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("pull me")
	uuid := hash.SHA1(data)
	storeBlob(r.DB(), uuid, "", data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	igots := findCards[*xfer.IGotCard](resp)
	found := false
	for _, ig := range igots {
		if ig.UUID == uuid {
			found = true
		}
	}
	if !found {
		t.Fatalf("pull response missing igot for %s", uuid)
	}
}

func TestHandleIGotGimme(t *testing.T) {
	r := testutil.TempRepo(t)
	unknownUUID := "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.IGotCard{UUID: unknownUUID},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	gimmes := findCards[*xfer.GimmeCard](resp)
	found := false
	for _, g := range gimmes {
		if g.UUID == unknownUUID {
			found = true
		}
	}
	if !found {
		t.Fatal("expected gimme for unknown UUID")
	}
}

func TestHandleGimme(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("gimme this")
	uuid := hash.SHA1(data)
	storeBlob(r.DB(), uuid, "", data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.GimmeCard{UUID: uuid},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	found := false
	for _, f := range files {
		if f.UUID == uuid && string(f.Content) == string(data) {
			found = true
		}
	}
	if !found {
		t.Fatal("expected file card with correct content")
	}
}

func TestHandlePushFile(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("push this")
	uuid := hash.SHA1(data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PushCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	// Should not have error cards
	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) > 0 {
		t.Fatalf("unexpected error: %s", errs[0].Message)
	}

	// Blob should be stored
	_, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("pushed blob not stored")
	}
}

func TestHandleFileWithoutPush(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("no push card")
	uuid := hash.SHA1(data)

	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.FileCard{UUID: uuid, Content: data},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	errs := findCards[*xfer.ErrorCard](resp)
	if len(errs) == 0 {
		t.Fatal("expected error for file without push")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test ./sync/ -run "TestHandle" -count=1 -v`
Expected: FAIL — `HandleSync` undefined

- [ ] **Step 3: Implement handler.go**

```go
// handler.go
package sync

import (
	"context"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// HandleFunc is the server-side sync handler signature.
// Transport listeners call this with decoded requests and write back the response.
type HandleFunc func(ctx context.Context, r *repo.Repo, req *xfer.Message) (*xfer.Message, error)

// HandleSync processes an incoming xfer request and produces a response.
// Stateless per-round — the client drives convergence.
func HandleSync(ctx context.Context, r *repo.Repo, req *xfer.Message) (*xfer.Message, error) {
	if r == nil {
		panic("sync.HandleSync: r must not be nil")
	}
	if req == nil {
		panic("sync.HandleSync: req must not be nil")
	}

	h := &handler{repo: r}
	return h.process(ctx, req)
}

// handler holds per-request state while processing cards.
type handler struct {
	repo      *repo.Repo
	resp      []xfer.Card
	pushOK    bool // client sent a valid push card
	pullOK    bool // client sent a valid pull card
	cloneMode bool // client sent a clone card
	cloneSeq  int  // clone_seqno cursor from client
}

func (h *handler) process(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	// First pass: extract control cards (login, pragma, push, pull, clone, clone_seqno).
	for _, card := range req.Cards {
		switch c := card.(type) {
		case *xfer.LoginCard:
			h.handleLogin(c)
		case *xfer.PragmaCard:
			h.handlePragma(c)
		case *xfer.PushCard:
			h.pushOK = true
		case *xfer.PullCard:
			h.pullOK = true
		case *xfer.CloneCard:
			h.cloneMode = true
		case *xfer.CloneSeqNoCard:
			h.cloneSeq = c.SeqNo
		default:
			// Data cards handled in second pass.
		}
	}

	// Second pass: handle data cards (igot, gimme, file, reqconfig).
	for _, card := range req.Cards {
		if err := h.handleDataCard(card); err != nil {
			return nil, err
		}
	}

	// If pull was requested, emit igot for all our blobs.
	if h.pullOK {
		if err := h.emitIGots(); err != nil {
			return nil, err
		}
	}

	// If clone, emit paginated file cards.
	if h.cloneMode {
		if err := h.emitCloneBatch(); err != nil {
			return nil, err
		}
	}

	return &xfer.Message{Cards: h.resp}, nil
}

func (h *handler) handleLogin(c *xfer.LoginCard) {
	// Basic: accept all logins. Future: verify credentials.
	_ = c
}

func (h *handler) handlePragma(c *xfer.PragmaCard) {
	// Acknowledge client-version, ignore unknown pragmas.
	_ = c
}

func (h *handler) handleDataCard(card xfer.Card) error {
	switch c := card.(type) {
	case *xfer.IGotCard:
		return h.handleIGot(c)
	case *xfer.GimmeCard:
		return h.handleGimme(c)
	case *xfer.FileCard:
		return h.handleFile(c.UUID, c.DeltaSrc, c.Content)
	case *xfer.CFileCard:
		return h.handleFile(c.UUID, c.DeltaSrc, c.Content)
	case *xfer.ReqConfigCard:
		return h.handleReqConfig(c)
	}
	return nil
}

func (h *handler) handleIGot(c *xfer.IGotCard) error {
	if !h.pullOK {
		return nil // ignore igot without pull
	}
	_, exists := blob.Exists(h.repo.DB(), c.UUID)
	if !exists {
		h.resp = append(h.resp, &xfer.GimmeCard{UUID: c.UUID})
	}
	return nil
}

func (h *handler) handleGimme(c *xfer.GimmeCard) error {
	card, _, err := loadBlob(h.repo.DB(), c.UUID)
	if err != nil {
		// Blob not found — not fatal, just skip.
		return nil
	}
	h.resp = append(h.resp, card)
	return nil
}

func (h *handler) handleFile(uuid, deltaSrc string, content []byte) error {
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("file %s rejected: no push card", uuid),
		})
		return nil
	}
	if err := storeBlob(h.repo.DB(), uuid, deltaSrc, content); err != nil {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("storing %s: %v", uuid, err),
		})
	}
	return nil
}

func (h *handler) handleReqConfig(c *xfer.ReqConfigCard) error {
	val, err := h.readConfig(c.Name)
	if err != nil {
		return nil // config not found — not fatal
	}
	h.resp = append(h.resp, &xfer.ConfigCard{
		Name:    c.Name,
		Content: []byte(val),
	})
	return nil
}

func (h *handler) readConfig(name string) (string, error) {
	var val string
	err := h.repo.DB().QueryRow(
		"SELECT value FROM config WHERE name = ?", name,
	).Scan(&val)
	return val, err
}

func (h *handler) emitIGots() error {
	uuids, err := listBlobUUIDs(h.repo.DB())
	if err != nil {
		return fmt.Errorf("handler: listing blobs: %w", err)
	}
	for _, uuid := range uuids {
		h.resp = append(h.resp, &xfer.IGotCard{UUID: uuid})
	}
	return nil
}

func (h *handler) emitCloneBatch() error {
	cards, lastRID, more, err := listBlobsFromRID(
		h.repo.DB(), h.cloneSeq, DefaultCloneBatchSize,
	)
	if err != nil {
		return fmt.Errorf("handler: clone batch: %w", err)
	}
	for i := range cards {
		h.resp = append(h.resp, &cards[i])
	}
	if more {
		h.resp = append(h.resp, &xfer.CloneSeqNoCard{SeqNo: lastRID})
	}
	return nil
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test ./sync/ -run "TestHandle" -count=1 -v`
Expected: All PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_test.go
git commit -m "sync: implement HandleSync with pull, push, igot, gimme, file support"
```

### Task 5: HandleSync — clone and reqconfig cards

**Files:**
- Modify: `go-libfossil/sync/handler_test.go`

- [ ] **Step 1: Write failing tests for clone and reqconfig**

```go
func TestHandleClone(t *testing.T) {
	r := testutil.TempRepo(t)
	// Store 5 blobs
	stored := map[string]bool{}
	for i := 0; i < 5; i++ {
		data := []byte(fmt.Sprintf("clone test %d", i))
		uuid := hash.SHA1(data)
		storeBlob(r.DB(), uuid, "", data)
		stored[uuid] = true
	}

	// First page: limit is DefaultCloneBatchSize so all 5 should come in one batch
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.CloneCard{Version: 1},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	for _, f := range files {
		delete(stored, f.UUID)
	}
	// All 5 of our stored blobs should have been served
	// (plus any initial blobs from TempRepo)
	if len(stored) > 0 {
		t.Fatalf("clone missing blobs: %v", stored)
	}
}

func TestHandleClonePagination(t *testing.T) {
	r := testutil.TempRepo(t)
	// Store enough blobs to exceed one batch
	for i := 0; i < DefaultCloneBatchSize+5; i++ {
		data := []byte(fmt.Sprintf("page blob %d", i))
		uuid := hash.SHA1(data)
		storeBlob(r.DB(), uuid, "", data)
	}

	// Page 1
	req1 := &xfer.Message{Cards: []xfer.Card{&xfer.CloneCard{Version: 1}}}
	resp1, err := HandleSync(context.Background(), r, req1)
	if err != nil {
		t.Fatalf("page 1: %v", err)
	}

	files1 := findCards[*xfer.FileCard](resp1)
	seqnos := findCards[*xfer.CloneSeqNoCard](resp1)
	if len(seqnos) == 0 {
		t.Fatal("page 1: expected clone_seqno card for continuation")
	}
	if len(files1) != DefaultCloneBatchSize {
		t.Fatalf("page 1: got %d files, want %d", len(files1), DefaultCloneBatchSize)
	}

	// Page 2
	req2 := &xfer.Message{Cards: []xfer.Card{
		&xfer.CloneCard{Version: 1},
		&xfer.CloneSeqNoCard{SeqNo: seqnos[0].SeqNo},
	}}
	resp2, err := HandleSync(context.Background(), r, req2)
	if err != nil {
		t.Fatalf("page 2: %v", err)
	}

	files2 := findCards[*xfer.FileCard](resp2)
	if len(files2) < 5 {
		t.Fatalf("page 2: got %d files, want >= 5", len(files2))
	}
}

func TestHandleReqConfig(t *testing.T) {
	r := testutil.TempRepo(t)
	// TempRepo should have project-code in config
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.ReqConfigCard{Name: "project-code"},
	}}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}

	configs := findCards[*xfer.ConfigCard](resp)
	found := false
	for _, c := range configs {
		if c.Name == "project-code" && len(c.Content) > 0 {
			found = true
		}
	}
	if !found {
		t.Fatal("expected config card for project-code")
	}
}
```

- [ ] **Step 2: Run tests**

Run: `cd go-libfossil && go test ./sync/ -run "TestHandleClone|TestHandleReqConfig" -count=1 -v`
Expected: PASS (these should work with our existing handler.go)

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/sync/handler_test.go
git commit -m "sync: add clone pagination and reqconfig tests for HandleSync"
```

---

## Chunk 3: Transport Listeners

### Task 6: ServeHTTP

**Files:**
- Create: `go-libfossil/sync/serve_http.go`
- Create: `go-libfossil/sync/serve_http_test.go`

- [ ] **Step 1: Write failing integration test**

```go
// serve_http_test.go
package sync

import (
	"context"
	"fmt"
	"net"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func TestServeHTTPRoundTrip(t *testing.T) {
	r := testutil.TempRepo(t)
	data := []byte("http test blob")
	uuid := hash.SHA1(data)
	storeBlob(r.DB(), uuid, "", data)

	// Find a free port
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	errCh := make(chan error, 1)
	go func() {
		errCh <- ServeHTTP(ctx, addr, r, HandleSync)
	}()

	// Give server time to start
	time.Sleep(100 * time.Millisecond)

	// Use HTTPTransport as client
	transport := &HTTPTransport{URL: fmt.Sprintf("http://%s", addr)}
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.GimmeCard{UUID: uuid},
	}}

	resp, err := transport.Exchange(ctx, req)
	if err != nil {
		t.Fatalf("exchange: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	found := false
	for _, f := range files {
		if f.UUID == uuid {
			found = true
		}
	}
	if !found {
		t.Fatal("expected file card in HTTP response")
	}

	cancel()
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test ./sync/ -run TestServeHTTP -count=1 -v`
Expected: FAIL — `ServeHTTP` undefined

- [ ] **Step 3: Implement ServeHTTP**

```go
// serve_http.go
package sync

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// ServeHTTP starts an HTTP server that accepts Fossil xfer requests.
// Blocks until ctx is cancelled. Stock fossil clone/sync can connect.
func ServeHTTP(ctx context.Context, addr string, r *repo.Repo, h HandleFunc) error {
	if r == nil {
		panic("sync.ServeHTTP: r must not be nil")
	}
	if h == nil {
		panic("sync.ServeHTTP: h must not be nil")
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", xferHandler(r, h))

	srv := &http.Server{
		Addr:    addr,
		Handler: mux,
	}

	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("sync.ServeHTTP: listen: %w", err)
	}

	go func() {
		<-ctx.Done()
		srv.Close()
	}()

	err = srv.Serve(ln)
	if err == http.ErrServerClosed {
		return nil
	}
	return err
}

// xferHandler returns an http.HandlerFunc that decodes xfer requests,
// dispatches to the HandleFunc, and encodes the response.
func xferHandler(r *repo.Repo, h HandleFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, req *http.Request) {
		if req.Method != http.MethodPost {
			http.Error(w, "POST required", http.StatusMethodNotAllowed)
			return
		}

		body, err := io.ReadAll(req.Body)
		if err != nil {
			http.Error(w, "read body: "+err.Error(), http.StatusBadRequest)
			return
		}

		msg, err := xfer.Decode(body)
		if err != nil {
			http.Error(w, "decode xfer: "+err.Error(), http.StatusBadRequest)
			return
		}

		resp, err := h(req.Context(), r, msg)
		if err != nil {
			http.Error(w, "handler: "+err.Error(), http.StatusInternalServerError)
			return
		}

		respBytes, err := resp.Encode()
		if err != nil {
			http.Error(w, "encode response: "+err.Error(), http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/x-fossil")
		w.Write(respBytes)
	}
}
```

- [ ] **Step 4: Run tests**

Run: `cd go-libfossil && go test ./sync/ -run TestServeHTTP -count=1 -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/sync/serve_http.go go-libfossil/sync/serve_http_test.go
git commit -m "sync: implement ServeHTTP for Fossil xfer over HTTP"
```

### Task 7: Integration test with real `fossil clone`

**Files:**
- Modify: `go-libfossil/sync/serve_http_test.go`

- [ ] **Step 1: Write integration test**

```go
func TestServeHTTPFossilSync(t *testing.T) {
	if _, err := exec.LookPath("fossil"); err != nil {
		t.Skip("fossil not in PATH")
	}

	r := testutil.TempRepo(t)
	data := []byte("fossil sync test")
	uuid := hash.SHA1(data)
	storeBlob(r.DB(), uuid, "", data)

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	addr := ln.Addr().String()
	ln.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go ServeHTTP(ctx, addr, r, HandleSync)
	time.Sleep(100 * time.Millisecond)

	// Use fossil sync to pull from our server
	clonePath := filepath.Join(t.TempDir(), "clone.fossil")
	cmd := exec.Command("fossil", "clone", fmt.Sprintf("http://%s", addr), clonePath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil clone failed: %v\n%s", err, out)
	}

	// Verify the cloned repo has the blob
	cmd2 := exec.Command("fossil", "artifact", uuid, "-R", clonePath)
	out2, err2 := cmd2.CombinedOutput()
	if err2 != nil {
		t.Logf("fossil artifact output: %s", out2)
		t.Fatalf("fossil artifact failed: %v", err2)
	}
	if string(out2) != string(data) {
		t.Fatalf("artifact content mismatch: got %q", out2)
	}

	cancel()
}
```

Add `"os/exec"` and `"path/filepath"` to imports.

- [ ] **Step 2: Run test**

Run: `cd go-libfossil && go test ./sync/ -run TestServeHTTPFossil -count=1 -v -timeout=60s`
Expected: PASS if fossil is available, SKIP otherwise.
Note: This test may need debugging — Fossil's clone protocol has nuances. Iterate until it passes.

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/sync/serve_http_test.go
git commit -m "sync: add fossil clone integration test for ServeHTTP"
```

### Task 8: ServeNATS and ServeP2P stub

**Files:**
- Create: `leaf/agent/serve_nats.go`
- Create: `leaf/agent/serve_nats_test.go`
- Create: `leaf/agent/serve_p2p.go`

- [ ] **Step 1: Write failing test for ServeNATS**

```go
// serve_nats_test.go
package agent

import (
	"context"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/hash"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
	"github.com/nats-io/nats-server/v2/server"
	natsserver "github.com/nats-io/nats-server/v2/test"
	"github.com/nats-io/nats.go"
)

func startTestNATS(t *testing.T) *server.Server {
	t.Helper()
	opts := natsserver.DefaultTestOptions
	opts.Port = -1
	return natsserver.RunServer(&opts)
}

func TestServeNATSRoundTrip(t *testing.T) {
	ns := startTestNATS(t)
	defer ns.Shutdown()

	r := testutil.TempRepo(t)
	data := []byte("nats serve test")
	uuid := hash.SHA1(data)
	libsync.StoreBlob(r.DB(), uuid, "", data) // needs export — see step 3

	nc, err := nats.Connect(ns.ClientURL())
	if err != nil {
		t.Fatal(err)
	}
	defer nc.Close()

	subject := "fossil.test.sync"
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	go ServeNATS(ctx, nc, subject, r, libsync.HandleSync)
	time.Sleep(100 * time.Millisecond)

	// Client sends a request via NATS
	transport := NewNATSTransport(nc, "test", 5*time.Second, "fossil")
	req := &xfer.Message{Cards: []xfer.Card{
		&xfer.PullCard{ServerCode: "test", ProjectCode: "test"},
		&xfer.GimmeCard{UUID: uuid},
	}}
	resp, err := transport.Exchange(ctx, req)
	if err != nil {
		t.Fatalf("exchange: %v", err)
	}

	files := findCards[*xfer.FileCard](resp)
	found := false
	for _, f := range files {
		if f.UUID == uuid {
			found = true
		}
	}
	if !found {
		t.Fatal("expected file card in NATS response")
	}
}
```

Note: `storeBlob` is unexported. Either export it as `StoreBlob` or add a test helper. Decision: export it — the handler is already public, and the test helper pattern doesn't add value here.

- [ ] **Step 2: Export storeBlob and loadBlob in blob_io.go**

Rename `storeBlob` → `StoreBlob`, `loadBlob` → `LoadBlob`, `listBlobUUIDs` → `ListBlobUUIDs`, `listBlobsFromRID` → `ListBlobsFromRID` in `blob_io.go`. Update all callers in `client.go` and `handler.go`.

- [ ] **Step 3: Implement ServeNATS**

```go
// serve_nats.go
package agent

import (
	"context"
	"fmt"
	"log"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
	"github.com/nats-io/nats.go"
)

// ServeNATS subscribes to the given subject and dispatches incoming
// xfer requests to the handler. Blocks until ctx is cancelled.
func ServeNATS(ctx context.Context, nc *nats.Conn, subject string, r *repo.Repo, h sync.HandleFunc) error {
	if nc == nil {
		panic("agent.ServeNATS: nc must not be nil")
	}
	if r == nil {
		panic("agent.ServeNATS: r must not be nil")
	}
	if h == nil {
		panic("agent.ServeNATS: h must not be nil")
	}

	sub, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		req, err := xfer.Decode(msg.Data)
		if err != nil {
			log.Printf("serve-nats: decode error: %v", err)
			return
		}

		resp, err := h(ctx, r, req)
		if err != nil {
			log.Printf("serve-nats: handler error: %v", err)
			resp = &xfer.Message{Cards: []xfer.Card{
				&xfer.ErrorCard{Message: fmt.Sprintf("handler error: %v", err)},
			}}
		}

		respBytes, err := resp.Encode()
		if err != nil {
			log.Printf("serve-nats: encode error: %v", err)
			return
		}

		if err := msg.Respond(respBytes); err != nil {
			log.Printf("serve-nats: respond error: %v", err)
		}
	})
	if err != nil {
		return fmt.Errorf("agent.ServeNATS: subscribe %s: %w", subject, err)
	}

	<-ctx.Done()
	sub.Unsubscribe()
	return nil
}
```

- [ ] **Step 4: Create ServeP2P stub**

```go
// serve_p2p.go
package agent

// ServeP2P is a placeholder for the libp2p transport listener.
// Not implemented — planned for the libp2p phase.
func ServeP2P() {
	panic("agent.ServeP2P: not implemented — planned for libp2p phase")
}
```

- [ ] **Step 5: Run tests**

Run: `cd leaf && go test ./agent/ -run TestServeNATS -count=1 -v`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/sync/blob_io.go go-libfossil/sync/handler.go go-libfossil/sync/client.go
git add leaf/agent/serve_nats.go leaf/agent/serve_nats_test.go leaf/agent/serve_p2p.go
git commit -m "sync: add ServeNATS listener and ServeP2P stub"
```

---

## Chunk 4: Leaf Agent Integration

### Task 9: Update Config and Agent to start listeners

**Files:**
- Modify: `leaf/agent/config.go`
- Modify: `leaf/agent/agent.go`

- [ ] **Step 1: Add server config fields**

In `leaf/agent/config.go`, add to the `Config` struct:

```go
// ServeHTTPAddr is the HTTP listen address (e.g. ":8080").
// Empty means do not serve HTTP. When set, the leaf acts as a
// Fossil-compatible HTTP sync server.
ServeHTTPAddr string

// ServeNATSEnabled starts a NATS request/reply listener on the
// project sync subject. Enables leaf-to-leaf sync without a bridge.
ServeNATSEnabled bool
```

- [ ] **Step 2: Update agent.go Start() to launch listeners**

In `agent.go`, modify `Start()`:

```go
func (a *Agent) Start() error {
	ctx, cancel := context.WithCancel(context.Background())
	a.cancel = cancel
	a.done = make(chan struct{})
	go a.pollLoop(ctx)

	// Server listeners
	if a.config.ServeHTTPAddr != "" {
		go func() {
			subject := a.config.SubjectPrefix + "." + a.projectCode + ".sync"
			_ = subject // used by NATS below
			if err := sync.ServeHTTP(ctx, a.config.ServeHTTPAddr, a.repo, sync.HandleSync); err != nil {
				log.Printf("agent: serve-http stopped: %v", err)
			}
		}()
	}
	if a.config.ServeNATSEnabled && a.conn != nil {
		go func() {
			subject := a.config.SubjectPrefix + "." + a.projectCode + ".sync"
			if err := ServeNATS(ctx, a.conn, subject, a.repo, sync.HandleSync); err != nil {
				log.Printf("agent: serve-nats stopped: %v", err)
			}
		}()
	}

	return nil
}
```

Add `sync "github.com/dmestas/edgesync/go-libfossil/sync"` to imports.

- [ ] **Step 3: Run existing agent tests to verify no regression**

Run: `cd leaf && go test ./agent/ -count=1 -v`
Expected: All PASS

- [ ] **Step 4: Commit**

```bash
git add leaf/agent/config.go leaf/agent/agent.go
git commit -m "agent: integrate server listeners (ServeHTTP, ServeNATS) into Start()"
```

### Task 10: Run full test suite and clean up stubs.go

**Files:**
- Modify: `go-libfossil/sync/stubs.go`

- [ ] **Step 1: Update stubs.go**

Remove the `ServerHandler` interface (replaced by `HandleFunc` in handler.go). Keep `Clone` stub.

```go
// stubs.go
package sync

import (
	"context"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// CloneOpts configures a clone operation.
type CloneOpts struct {
	ProjectCode string
	ServerCode  string
	User        string
	Password    string
	Version     int
}

// Clone performs a full repository clone from a remote.
// NOT IMPLEMENTED — panics.
func Clone(ctx context.Context, r *repo.Repo, t Transport, opts CloneOpts) error {
	panic("sync.Clone: not implemented — planned for Phase G")
}
```

- [ ] **Step 2: Run full test suite**

Run: `make test`
Expected: All PASS

- [ ] **Step 3: Run DST quick**

Run: `make dst`
Expected: All 8 seeds PASS

- [ ] **Step 4: Final commit**

```bash
git add go-libfossil/sync/stubs.go
git commit -m "sync: clean up stubs.go, remove ServerHandler interface (replaced by HandleFunc)"
```
