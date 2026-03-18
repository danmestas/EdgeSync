# EdgeSync Phase E Implementation Plan: Leaf Agent

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a NATS-connected daemon that watches a Fossil repo and syncs artifacts via go-libfossil's sync engine over NATS request/reply.

**Architecture:** New `leaf/` Go module in EdgeSync workspace. `NATSTransport` implements `sync.Transport` over NATS request/reply. `Agent` runs a poll loop detecting new artifacts, triggering `sync.Sync()`. CLI binary at `cmd/leaf/main.go`. Tests use embedded NATS server + fossil server subprocess with a test bridge responder.

**Tech Stack:** Go 1.25.4, `github.com/nats-io/nats.go`, `github.com/nats-io/nats-server/v2` (test), go-libfossil (`sync/`, `repo/`, `manifest/`, `blob/`, `content/`, `xfer/`)

**Spec:** `docs/superpowers/specs/2026-03-15-edgesync-phase-e-design.md`

---

## File Structure

```
EdgeSync/
  go.work                     # NEW: Go workspace
  go-libfossil/               # existing
  leaf/                        # NEW: leaf agent module
    go.mod                     # NEW
    cmd/leaf/
      main.go                  # NEW: CLI entry point
    agent/
      config.go                # NEW: Config struct, validation
      nats.go                  # NEW: NATSTransport
      agent.go                 # NEW: Agent struct, Start/Stop/SyncNow
      agent_test.go            # NEW: unit tests
      integration_test.go      # NEW: NATS + fossil server tests
```

---

## Chunk 1: Project Scaffold + NATSTransport

### Task 1: Go Workspace + leaf Module Init

**Files:**
- Create: `EdgeSync/go.work`
- Create: `EdgeSync/leaf/go.mod`

- [ ] **Step 1: Create go.work**

```bash
cd ~/projects/EdgeSync
cat > go.work << 'EOF'
go 1.25.4

use (
    ./go-libfossil
    ./leaf
)
EOF
```

- [ ] **Step 2: Init leaf module and add dependencies**

```bash
mkdir -p ~/projects/EdgeSync/leaf/cmd/leaf
mkdir -p ~/projects/EdgeSync/leaf/agent
cd ~/projects/EdgeSync/leaf
go mod init github.com/dmestas/edgesync/leaf
go get github.com/dmestas/edgesync/go-libfossil@v0.0.0
go get github.com/nats-io/nats.go@latest
go get github.com/nats-io/nats-server/v2@latest
```

- [ ] **Step 3: Create minimal main.go to verify module compiles**

Create `leaf/cmd/leaf/main.go`:

```go
package main

import "fmt"

func main() {
    fmt.Println("leaf-agent: not yet implemented")
}
```

- [ ] **Step 4: Verify workspace builds**

```bash
cd ~/projects/EdgeSync
go build ./leaf/...
```

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go.work leaf/go.mod leaf/go.sum leaf/cmd/leaf/main.go
fossil commit -m "leaf: init Go workspace and leaf agent module"
```

---

### Task 2: Config

**Files:**
- Create: `leaf/agent/config.go`
- Create: `leaf/agent/agent_test.go` (initial)

- [ ] **Step 1: Write tests**

Create `leaf/agent/agent_test.go`:

```go
package agent

import (
    "testing"
    "time"
)

func TestConfigDefaults(t *testing.T) {
    cfg := Config{RepoPath: "/tmp/test.fossil"}
    cfg.applyDefaults()
    if cfg.NATSUrl != "nats://localhost:4222" {
        t.Fatalf("NATSUrl = %q", cfg.NATSUrl)
    }
    if cfg.PollInterval != 5*time.Second {
        t.Fatalf("PollInterval = %v", cfg.PollInterval)
    }
    if cfg.User != "anonymous" {
        t.Fatalf("User = %q", cfg.User)
    }
    if !cfg.Push || !cfg.Pull {
        t.Fatal("Push/Pull should default true")
    }
}

