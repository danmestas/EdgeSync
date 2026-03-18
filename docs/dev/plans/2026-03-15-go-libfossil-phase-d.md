# go-libfossil Phase D Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a transport-agnostic sync engine that pushes and pulls artifacts between a local Go repo and a remote Fossil server, with multi-round convergence.

**Architecture:** New `sync/` package with Transport interface, client-side sync engine (buildRequest/processResponse), convergence loop, and login auth. HTTPTransport provided for integration tests against `fossil server`. Mock transport for unit tests. Server role and clone mode stubbed.

**Tech Stack:** Go 1.23+, `crypto/sha1` (auth nonce/signature), `crypto/rand` (nonce randomness), `net/http` (HTTPTransport), `compress/zlib` (xfer message encoding). Phase A-C packages: `xfer/`, `blob/`, `content/`, `repo/`, `db/`, `hash/`, `delta/`.

**Spec:** `docs/superpowers/specs/2026-03-15-go-libfossil-phase-d-design.md`

---

## File Structure

```
go-libfossil/
  sync/
    transport.go        # NEW: Transport interface, HTTPTransport, MockTransport
    auth.go             # NEW: Login card nonce/signature computation
    client.go           # NEW: buildRequest, processResponse (per-round logic)
    session.go          # NEW: Sync() entry point, convergence loop, SyncOpts, SyncResult
    stubs.go            # NEW: ServerHandler interface, Clone() stub
    sync_test.go        # NEW: Unit tests with mock transport
    integration_test.go # NEW: Tests against fossil server subprocess
```

---

## Chunk 1: Transport and Auth

### Task 1: Transport Interface + MockTransport

**Files:**
- Create: `go-libfossil/sync/transport.go`
- Create: `go-libfossil/sync/sync_test.go` (initial)

- [ ] **Step 1: Write test**

Create `go-libfossil/sync/sync_test.go`:

```go
package sync

import (
	"context"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func TestMockTransportExchange(t *testing.T) {
	mt := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			return &xfer.Message{Cards: []xfer.Card{
				&xfer.IGotCard{UUID: "response-uuid"},
			}}
		},
	}
	resp, err := mt.Exchange(context.Background(), &xfer.Message{})
	if err != nil {
		t.Fatalf("Exchange: %v", err)
	}
	if len(resp.Cards) != 1 {
		t.Fatalf("cards = %d, want 1", len(resp.Cards))
	}
}

func TestMockTransportNilHandler(t *testing.T) {
	mt := &MockTransport{}
	resp, err := mt.Exchange(context.Background(), &xfer.Message{})
	if err != nil {
		t.Fatalf("Exchange: %v", err)
	}
	if len(resp.Cards) != 0 {
		t.Fatalf("expected empty response")
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement transport.go**

Create `go-libfossil/sync/transport.go`:

```go
package sync

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net/http"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// Transport sends an xfer request and returns the response.
type Transport interface {
	Exchange(ctx context.Context, request *xfer.Message) (*xfer.Message, error)
}

// MockTransport replays canned responses for testing.
type MockTransport struct {
	Handler func(req *xfer.Message) *xfer.Message
}

func (t *MockTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	if t.Handler == nil {
		return &xfer.Message{}, nil
	}
	return t.Handler(req), nil
}

// HTTPTransport speaks Fossil's HTTP /xfer protocol.
type HTTPTransport struct {
	URL string
}

