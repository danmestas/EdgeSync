# EdgeSync Notify — Go Backend Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `edgesync notify` CLI commands — message format, Fossil repo storage, NATS pub/sub, and the `init`, `send`, `watch`, `ask`, `threads`, `log`, `status` commands — all TDD.

**Architecture:** A `leaf/agent/notify/` package provides message types, free functions for repo I/O (`CommitMessage`, `ListThreads`, `ReadThread` operating on `*libfossil.Repo`), a `Publish` free function and `Subscriber` type for NATS, and a `Service` that composes them. No wrapper types around `libfossil.Repo` or `nats.Conn` — functions operate on standard types directly (Ousterhout: deep modules, no pass-throughs, no leaking wrappers). CLI commands in `cmd/edgesync/` wire it together via Kong. NATS subjects follow `notify.<project>.<thread-id>` for messages and `notify.<project>.*` for wildcard subscriptions.

**Tech Stack:** Go 1.26, go-libfossil v0.2.x, nats.go, Kong CLI, stdlib `encoding/json`

**Spec:** `docs/superpowers/specs/2026-04-10-edgesync-notify-design.md`

**No Claude attribution on EdgeSync PRs.**

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `leaf/agent/notify/message.go` | Message struct, JSON serialization, path generation. Two constructors: `NewMessage`, `NewReply`. No `NewActionReply` (caller sets `ActionResponse: true` on a reply). |
| `leaf/agent/notify/message_test.go` | Unit tests for message format |
| `leaf/agent/notify/store.go` | Free functions on `*libfossil.Repo`: `CommitMessage`, `ReadMessage`, `ListThreads`, `ReadThread`. No wrapper type. |
| `leaf/agent/notify/store_test.go` | Unit tests for repo operations |
| `leaf/agent/notify/pubsub.go` | `Publish` free function + `Subscriber` type (manages dedup + subscriptions). No `Publisher` type (one function doesn't need a type). |
| `leaf/agent/notify/pubsub_test.go` | Unit tests for NATS pub/sub |
| `leaf/agent/notify/notify.go` | `Service` — holds `*libfossil.Repo` + `*nats.Conn` + `Subscriber` directly. `Send`, `Watch`, `FormatWatchLine`. |
| `leaf/agent/notify/notify_test.go` | Integration-level tests for Service |
| `cmd/edgesync/notify.go` | Kong command structs and Run methods for all notify subcommands |
| `cmd/edgesync/notify_test.go` | CLI end-to-end tests |

### Modified Files

| File | Change |
|------|--------|
| `cmd/edgesync/cli.go` | Add `Notify NotifyCmd` field to CLI struct |

---

## Task 1: Message Struct & Serialization

**Files:**
- Create: `leaf/agent/notify/message.go`
- Create: `leaf/agent/notify/message_test.go`

- [ ] **Step 1: Create the notify package and write the failing test for message creation**

Create `leaf/agent/notify/message_test.go`:

```go
package notify

import (
	"encoding/json"
	"testing"
)

func TestNewMessage(t *testing.T) {
	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Build failed. Retry?",
		Priority: PriorityActionRequired,
		Actions: []Action{
			{ID: "retry", Label: "Retry"},
			{ID: "skip", Label: "Skip"},
		},
	})

	if msg.V != 1 {
		t.Errorf("V = %d, want 1", msg.V)
	}
	if msg.Project != "edgesync" {
		t.Errorf("Project = %q, want %q", msg.Project, "edgesync")
	}
	if msg.From != "endpoint-abc123" {
		t.Errorf("From = %q, want %q", msg.From, "endpoint-abc123")
	}
	if msg.Body != "Build failed. Retry?" {
		t.Errorf("Body = %q, want %q", msg.Body, "Build failed. Retry?")
	}
	if msg.Priority != PriorityActionRequired {
		t.Errorf("Priority = %q, want %q", msg.Priority, PriorityActionRequired)
	}
	if len(msg.Actions) != 2 {
		t.Fatalf("Actions len = %d, want 2", len(msg.Actions))
	}
	if msg.ID == "" {
		t.Error("ID should not be empty")
	}
	if msg.Thread == "" {
		t.Error("Thread should not be empty")
	}
	if msg.Timestamp.IsZero() {
		t.Error("Timestamp should not be zero")
	}
}

func TestNewReply(t *testing.T) {
	original := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Deploy?",
	})

	reply := NewReply(original, ReplyOpts{
		From:     "endpoint-xyz789",
		FromName: "dan-iphone",
		Body:     "yes",
	})

	if reply.Thread != original.Thread {
		t.Errorf("reply Thread = %q, want %q (same as original)", reply.Thread, original.Thread)
	}
	if reply.ReplyTo != original.ID {
		t.Errorf("reply ReplyTo = %q, want %q", reply.ReplyTo, original.ID)
	}
	if reply.Project != original.Project {
		t.Errorf("reply Project = %q, want %q", reply.Project, original.Project)
	}
}

func TestActionReplyViaNewReply(t *testing.T) {
	original := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "Retry?",
		Actions:  []Action{{ID: "retry", Label: "Retry"}},
	})

	// Action replies use NewReply with ActionResponse set directly — no separate constructor.
	reply := NewReply(original, ReplyOpts{From: "endpoint-xyz789", FromName: "dan-iphone", Body: "retry"})
	reply.ActionResponse = true

	if reply.Body != "retry" {
		t.Errorf("Body = %q, want %q", reply.Body, "retry")
	}
	if !reply.ActionResponse {
		t.Error("ActionResponse should be true")
	}
	if reply.Thread != original.Thread {
		t.Errorf("Thread = %q, want %q", reply.Thread, original.Thread)
	}
}

func TestMessageJSON(t *testing.T) {
	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "hello",
	})

	data, err := json.Marshal(msg)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}

	var decoded Message
	if err := json.Unmarshal(data, &decoded); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}

	if decoded.ID != msg.ID {
		t.Errorf("roundtrip ID = %q, want %q", decoded.ID, msg.ID)
	}
	if decoded.Body != msg.Body {
		t.Errorf("roundtrip Body = %q, want %q", decoded.Body, msg.Body)
	}
}

func TestMessageDefaultPriority(t *testing.T) {
	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "FYI: build done",
	})

	if msg.Priority != PriorityInfo {
		t.Errorf("default Priority = %q, want %q", msg.Priority, PriorityInfo)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1`
Expected: FAIL — package does not exist

- [ ] **Step 3: Write minimal implementation**

Create `leaf/agent/notify/message.go`:

```go
// Package notify implements the EdgeSync bidirectional messaging system.
// Messages are JSON files stored in a Fossil repo and delivered in real-time via NATS.
package notify

import (
	"crypto/rand"
	"fmt"
	"time"
)

// Priority levels for message urgency.
type Priority string

const (
	PriorityInfo           Priority = "info"
	PriorityActionRequired Priority = "action_required"
	PriorityUrgent         Priority = "urgent"
)

// Action represents a quick-response button on a message.
type Action struct {
	ID    string `json:"id"`
	Label string `json:"label"`
}

// Message is a single notification message stored as a JSON file in the notify repo.
type Message struct {
	V              int       `json:"v"`
	ID             string    `json:"id"`
	Thread         string    `json:"thread"`
	Project        string    `json:"project"`
	From           string    `json:"from"`
	FromName       string    `json:"from_name"`
	Timestamp      time.Time `json:"timestamp"`
	Body           string    `json:"body"`
	Priority       Priority  `json:"priority,omitempty"`
	Actions        []Action  `json:"actions,omitempty"`
	ReplyTo        string    `json:"reply_to,omitempty"`
	Media          []string  `json:"media,omitempty"`
	ActionResponse bool      `json:"action_response,omitempty"`
}

// MessageOpts are the caller-provided fields for creating a new message.
type MessageOpts struct {
	Project  string
	From     string
	FromName string
	Body     string
	Priority Priority
	Actions  []Action
	Media    []string
}

// ReplyOpts are the caller-provided fields for creating a reply.
type ReplyOpts struct {
	From     string
	FromName string
	Body     string
	Media    []string
}

// NewMessage creates a new message with a generated ID, thread ID, and timestamp.
func NewMessage(opts MessageOpts) Message {
	pri := opts.Priority
	if pri == "" {
		pri = PriorityInfo
	}
	return Message{
		V:         1,
		ID:        "msg-" + newUUID(),
		Thread:    "thread-" + newUUID(),
		Project:   opts.Project,
		From:      opts.From,
		FromName:  opts.FromName,
		Timestamp: time.Now().UTC(),
		Body:      opts.Body,
		Priority:  pri,
		Actions:   opts.Actions,
		Media:     opts.Media,
	}
}

// NewReply creates a reply to an existing message, preserving the thread.
func NewReply(original Message, opts ReplyOpts) Message {
	return Message{
		V:         1,
		ID:        "msg-" + newUUID(),
		Thread:    original.Thread,
		Project:   original.Project,
		From:      opts.From,
		FromName:  opts.FromName,
		Timestamp: time.Now().UTC(),
		Body:      opts.Body,
		ReplyTo:   original.ID,
		Media:     opts.Media,
	}
}

// No NewActionReply — callers use NewReply and set ActionResponse = true directly.
// This avoids a shallow method that hides almost nothing (Ousterhout: deep modules).

// FilePath returns the repo-relative path for this message file.
// Format: <project>/threads/<thread-short>/<unix-timestamp>-<msg-short>.json
func (m Message) FilePath() string {
	threadShort := shortID(m.Thread, "thread-")
	msgShort := shortID(m.ID, "msg-")
	return fmt.Sprintf("%s/threads/%s/%d-%s.json", m.Project, threadShort, m.Timestamp.Unix(), msgShort)
}

// ThreadShort returns the first 8 characters of the thread UUID (after "thread-" prefix).
func (m Message) ThreadShort() string {
	return shortID(m.Thread, "thread-")
}

// NATSSubject returns the NATS subject for this message.
// Format: notify.<project>.<thread-short>
func (m Message) NATSSubject() string {
	return "notify." + m.Project + "." + m.ThreadShort()
}

// shortID strips a prefix and returns the first 8 characters.
func shortID(id, prefix string) string {
	s := id
	if len(prefix) > 0 && len(s) > len(prefix) {
		s = s[len(prefix):]
	}
	if len(s) > 8 {
		s = s[:8]
	}
	return s
}

// newUUID generates a random UUID (v4-like, 32 hex chars).
func newUUID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return fmt.Sprintf("%x", b)
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1`
Expected: PASS (all 5 tests)

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/message.go leaf/agent/notify/message_test.go
git commit -m "feat(notify): add message struct, serialization, and path generation"
```

---

## Task 2: File Path & NATS Subject Generation

**Files:**
- Modify: `leaf/agent/notify/message_test.go`
- Modify: `leaf/agent/notify/message.go` (already implemented, just verifying)

- [ ] **Step 1: Write the failing tests for path and subject generation**

Append to `leaf/agent/notify/message_test.go`:

```go
func TestMessageFilePath(t *testing.T) {
	msg := Message{
		Project:   "edgesync",
		Thread:    "thread-a1b2c3d4e5f6a7b8",
		ID:        "msg-f9e8d7c6b5a4f3e2",
		Timestamp: time.Date(2026, 4, 10, 12, 0, 0, 0, time.UTC),
	}

	got := msg.FilePath()
	want := "edgesync/threads/a1b2c3d4/1775908800-f9e8d7c6.json"
	if got != want {
		t.Errorf("FilePath() = %q, want %q", got, want)
	}
}

func TestMessageNATSSubject(t *testing.T) {
	msg := Message{
		Project: "edgesync",
		Thread:  "thread-a1b2c3d4e5f6a7b8",
	}

	got := msg.NATSSubject()
	want := "notify.edgesync.a1b2c3d4"
	if got != want {
		t.Errorf("NATSSubject() = %q, want %q", got, want)
	}
}

func TestThreadShort(t *testing.T) {
	msg := Message{Thread: "thread-abcdef1234567890"}
	got := msg.ThreadShort()
	want := "abcdef12"
	if got != want {
		t.Errorf("ThreadShort() = %q, want %q", got, want)
	}
}
```

- [ ] **Step 2: Run tests to verify they pass (these exercise already-written code)**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestMessage(FilePath|NATSSubject)|TestThreadShort"`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add leaf/agent/notify/message_test.go
git commit -m "test(notify): add file path and NATS subject generation tests"
```

---

## Task 3: Store Operations — Init & Commit Message

**Files:**
- Create: `leaf/agent/notify/store.go`
- Create: `leaf/agent/notify/store_test.go`

- [ ] **Step 1: Write the failing test for store init and commit**

Create `leaf/agent/notify/store_test.go`:

```go
package notify

import (
	"path/filepath"
	"testing"

	libfossil "github.com/danmestas/go-libfossil"
	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
	"github.com/danmestas/go-libfossil/simio"
)

// createTestRepo creates a notify.fossil repo in a temp dir for testing.
func createTestRepo(t *testing.T) *libfossil.Repo {
	t.Helper()
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "notify.fossil")
	r, err := InitNotifyRepo(repoPath)
	if err != nil {
		t.Fatalf("InitNotifyRepo: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestInitAndCommitMessage(t *testing.T) {
	r := createTestRepo(t)

	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc123",
		FromName: "claude-macbook",
		Body:     "hello world",
	})

	if err := CommitMessage(r, msg); err != nil {
		t.Fatalf("CommitMessage: %v", err)
	}

	// Verify the file exists in the repo by reading it back.
	got, err := ReadMessage(r, msg.FilePath())
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if got.ID != msg.ID {
		t.Errorf("read back ID = %q, want %q", got.ID, msg.ID)
	}
	if got.Body != "hello world" {
		t.Errorf("read back Body = %q, want %q", got.Body, "hello world")
	}
}