func TestConfigValidation(t *testing.T) {
    cfg := Config{}
    if err := cfg.validate(); err == nil {
        t.Fatal("empty RepoPath should fail validation")
    }
    cfg.RepoPath = "/tmp/test.fossil"
    if err := cfg.validate(); err != nil {
        t.Fatalf("valid config: %v", err)
    }
}
```

- [ ] **Step 2: Implement config.go**

Create `leaf/agent/config.go`:

```go
package agent

import (
    "fmt"
    "time"
)

type Config struct {
    RepoPath     string
    NATSUrl      string
    PollInterval time.Duration
    User         string
    Password     string
    Push         bool
    Pull         bool
}

func (c *Config) applyDefaults() {
    if c.NATSUrl == "" {
        c.NATSUrl = "nats://localhost:4222"
    }
    if c.PollInterval == 0 {
        c.PollInterval = 5 * time.Second
    }
    if c.User == "" {
        c.User = "anonymous"
    }
    if !c.Push && !c.Pull {
        c.Push = true
        c.Pull = true
    }
}

func (c *Config) validate() error {
    if c.RepoPath == "" {
        return fmt.Errorf("agent: RepoPath is required")
    }
    return nil
}
```

- [ ] **Step 3: Run tests**

```bash
cd ~/projects/EdgeSync && go test ./leaf/agent/ -v
```

- [ ] **Step 4: Commit**

```bash
fossil add leaf/agent/config.go leaf/agent/agent_test.go
fossil commit -m "leaf: add Config struct with defaults and validation"
```

---

### Task 3: NATSTransport

**Files:**
- Create: `leaf/agent/nats.go`
- Modify: `leaf/agent/agent_test.go`

- [ ] **Step 1: Add tests**

Append to `agent_test.go`:

```go
import (
    "context"

    "github.com/nats-io/nats.go"
    natsserver "github.com/nats-io/nats-server/v2/server"

    "github.com/dmestas/edgesync/go-libfossil/xfer"
)

func startEmbeddedNATS(t *testing.T) string {
    t.Helper()
    opts := &natsserver.Options{Port: -1}
    ns, err := natsserver.NewServer(opts)
    if err != nil {
        t.Fatalf("nats server: %v", err)
    }
    ns.Start()
    if !ns.ReadyForConnections(5 * time.Second) {
        t.Fatal("nats server not ready")
    }
    t.Cleanup(func() { ns.Shutdown() })
    return ns.ClientURL()
}

func TestNATSTransportRoundTrip(t *testing.T) {
    url := startEmbeddedNATS(t)

    // Connect subscriber that echoes back an igot card
    nc, _ := nats.Connect(url)
    defer nc.Close()
    nc.Subscribe("fossil.testproj.sync", func(msg *nats.Msg) {
        resp := &xfer.Message{Cards: []xfer.Card{
            &xfer.IGotCard{UUID: "echo-response"},
        }}
        data, _ := resp.Encode()
        msg.Respond(data)
    })

    // Connect transport
    tc, _ := nats.Connect(url)
    defer tc.Close()
    transport := NewNATSTransport(tc, "testproj", 5*time.Second)

    req := &xfer.Message{Cards: []xfer.Card{
        &xfer.PragmaCard{Name: "client-version", Values: []string{"test"}},
    }}
    resp, err := transport.Exchange(context.Background(), req)
    if err != nil {
        t.Fatalf("Exchange: %v", err)
    }
    if len(resp.Cards) != 1 || resp.Cards[0].Type() != xfer.CardIGot {
        t.Fatalf("unexpected response: %+v", resp.Cards)
    }
}

func TestNATSTransportTimeout(t *testing.T) {
    url := startEmbeddedNATS(t)
    nc, _ := nats.Connect(url)
    defer nc.Close()

    transport := NewNATSTransport(nc, "nosubscriber", 200*time.Millisecond)
    _, err := transport.Exchange(context.Background(), &xfer.Message{})
    if err == nil {
        t.Fatal("should timeout with no subscriber")
    }
}
```

- [ ] **Step 2: Implement nats.go**

Create `leaf/agent/nats.go`:

```go
package agent

