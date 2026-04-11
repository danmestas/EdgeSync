# EdgeSync Notify Expo App — Phase 1: Go HTTP Server

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a localhost HTTP + SSE server in Go that wraps `notify.Service` from EdgeSync. Testable entirely from CLI with `curl` — no iOS or React Native needed yet.

**Architecture:** A thin HTTP bridge over `notify.Service`. The server never defines its own message types — all JSON is passed through from `notify.Message` and `notify.ThreadSummary` directly (Ousterhout: no pass-through wrappers). `server.go` owns lifecycle (random port, port file, graceful shutdown). `bridge.go` maps HTTP endpoints to `notify.Service` methods. `sse.go` manages a single SSE client connection with message queuing during disconnects and replay on reconnect.

**Tech Stack:** Go 1.26, `github.com/danmestas/go-libfossil` v0.2.4, `github.com/dmestas/edgesync/leaf` (latest), stdlib `net/http`, `encoding/json`, `net/http/httptest`

**Target repo:** `edgesync-notify-app` (NEW repo, Go module at `go/`)

**Import paths:**
- `github.com/danmestas/go-libfossil` — Fossil repo handle
- `github.com/dmestas/edgesync/leaf/agent/notify` — Service, Message, ThreadSummary, SendOpts, etc.

**No Claude attribution on commits.**

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `go/go.mod` | Module `github.com/danmestas/edgesync-notify-app/go`. Requires go-libfossil v0.2.4, edgesync/leaf latest. |
| `go/server.go` | HTTP server lifecycle: `Start(dataDir string) (int, error)`, `Stop()`, `IsRunning() bool`. Picks random port, writes `dataDir/server.port`, composes mux from bridge handlers. |
| `go/bridge.go` | HTTP handlers wrapping `notify.Service`. Each handler is a method on `Bridge` struct (holds `*notify.Service`). Never redefines `notify.Message` or `notify.ThreadSummary`. |
| `go/sse.go` | SSE streaming: `SSEManager` tracks one client connection, queues messages when disconnected, replays on reconnect. Sends `message`, `connected`, `disconnected` event types. |
| `go/server_test.go` | `httptest`-based tests for all non-SSE endpoints. |
| `go/sse_test.go` | SSE streaming tests: connection, message delivery, disconnect queuing, reconnect replay. |

---

## Task 1: Module Setup and Server Skeleton

**Files:**
- Create: `go/go.mod`
- Create: `go/server.go`
- Create: `go/server_test.go`

- [ ] **Step 1: Initialize the Go module**

Create `go/go.mod`:

```go
module github.com/danmestas/edgesync-notify-app/go

go 1.26.0

require (
	github.com/danmestas/go-libfossil v0.2.4
	github.com/danmestas/go-libfossil/db/driver/modernc v0.2.4
	github.com/dmestas/edgesync/leaf v0.0.0
)
```

Run:
```bash
cd go && GOPRIVATE=github.com/danmestas go mod tidy
```

- [ ] **Step 2: Write failing test for server lifecycle**

Create `go/server_test.go`:

```go
package server

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
)

func TestStartStop(t *testing.T) {
	dir := t.TempDir()

	port, err := Start(dir)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	if port <= 0 {
		t.Fatalf("port = %d, want > 0", port)
	}

	// Port file should exist.
	data, err := os.ReadFile(filepath.Join(dir, "server.port"))
	if err != nil {
		t.Fatalf("read port file: %v", err)
	}
	got, _ := strconv.Atoi(string(data))
	if got != port {
		t.Errorf("port file = %d, want %d", got, port)
	}

	if !IsRunning() {
		t.Error("IsRunning() = false after Start")
	}

	Stop()

	if IsRunning() {
		t.Error("IsRunning() = true after Stop")
	}
}

func TestStartDoubleStartFails(t *testing.T) {
	dir := t.TempDir()

	_, err := Start(dir)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer Stop()

	_, err = Start(dir)
	if err == nil {
		t.Fatal("expected error on double Start")
	}
}
```

Run (expect compile failure):
```bash
cd go && go test -run TestStartStop -count=1 ./...
```

- [ ] **Step 3: Implement server skeleton**

Create `go/server.go` with:

- Package `server`
- `var` block: `mu sync.Mutex`, `srv *http.Server`, `running bool`
- `Start(dataDir string) (int, error)`:
  1. Lock mutex, check not already running
  2. `net.Listen("tcp", "127.0.0.1:0")` for random port
  3. Extract port from `listener.Addr().(*net.TCPAddr).Port`
  4. Write port to `dataDir/server.port` via `os.WriteFile`
  5. Create `http.ServeMux` (handlers added in Task 3)
  6. Create `http.Server{Handler: mux}`
  7. `go srv.Serve(listener)`
  8. Set `running = true`, return port
- `Stop()`: `srv.Shutdown(ctx)`, remove port file, set `running = false`
- `IsRunning() bool`: return `running` under lock

Run:
```bash
cd go && go test -run TestStart -count=1 -v ./...
```

**Commit:** `feat(server): scaffold Go module and server lifecycle with random port`

---

## Task 2: Bridge Handlers — Init, Status, Stop

**Files:**
- Create: `go/bridge.go`
- Modify: `go/server.go` (wire bridge into mux)
- Modify: `go/server_test.go` (add handler tests)

- [ ] **Step 1: Write failing tests for /init, /status, /stop**

Add to `go/server_test.go`:

```go
func TestInitAndStatus(t *testing.T) {
	dir := t.TempDir()
	port, err := Start(dir)
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)

	// Status before init — should report not initialized.
	resp, err := http.Get(base + "/status")
	if err != nil {
		t.Fatalf("GET /status: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET /status status = %d", resp.StatusCode)
	}
	var status map[string]any
	json.NewDecoder(resp.Body).Decode(&status)
	if status["initialized"] != false {
		t.Errorf("expected initialized=false before /init")
	}

	// Init.
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "test-project"}`, dir)
	resp, err = http.Post(base+"/init", "application/json", strings.NewReader(initBody))
	if err != nil {
		t.Fatalf("POST /init: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		t.Fatalf("POST /init status = %d, body = %s", resp.StatusCode, body)
	}

	// Status after init — should report initialized.
	resp, err = http.Get(base + "/status")
	if err != nil {
		t.Fatalf("GET /status after init: %v", err)
	}
	defer resp.Body.Close()
	json.NewDecoder(resp.Body).Decode(&status)
	if status["initialized"] != true {
		t.Errorf("expected initialized=true after /init")
	}
}
```

Run (expect compile failure):
```bash
cd go && go test -run TestInitAndStatus -count=1 ./...
```

- [ ] **Step 2: Implement Bridge struct and handlers**

Create `go/bridge.go`:

- `Bridge` struct: `svc *notify.Service`, `repo *libfossil.Repo`, `project string`, `initialized bool`, `mu sync.Mutex`
- `InitRequest` struct (only for parsing init JSON): `DataDir string`, `Project string`, `NATSUrl string` (optional)
- `handleInit(w, r)`:
  1. Decode `InitRequest` from body
  2. Call `notify.InitNotifyRepo(dataDir + "/notify.fossil")` — creates or opens
  3. Optionally connect NATS (best-effort — if NATS fails, log warning, continue)
  4. Create `notify.NewService(notify.ServiceConfig{Repo: repo})`
  5. Set `initialized = true`
  6. Return `{"ok": true}`
- `handleStatus(w, r)`:
  1. Return JSON: `{"initialized": bool, "nats_connected": bool}`
- `handleStop(w, r)`:
  1. Close service, close repo
  2. Set `initialized = false`
  3. Return `{"ok": true}`

Wire into mux in `server.go`:
```go
mux.HandleFunc("POST /init", bridge.handleInit)
mux.HandleFunc("GET /status", bridge.handleStatus)
mux.HandleFunc("POST /stop", bridge.handleStop)
```

Run:
```bash
cd go && go test -run TestInitAndStatus -count=1 -v ./...
```

- [ ] **Step 3: Test /init succeeds even when NATS unavailable**

Add test:
```go
func TestInitSucceedsWithoutNATS(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)
	body := fmt.Sprintf(`{"data_dir": %q, "project": "test", "nats_url": "nats://127.0.0.1:59999"}`, dir)
	resp, _ := http.Post(base+"/init", "application/json", strings.NewReader(body))
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("POST /init should succeed even with bad NATS, got %d", resp.StatusCode)
	}
}
```

Run:
```bash
cd go && go test -run TestInitSucceeds -count=1 -v ./...
```

**Commit:** `feat(bridge): POST /init, GET /status, POST /stop handlers`

---

## Task 3: Send and Read Handlers

**Files:**
- Modify: `go/bridge.go` (add handlers)
- Modify: `go/server.go` (wire routes)
- Modify: `go/server_test.go` (add tests)

- [ ] **Step 1: Write failing test for POST /send**

Add to `go/server_test.go`:

```go
func TestSendAndReadThread(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)

	// Init first.
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "test"}`, dir)
	http.Post(base+"/init", "application/json", strings.NewReader(initBody))

	// Send a message.
	sendBody := `{"project": "test", "body": "Hello from curl", "priority": "info"}`
	resp, err := http.Post(base+"/send", "application/json", strings.NewReader(sendBody))
	if err != nil {
		t.Fatalf("POST /send: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		t.Fatalf("POST /send status = %d, body = %s", resp.StatusCode, body)
	}

	// Response should be a notify.Message JSON (pass-through).
	var msg map[string]any
	json.NewDecoder(resp.Body).Decode(&msg)
	if msg["body"] != "Hello from curl" {
		t.Errorf("send response body = %v, want 'Hello from curl'", msg["body"])
	}
	if msg["id"] == nil || msg["id"] == "" {
		t.Error("send response should have an id")
	}
	if msg["thread"] == nil || msg["thread"] == "" {
		t.Error("send response should have a thread")
	}
}
```