func TestOpenExistingRepo(t *testing.T) {
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "notify.fossil")

	// Create and close.
	r, err := InitNotifyRepo(repoPath)
	if err != nil {
		t.Fatalf("InitNotifyRepo: %v", err)
	}
	r.Close()

	// Reopen via standard libfossil.Open.
	r, err = libfossil.Open(repoPath)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer r.Close()
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestInit|TestOpen"`
Expected: FAIL — `InitNotifyRepo` undefined

- [ ] **Step 3: Write minimal implementation**

Create `leaf/agent/notify/store.go`:

```go
package notify

import (
	"encoding/json"
	"fmt"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/simio"
)

// InitNotifyRepo creates a new notify.fossil repo at the given path.
// Returns the opened *libfossil.Repo — caller owns it and must Close() it.
func InitNotifyRepo(path string) (*libfossil.Repo, error) {
	r, err := libfossil.Create(path, libfossil.CreateOpts{
		ProjectName: "edgesync-notify",
		CryptoRand:  simio.CryptoRand{},
	})
	if err != nil {
		return nil, fmt.Errorf("notify: create repo: %w", err)
	}
	return r, nil
}

// CommitMessage serializes a message to JSON and commits it to the repo.
func CommitMessage(r *libfossil.Repo, msg Message) error {
	data, err := json.MarshalIndent(msg, "", "  ")
	if err != nil {
		return fmt.Errorf("notify: marshal message: %w", err)
	}

	path := msg.FilePath()
	if err := r.WriteFile(path, data); err != nil {
		return fmt.Errorf("notify: write file %s: %w", path, err)
	}

	_, err = r.Commit(libfossil.CommitOpts{
		Comment: fmt.Sprintf("notify: %s", msg.ID),
	})
	if err != nil {
		return fmt.Errorf("notify: commit: %w", err)
	}

	return nil
}

// ReadMessage reads and deserializes a message from the repo by its file path.
func ReadMessage(r *libfossil.Repo, filePath string) (Message, error) {
	data, err := r.ReadFile(filePath)
	if err != nil {
		return Message{}, fmt.Errorf("notify: read file %s: %w", filePath, err)
	}

	var msg Message
	if err := json.Unmarshal(data, &msg); err != nil {
		return Message{}, fmt.Errorf("notify: unmarshal %s: %w", filePath, err)
	}

	return msg, nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestInit|TestOpen"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/store.go leaf/agent/notify/store_test.go
git commit -m "feat(notify): add store functions — init, commit, read (no wrapper type)"
```