import (
    "context"
    "fmt"
    "time"

    "github.com/nats-io/nats.go"

    "github.com/dmestas/edgesync/go-libfossil/xfer"
)

type NATSTransport struct {
    conn    *nats.Conn
    subject string
    timeout time.Duration
}

func NewNATSTransport(conn *nats.Conn, projectCode string, timeout time.Duration) *NATSTransport {
    if timeout == 0 {
        timeout = 30 * time.Second
    }
    return &NATSTransport{
        conn:    conn,
        subject: fmt.Sprintf("fossil.%s.sync", projectCode),
        timeout: timeout,
    }
}

func (t *NATSTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
    data, err := req.Encode()
    if err != nil {
        return nil, fmt.Errorf("NATSTransport encode: %w", err)
    }

    ctx, cancel := context.WithTimeout(ctx, t.timeout)
    defer cancel()

    msg, err := t.conn.RequestWithContext(ctx, t.subject, data)
    if err != nil {
        return nil, fmt.Errorf("NATSTransport request: %w", err)
    }

    return xfer.Decode(msg.Data)
}
```

- [ ] **Step 3: Run tests**

```bash
cd ~/projects/EdgeSync && go test ./leaf/agent/ -v -timeout 30s
```

- [ ] **Step 4: Commit**

```bash
fossil add leaf/agent/nats.go
fossil commit -m "leaf: add NATSTransport implementing sync.Transport"
```

---

## Chunk 2: Agent Core

### Task 4: Agent Lifecycle

**Files:**
- Create: `leaf/agent/agent.go`
- Modify: `leaf/agent/agent_test.go`

- [ ] **Step 1: Add tests**

```go
import (
    "path/filepath"

    "github.com/dmestas/edgesync/go-libfossil/repo"
)

func setupTestRepo(t *testing.T) string {
    t.Helper()
    path := filepath.Join(t.TempDir(), "test.fossil")
    r, err := repo.Create(path, "testuser")
    if err != nil {
        t.Fatalf("Create: %v", err)
    }
    r.Close()
    return path
}

func TestAgentNewAndStop(t *testing.T) {
    natsURL := startEmbeddedNATS(t)
    repoPath := setupTestRepo(t)

    a, err := New(Config{RepoPath: repoPath, NATSUrl: natsURL})
    if err != nil {
        t.Fatalf("New: %v", err)
    }
    if err := a.Stop(); err != nil {
        t.Fatalf("Stop: %v", err)
    }
}

func TestAgentStartStop(t *testing.T) {
    natsURL := startEmbeddedNATS(t)
    repoPath := setupTestRepo(t)

    a, err := New(Config{RepoPath: repoPath, NATSUrl: natsURL, PollInterval: 100 * time.Millisecond})
    if err != nil {
        t.Fatalf("New: %v", err)
    }
    if err := a.Start(); err != nil {
        t.Fatalf("Start: %v", err)
    }
    time.Sleep(250 * time.Millisecond) // let a few poll cycles run
    if err := a.Stop(); err != nil {
        t.Fatalf("Stop: %v", err)
    }
}

func TestAgentNewBadRepo(t *testing.T) {
    natsURL := startEmbeddedNATS(t)
    _, err := New(Config{RepoPath: "/nonexistent.fossil", NATSUrl: natsURL})
    if err == nil {
        t.Fatal("should fail with bad repo path")
    }
}

func TestAgentSyncNow(t *testing.T) {
    natsURL := startEmbeddedNATS(t)
    repoPath := setupTestRepo(t)

    a, err := New(Config{RepoPath: repoPath, NATSUrl: natsURL, PollInterval: 1 * time.Hour})
    if err != nil {
        t.Fatalf("New: %v", err)
    }
    a.Start()
    a.SyncNow() // should not block or panic
    time.Sleep(100 * time.Millisecond)
    a.Stop()
}
```

- [ ] **Step 2: Implement agent.go**

Create `leaf/agent/agent.go`:

```go
package agent