Run (expect failure):
```bash
cd go && go test -run TestSendAndRead -count=1 ./...
```

- [ ] **Step 2: Implement handleSend**

Add to `go/bridge.go`:

- `handleSend(w, r)`:
  1. Guard: if not initialized, return 400 `{"error": "not initialized"}`
  2. Decode JSON body into `notify.SendOpts` (pass-through — no intermediate type)
  3. Call `svc.Send(opts)`
  4. Marshal the returned `notify.Message` directly to response
  5. Content-Type: `application/json`

Wire: `mux.HandleFunc("POST /send", bridge.handleSend)`

Run:
```bash
cd go && go test -run TestSendAndRead -count=1 -v ./...
```

- [ ] **Step 3: Write failing test for GET /threads and GET /thread/:id**

Add to `go/server_test.go`:

```go
func TestThreadsAndThreadRead(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)

	// Init and send two messages.
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "proj"}`, dir)
	http.Post(base+"/init", "application/json", strings.NewReader(initBody))

	http.Post(base+"/send", "application/json",
		strings.NewReader(`{"project": "proj", "body": "First"}`))
	http.Post(base+"/send", "application/json",
		strings.NewReader(`{"project": "proj", "body": "Second"}`))

	// GET /threads?project=proj
	resp, _ := http.Get(base + "/threads?project=proj")
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET /threads status = %d", resp.StatusCode)
	}
	var threads []map[string]any
	json.NewDecoder(resp.Body).Decode(&threads)
	if len(threads) != 2 {
		t.Fatalf("thread count = %d, want 2", len(threads))
	}

	// GET /thread/<id>?project=proj
	threadShort := threads[0]["ThreadShort"].(string)
	resp, _ = http.Get(fmt.Sprintf("%s/thread/%s?project=proj", base, threadShort))
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("GET /thread/:id status = %d", resp.StatusCode)
	}
	var messages []map[string]any
	json.NewDecoder(resp.Body).Decode(&messages)
	if len(messages) != 1 {
		t.Fatalf("message count = %d, want 1", len(messages))
	}
}
```

Run (expect failure):
```bash
cd go && go test -run TestThreadsAndThread -count=1 ./...
```

- [ ] **Step 4: Implement handleThreads and handleThread**

Add to `go/bridge.go`:

- `handleThreads(w, r)`:
  1. Parse `project` from query string
  2. Call `notify.ListThreads(svc.Repo(), project)`
  3. Marshal `[]notify.ThreadSummary` directly to response
- `handleThread(w, r)`:
  1. Parse thread ID from URL path (`/thread/{id}`)
  2. Parse `project` from query string
  3. Call `notify.ReadThread(svc.Repo(), project, id)`
  4. Marshal `[]notify.Message` directly to response

Wire:
```go
mux.HandleFunc("GET /threads", bridge.handleThreads)
mux.HandleFunc("GET /thread/{id}", bridge.handleThread)
```

Run:
```bash
cd go && go test -run TestThreads -count=1 -v ./...
```

**Commit:** `feat(bridge): POST /send, GET /threads, GET /thread/:id handlers`

---

## Task 4: Media Endpoint

**Files:**
- Modify: `go/bridge.go`
- Modify: `go/server.go`
- Modify: `go/server_test.go`

- [ ] **Step 1: Write failing test for GET /media/:filename**

Add to `go/server_test.go`:

```go
func TestMediaEndpoint(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "proj"}`, dir)
	http.Post(base+"/init", "application/json", strings.NewReader(initBody))

	// Write a UV file to the repo.
	// (This tests that the media endpoint reads UV content.)
	// We need to write via the repo directly for setup — the server
	// doesn't expose UV write yet.

	resp, _ := http.Get(base + "/media/test.png?project=proj")
	if resp.StatusCode != http.StatusNotFound {
		t.Errorf("GET /media for missing file: status = %d, want 404", resp.StatusCode)
	}
}
```