---

## Task 4: Store Operations — List Threads & Read Thread History

**Files:**
- Modify: `leaf/agent/notify/store.go`
- Modify: `leaf/agent/notify/store_test.go`

- [ ] **Step 1: Write the failing tests for thread listing and history**

Append to `leaf/agent/notify/store_test.go`:

```go
func TestListThreads(t *testing.T) {
	r := createTestRepo(t)

	// Create messages in two threads.
	msg1 := NewMessage(MessageOpts{
		Project: "edgesync", From: "a", FromName: "alice", Body: "thread 1",
		Priority: PriorityUrgent,
	})
	msg2 := NewMessage(MessageOpts{
		Project: "edgesync", From: "b", FromName: "bob", Body: "thread 2",
	})
	// A reply in thread 1.
	reply1 := NewReply(msg1, ReplyOpts{From: "c", FromName: "charlie", Body: "reply"})

	for _, m := range []Message{msg1, msg2, reply1} {
		if err := CommitMessage(r, m); err != nil {
			t.Fatalf("CommitMessage(%s): %v", m.ID, err)
		}
	}

	threads, err := ListThreads(r, "edgesync")
	if err != nil {
		t.Fatalf("ListThreads: %v", err)
	}

	if len(threads) != 2 {
		t.Fatalf("ListThreads len = %d, want 2", len(threads))
	}

	// Threads should be sorted by last activity (most recent first).
	// reply1 is newer than msg2 because it was committed after.
	if threads[0].ThreadShort != msg1.ThreadShort() {
		t.Errorf("first thread = %q, want %q (has most recent activity)", threads[0].ThreadShort, msg1.ThreadShort())
	}
}

func TestReadThread(t *testing.T) {
	r := createTestRepo(t)

	msg := NewMessage(MessageOpts{
		Project: "edgesync", From: "a", FromName: "alice", Body: "first",
	})
	reply := NewReply(msg, ReplyOpts{From: "b", FromName: "bob", Body: "second"})

	for _, m := range []Message{msg, reply} {
		if err := CommitMessage(r, m); err != nil {
			t.Fatalf("CommitMessage: %v", err)
		}
	}

	messages, err := ReadThread(r, "edgesync", msg.ThreadShort())
	if err != nil {
		t.Fatalf("ReadThread: %v", err)
	}

	if len(messages) != 2 {
		t.Fatalf("ReadThread len = %d, want 2", len(messages))
	}

	// Messages sorted by timestamp (oldest first).
	if messages[0].Body != "first" {
		t.Errorf("first message Body = %q, want %q", messages[0].Body, "first")
	}
	if messages[1].Body != "second" {
		t.Errorf("second message Body = %q, want %q", messages[1].Body, "second")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestList|TestReadThread"`
Expected: FAIL — `ListThreads` undefined

- [ ] **Step 3: Write the implementation**

Add to `leaf/agent/notify/store.go`:

```go
import (
	"sort"
	"strings"
	"time"
)

// ThreadSummary is a summary of a thread for the thread list.
type ThreadSummary struct {
	ThreadShort  string
	Project      string
	LastActivity time.Time
	MessageCount int
	LastMessage  Message
	Priority     Priority
}

// ListThreads returns all threads for a project, sorted by last activity (most recent first).
func ListThreads(r *libfossil.Repo, project string) ([]ThreadSummary, error) {
	prefix := project + "/threads/"
	files, err := r.ListFiles()
	if err != nil {
		return nil, fmt.Errorf("notify: list files: %w", err)
	}

	// Group files by thread directory.
	threadFiles := make(map[string][]string)
	for _, f := range files {
		if !strings.HasPrefix(f, prefix) {
			continue
		}
		rel := strings.TrimPrefix(f, prefix)
		parts := strings.SplitN(rel, "/", 2)
		if len(parts) != 2 {
			continue
		}
		threadID := parts[0]
		threadFiles[threadID] = append(threadFiles[threadID], f)
	}

	var summaries []ThreadSummary
	for threadID, files := range threadFiles {
		sort.Strings(files) // lexicographic = chronological (timestamp prefix)
		lastFile := files[len(files)-1]
		lastMsg, err := ReadMessage(r, lastFile)
		if err != nil {
			continue // skip unreadable threads
		}

		// Thread priority = highest priority of any message.
		highestPri := PriorityInfo
		for _, f := range files {
			m, err := ReadMessage(r, f)
			if err != nil {
				continue
			}
			if priorityRank(m.Priority) > priorityRank(highestPri) {
				highestPri = m.Priority
			}
		}

		summaries = append(summaries, ThreadSummary{
			ThreadShort:  threadID,
			Project:      project,
			LastActivity: lastMsg.Timestamp,
			MessageCount: len(files),
			LastMessage:  lastMsg,
			Priority:     highestPri,
		})
	}

	// Sort by last activity, most recent first.
	sort.Slice(summaries, func(i, j int) bool {
		return summaries[i].LastActivity.After(summaries[j].LastActivity)
	})

	return summaries, nil
}

// ReadThread returns all messages in a thread, sorted by timestamp (oldest first).
func ReadThread(r *libfossil.Repo, project, threadShort string) ([]Message, error) {
	prefix := project + "/threads/" + threadShort + "/"
	files, err := repo.r.ListFiles()
	if err != nil {
		return nil, fmt.Errorf("notify: list files: %w", err)
	}

	var threadFiles []string
	for _, f := range files {
		if strings.HasPrefix(f, prefix) {
			threadFiles = append(threadFiles, f)
		}
	}

	sort.Strings(threadFiles) // lexicographic = chronological

	var messages []Message
	for _, f := range threadFiles {
		msg, err := ReadMessage(r, f)
		if err != nil {
			continue
		}
		messages = append(messages, msg)
	}

	return messages, nil
}

// priorityRank returns a numeric rank for sorting (higher = more urgent).
func priorityRank(p Priority) int {
	switch p {
	case PriorityUrgent:
		return 2
	case PriorityActionRequired:
		return 1
	default:
		return 0
	}
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestList|TestReadThread"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/store.go leaf/agent/notify/store_test.go
git commit -m "feat(notify): add thread listing and history reading (free functions)"
```