func (t *HTTPTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	body, err := req.Encode()
	if err != nil {
		return nil, fmt.Errorf("sync.HTTPTransport encode: %w", err)
	}

	httpReq, err := http.NewRequestWithContext(ctx, "POST", t.URL+"/xfer", bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("sync.HTTPTransport request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/x-fossil")

	resp, err := http.DefaultClient.Do(httpReq)
	if err != nil {
		return nil, fmt.Errorf("sync.HTTPTransport do: %w", err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("sync.HTTPTransport read: %w", err)
	}

	return xfer.Decode(respBody)
}
```

- [ ] **Step 4: Run tests — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/sync/transport.go go-libfossil/sync/sync_test.go
fossil commit -m "sync: add Transport interface, MockTransport, HTTPTransport"
```

---

### Task 2: Login Authentication

**Files:**
- Create: `go-libfossil/sync/auth.go`
- Modify: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Add tests**

```go
import (
	"crypto/sha1"
	"encoding/hex"
)

func sha1hex(data string) string {
	h := sha1.Sum([]byte(data))
	return hex.EncodeToString(h[:])
}

func TestComputeLogin(t *testing.T) {
	payload := []byte("pull abc def\nigot da39a3ee5e6b4b0d3255bfef95601890afd80709\n")
	card := computeLogin("testuser", "secret", "projcode", payload)

	if card.User != "testuser" {
		t.Fatalf("User = %q", card.User)
	}
	// Nonce should be SHA1 of payload
	expectedNonce := sha1hex(string(payload))
	if card.Nonce != expectedNonce {
		t.Fatalf("Nonce = %q, want %q", card.Nonce, expectedNonce)
	}
	// Signature = SHA1(nonce + SHA1(projectCode/user/password))
	sharedSecret := sha1hex("projcode/testuser/secret")
	expectedSig := sha1hex(card.Nonce + sharedSecret)
	if card.Signature != expectedSig {
		t.Fatalf("Signature = %q, want %q", card.Signature, expectedSig)
	}
}

func TestComputeLoginAnonymous(t *testing.T) {
	card := computeLogin("anonymous", "", "projcode", []byte("test\n"))
	if card.User != "anonymous" {
		t.Fatalf("User = %q", card.User)
	}
	// Anonymous still has a nonce
	if card.Nonce == "" {
		t.Fatal("anonymous should still have nonce")
	}
}

func TestAppendRandomComment(t *testing.T) {
	payload1 := appendRandomComment([]byte("test\n"))
	payload2 := appendRandomComment([]byte("test\n"))
	// Random comments should differ
	if string(payload1) == string(payload2) {
		t.Fatal("random comments should be unique")
	}
	// Should end with newline
	if payload1[len(payload1)-1] != '\n' {
		t.Fatal("random comment should end with newline")
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement auth.go**

```go
package sync

import (
	"crypto/rand"
	"crypto/sha1"
	"encoding/hex"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// computeLogin produces a LoginCard for the given credentials.
// payload is the encoded bytes of all non-login cards (including random comment).
func computeLogin(user, password, projectCode string, payload []byte) *xfer.LoginCard {
	nonce := sha1Hex(payload)
	sharedSecret := sha1Hex([]byte(projectCode + "/" + user + "/" + password))
	signature := sha1Hex([]byte(nonce + sharedSecret))
	return &xfer.LoginCard{User: user, Nonce: nonce, Signature: signature}
}

// appendRandomComment appends "# <random-hex>\n" to payload for nonce uniqueness.
func appendRandomComment(payload []byte) []byte {
	rb := make([]byte, 20)
	rand.Read(rb)
	comment := fmt.Sprintf("# %s\n", hex.EncodeToString(rb))
	return append(payload, []byte(comment)...)
}

func sha1Hex(data []byte) string {
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}
```

- [ ] **Step 4: Run tests — should pass**

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/sync/auth.go
fossil commit -m "sync: add login nonce/signature computation"
```

---

## Chunk 2: Client Sync Engine

### Task 3: buildRequest

**Files:**
- Create: `go-libfossil/sync/client.go`
- Create: `go-libfossil/sync/session.go` (types only, Sync() in Task 5)
- Modify: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Add tests**

```go
import (
	"path/filepath"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"time"
)

func setupSyncTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "testuser")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestBuildRequestPushOnly(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Push: true, ProjectCode: "pc", ServerCode: "sc"})
	msg, err := s.buildRequest(0)
	if err != nil {
		t.Fatalf("buildRequest: %v", err)
	}
	hasPush, hasPull := false, false
	for _, c := range msg.Cards {
		switch c.Type() {
		case xfer.CardPush:
			hasPush = true
		case xfer.CardPull:
			hasPull = true
		}
	}
	if !hasPush {
		t.Fatal("missing push card")
	}
	if hasPull {
		t.Fatal("should not have pull card")
	}
}

func TestBuildRequestPullOnly(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})
	msg, _ := s.buildRequest(0)
	hasPush, hasPull := false, false
	for _, c := range msg.Cards {
		switch c.Type() {
		case xfer.CardPush:
			hasPush = true
		case xfer.CardPull:
			hasPull = true
		}
	}
	if hasPush {
		t.Fatal("should not have push card")
	}
	if !hasPull {
		t.Fatal("missing pull card")
	}
}

func TestBuildRequestHasPragma(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})
	msg, _ := s.buildRequest(0)
	hasPragma := false
	for _, c := range msg.Cards {
		if p, ok := c.(*xfer.PragmaCard); ok && p.Name == "client-version" {
			hasPragma = true
		}
	}
	if !hasPragma {
		t.Fatal("missing pragma client-version")
	}
}

func TestBuildRequestIGotFromUnclustered(t *testing.T) {
	r := setupSyncTestRepo(t)
	// Create a checkin to populate unclustered table
	manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello")}},
		Comment: "test", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	s := newSession(r, SyncOpts{Push: true, ProjectCode: "pc", ServerCode: "sc"})
	msg, _ := s.buildRequest(0)
	igotCount := 0
	for _, c := range msg.Cards {
		if c.Type() == xfer.CardIGot {
			igotCount++
		}
	}
	if igotCount == 0 {
		t.Fatal("expected igot cards from unclustered table")
	}
}

func TestBuildRequestGimmeForPhantoms(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})
	s.phantoms["da39a3ee5e6b4b0d3255bfef95601890afd80709"] = true
	msg, _ := s.buildRequest(0)
	gimmeCount := 0
	for _, c := range msg.Cards {
		if c.Type() == xfer.CardGimme {
			gimmeCount++
		}
	}
	if gimmeCount != 1 {
		t.Fatalf("gimme count = %d, want 1", gimmeCount)
	}
}

func TestBuildRequestFileForPendingSend(t *testing.T) {
	r := setupSyncTestRepo(t)
	// Store a blob so we can reference it
	rid, uuid, _ := blob.Store(r.DB(), []byte("file content"))
	_ = rid
	s := newSession(r, SyncOpts{Push: true, ProjectCode: "pc", ServerCode: "sc"})
	s.pendingSend[uuid] = true
	msg, _ := s.buildRequest(0)
	fileCount := 0
	for _, c := range msg.Cards {
		if c.Type() == xfer.CardFile {
			fileCount++
		}
	}
	if fileCount != 1 {
		t.Fatalf("file count = %d, want 1", fileCount)
	}
}

func TestBuildRequestWithLogin(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{
		Push: true, ProjectCode: "pc", ServerCode: "sc",
		User: "testuser", Password: "secret",
	})
	msg, _ := s.buildRequest(0)
	hasLogin := false
	for _, c := range msg.Cards {
		if c.Type() == xfer.CardLogin {
			hasLogin = true
		}
	}
	if !hasLogin {
		t.Fatal("missing login card")
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Implement session.go (types only)**

Create `go-libfossil/sync/session.go`:

```go
package sync

import (
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

const (
	DefaultMaxSend = 250000
	MaxRounds      = 100
	MaxGimmeBase   = 200
)

// SyncOpts configures a sync operation.
type SyncOpts struct {
	Push        bool
	Pull        bool
	ProjectCode string
	ServerCode  string
	User        string
	Password    string
	MaxSend     int
}

// SyncResult reports what happened during sync.
type SyncResult struct {
	Rounds     int
	FilesSent  int
	FilesRecvd int
	Errors     []string
}

type session struct {
	repo              *repo.Repo
	opts              SyncOpts
	result            SyncResult
	cookie            string
	remoteHas         map[string]bool
	phantoms          map[string]bool
	pendingSend       map[string]bool
	filesRecvdLastRound int
	maxSend           int
}

func newSession(r *repo.Repo, opts SyncOpts) *session {
	ms := opts.MaxSend
	if ms <= 0 {
		ms = DefaultMaxSend
	}
	return &session{
		repo:        r,
		opts:        opts,
		maxSend:     ms,
		remoteHas:   make(map[string]bool),
		phantoms:    make(map[string]bool),
		pendingSend: make(map[string]bool),
	}
}
```

- [ ] **Step 4: Implement client.go (buildRequest)**

Create `go-libfossil/sync/client.go`:

```go
package sync

import (
	"bytes"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

const clientVersion = "go-libfossil 0.1.0"

// buildRequest constructs the outgoing xfer.Message for one round.
func (s *session) buildRequest(cycle int) (*xfer.Message, error) {
	var cards []xfer.Card

	// Header cards (every cycle)
	cards = append(cards, &xfer.PragmaCard{Name: "client-version", Values: []string{clientVersion}})
	if s.opts.Push {
		cards = append(cards, &xfer.PushCard{ServerCode: s.opts.ServerCode, ProjectCode: s.opts.ProjectCode})
	}
	if s.opts.Pull {
		cards = append(cards, &xfer.PullCard{ServerCode: s.opts.ServerCode, ProjectCode: s.opts.ProjectCode})
	}
	if s.cookie != "" {
		cards = append(cards, &xfer.CookieCard{Value: s.cookie})
	}

	// IGot cards from unclustered table (no count limit, budget-limited)
	igotCards, err := s.buildIGotCards()
	if err != nil {
		return nil, fmt.Errorf("buildRequest igot: %w", err)
	}
	cards = append(cards, igotCards...)

	// File cards for pendingSend + unsent (budget-limited)
	fileCards, err := s.buildFileCards()
	if err != nil {
		return nil, fmt.Errorf("buildRequest files: %w", err)
	}
	cards = append(cards, fileCards...)

	// Gimme cards for phantoms (max scaled dynamically)
	gimmeCards := s.buildGimmeCards()
	cards = append(cards, gimmeCards...)

	// Login card (computed last — nonce depends on payload)
	if s.opts.User != "" {
		// Encode non-login cards to compute nonce
		var buf bytes.Buffer
		for _, c := range cards {
			xfer.EncodeCard(&buf, c)
		}
		payload := appendRandomComment(buf.Bytes())
		loginCard := computeLogin(s.opts.User, s.opts.Password, s.opts.ProjectCode, payload)
		cards = append([]xfer.Card{loginCard}, cards...)
	}

	return &xfer.Message{Cards: cards}, nil
}

func (s *session) buildIGotCards() ([]xfer.Card, error) {
	rows, err := s.repo.DB().Query("SELECT uuid FROM blob WHERE rid IN (SELECT rid FROM unclustered)")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cards []xfer.Card
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		if !s.remoteHas[uuid] {
			cards = append(cards, &xfer.IGotCard{UUID: uuid})
		}
	}
	return cards, rows.Err()
}

func (s *session) buildFileCards() ([]xfer.Card, error) {
	var cards []xfer.Card
	budget := s.maxSend

	// From pendingSend (server gimme'd these)
	for uuid := range s.pendingSend {
		if budget <= 0 {
			break
		}
		card, size, err := s.loadFileCard(uuid)
		if err != nil {
			continue // skip artifacts we can't load
		}
		cards = append(cards, card)
		budget -= size
		delete(s.pendingSend, uuid)
		s.result.FilesSent++
	}

	// From unsent table (new local artifacts)
	if s.opts.Push && budget > 0 {
		rows, err := s.repo.DB().Query("SELECT uuid FROM blob WHERE rid IN (SELECT rid FROM unsent)")
		if err != nil {
			return cards, nil // non-fatal
		}
		defer rows.Close()

		for rows.Next() && budget > 0 {
			var uuid string
			rows.Scan(&uuid)
			if s.remoteHas[uuid] {
				continue
			}
			card, size, err := s.loadFileCard(uuid)
			if err != nil {
				continue
			}
			cards = append(cards, card)
			budget -= size
			s.result.FilesSent++
		}
	}

	return cards, nil
}

func (s *session) loadFileCard(uuid string) (xfer.Card, int, error) {
	rid, ok := blob.Exists(s.repo.DB(), uuid)
	if !ok {
		return nil, 0, fmt.Errorf("blob not found: %s", uuid)
	}
	data, err := content.Expand(s.repo.DB(), rid)
	if err != nil {
		return nil, 0, err
	}
	return &xfer.FileCard{UUID: uuid, Content: data}, len(data), nil
}

func (s *session) buildGimmeCards() []xfer.Card {
	maxGimme := MaxGimmeBase
	if scaled := s.filesRecvdLastRound * 2; scaled > maxGimme {
		maxGimme = scaled
	}

	var cards []xfer.Card
	count := 0
	for uuid := range s.phantoms {
		if count >= maxGimme {
			break
		}
		cards = append(cards, &xfer.GimmeCard{UUID: uuid})
		count++
	}
	return cards
}

// processResponse handles the incoming xfer.Message from one round.
// Returns done=true when convergence criteria are met.
func (s *session) processResponse(msg *xfer.Message) (bool, error) {
	filesRecvd := 0
	filesSent := s.result.FilesSent // already counted in buildFileCards

	for _, c := range msg.Cards {
		switch v := c.(type) {
		case *xfer.FileCard:
			if err := s.storeArtifact(v.UUID, v.DeltaSrc, v.Content); err != nil {
				return false, fmt.Errorf("store file: %w", err)
			}
			delete(s.phantoms, v.UUID)
			filesRecvd++

		case *xfer.CFileCard:
			if err := s.storeArtifact(v.UUID, v.DeltaSrc, v.Content); err != nil {
				return false, fmt.Errorf("store cfile: %w", err)
			}
			delete(s.phantoms, v.UUID)
			filesRecvd++

		case *xfer.IGotCard:
			s.remoteHas[v.UUID] = true
			if s.opts.Pull {
				if _, ok := blob.Exists(s.repo.DB(), v.UUID); !ok {
					s.phantoms[v.UUID] = true
				}
			}

		case *xfer.GimmeCard:
			s.pendingSend[v.UUID] = true

		case *xfer.CookieCard:
			s.cookie = v.Value

		case *xfer.ErrorCard:
			s.result.Errors = append(s.result.Errors, v.Message)

		case *xfer.MessageCard:
			s.result.Errors = append(s.result.Errors, v.Message)
		}
	}

	s.result.FilesRecvd += filesRecvd
	s.filesRecvdLastRound = filesRecvd

	// Convergence: nothing exchanged, nothing pending
	done := filesRecvd == 0 &&
		len(s.pendingSend) == 0 &&
		len(s.phantoms) == 0
	// Check unsent table for remaining artifacts
	if done && s.opts.Push {
		var count int
		s.repo.DB().QueryRow("SELECT count(*) FROM unsent").Scan(&count)
		if count > 0 {
			done = false
		}
	}

	return done, nil
}

func (s *session) storeArtifact(uuid, deltaSrc string, data []byte) error {
	var fullContent []byte
	if deltaSrc != "" {
		baseRid, ok := blob.Exists(s.repo.DB(), deltaSrc)
		if !ok {
			return fmt.Errorf("delta base not found: %s", deltaSrc)
		}
		baseData, err := content.Expand(s.repo.DB(), baseRid)
		if err != nil {
			return fmt.Errorf("expand delta base: %w", err)
		}
		expanded, err := delta.Apply(baseData, data)
		if err != nil {
			return fmt.Errorf("apply delta: %w", err)
		}
		fullContent = expanded
	} else {
		fullContent = data
	}

	_, storedUUID, err := blob.Store(s.repo.DB(), fullContent)
	if err != nil {
		return fmt.Errorf("blob.Store: %w", err)
	}
	if storedUUID != uuid {
		return fmt.Errorf("UUID mismatch: card=%s stored=%s", uuid, storedUUID)
	}
	return nil
}
```

Add `delta` import: `"github.com/dmestas/edgesync/go-libfossil/delta"`

- [ ] **Step 5: Run tests — should pass**

- [ ] **Step 6: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/sync/client.go go-libfossil/sync/session.go
fossil commit -m "sync: add buildRequest and processResponse with per-round logic"
```

---

### Task 4: processResponse Tests

**Files:**
- Modify: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Add processResponse tests**

```go
func TestProcessResponseStoresFile(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})
	content := []byte("hello artifact")
	uuid := sha1hex(string(content))

	resp := &xfer.Message{Cards: []xfer.Card{
		&xfer.FileCard{UUID: uuid, Content: content},
	}}
	done, err := s.processResponse(resp)
	if err != nil {
		t.Fatalf("processResponse: %v", err)
	}
	if done {
		t.Fatal("should not be done yet (files received)")
	}
	// Verify blob was stored
	_, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("blob not stored")
	}
}

func TestProcessResponseIGotAddsPhantom(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})

	uuid := "da39a3ee5e6b4b0d3255bfef95601890afd80709"
	resp := &xfer.Message{Cards: []xfer.Card{
		&xfer.IGotCard{UUID: uuid},
	}}
	s.processResponse(resp)
	if !s.phantoms[uuid] {
		t.Fatal("igot should add phantom when Pull=true and blob missing")
	}
	if !s.remoteHas[uuid] {
		t.Fatal("igot should add to remoteHas")
	}
}

func TestProcessResponseGimmeAddsPendingSend(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Push: true, ProjectCode: "pc", ServerCode: "sc"})

	uuid := "da39a3ee5e6b4b0d3255bfef95601890afd80709"
	resp := &xfer.Message{Cards: []xfer.Card{
		&xfer.GimmeCard{UUID: uuid},
	}}
	s.processResponse(resp)
	if !s.pendingSend[uuid] {
		t.Fatal("gimme should add to pendingSend")
	}
}

func TestProcessResponseCookieCached(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})

	resp := &xfer.Message{Cards: []xfer.Card{
		&xfer.CookieCard{Value: "sess123"},
	}}
	s.processResponse(resp)
	if s.cookie != "sess123" {
		t.Fatalf("cookie = %q", s.cookie)
	}
}

func TestProcessResponseConvergence(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})

	// Empty response = converged
	done, _ := s.processResponse(&xfer.Message{})
	if !done {
		t.Fatal("empty response should be converged")
	}
}

func TestProcessResponseErrorCard(t *testing.T) {
	r := setupSyncTestRepo(t)
	s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})

	resp := &xfer.Message{Cards: []xfer.Card{
		&xfer.ErrorCard{Message: "access denied"},
	}}
	s.processResponse(resp)
	if len(s.result.Errors) != 1 || s.result.Errors[0] != "access denied" {
		t.Fatalf("errors = %v", s.result.Errors)
	}
}
```

- [ ] **Step 2: Run — should pass**

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "sync: add processResponse unit tests"
```

---

### Task 5: Convergence Loop (Sync entry point)

**Files:**
- Modify: `go-libfossil/sync/session.go`
- Modify: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Add session tests**

```go
func TestSyncSingleRound(t *testing.T) {
	r := setupSyncTestRepo(t)
	mt := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			return &xfer.Message{} // empty response = converged
		},
	}
	result, err := Sync(context.Background(), r, mt, SyncOpts{
		Pull: true, ProjectCode: "pc", ServerCode: "sc",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}
	if result.Rounds != 1 {
		t.Fatalf("rounds = %d, want 1", result.Rounds)
	}
}

func TestSyncMultiRound(t *testing.T) {
	r := setupSyncTestRepo(t)
	round := 0
	mt := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			round++
			if round <= 2 {
				// First 2 rounds: server sends igot (creates phantoms), then file
				if round == 1 {
					return &xfer.Message{Cards: []xfer.Card{
						&xfer.IGotCard{UUID: "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
					}}
				}
				return &xfer.Message{Cards: []xfer.Card{
					&xfer.FileCard{
						UUID:    "da39a3ee5e6b4b0d3255bfef95601890afd80709",
						Content: []byte(""),
					},
				}}
			}
			return &xfer.Message{} // converged
		},
	}
	result, err := Sync(context.Background(), r, mt, SyncOpts{
		Pull: true, ProjectCode: "pc", ServerCode: "sc",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}
	if result.Rounds < 2 {
		t.Fatalf("rounds = %d, want >= 2", result.Rounds)
	}
}

func TestSyncContextCancellation(t *testing.T) {
	r := setupSyncTestRepo(t)
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // cancel immediately

	mt := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			return &xfer.Message{Cards: []xfer.Card{
				&xfer.IGotCard{UUID: "abc123"},
			}}
		},
	}
	_, err := Sync(ctx, r, mt, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})
	if err == nil {
		t.Fatal("should fail on cancelled context")
	}
}
```

- [ ] **Step 2: Run — should fail**

- [ ] **Step 3: Add Sync() to session.go**

```go
import (
	"context"
	"fmt"
)

