# EdgeSync Phase E Design Spec: Leaf Agent

NATS-connected daemon that watches Fossil repos and syncs artifacts via go-libfossil's sync engine. First component outside go-libfossil — separate Go module with NATS dependency.

## Scope

Build a leaf agent daemon that:
- Opens a Fossil repo via go-libfossil
- Connects to NATS
- Polls for new local artifacts on a configurable interval
- Syncs via `sync.Sync()` over a `NATSTransport` (NATS request/reply)
- Supports manual sync trigger via SIGUSR1
- Shuts down gracefully on SIGINT/SIGTERM

**In scope:** NATSTransport, agent poll loop, CLI binary, configuration, graceful lifecycle, embedded NATS server for tests, test bridge responder (proxies NATS→fossil server).
**Out of scope:** Bridge binary (Phase F), JetStream persistence (future upgrade), multi-repo support (future), web UI, metrics/monitoring.

## Constraints

- Go 1.25.4 (matching go-libfossil)
- External dependency: `github.com/nats-io/nats.go` (runtime), `github.com/nats-io/nats-server/v2` (test only)
- TDD, race clean, fossil CLI oracle for integration tests
- `nats` CLI (v0.3.0, installed at `/opt/homebrew/bin/nats`) available as additional test tool
- `fossil` binary required for integration tests (same as Phases A-D)
- `nats-server` binary NOT available on host — use embedded Go server in tests

## Project Structure

```
EdgeSync/
  go-libfossil/           # existing (own go.mod)
  leaf/                    # NEW (own go.mod)
    cmd/leaf/
      main.go             # CLI entry point, flag parsing, signal handling
    agent/
      agent.go            # Agent struct: New, Start, Stop, SyncNow
      nats.go             # NATSTransport implementing sync.Transport
      config.go           # Config struct, validation, defaults
    agent/
      agent_test.go       # Unit tests (embedded NATS, mock)
      integration_test.go # End-to-end: agent + NATS + fossil server
    go.mod                # module github.com/dmestas/edgesync/leaf
    go.sum
  go.work                 # NEW: Go workspace
```

### go.work

```
go 1.25.4

use (
    ./go-libfossil
    ./leaf
)
```

### leaf/go.mod

```
module github.com/dmestas/edgesync/leaf

go 1.25.4

require (
    github.com/dmestas/edgesync/go-libfossil v0.0.0
    github.com/nats-io/nats.go v1.37.0
    github.com/nats-io/nats-server/v2 v2.10.0 // test only
)
```

The `go.work` file makes `go-libfossil` resolve locally during development. For releases, the `go.mod` replace directive or published module version would be used.

## NATSTransport

Implements `sync.Transport` over NATS request/reply:

```go
package agent

type NATSTransport struct {
    conn    *nats.Conn
    subject string        // "fossil.<project-code>.sync"
    timeout time.Duration // default 30s
}

func NewNATSTransport(conn *nats.Conn, projectCode string, timeout time.Duration) *NATSTransport

func (t *NATSTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error)
```

**Exchange flow:**
1. `req.Encode()` → zlib-compressed bytes
2. `conn.RequestWithContext(ctx, t.subject, bytes)` → NATS reply
3. `xfer.Decode(reply.Data)` → `*xfer.Message`
4. Return decoded message

**Subject scheme:** `fossil.<project-code>.sync` — one subject per project. The bridge (Phase F) subscribes to this subject.

**JetStream upgrade path (future):** Replace `Request` with publish to a JetStream stream + consume from a response stream. The `NATSTransport` struct would gain a JetStream context. The `Exchange` signature stays the same.

## Agent

```go
package agent

type Config struct {
    RepoPath     string        // required: path to .fossil file
    NATSUrl      string        // default "nats://localhost:4222"
    PollInterval time.Duration // default 5s
    User         string        // sync user, default "anonymous"
    Password     string        // sync password
    Push         bool          // default true
    Pull         bool          // default true
}

type Agent struct {
    config  Config
    repo    *repo.Repo
    conn    *nats.Conn
    transport *NATSTransport
    cancel  context.CancelFunc
    done    chan struct{}
    syncNow chan struct{} // buffered channel for manual trigger
}

func New(cfg Config) (*Agent, error)
func (a *Agent) Start() error
func (a *Agent) Stop() error
func (a *Agent) SyncNow()          // non-blocking: sends on buffered(1) channel; poll goroutine is sole executor
```

### New(cfg)

1. Validate config (RepoPath required)
2. Open repo via `repo.Open(cfg.RepoPath)`
3. Read `project-code` and `server-code` from repo's config table via raw SQL: `r.DB().QueryRow("SELECT value FROM config WHERE name=?", "project-code")`. These are NOT user-configurable — always derived from the repo. The NATS subject is derived from the project code.
4. Connect to NATS via `nats.Connect(cfg.NATSUrl)`
5. Create `NATSTransport` with project code
6. Return `*Agent`

### Start()

1. Create cancellable context
2. Launch poll goroutine
3. Return immediately

**Poll goroutine:**
```
for {
    select {
    case <-ctx.Done():
        close(done)
        return
    case <-time.After(pollInterval):
        doSync()
    case <-syncNow:
        doSync()
    }
}
```