---

## Task 5: NATS Pub/Sub — Publish & Subscribe

**Files:**
- Create: `leaf/agent/notify/pubsub.go`
- Create: `leaf/agent/notify/pubsub_test.go`

- [ ] **Step 1: Write the failing test for NATS publish and subscribe**

Create `leaf/agent/notify/pubsub_test.go`:

```go
package notify

import (
	"encoding/json"
	"sync"
	"testing"
	"time"

	"github.com/nats-io/nats.go"
	natsserver "github.com/nats-io/nats-server/v2/server"
)

func startTestNATS(t *testing.T) string {
	t.Helper()
	opts := &natsserver.Options{Port: -1}
	ns, err := natsserver.NewServer(opts)
	if err != nil {
		t.Fatalf("nats-server: %v", err)
	}
	ns.Start()
	if !ns.ReadyForConnections(5 * time.Second) {
		t.Fatal("nats-server not ready")
	}
	t.Cleanup(func() { ns.Shutdown() })
	return ns.ClientURL()
}

func TestPublishAndSubscribe(t *testing.T) {
	url := startTestNATS(t)

	pubConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("pub connect: %v", err)
	}
	defer pubConn.Close()

	subConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("sub connect: %v", err)
	}
	defer subConn.Close()

	sub := NewSubscriber(subConn)

	var received Message
	var mu sync.Mutex
	done := make(chan struct{})

	err = sub.Subscribe("edgesync", func(msg Message) {
		mu.Lock()
		defer mu.Unlock()
		received = msg
		close(done)
	})
	if err != nil {
		t.Fatalf("Subscribe: %v", err)
	}

	msg := NewMessage(MessageOpts{
		Project:  "edgesync",
		From:     "endpoint-abc",
		FromName: "claude",
		Body:     "test message",
	})

	if err := Publish(pubConn, msg); err != nil {
		t.Fatalf("Publish: %v", err)
	}

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("timeout waiting for message")
	}

	mu.Lock()
	defer mu.Unlock()
	if received.ID != msg.ID {
		t.Errorf("received ID = %q, want %q", received.ID, msg.ID)
	}
	if received.Body != "test message" {
		t.Errorf("received Body = %q, want %q", received.Body, "test message")
	}
}

func TestSubscribeWildcard(t *testing.T) {
	url := startTestNATS(t)

	pubConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("pub connect: %v", err)
	}
	defer pubConn.Close()

	subConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("sub connect: %v", err)
	}
	defer subConn.Close()

	sub := NewSubscriber(subConn)

	var messages []Message
	var mu sync.Mutex
	done := make(chan struct{}, 2)

	// Subscribe to all threads in "edgesync".
	err = sub.Subscribe("edgesync", func(msg Message) {
		mu.Lock()
		defer mu.Unlock()
		messages = append(messages, msg)
		done <- struct{}{}
	})
	if err != nil {
		t.Fatalf("Subscribe: %v", err)
	}

	// Publish to two different threads.
	msg1 := NewMessage(MessageOpts{Project: "edgesync", From: "a", FromName: "a", Body: "one"})
	msg2 := NewMessage(MessageOpts{Project: "edgesync", From: "b", FromName: "b", Body: "two"})

	if err := Publish(pubConn, msg1); err != nil {
		t.Fatalf("Publish msg1: %v", err)
	}
	if err := Publish(pubConn, msg2); err != nil {
		t.Fatalf("Publish msg2: %v", err)
	}

	for i := 0; i < 2; i++ {
		select {
		case <-done:
		case <-time.After(5 * time.Second):
			t.Fatalf("timeout waiting for message %d", i+1)
		}
	}

	mu.Lock()
	defer mu.Unlock()
	if len(messages) != 2 {
		t.Errorf("received %d messages, want 2", len(messages))
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestPublish|TestSubscribe"`
Expected: FAIL — `Publish` undefined

- [ ] **Step 3: Write minimal implementation**

Create `leaf/agent/notify/pubsub.go`:

```go
package notify

import (
	"encoding/json"
	"fmt"

	"github.com/nats-io/nats.go"
)

// Publish sends a message to its NATS subject. Free function — no Publisher type needed
// for a single operation (Ousterhout: one method doesn't justify a type).
func Publish(conn *nats.Conn, msg Message) error {
	data, err := json.Marshal(msg)
	if err != nil {
		return fmt.Errorf("notify: marshal for publish: %w", err)
	}

	subject := msg.NATSSubject()
	if err := conn.Publish(subject, data); err != nil {
		return fmt.Errorf("notify: publish to %s: %w", subject, err)
	}

	return conn.Flush()
}

// Subscriber receives messages from NATS subjects.
type Subscriber struct {
	conn *nats.Conn
	subs []*nats.Subscription
}

// NewSubscriber creates a Subscriber for the given NATS connection.
func NewSubscriber(conn *nats.Conn) *Subscriber {
	return &Subscriber{conn: conn}
}

// Subscribe listens for all notify messages in a project (wildcard subscription).
// The callback is invoked for each received message.
func (s *Subscriber) Subscribe(project string, cb func(Message)) error {
	subject := "notify." + project + ".*"
	sub, err := s.conn.Subscribe(subject, func(natsMsg *nats.Msg) {
		var msg Message
		if err := json.Unmarshal(natsMsg.Data, &msg); err != nil {
			return // skip malformed messages
		}
		cb(msg)
	})
	if err != nil {
		return fmt.Errorf("notify: subscribe %s: %w", subject, err)
	}
	s.subs = append(s.subs, sub)
	return s.conn.Flush()
}

// SubscribeThread listens for messages in a specific thread.
func (s *Subscriber) SubscribeThread(project, threadShort string, cb func(Message)) error {
	subject := "notify." + project + "." + threadShort
	sub, err := s.conn.Subscribe(subject, func(natsMsg *nats.Msg) {
		var msg Message
		if err := json.Unmarshal(natsMsg.Data, &msg); err != nil {
			return
		}
		cb(msg)
	})
	if err != nil {
		return fmt.Errorf("notify: subscribe %s: %w", subject, err)
	}
	s.subs = append(s.subs, sub)
	return s.conn.Flush()
}

// Unsubscribe removes all active subscriptions.
func (s *Subscriber) Unsubscribe() {
	for _, sub := range s.subs {
		sub.Unsubscribe()
	}
	s.subs = nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestPublish|TestSubscribe"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/pubsub.go leaf/agent/notify/pubsub_test.go
git commit -m "feat(notify): add NATS publish and subscribe"
```