Run:
```bash
cd go && go test -run TestMediaEndpoint -count=1 ./...
```

- [ ] **Step 2: Implement handleMedia**

Add to `go/bridge.go`:

- `handleMedia(w, r)`:
  1. Parse `filename` from URL path (`/media/{filename}`)
  2. Parse `project` from query string
  3. Call `svc.Repo().UVRead(filename)` (go-libfossil Repo method)
  4. If not found, return 404
  5. Set Content-Type from file extension (`mime.TypeByExtension`)
  6. Write content bytes to response

Wire: `mux.HandleFunc("GET /media/{filename}", bridge.handleMedia)`

Run:
```bash
cd go && go test -run TestMedia -count=1 -v ./...
```

**Commit:** `feat(bridge): GET /media/:filename serves UV file content`

---

## Task 5: SSE Streaming

**Files:**
- Create: `go/sse.go`
- Create: `go/sse_test.go`

- [ ] **Step 1: Write failing test for SSE connection and message delivery**

Create `go/sse_test.go`:

```go
package server

import (
	"bufio"
	"context"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"
)

func TestSSEReceivesMessages(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)

	// Init.
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "proj"}`, dir)
	http.Post(base+"/init", "application/json", strings.NewReader(initBody))

	// Connect SSE.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	req, _ := http.NewRequestWithContext(ctx, "GET", base+"/subscribe?project=proj", nil)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /subscribe: %v", err)
	}
	defer resp.Body.Close()

	if resp.Header.Get("Content-Type") != "text/event-stream" {
		t.Fatalf("Content-Type = %q, want text/event-stream", resp.Header.Get("Content-Type"))
	}

	scanner := bufio.NewScanner(resp.Body)

	// Should get a "connected" event first.
	var event string
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "event: ") {
			event = strings.TrimPrefix(line, "event: ")
			if event == "connected" {
				break
			}
		}
	}
	if event != "connected" {
		t.Fatalf("first event = %q, want 'connected'", event)
	}

	// Send a message via HTTP.
	http.Post(base+"/send", "application/json",
		strings.NewReader(`{"project": "proj", "body": "SSE test"}`))

	// Should receive a "message" event.
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "event: message") {
			// Next line should be data.
			scanner.Scan()
			data := scanner.Text()
			if !strings.Contains(data, "SSE test") {
				t.Errorf("SSE data = %q, want to contain 'SSE test'", data)
			}
			return
		}
	}
	t.Fatal("never received SSE message event")
}
```

Run (expect compile failure):
```bash
cd go && go test -run TestSSEReceives -count=1 ./...
```

- [ ] **Step 2: Implement SSEManager**

Create `go/sse.go`:

- `SSEManager` struct:
  - `client chan sseEvent` — the single active SSE client channel (nil when disconnected)
  - `queue []sseEvent` — messages queued while client is disconnected
  - `mu sync.Mutex`
  - `maxQueue int` (default 1000)
- `sseEvent` struct: `Type string` (message/connected/disconnected), `Data string` (JSON)
- `NewSSEManager() *SSEManager`
- `Broadcast(event sseEvent)`:
  1. Lock mutex
  2. If client channel is non-nil, send (non-blocking, drop if full)
  3. If client channel is nil, append to queue
- `handleSubscribe(w, r)`:
  1. Set headers: `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: keep-alive`
  2. Flush headers
  3. Create client channel (buffered, 64)
  4. Lock mutex, set `client = ch`, replay queued events, clear queue, unlock
  5. Send `event: connected\ndata: {}\n\n`
  6. Loop: read from channel, write SSE format (`event: <type>\ndata: <json>\n\n`), flush
  7. On context done: lock mutex, set `client = nil`, send `event: disconnected` to old channel, unlock

Run:
```bash
cd go && go test -run TestSSEReceives -count=1 -v ./...
```

- [ ] **Step 3: Wire SSE into bridge — forward Watch messages to SSEManager**

Modify `go/bridge.go`:

- Add `sse *SSEManager` field to `Bridge`
- In `handleInit`: after creating service, start a goroutine that calls `svc.Watch(ctx, WatchOpts{Project: project})` and broadcasts each message to `sse.Broadcast(sseEvent{Type: "message", Data: msgJSON})`
- Wire: `mux.HandleFunc("GET /subscribe", bridge.sse.handleSubscribe)`

Run:
```bash
cd go && go test -run TestSSE -count=1 -v ./...
```

**Commit:** `feat(sse): SSE streaming with message delivery via notify.Watch`

---

## Task 6: SSE Disconnect Queuing and Reconnect Replay

**Files:**
- Modify: `go/sse.go`
- Modify: `go/sse_test.go`

- [ ] **Step 1: Write failing test for disconnect queuing and replay**

Add to `go/sse_test.go`:

```go
func TestSSEQueueAndReplay(t *testing.T) {
	mgr := NewSSEManager()

	// No client connected — messages should queue.
	mgr.Broadcast(sseEvent{Type: "message", Data: `{"body":"queued1"}`})
	mgr.Broadcast(sseEvent{Type: "message", Data: `{"body":"queued2"}`})

	mgr.mu.Lock()
	queueLen := len(mgr.queue)
	mgr.mu.Unlock()

	if queueLen != 2 {
		t.Fatalf("queue length = %d, want 2", queueLen)
	}

	// Simulate client connect — should get replayed events.
	ch := make(chan sseEvent, 64)
	mgr.mu.Lock()
	mgr.client = ch
	replayed := mgr.replayQueue()
	mgr.mu.Unlock()

	if replayed != 2 {
		t.Fatalf("replayed = %d, want 2", replayed)
	}

	// Queue should be empty now.
	mgr.mu.Lock()
	queueLen = len(mgr.queue)
	mgr.mu.Unlock()
	if queueLen != 0 {
		t.Fatalf("queue length after replay = %d, want 0", queueLen)
	}

	// Channel should have the replayed events.
	ev1 := <-ch
	if ev1.Data != `{"body":"queued1"}` {
		t.Errorf("replayed event 1 = %q", ev1.Data)
	}
	ev2 := <-ch
	if ev2.Data != `{"body":"queued2"}` {
		t.Errorf("replayed event 2 = %q", ev2.Data)
	}
}