**doSync:**
1. **Optimization (not correctness):** Check `unsent`/`unclustered` tables — if both empty and pull not enabled, skip this round to avoid a NATS round-trip. This is a TOCTOU optimization; artifacts arriving between the check and next poll are caught on the following cycle.
2. Call `sync.Sync(ctx, repo, transport, syncOpts)`
3. Log result (rounds, files sent/received, errors)
4. On error: log and continue (don't crash the daemon)

### Stop()

1. Cancel context (stops poll goroutine)
2. Wait on `done` channel (goroutine finished)
3. Close NATS connection
4. Close repo
5. Return

### SyncNow()

Non-blocking send on `syncNow` channel (buffer size 1). If a sync is already in progress, the signal is silently coalesced — the next poll will catch any remaining work. The poll goroutine is the sole executor of `doSync`; `SyncNow` never calls `doSync` directly. This avoids concurrent SQLite access.

## CLI

```
leaf-agent --repo /path/to/repo.fossil [--nats nats://localhost:4222] [--poll 5s] [--user anonymous] [--push] [--pull] [--no-push] [--no-pull]
```

`cmd/leaf/main.go`:
1. Parse flags
2. Create `Config` from flags + env vars (`LEAF_NATS_URL`, `LEAF_REPO`, etc.)
3. Create `Agent`
4. `agent.Start()`
5. Wait for SIGINT, SIGTERM, or SIGUSR1
   - SIGUSR1: call `agent.SyncNow()`
   - SIGINT/SIGTERM: call `agent.Stop()`, exit 0
6. Log startup and shutdown

## Testing Architecture

### Test Dependencies

- `github.com/nats-io/nats-server/v2/server` — embedded NATS server for Go tests (no external `nats-server` binary needed). **Must only be imported from `_test.go` files** so it doesn't end up in the production binary.
- `fossil` binary — for integration tests (`testutil.HasFossil()`)
- `nats` CLI (`/opt/homebrew/bin/nats`) — optional, for manual testing and debugging

### Test Helper: Embedded NATS

```go
func startEmbeddedNATS(t *testing.T) (url string) {
    opts := &server.Options{Port: -1} // random port
    ns, err := server.NewServer(opts)
    ns.Start()
    t.Cleanup(func() { ns.Shutdown() })
    // wait for ready
    return ns.ClientURL()
}
```

### Test Helper: Bridge Responder

A test subscriber that proxies NATS request/reply to a fossil server via HTTPTransport. This is the minimal bridge needed for integration tests:

```go
func startTestBridge(t *testing.T, nc *nats.Conn, projectCode, fossilURL string) {
    subject := fmt.Sprintf("fossil.%s.sync", projectCode)
    nc.Subscribe(subject, func(msg *nats.Msg) {
        // 1. xfer.Decode(msg.Data) → request
        // 2. HTTPTransport.Exchange(ctx, request) → response
        // 3. response.Encode() → reply bytes
        // 4. msg.Respond(reply)
    })
}
```

### Layer 1: Unit Tests (embedded NATS, no fossil)

**NATSTransport:**
- Round-trip: start embedded NATS, subscribe to subject, echo back a canned response, verify NATSTransport.Exchange returns it
- Timeout: no subscriber, verify context deadline exceeded
- Connection error: nil conn, verify error

**Agent lifecycle:**
- New with valid config opens repo and connects NATS
- New with bad repo path returns error
- Start/Stop without syncing (no bridge, just lifecycle)
- SyncNow triggers immediate sync attempt

**Config:**
- Defaults applied (PollInterval=5s, NATSUrl=nats://localhost:4222, Push=true, Pull=true)
- Validation: missing RepoPath returns error

### Layer 2: Integration Tests (NATS + fossil server)

**Full end-to-end:**
1. Start embedded NATS server
2. Start `fossil server` subprocess
3. Create local repo with `repo.Create` + `manifest.Checkin`
4. Create matching remote via `fossil clone`
5. Start test bridge responder (NATS→HTTPTransport→fossil server)
6. Create Agent, Start, trigger SyncNow
7. Wait briefly for sync to complete
8. Verify: `fossil timeline -R <remote>` shows the checkin
9. Stop agent, cleanup

**Pull test:**
1. Same setup but add content to remote via `fossil commit`
2. Agent pulls → verify local repo has new artifacts

### Layer 3: Manual Testing (nats CLI)

Not automated — documented for developer use:
```bash
# In one terminal: start nats-server (or use nats CLI to connect to existing)
# In another: start leaf-agent
leaf-agent --repo test.fossil --nats nats://localhost:4222

# Monitor sync traffic:
nats sub "fossil.*.sync"

# Trigger manual sync:
kill -USR1 <agent-pid>
```

## Phase E Exit Criteria

1. `NATSTransport.Exchange()` successfully sends/receives xfer Messages over NATS request/reply
2. Agent poll loop detects new local artifacts and triggers sync
3. SIGUSR1 triggers immediate sync
4. SIGINT/SIGTERM triggers graceful shutdown (NATS disconnected, repo closed)
5. Integration test: agent pushes Go-created checkin through NATS→bridge→fossil server, verified by `fossil timeline`
6. Integration test: agent pulls from fossil server through bridge→NATS, verified by `content.Verify`
7. Embedded NATS server used in tests (no external nats-server dependency)
8. `go.work` workspace builds both `go-libfossil` and `leaf` modules
9. All tests green including race detector
10. CLI binary builds and runs: `go build ./cmd/leaf/`