---

## Task 6: Notify Service — Send & Watch

**Files:**
- Create: `leaf/agent/notify/notify.go`
- Create: `leaf/agent/notify/notify_test.go`

- [ ] **Step 1: Write the failing test for the Service**

Create `leaf/agent/notify/notify_test.go`:

```go
package notify

import (
	"context"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"

	_ "github.com/danmestas/go-libfossil/db/driver/modernc"
	"github.com/nats-io/nats.go"
)

func newTestService(t *testing.T) (*Service, *nats.Conn) {
	t.Helper()
	url := startTestNATS(t)
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "notify.fossil")

	// Create the repo explicitly (init is separate from open).
	r, err := InitNotifyRepo(repoPath)
	if err != nil {
		t.Fatalf("InitNotifyRepo: %v", err)
	}

	conn, err := nats.Connect(url)
	if err != nil {
		r.Close()
		t.Fatalf("nats connect: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	svc, err := NewService(ServiceConfig{
		Repo:     r,
		NATSConn: conn,
		From:     "endpoint-test",
		FromName: "test-agent",
	})
	if err != nil {
		t.Fatalf("NewService: %v", err)
	}
	t.Cleanup(func() { svc.Close() })

	return svc, conn
}

func TestServiceSend(t *testing.T) {
	svc, conn := newTestService(t)

	// Subscribe to verify NATS publish.
	received := make(chan Message, 1)
	sub, err := conn.Subscribe("notify.edgesync.*", func(natsMsg *nats.Msg) {
		var msg Message
		json.Unmarshal(natsMsg.Data, &msg)
		received <- msg
	})
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	defer sub.Unsubscribe()
	conn.Flush()

	msg, err := svc.Send(SendOpts{
		Project: "edgesync",
		Body:    "hello from test",
	})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	if msg.ID == "" {
		t.Error("returned message should have an ID")
	}

	// Verify NATS delivery.
	select {
	case got := <-received:
		if got.ID != msg.ID {
			t.Errorf("NATS received ID = %q, want %q", got.ID, msg.ID)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("timeout waiting for NATS message")
	}

	// Verify repo commit.
	readBack, err := ReadMessage(svc.Repo(), msg.FilePath())
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if readBack.Body != "hello from test" {
		t.Errorf("repo Body = %q, want %q", readBack.Body, "hello from test")
	}
}

func TestServiceSendToExistingThread(t *testing.T) {
	svc, _ := newTestService(t)

	msg1, err := svc.Send(SendOpts{
		Project: "edgesync",
		Body:    "first",
	})
	if err != nil {
		t.Fatalf("Send 1: %v", err)
	}

	msg2, err := svc.Send(SendOpts{
		Project:     "edgesync",
		Body:        "second",
		ThreadShort: msg1.ThreadShort(),
	})
	if err != nil {
		t.Fatalf("Send 2: %v", err)
	}

	if msg2.Thread != msg1.Thread {
		t.Errorf("msg2 thread = %q, want %q (same thread)", msg2.Thread, msg1.Thread)
	}
}

func TestServiceWatch(t *testing.T) {
	svc, conn := newTestService(t)

	// Send a message first to establish the thread.
	msg, err := svc.Send(SendOpts{
		Project: "edgesync",
		Body:    "question?",
	})
	if err != nil {
		t.Fatalf("Send: %v", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	replies := make(chan Message, 1)
	go func() {
		for m := range svc.Watch(ctx, WatchOpts{Project: "edgesync", ThreadShort: msg.ThreadShort()}) {
			replies <- m
			cancel()
		}
	}()

	// Simulate an external reply via NATS.
	time.Sleep(100 * time.Millisecond) // let subscription start
	reply := NewReply(msg, ReplyOpts{From: "other", FromName: "dan", Body: "yes"})
	data, _ := json.Marshal(reply)
	conn.Publish(reply.NATSSubject(), data)
	conn.Flush()

	select {
	case got := <-replies:
		if got.Body != "yes" {
			t.Errorf("watch got Body = %q, want %q", got.Body, "yes")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("timeout waiting for watch reply")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestService"`
Expected: FAIL — `NewService` undefined

- [ ] **Step 3: Write minimal implementation**

Create `leaf/agent/notify/notify.go`:

```go
package notify

import (
	"context"
	"fmt"
	"log/slog"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/nats-io/nats.go"
)

// ServiceConfig holds configuration for the notify Service.
type ServiceConfig struct {
	Repo     *libfossil.Repo // Already-opened notify repo (caller owns lifecycle)
	NATSConn *nats.Conn      // Existing NATS connection (may be nil for repo-only mode)
	From     string          // This peer's iroh endpoint ID
	FromName string          // Human-readable name for this peer
}

// Service is the top-level notify API. Holds *libfossil.Repo and *nats.Conn
// directly — no wrapper types (Ousterhout: no shallow wrappers, no pass-throughs).
type Service struct {
	repo      *libfossil.Repo
	conn      *nats.Conn
	sub       *Subscriber
	config    ServiceConfig
	// threadMap caches thread-short → full thread ID for Send to existing threads.
	threadMap map[string]string
}

// NewService creates a new notify Service. The repo must already exist —
// use InitNotifyRepo to create it first. This separation makes errors explicit
// (Ousterhout: define errors out of existence — don't silently create repos).
func NewService(cfg ServiceConfig) (*Service, error) {
	if cfg.Repo == nil {
		return nil, fmt.Errorf("notify: Repo is required (use InitNotifyRepo or libfossil.Open first)")
	}

	svc := &Service{
		repo:      cfg.Repo,
		conn:      cfg.NATSConn,
		config:    cfg,
		threadMap: make(map[string]string),
	}

	if cfg.NATSConn != nil {
		svc.sub = NewSubscriber(cfg.NATSConn)
	}

	return svc, nil
}

// Close shuts down subscriptions. Does NOT close the repo — caller owns it.
func (s *Service) Close() error {
	if s.sub != nil {
		s.sub.Unsubscribe()
	}
	return nil
}

// Repo returns the underlying libfossil.Repo for direct operations (sync, etc.).
func (s *Service) Repo() *libfossil.Repo {
	return s.repo
}

// SendOpts are options for sending a message.
type SendOpts struct {
	Project     string
	Body        string
	Priority    Priority
	Actions     []Action
	Media       []string
	ThreadShort string // If set, send to existing thread instead of creating new one.
}

// Send creates a message, commits it to the repo, and publishes it via NATS.
func (s *Service) Send(opts SendOpts) (Message, error) {
	var msg Message

	if opts.ThreadShort != "" {
		// Find the full thread ID from cache or by scanning the repo.
		fullThread, err := s.resolveThread(opts.Project, opts.ThreadShort)
		if err != nil {
			return Message{}, err
		}
		// Create a message with the existing thread ID.
		msg = NewMessage(MessageOpts{
			Project:  opts.Project,
			From:     s.config.From,
			FromName: s.config.FromName,
			Body:     opts.Body,
			Priority: opts.Priority,
			Actions:  opts.Actions,
			Media:    opts.Media,
		})
		msg.Thread = fullThread
	} else {
		msg = NewMessage(MessageOpts{
			Project:  opts.Project,
			From:     s.config.From,
			FromName: s.config.FromName,
			Body:     opts.Body,
			Priority: opts.Priority,
			Actions:  opts.Actions,
			Media:    opts.Media,
		})
	}

	// Cache the thread mapping.
	s.threadMap[msg.ThreadShort()] = msg.Thread

	// Commit to repo.
	if err := CommitMessage(s.repo, msg); err != nil {
		return msg, fmt.Errorf("notify: send commit: %w", err)
	}

	// Publish to NATS (best-effort — if NATS is down, repo still has the message).
	if s.conn != nil {
		if err := Publish(s.conn, msg); err != nil {
			slog.Warn("notify: NATS publish failed (message committed to repo)", "error", err, "msg_id", msg.ID)
		}
	}

	return msg, nil
}

// WatchOpts are options for watching messages.
type WatchOpts struct {
	Project     string
	ThreadShort string // If empty, watch all threads in the project.
}

// Watch returns a channel of messages received via NATS subscription.
// The channel is closed when the context is cancelled.
func (s *Service) Watch(ctx context.Context, opts WatchOpts) <-chan Message {
	ch := make(chan Message, 16)

	if s.sub == nil {
		close(ch)
		return ch
	}

	cb := func(msg Message) {
		select {
		case ch <- msg:
		case <-ctx.Done():
		}
	}

	var err error
	if opts.ThreadShort != "" {
		err = s.sub.SubscribeThread(opts.Project, opts.ThreadShort, cb)
	} else {
		err = s.sub.Subscribe(opts.Project, cb)
	}
	if err != nil {
		slog.Error("notify: watch subscribe failed", "error", err)
		close(ch)
		return ch
	}

	go func() {
		<-ctx.Done()
		s.sub.Unsubscribe()
		close(ch)
	}()

	return ch
}

// resolveThread finds the full thread ID from a short ID.
func (s *Service) resolveThread(project, threadShort string) (string, error) {
	if full, ok := s.threadMap[threadShort]; ok {
		return full, nil
	}

	// Scan the repo for a message in this thread.
	messages, err := ReadThread(s.repo, project, threadShort)
	if err != nil {
		return "", err
	}
	if len(messages) == 0 {
		return "", fmt.Errorf("notify: thread %q not found in project %q", threadShort, project)
	}

	full := messages[0].Thread
	s.threadMap[threadShort] = full
	return full, nil
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestService"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/notify.go leaf/agent/notify/notify_test.go
git commit -m "feat(notify): add Service with send, watch, thread listing"
```

---

## Task 7: Watch Output Formatter

**Files:**
- Modify: `leaf/agent/notify/notify.go`
- Modify: `leaf/agent/notify/notify_test.go`

- [ ] **Step 1: Write the failing test for FormatWatchLine**

Append to `leaf/agent/notify/notify_test.go`:

```go
func TestFormatWatchLine(t *testing.T) {
	tests := []struct {
		name string
		msg  Message
		want string
	}{
		{
			name: "action response",
			msg: Message{
				Timestamp:      time.Date(2026, 4, 10, 12, 1, 3, 0, time.UTC),
				Thread:         "thread-a1b2c3d4e5f6a7b8",
				FromName:       "dan-iphone",
				Body:           "retry",
				ActionResponse: true,
			},
			want: "[2026-04-10T12:01:03Z] thread:a1b2c3d4 from:dan-iphone action:retry",
		},
		{
			name: "text reply",
			msg: Message{
				Timestamp: time.Date(2026, 4, 10, 12, 1, 15, 0, time.UTC),
				Thread:    "thread-a1b2c3d4e5f6a7b8",
				FromName:  "dan-iphone",
				Body:      "also bump the version",
			},
			want: "[2026-04-10T12:01:15Z] thread:a1b2c3d4 from:dan-iphone text:also bump the version",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := FormatWatchLine(tt.msg)
			if got != tt.want {
				t.Errorf("FormatWatchLine() = %q, want %q", got, tt.want)
			}
		})
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestFormatWatchLine"`
Expected: FAIL — `FormatWatchLine` undefined

- [ ] **Step 3: Write minimal implementation**

Add to `leaf/agent/notify/notify.go`:

```go
// FormatWatchLine formats a message as a structured watch output line.
// Format: [<timestamp>] thread:<short> from:<name> action:<id>|text:<body>
func FormatWatchLine(msg Message) string {
	ts := msg.Timestamp.UTC().Format(time.RFC3339)
	threadShort := shortID(msg.Thread, "thread-")

	if msg.ActionResponse {
		return fmt.Sprintf("[%s] thread:%s from:%s action:%s", ts, threadShort, msg.FromName, msg.Body)
	}
	return fmt.Sprintf("[%s] thread:%s from:%s text:%s", ts, threadShort, msg.FromName, msg.Body)
}
```