func TestSSEQueueMaxSize(t *testing.T) {
	mgr := NewSSEManager()
	mgr.maxQueue = 5

	for i := 0; i < 10; i++ {
		mgr.Broadcast(sseEvent{Type: "message", Data: fmt.Sprintf(`{"i":%d}`, i)})
	}

	mgr.mu.Lock()
	queueLen := len(mgr.queue)
	mgr.mu.Unlock()

	if queueLen != 5 {
		t.Fatalf("queue should cap at maxQueue=5, got %d", queueLen)
	}
}
```

Run (expect failure):
```bash
cd go && go test -run TestSSEQueue -count=1 ./...
```

- [ ] **Step 2: Implement queue replay and max queue eviction**

Modify `go/sse.go`:

- `replayQueue() int` (called under lock):
  1. Send each queued event to client channel
  2. Clear queue slice
  3. Return count replayed
- `Broadcast`: when queue exceeds `maxQueue`, drop oldest (FIFO eviction)
- In `handleSubscribe`: call `replayQueue()` after setting client channel

Run:
```bash
cd go && go test -run TestSSEQueue -count=1 -v ./...
```

**Commit:** `feat(sse): disconnect queuing and reconnect replay`

---

## Task 7: Guard Endpoints and Error Handling

**Files:**
- Modify: `go/bridge.go`
- Modify: `go/server_test.go`

- [ ] **Step 1: Write failing tests for error cases**

Add to `go/server_test.go`:

```go
func TestEndpointsRequireInit(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)

	endpoints := []struct {
		method string
		path   string
	}{
		{"POST", "/send"},
		{"GET", "/threads?project=x"},
		{"GET", "/thread/abc?project=x"},
		{"GET", "/media/test.png?project=x"},
		{"GET", "/subscribe?project=x"},
	}

	for _, ep := range endpoints {
		t.Run(ep.method+" "+ep.path, func(t *testing.T) {
			var resp *http.Response
			var err error
			if ep.method == "POST" {
				resp, err = http.Post(base+ep.path, "application/json", strings.NewReader("{}"))
			} else {
				resp, err = http.Get(base + ep.path)
			}
			if err != nil {
				t.Fatalf("request: %v", err)
			}
			if resp.StatusCode != http.StatusBadRequest {
				t.Errorf("status = %d, want 400 (not initialized)", resp.StatusCode)
			}
		})
	}
}