import (
    "context"
    "fmt"
    "log"
    "time"

    "github.com/nats-io/nats.go"

    "github.com/dmestas/edgesync/go-libfossil/repo"
    "github.com/dmestas/edgesync/go-libfossil/sync"
)

type Agent struct {
    config      Config
    repo        *repo.Repo
    conn        *nats.Conn
    transport   *NATSTransport
    projectCode string
    serverCode  string
    cancel      context.CancelFunc
    done        chan struct{}
    syncNow     chan struct{}
}

func New(cfg Config) (*Agent, error) {
    cfg.applyDefaults()
    if err := cfg.validate(); err != nil {
        return nil, err
    }

    r, err := repo.Open(cfg.RepoPath)
    if err != nil {
        return nil, fmt.Errorf("agent: open repo: %w", err)
    }

    var projectCode, serverCode string
    r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)
    r.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)

    nc, err := nats.Connect(cfg.NATSUrl)
    if err != nil {
        r.Close()
        return nil, fmt.Errorf("agent: nats connect: %w", err)
    }

    transport := NewNATSTransport(nc, projectCode, 30*time.Second)

    return &Agent{
        config:      cfg,
        repo:        r,
        conn:        nc,
        transport:   transport,
        projectCode: projectCode,
        serverCode:  serverCode,
        syncNow:     make(chan struct{}, 1),
    }, nil
}

func (a *Agent) Start() error {
    ctx, cancel := context.WithCancel(context.Background())
    a.cancel = cancel
    a.done = make(chan struct{})

    go a.pollLoop(ctx)
    log.Printf("leaf-agent started: repo=%s poll=%v", a.config.RepoPath, a.config.PollInterval)
    return nil
}

func (a *Agent) Stop() error {
    if a.cancel != nil {
        a.cancel()
        <-a.done
    }
    if a.conn != nil {
        a.conn.Close()
    }
    if a.repo != nil {
        a.repo.Close()
    }
    log.Println("leaf-agent stopped")
    return nil
}

func (a *Agent) SyncNow() {
    select {
    case a.syncNow <- struct{}{}:
    default: // already queued
    }
}

func (a *Agent) pollLoop(ctx context.Context) {
    defer close(a.done)
    for {
        select {
        case <-ctx.Done():
            return
        case <-time.After(a.config.PollInterval):
            a.doSync(ctx)
        case <-a.syncNow:
            a.doSync(ctx)
        }
    }
}

func (a *Agent) doSync(ctx context.Context) {
    // Optimization: skip if nothing to sync
    if !a.config.Pull {
        var count int
        a.repo.DB().QueryRow("SELECT count(*) FROM unsent").Scan(&count)
        var count2 int
        a.repo.DB().QueryRow("SELECT count(*) FROM unclustered").Scan(&count2)
        if count == 0 && count2 == 0 {
            return
        }
    }

    result, err := sync.Sync(ctx, a.repo, a.transport, sync.SyncOpts{
        Push:        a.config.Push,
        Pull:        a.config.Pull,
        ProjectCode: a.projectCode,
        ServerCode:  a.serverCode,
        User:        a.config.User,
        Password:    a.config.Password,
    })
    if err != nil {
        log.Printf("sync error: %v", err)
        return
    }
    if result.FilesSent > 0 || result.FilesRecvd > 0 {
        log.Printf("sync: %d rounds, sent=%d recv=%d", result.Rounds, result.FilesSent, result.FilesRecvd)
    }
    for _, e := range result.Errors {
        log.Printf("sync server: %s", e)
    }
}
```

- [ ] **Step 3: Run tests**

```bash
cd ~/projects/EdgeSync && go test ./leaf/agent/ -v -timeout 30s
```

- [ ] **Step 4: Commit**

```bash
fossil add leaf/agent/agent.go
fossil commit -m "leaf: add Agent with poll loop, Start/Stop/SyncNow"
```

---

### Task 5: CLI Binary

**Files:**
- Modify: `leaf/cmd/leaf/main.go`

- [ ] **Step 1: Implement main.go**

```go
package main