Also add `"time"` to the imports in `notify.go` if not already present.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestFormatWatchLine"`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/notify.go leaf/agent/notify/notify_test.go
git commit -m "feat(notify): add watch output formatter"
```

---

## Task 8: Deduplication

**Files:**
- Modify: `leaf/agent/notify/pubsub.go`
- Modify: `leaf/agent/notify/pubsub_test.go`

- [ ] **Step 1: Write the failing test for deduplication**

Append to `leaf/agent/notify/pubsub_test.go`:

```go
func TestSubscribeDedup(t *testing.T) {
	url := startTestNATS(t)

	pubConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer pubConn.Close()

	subConn, err := nats.Connect(url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer subConn.Close()

	sub := NewSubscriber(subConn)
	sub.EnableDedup()

	var count int
	var mu sync.Mutex
	done := make(chan struct{}, 3)

	err = sub.Subscribe("edgesync", func(msg Message) {
		mu.Lock()
		defer mu.Unlock()
		count++
		done <- struct{}{}
	})
	if err != nil {
		t.Fatalf("Subscribe: %v", err)
	}

	msg := NewMessage(MessageOpts{
		Project: "edgesync", From: "a", FromName: "a", Body: "hello",
	})

	// Publish the same message three times.
	for i := 0; i < 3; i++ {
		if err := Publish(pubConn, msg); err != nil {
			t.Fatalf("Publish: %v", err)
		}
	}

	// Wait for at least one delivery.
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("timeout")
	}

	// Short sleep to let any duplicate deliveries arrive.
	time.Sleep(200 * time.Millisecond)

	mu.Lock()
	defer mu.Unlock()
	if count != 1 {
		t.Errorf("callback invoked %d times, want 1 (dedup should filter duplicates)", count)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestSubscribeDedup"`
Expected: FAIL — `EnableDedup` undefined

- [ ] **Step 3: Write minimal implementation**

Add dedup to `leaf/agent/notify/pubsub.go`. Add a `seen` map and `dedup` flag to `Subscriber`:

```go
// Add to Subscriber struct:
//   seen  map[string]struct{}
//   dedup bool
//   mu    sync.Mutex

// EnableDedup turns on message ID deduplication for this subscriber.
func (s *Subscriber) EnableDedup() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.dedup = true
	s.seen = make(map[string]struct{})
}

// isDuplicate checks if a message ID has been seen before. Returns true if duplicate.
func (s *Subscriber) isDuplicate(id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.dedup {
		return false
	}
	if _, ok := s.seen[id]; ok {
		return true
	}
	s.seen[id] = struct{}{}
	return false
}
```

Update the `Subscribe` and `SubscribeThread` methods to call `s.isDuplicate(msg.ID)` before invoking the callback:

```go
// In Subscribe's callback:
sub, err := s.conn.Subscribe(subject, func(natsMsg *nats.Msg) {
    var msg Message
    if err := json.Unmarshal(natsMsg.Data, &msg); err != nil {
        return
    }
    if s.isDuplicate(msg.ID) {
        return
    }
    cb(msg)
})

// Same change in SubscribeThread's callback.
```

Add `"sync"` to imports.

Also update `NewSubscriber` to initialize the mutex (it's zero-value safe, but update the struct definition):

```go
type Subscriber struct {
	conn  *nats.Conn
	subs  []*nats.Subscription
	seen  map[string]struct{}
	dedup bool
	mu    sync.Mutex
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1 -run "TestSubscribe"`
Expected: PASS (both TestSubscribeWildcard and TestSubscribeDedup)

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/notify/pubsub.go leaf/agent/notify/pubsub_test.go
git commit -m "feat(notify): add message deduplication to subscriber"
```

---

## Task 9: CLI Commands — Kong Wiring

**Files:**
- Create: `cmd/edgesync/notify.go`
- Modify: `cmd/edgesync/cli.go`

- [ ] **Step 1: Write the CLI command structs and Run methods**

Create `cmd/edgesync/notify.go`:

```go
package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/cli"
	"github.com/dmestas/edgesync/leaf/agent/notify"
)

type NotifyCmd struct {
	Init    NotifyInitCmd    `cmd:"" help:"Initialize the notify repo"`
	Send    NotifySendCmd    `cmd:"" help:"Send a notification message"`
	Ask     NotifyAskCmd     `cmd:"" help:"Send a message and wait for a reply"`
	Watch   NotifyWatchCmd   `cmd:"" help:"Watch for incoming messages"`
	Threads NotifyThreadsCmd `cmd:"" help:"List active threads"`
	Log     NotifyLogCmd     `cmd:"" help:"Show thread message history"`
	Status  NotifyStatusCmd  `cmd:"" help:"Show connection state and unread counts"`
}

// notifyRepoPath returns the path to notify.fossil, defaulting to <repo-dir>/notify.fossil.
func notifyRepoPath(g *cli.Globals) string {
	if g.Repo != "" {
		return filepath.Join(filepath.Dir(g.Repo), "notify.fossil")
	}
	return "notify.fossil"
}

// openNotifyRepo opens an existing notify.fossil, returning a clear error if it doesn't exist.
func openNotifyRepo(g *cli.Globals) (*libfossil.Repo, error) {
	path := notifyRepoPath(g)
	r, err := libfossil.Open(path)
	if err != nil {
		return nil, fmt.Errorf("notify repo not found at %s (run 'edgesync notify init' first): %w", path, err)
	}
	return r, nil
}

// --- init ---

type NotifyInitCmd struct{}

func (c *NotifyInitCmd) Run(g *cli.Globals) error {
	path := notifyRepoPath(g)
	r, err := notify.InitNotifyRepo(path)
	if err != nil {
		return err
	}
	r.Close()
	fmt.Printf("Initialized notify repo at %s\n", path)
	return nil
}

// --- send ---

type NotifySendCmd struct {
	Project   string `help:"Project name" required:""`
	Body      string `arg:"" help:"Message body"`
	Thread    string `help:"Existing thread short ID (creates new if omitted)"`
	NewThread bool   `help:"Force create a new thread" xor:"thread"`
	Actions   string `help:"Comma-separated quick actions (e.g. 'Retry,Skip')"`
	Priority  string `help:"Priority: info, action_required, urgent" default:"info" enum:"info,action_required,urgent"`
}

func (c *NotifySendCmd) Run(g *cli.Globals) error {
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	svc, err := notify.NewService(notify.ServiceConfig{
		Repo:     r,
		From:     "cli", // Will be replaced by iroh endpoint ID when agent is running
		FromName: hostname(),
	})
	if err != nil {
		return err
	}
	defer svc.Close()

	opts := notify.SendOpts{
		Project:  c.Project,
		Body:     c.Body,
		Priority: notify.Priority(c.Priority),
		Actions:  parseActions(c.Actions),
	}
	if c.Thread != "" {
		opts.ThreadShort = c.Thread
	}

	msg, err := svc.Send(opts)
	if err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "thread:%s\n", msg.ThreadShort())
	fmt.Printf("sent %s to thread %s\n", msg.ID, msg.ThreadShort())
	return nil
}

// --- ask ---

type NotifyAskCmd struct {
	Project  string        `help:"Project name" required:""`
	Body     string        `arg:"" help:"Message body"`
	Actions  string        `help:"Comma-separated quick actions"`
	Priority string        `help:"Priority: info, action_required, urgent" default:"action_required" enum:"info,action_required,urgent"`
	Timeout  time.Duration `help:"Timeout waiting for reply (0 = forever)" default:"0"`
}

func (c *NotifyAskCmd) Run(g *cli.Globals) error {
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	svc, err := notify.NewService(notify.ServiceConfig{
		Repo:     r,
		From:     "cli",
		FromName: hostname(),
	})
	if err != nil {
		return err
	}
	defer svc.Close()

	msg, err := svc.Send(notify.SendOpts{
		Project:  c.Project,
		Body:     c.Body,
		Priority: notify.Priority(c.Priority),
		Actions:  parseActions(c.Actions),
	})
	if err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "thread:%s\n", msg.ThreadShort())

	ctx := context.Background()
	if c.Timeout > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, c.Timeout)
		defer cancel()
	}

	// Also handle SIGINT/SIGTERM.
	ctx, stop := signal.NotifyContext(ctx, syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	for reply := range svc.Watch(ctx, notify.WatchOpts{
		Project:     c.Project,
		ThreadShort: msg.ThreadShort(),
	}) {
		fmt.Println(notify.FormatWatchLine(reply))
		return nil // Exit after first reply.
	}

	// Channel closed = context cancelled (timeout or signal).
	if ctx.Err() == context.DeadlineExceeded {
		fmt.Fprintln(os.Stderr, "timeout: no reply received")
		os.Exit(2)
	}
	return nil
}

// --- watch ---

type NotifyWatchCmd struct {
	Project string `help:"Project name" required:""`
	Thread  string `help:"Thread short ID (watches all if omitted)"`
}

func (c *NotifyWatchCmd) Run(g *cli.Globals) error {
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	svc, err := notify.NewService(notify.ServiceConfig{
		Repo:     r,
		From:     "cli",
		FromName: hostname(),
	})
	if err != nil {
		return err
	}
	defer svc.Close()

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	for msg := range svc.Watch(ctx, notify.WatchOpts{
		Project:     c.Project,
		ThreadShort: c.Thread,
	}) {
		fmt.Println(notify.FormatWatchLine(msg))
	}
	return nil
}