func TestSendBadJSON(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "test"}`, dir)
	http.Post(base+"/init", "application/json", strings.NewReader(initBody))

	resp, _ := http.Post(base+"/send", "application/json", strings.NewReader("not json"))
	if resp.StatusCode != http.StatusBadRequest {
		t.Errorf("status = %d, want 400 for bad JSON", resp.StatusCode)
	}
}
```

Run (expect some failures):
```bash
cd go && go test -run TestEndpointsRequire -count=1 ./...
```

- [ ] **Step 2: Add init guards and error responses to all handlers**

Modify `go/bridge.go`:

- Extract helper: `requireInit(w) bool` — returns true if initialized, writes 400 JSON error if not
- Extract helper: `writeError(w, status int, msg string)` — writes `{"error": msg}` with status code
- Add `requireInit` guard to: `handleSend`, `handleThreads`, `handleThread`, `handleMedia`, SSE `handleSubscribe`
- Add JSON decode error handling to `handleSend` and `handleInit`

Run:
```bash
cd go && go test -count=1 -v ./...
```

**Commit:** `feat(bridge): init guards and error handling on all endpoints`

---

## Task 8: Full Integration Test

**Files:**
- Modify: `go/server_test.go`

- [ ] **Step 1: Write full round-trip integration test**

Add to `go/server_test.go`:

```go
func TestFullRoundTrip(t *testing.T) {
	dir := t.TempDir()
	port, _ := Start(dir)
	defer Stop()

	base := fmt.Sprintf("http://127.0.0.1:%d", port)

	// 1. Init.
	initBody := fmt.Sprintf(`{"data_dir": %q, "project": "demo"}`, dir)
	resp, _ := http.Post(base+"/init", "application/json", strings.NewReader(initBody))
	assertStatus(t, resp, 200)

	// 2. Status shows initialized.
	resp, _ = http.Get(base + "/status")
	var status map[string]any
	json.NewDecoder(resp.Body).Decode(&status)
	if status["initialized"] != true {
		t.Fatal("not initialized")
	}

	// 3. Send three messages, two in same thread.
	var msg1ID, msg1Thread string
	resp, _ = http.Post(base+"/send", "application/json",
		strings.NewReader(`{"project":"demo","body":"First message","priority":"info"}`))
	assertStatus(t, resp, 200)
	var msg1 map[string]any
	json.NewDecoder(resp.Body).Decode(&msg1)
	msg1ID = msg1["id"].(string)
	msg1Thread = msg1["thread"].(string)
	_ = msg1ID

	// Second message (new thread).
	resp, _ = http.Post(base+"/send", "application/json",
		strings.NewReader(`{"project":"demo","body":"Second message"}`))
	assertStatus(t, resp, 200)

	// Third message (reply to first thread).
	threadShort := msg1Thread[len("thread-"):]
	if len(threadShort) > 8 {
		threadShort = threadShort[:8]
	}
	replyBody := fmt.Sprintf(`{"project":"demo","body":"Reply","thread_short":%q}`, threadShort)
	resp, _ = http.Post(base+"/send", "application/json", strings.NewReader(replyBody))
	assertStatus(t, resp, 200)

	// 4. List threads — should be 2.
	resp, _ = http.Get(base + "/threads?project=demo")
	assertStatus(t, resp, 200)
	var threads []map[string]any
	json.NewDecoder(resp.Body).Decode(&threads)
	if len(threads) != 2 {
		t.Fatalf("thread count = %d, want 2", len(threads))
	}

	// 5. Read thread with 2 messages.
	resp, _ = http.Get(fmt.Sprintf("%s/thread/%s?project=demo", base, threadShort))
	assertStatus(t, resp, 200)
	var messages []map[string]any
	json.NewDecoder(resp.Body).Decode(&messages)
	if len(messages) != 2 {
		t.Fatalf("messages in thread = %d, want 2", len(messages))
	}

	// 6. Stop.
	resp, _ = http.Post(base+"/stop", "application/json", nil)
	assertStatus(t, resp, 200)

	// 7. Status shows not initialized.
	resp, _ = http.Get(base + "/status")
	json.NewDecoder(resp.Body).Decode(&status)
	if status["initialized"] != false {
		t.Fatal("should not be initialized after /stop")
	}
}

func assertStatus(t *testing.T, resp *http.Response, want int) {
	t.Helper()
	if resp.StatusCode != want {
		t.Fatalf("status = %d, want %d", resp.StatusCode, want)
	}
}
```

Run:
```bash
cd go && go test -count=1 -v ./...
```

- [ ] **Step 2: Verify all tests pass**

Run the full suite:
```bash
cd go && GOPRIVATE=github.com/danmestas go test -count=1 -v ./...
```

**Commit:** `test(server): full round-trip integration test`

---

## curl Smoke Test Reference

After all tasks are complete, manually verify with curl:

```bash
# Start the server (add a main.go or use go run with a thin main)
cd go && go run ./cmd/main.go &

PORT=$(cat /tmp/notify-test/server.port)
BASE="http://127.0.0.1:$PORT"

# Init
curl -s -X POST "$BASE/init" \
  -H 'Content-Type: application/json' \
  -d '{"data_dir": "/tmp/notify-test", "project": "demo"}' | jq .

# Status
curl -s "$BASE/status" | jq .

# Send
curl -s -X POST "$BASE/send" \
  -H 'Content-Type: application/json' \
  -d '{"project": "demo", "body": "Hello from curl", "priority": "info"}' | jq .

# Threads
curl -s "$BASE/threads?project=demo" | jq .

# SSE (in another terminal)
curl -N "$BASE/subscribe?project=demo"

# Stop
curl -s -X POST "$BASE/stop" | jq .
```

---

## Summary

| Task | Files | Commit |
|------|-------|--------|
| 1 | `go.mod`, `server.go`, `server_test.go` | `feat(server): scaffold Go module and server lifecycle with random port` |
| 2 | `bridge.go`, `server.go`, `server_test.go` | `feat(bridge): POST /init, GET /status, POST /stop handlers` |
| 3 | `bridge.go`, `server.go`, `server_test.go` | `feat(bridge): POST /send, GET /threads, GET /thread/:id handlers` |
| 4 | `bridge.go`, `server.go`, `server_test.go` | `feat(bridge): GET /media/:filename serves UV file content` |
| 5 | `sse.go`, `sse_test.go` | `feat(sse): SSE streaming with message delivery via notify.Watch` |
| 6 | `sse.go`, `sse_test.go` | `feat(sse): disconnect queuing and reconnect replay` |
| 7 | `bridge.go`, `server_test.go` | `feat(bridge): init guards and error handling on all endpoints` |
| 8 | `server_test.go` | `test(server): full round-trip integration test` |