// Sync runs a complete sync session.
func Sync(ctx context.Context, r *repo.Repo, t Transport, opts SyncOpts) (*SyncResult, error) {
	s := newSession(r, opts)

	for cycle := 0; ; cycle++ {
		select {
		case <-ctx.Done():
			return &s.result, ctx.Err()
		default:
		}

		if cycle >= MaxRounds {
			return &s.result, fmt.Errorf("sync: exceeded %d rounds", MaxRounds)
		}

		req, err := s.buildRequest(cycle)
		if err != nil {
			return &s.result, fmt.Errorf("sync round %d build: %w", cycle, err)
		}

		resp, err := t.Exchange(ctx, req)
		if err != nil {
			return &s.result, fmt.Errorf("sync round %d exchange: %w", cycle, err)
		}

		done, err := s.processResponse(resp)
		if err != nil {
			return &s.result, fmt.Errorf("sync round %d process: %w", cycle, err)
		}

		s.result.Rounds = cycle + 1

		if done {
			break
		}
	}

	return &s.result, nil
}
```

- [ ] **Step 4: Run tests — should pass**

- [ ] **Step 5: Run full test suite**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./...`

- [ ] **Step 6: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "sync: add Sync() convergence loop"
```

---

## Chunk 3: Stubs, Integration, Validation

### Task 6: Server/Clone Stubs

**Files:**
- Create: `go-libfossil/sync/stubs.go`

- [ ] **Step 1: Create stubs.go**

```go
package sync