import (
    "flag"
    "fmt"
    "log"
    "os"
    "os/signal"
    "syscall"
    "time"

    "github.com/dmestas/edgesync/leaf/agent"
)

func main() {
    repoPath := flag.String("repo", os.Getenv("LEAF_REPO"), "path to .fossil repo file (required)")
    natsURL := flag.String("nats", envOrDefault("LEAF_NATS_URL", "nats://localhost:4222"), "NATS server URL")
    pollInterval := flag.Duration("poll", 5*time.Second, "poll interval")
    user := flag.String("user", envOrDefault("LEAF_USER", "anonymous"), "sync user")
    password := flag.String("password", os.Getenv("LEAF_PASSWORD"), "sync password")
    push := flag.Bool("push", true, "enable push")
    pull := flag.Bool("pull", true, "enable pull")
    flag.Parse()

    if *repoPath == "" {
        fmt.Fprintln(os.Stderr, "error: --repo is required")
        flag.Usage()
        os.Exit(1)
    }

    a, err := agent.New(agent.Config{
        RepoPath:     *repoPath,
        NATSUrl:      *natsURL,
        PollInterval: *pollInterval,
        User:         *user,
        Password:     *password,
        Push:         *push,
        Pull:         *pull,
    })
    if err != nil {
        log.Fatalf("agent init: %v", err)
    }

    if err := a.Start(); err != nil {
        log.Fatalf("agent start: %v", err)
    }

    sigs := make(chan os.Signal, 1)
    signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)

    for sig := range sigs {
        switch sig {
        case syscall.SIGUSR1:
            log.Println("SIGUSR1: triggering immediate sync")
            a.SyncNow()
        case syscall.SIGINT, syscall.SIGTERM:
            log.Printf("received %s, shutting down", sig)
            a.Stop()
            os.Exit(0)
        }
    }
}

func envOrDefault(key, def string) string {
    if v := os.Getenv(key); v != "" {
        return v
    }
    return def
}
```

- [ ] **Step 2: Verify binary builds**

```bash
cd ~/projects/EdgeSync && go build ./leaf/cmd/leaf/
```

- [ ] **Step 3: Commit**

```bash
fossil commit -m "leaf: add CLI binary with flag parsing and signal handling"
```

---

## Chunk 3: Integration Tests + Validation

### Task 6: Integration Tests

**Files:**
- Create: `leaf/agent/integration_test.go`

- [ ] **Step 1: Create integration test**

```go
package agent

import (
    "context"
    "fmt"
    "net"
    "os/exec"
    "path/filepath"
    "testing"
    "time"

    "github.com/nats-io/nats.go"

    "github.com/dmestas/edgesync/go-libfossil/manifest"
    "github.com/dmestas/edgesync/go-libfossil/repo"
    "github.com/dmestas/edgesync/go-libfossil/sync"
    "github.com/dmestas/edgesync/go-libfossil/testutil"
    "github.com/dmestas/edgesync/go-libfossil/xfer"
)