// --- threads ---

type NotifyThreadsCmd struct {
	Project string `help:"Project name" required:""`
}

func (c *NotifyThreadsCmd) Run(g *cli.Globals) error {
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	threads, err := notify.ListThreads(r, c.Project)
	if err != nil {
		return err
	}

	if len(threads) == 0 {
		fmt.Println("no threads")
		return nil
	}

	for _, th := range threads {
		pri := ""
		if th.Priority != notify.PriorityInfo {
			pri = fmt.Sprintf(" [%s]", th.Priority)
		}
		fmt.Printf("%s  %d msgs  %s  %q%s\n",
			th.ThreadShort,
			th.MessageCount,
			th.LastActivity.Format(time.RFC3339),
			th.LastMessage.Body,
			pri,
		)
	}
	return nil
}

// --- log ---

type NotifyLogCmd struct {
	Project string `help:"Project name" required:""`
	Thread  string `arg:"" help:"Thread short ID"`
}

func (c *NotifyLogCmd) Run(g *cli.Globals) error {
	r, err := openNotifyRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	messages, err := notify.ReadThread(r, c.Project, c.Thread)
	if err != nil {
		return err
	}

	if len(messages) == 0 {
		fmt.Printf("no messages in thread %s\n", c.Thread)
		return nil
	}

	for _, msg := range messages {
		fmt.Println(notify.FormatWatchLine(msg))
	}
	return nil
}

// --- status ---

type NotifyStatusCmd struct{}

func (c *NotifyStatusCmd) Run(g *cli.Globals) error {
	path := notifyRepoPath(g)
	_, err := os.Stat(path)
	if os.IsNotExist(err) {
		fmt.Printf("notify repo: not initialized (run 'edgesync notify init')\n")
		return nil
	}

	fmt.Printf("notify repo: %s\n", path)
	fmt.Printf("nats: not connected (standalone mode)\n")
	return nil
}

// --- helpers ---

func parseActions(s string) []notify.Action {
	if s == "" {
		return nil
	}
	parts := strings.Split(s, ",")
	actions := make([]notify.Action, len(parts))
	for i, p := range parts {
		p = strings.TrimSpace(p)
		actions[i] = notify.Action{
			ID:    strings.ToLower(strings.ReplaceAll(p, " ", "_")),
			Label: p,
		}
	}
	return actions
}

func hostname() string {
	h, err := os.Hostname()
	if err != nil {
		return "unknown"
	}
	return h
}
```

- [ ] **Step 2: Wire the NotifyCmd into the CLI struct**

Modify `cmd/edgesync/cli.go`:

```go
type CLI struct {
	cli.Globals

	Repo   cli.RepoCmd `cmd:"" help:"Repository operations"`
	Sync   SyncCmd     `cmd:"" help:"Leaf agent sync"`
	Bridge BridgeCmd   `cmd:"" help:"NATS-to-Fossil bridge"`
	Notify NotifyCmd   `cmd:"" help:"Bidirectional notification messaging"`
	Doctor DoctorCmd   `cmd:"" help:"Check development environment health"`
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cd /Users/dmestas/projects/EdgeSync && go build -buildvcs=false ./cmd/edgesync/`
Expected: Build succeeds

- [ ] **Step 4: Verify CLI help output shows notify commands**

Run: `cd /Users/dmestas/projects/EdgeSync && ./edgesync notify --help`
Expected: Shows init, send, ask, watch, threads, log, status subcommands

- [ ] **Step 5: Commit**

```bash
git add cmd/edgesync/notify.go cmd/edgesync/cli.go
git commit -m "feat(notify): add CLI commands (init, send, ask, watch, threads, log, status)"
```

---

## Task 10: CLI End-to-End Test

**Files:**
- Create: `cmd/edgesync/notify_test.go`

- [ ] **Step 1: Write the end-to-end test**

Create `cmd/edgesync/notify_test.go`:

```go
package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// buildBinary builds the edgesync binary for testing.
func buildBinary(t *testing.T) string {
	t.Helper()
	bin := filepath.Join(t.TempDir(), "edgesync")
	cmd := exec.Command("go", "build", "-buildvcs=false", "-o", bin, "./")
	cmd.Dir = filepath.Join(os.Getenv("PWD"))
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("build failed: %s\n%s", err, out)
	}
	return bin
}

func TestNotifyCLIInit(t *testing.T) {
	bin := buildBinary(t)
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")

	// Create a dummy fossil repo so -R works (notify.fossil is placed next to it).
	cmd := exec.Command(bin, "-R", repoPath, "notify", "init")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("notify init: %s\n%s", err, out)
	}

	notifyPath := filepath.Join(dir, "notify.fossil")
	if _, err := os.Stat(notifyPath); os.IsNotExist(err) {
		t.Fatalf("notify.fossil not created at %s", notifyPath)
	}
	t.Logf("output: %s", out)
}

func TestNotifyCLISendAndThreads(t *testing.T) {
	bin := buildBinary(t)
	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")

	// Init.
	cmd := exec.Command(bin, "-R", repoPath, "notify", "init")
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("init: %s\n%s", err, out)
	}

	// Send.
	cmd = exec.Command(bin, "-R", repoPath, "notify", "send", "--project", "edgesync", "hello world")
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("send: %s\n%s", err, out)
	}
	if !strings.Contains(string(out), "sent msg-") {
		t.Errorf("send output missing message ID: %s", out)
	}

	// Threads.
	cmd = exec.Command(bin, "-R", repoPath, "notify", "threads", "--project", "edgesync")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("threads: %s\n%s", err, out)
	}
	if !strings.Contains(string(out), "1 msgs") {
		t.Errorf("threads output missing message count: %s", out)
	}
	if !strings.Contains(string(out), "hello world") {
		t.Errorf("threads output missing body preview: %s", out)
	}

	// Status.
	cmd = exec.Command(bin, "-R", repoPath, "notify", "status")
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("status: %s\n%s", err, out)
	}
	if !strings.Contains(string(out), "notify repo:") {
		t.Errorf("status output missing repo info: %s", out)
	}
}
```

- [ ] **Step 2: Run tests to verify they pass**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./cmd/edgesync/ -v -count=1 -run "TestNotifyCLI" -timeout 60s`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add cmd/edgesync/notify_test.go
git commit -m "test(notify): add CLI end-to-end tests for init, send, threads, status"
```

---

## Task 11: Run Full Test Suite

- [ ] **Step 1: Run all notify package tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test ./leaf/agent/notify/ -v -count=1`
Expected: All tests PASS

- [ ] **Step 2: Run CLI tests**

Run: `cd /Users/dmestas/projects/EdgeSync && go test -buildvcs=false ./cmd/edgesync/ -v -count=1 -run "TestNotifyCLI" -timeout 60s`
Expected: All tests PASS

- [ ] **Step 3: Run existing tests to verify no regressions**

Run: `cd /Users/dmestas/projects/EdgeSync && make test`
Expected: All existing tests still PASS

- [ ] **Step 4: Final commit if any fixups were needed**

Only if test failures required fixes. Otherwise skip this step.