import (
	"context"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// ServerHandler processes an incoming sync request and produces a response.
// NOT IMPLEMENTED in Phase D — placeholder for future work.
type ServerHandler interface {
	HandleSync(ctx context.Context, r *repo.Repo, request *xfer.Message) (*xfer.Message, error)
}

// CloneOpts configures a clone operation.
type CloneOpts struct {
	ProjectCode string
	ServerCode  string
	User        string
	Password    string
	Version     int
}

// Clone performs a full repository clone from a remote.
// NOT IMPLEMENTED — returns error.
func Clone(ctx context.Context, r *repo.Repo, t Transport, opts CloneOpts) error {
	return fmt.Errorf("sync.Clone: not yet implemented")
}
```

- [ ] **Step 2: Verify compilation**

Run: `cd ~/projects/EdgeSync/go-libfossil && go build ./sync/...`

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/sync/stubs.go
fossil commit -m "sync: add server handler interface and clone stub"
```

---

### Task 7: Integration Tests (fossil server)

**Files:**
- Create: `go-libfossil/sync/integration_test.go`

- [ ] **Step 1: Create integration_test.go**

```go
package sync

import (
	"context"
	"fmt"
	"net"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

func startFossilServer(t *testing.T, repoPath string) (url string, cleanup func()) {
	t.Helper()
	bin := testutil.FossilBinary()
	if bin == "" {
		t.Skip("fossil not in PATH")
	}

	// Find a free port
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("find port: %v", err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()

	cmd := exec.Command(bin, "server", "--port", fmt.Sprintf("%d", port), repoPath)
	if err := cmd.Start(); err != nil {
		t.Fatalf("start fossil server: %v", err)
	}

	url = fmt.Sprintf("http://127.0.0.1:%d", port)

	// Wait for server to be ready
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 100*time.Millisecond)
		if err == nil {
			conn.Close()
			break
		}
		time.Sleep(50 * time.Millisecond)
	}

	return url, func() { cmd.Process.Kill(); cmd.Wait() }
}

func getProjectCode(t *testing.T, r *repo.Repo) string {
	t.Helper()
	var code string
	err := r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&code)
	if err != nil {
		t.Fatalf("get project-code: %v", err)
	}
	return code
}

func getServerCode(t *testing.T, r *repo.Repo) string {
	t.Helper()
	var code string
	err := r.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&code)
	if err != nil {
		t.Fatalf("get server-code: %v", err)
	}
	return code
}

func TestIntegrationPush(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}

	// Create local repo with Go and add a checkin
	localPath := filepath.Join(t.TempDir(), "local.fossil")
	localRepo, err := repo.Create(localPath, "testuser")
	if err != nil {
		t.Fatalf("Create local: %v", err)
	}
	defer localRepo.Close()

	_, _, err = manifest.Checkin(localRepo, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world")}},
		Comment: "initial commit from Go",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Create remote repo with fossil new (shares project code)
	remotePath := filepath.Join(t.TempDir(), "remote.fossil")
	exec.Command(testutil.FossilBinary(), "clone", localPath, remotePath).Run()

	// Start fossil server on the remote
	url, cleanup := startFossilServer(t, remotePath)
	defer cleanup()

	// Get codes from local repo
	pc := getProjectCode(t, localRepo)
	sc := getServerCode(t, localRepo)

	// Push from local to remote
	result, err := Sync(context.Background(), localRepo, &HTTPTransport{URL: url}, SyncOpts{
		Push:        true,
		ProjectCode: pc,
		ServerCode:  sc,
		User:        "anonymous",
	})
	if err != nil {
		t.Fatalf("Sync push: %v", err)
	}
	t.Logf("Push: %d rounds, %d files sent, %d received", result.Rounds, result.FilesSent, result.FilesRecvd)
}

func TestIntegrationPull(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}

	// Create remote repo with fossil and add content
	remotePath := filepath.Join(t.TempDir(), "remote.fossil")
	exec.Command(testutil.FossilBinary(), "new", remotePath).Run()

	// Create local repo cloned from remote
	localPath := filepath.Join(t.TempDir(), "local.fossil")
	exec.Command(testutil.FossilBinary(), "clone", remotePath, localPath).Run()

	localRepo, err := repo.Open(localPath)
	if err != nil {
		t.Fatalf("Open local: %v", err)
	}
	defer localRepo.Close()

	// Start server on remote
	url, cleanup := startFossilServer(t, remotePath)
	defer cleanup()

	pc := getProjectCode(t, localRepo)
	sc := getServerCode(t, localRepo)

	// Pull
	result, err := Sync(context.Background(), localRepo, &HTTPTransport{URL: url}, SyncOpts{
		Pull:        true,
		ProjectCode: pc,
		ServerCode:  sc,
		User:        "anonymous",
	})
	if err != nil {
		t.Fatalf("Sync pull: %v", err)
	}
	t.Logf("Pull: %d rounds, %d files sent, %d received", result.Rounds, result.FilesSent, result.FilesRecvd)

	// Verify artifacts received are valid
	if result.FilesRecvd > 0 {
		rows, _ := localRepo.DB().Query("SELECT rid FROM blob WHERE size >= 0")
		defer rows.Close()
		for rows.Next() {
			var rid int64
			rows.Scan(&rid)
			if err := content.Verify(localRepo.DB(), libfossil.FslID(rid)); err != nil {
				t.Errorf("Verify rid=%d: %v", rid, err)
			}
		}
	}
}
```

Add import: `libfossil "github.com/dmestas/edgesync/go-libfossil"`

NOTE: The integration tests may need adjustment during implementation depending on how fossil server responds. The test structure is correct but exact behavior (e.g., whether fossil clone sets up matching project codes) should be verified during implementation.

- [ ] **Step 2: Run integration tests**

Run: `cd ~/projects/EdgeSync/go-libfossil && go test ./sync/ -run TestIntegration -v -timeout 30s`

- [ ] **Step 3: Fix any issues and iterate**

- [ ] **Step 4: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/sync/integration_test.go
fossil commit -m "sync: add integration tests against fossil server"
```

---

### Task 8: Benchmarks + Full Validation

**Files:**
- Modify: `go-libfossil/sync/sync_test.go`

- [ ] **Step 1: Add benchmarks**

```go
func BenchmarkBuildRequest(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	r, _ := repo.Create(path, "bench")
	defer r.Close()
	// Create many blobs to populate unclustered
	for i := 0; i < 100; i++ {
		blob.Store(r.DB(), []byte(fmt.Sprintf("artifact-%d", i)))
	}
	s := newSession(r, SyncOpts{Push: true, ProjectCode: "pc", ServerCode: "sc"})
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s.buildRequest(0)
	}
}

func BenchmarkProcessResponse(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	r, _ := repo.Create(path, "bench")
	defer r.Close()
	// Build a response with 50 file cards
	cards := make([]xfer.Card, 50)
	for i := range cards {
		content := []byte(fmt.Sprintf("bench-artifact-%d", i))
		cards[i] = &xfer.FileCard{
			UUID:    sha1hex(string(content)),
			Content: content,
		}
	}
	msg := &xfer.Message{Cards: cards}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		s := newSession(r, SyncOpts{Pull: true, ProjectCode: "pc", ServerCode: "sc"})
		s.processResponse(msg)
	}
}
```

- [ ] **Step 2: Run full validation**

```bash
cd ~/projects/EdgeSync/go-libfossil
go vet ./...
go test -count=1 ./...
go test -race ./...
go test ./sync/ -bench=. -benchmem
go test -cover ./...
```

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "sync: add benchmarks and complete Phase D validation"
```

- [ ] **Step 4: Push to go-libfossil remote**

```bash
cd ~/projects/go-libfossil-remote
cp -r ~/projects/EdgeSync/go-libfossil/sync .
fossil addremove
fossil commit -m "sync: Phase D — transport-agnostic sync engine with convergence loop"
fossil push
```