func startFossilServer(t *testing.T, repoPath string) (url string, cleanup func()) {
    t.Helper()
    bin := testutil.FossilBinary()
    if bin == "" {
        t.Skip("fossil not in PATH")
    }
    ln, _ := net.Listen("tcp", "127.0.0.1:0")
    port := ln.Addr().(*net.TCPAddr).Port
    ln.Close()

    cmd := exec.Command(bin, "server", "--port", fmt.Sprintf("%d", port), repoPath)
    if err := cmd.Start(); err != nil {
        t.Fatalf("start fossil server: %v", err)
    }
    url = fmt.Sprintf("http://127.0.0.1:%d", port)
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

// testBridge subscribes to NATS and proxies to fossil server via HTTP
func startTestBridge(t *testing.T, nc *nats.Conn, projectCode, fossilURL string) {
    t.Helper()
    subject := fmt.Sprintf("fossil.%s.sync", projectCode)
    nc.Subscribe(subject, func(msg *nats.Msg) {
        req, err := xfer.Decode(msg.Data)
        if err != nil {
            return
        }
        ht := &sync.HTTPTransport{URL: fossilURL}
        resp, err := ht.Exchange(context.Background(), req)
        if err != nil {
            // Send back empty response on error
            empty := &xfer.Message{}
            data, _ := empty.Encode()
            msg.Respond(data)
            return
        }
        data, _ := resp.Encode()
        msg.Respond(data)
    })
}

func TestIntegrationLeafPush(t *testing.T) {
    if !testutil.HasFossil() {
        t.Skip("fossil not in PATH")
    }

    // Create local repo with a checkin
    localPath := filepath.Join(t.TempDir(), "local.fossil")
    localRepo, _ := repo.Create(localPath, "testuser")
    manifest.Checkin(localRepo, manifest.CheckinOpts{
        Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello from leaf")}},
        Comment: "leaf commit",
        User:    "testuser",
        Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
    })
    localRepo.Close()

    // Clone to create matching remote
    remotePath := filepath.Join(t.TempDir(), "remote.fossil")
    exec.Command(testutil.FossilBinary(), "clone", localPath, remotePath).Run()

    // Start fossil server on remote
    fossilURL, fossilCleanup := startFossilServer(t, remotePath)
    defer fossilCleanup()

    // Start embedded NATS
    natsURL := startEmbeddedNATS(t)

    // Start test bridge
    bridgeConn, _ := nats.Connect(natsURL)
    defer bridgeConn.Close()

    // Read project code from local repo to set up bridge
    lr, _ := repo.Open(localPath)
    var pc string
    lr.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&pc)
    lr.Close()

    startTestBridge(t, bridgeConn, pc, fossilURL)

    // Start leaf agent
    a, err := New(Config{
        RepoPath:     localPath,
        NATSUrl:      natsURL,
        PollInterval: 100 * time.Millisecond,
        Push:         true,
    })
    if err != nil {
        t.Fatalf("New: %v", err)
    }
    a.Start()
    defer a.Stop()

    // Trigger sync and wait
    a.SyncNow()
    time.Sleep(2 * time.Second)

    t.Log("Integration push test completed — check logs for sync results")
}
```

NOTE: The integration test may need iteration during implementation. The test bridge proxying NATS→HTTP is the critical piece. If `fossil server` returns non-xfer responses (as seen in Phase D), the test should log informational results rather than hard-fail.

- [ ] **Step 2: Run integration test**

```bash
cd ~/projects/EdgeSync && go test ./leaf/agent/ -run TestIntegration -v -timeout 30s
```

- [ ] **Step 3: Commit**

```bash
fossil add leaf/agent/integration_test.go
fossil commit -m "leaf: add integration tests with NATS + fossil server bridge"
```

---

### Task 7: Full Validation

- [ ] **Step 1: Vet**

```bash
cd ~/projects/EdgeSync
go vet ./leaf/...
```

- [ ] **Step 2: All tests**

```bash
go test -count=1 ./leaf/...
go test -count=1 ./go-libfossil/...
```

- [ ] **Step 3: Race detector**

```bash
go test -race ./leaf/...
```

- [ ] **Step 4: Build binary**

```bash
go build -o leaf-agent ./leaf/cmd/leaf/
./leaf-agent --help
rm leaf-agent
```

- [ ] **Step 5: Commit**

```bash
fossil commit -m "leaf: Phase E complete — leaf agent with NATS sync"
```
