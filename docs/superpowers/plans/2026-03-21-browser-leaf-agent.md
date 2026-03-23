# Browser Leaf Agent Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace manual sync with a real leaf agent in the browser — auto-sync via NATS WebSocket.

**Architecture:** Add `CustomDialer` and `Logger` to agent Config, wire WSDialer in the playground, add NATS WebSocket listener to deploy config.

**Tech Stack:** leaf/agent, leaf/wsdialer, nats.go, spike/opfs-poc

**Spec:** `docs/superpowers/specs/2026-03-21-browser-leaf-agent-design.md`

---

### File Structure

| File | Action | Responsibility |
|------|--------|---------------|
| `leaf/agent/config.go` | Modify | Add `CustomDialer` and `Logger` fields |
| `leaf/agent/agent.go` | Modify | Use CustomDialer in NATS connect, call Logger in pollLoop |
| `deploy/nats.conf` | Modify | Add WebSocket listener on port 8222 |
| `spike/opfs-poc/main.go` | Modify | Replace manual sync with agent lifecycle |
| `spike/opfs-poc/index.html` | Modify | Agent status in UI, start/stop toggle, sync now button |
| `spike/opfs-poc/worker.js` | Modify | Add agent message handlers |

---

### Task 1: Add CustomDialer and Logger to Agent Config

**Files:**
- Modify: `leaf/agent/config.go`
- Modify: `leaf/agent/agent.go`

These are production-quality changes to the leaf package.

- [ ] **Step 1: Add fields to Config**

In `leaf/agent/config.go`, add after the `ServeNATSEnabled` field:

```go
	// CustomDialer overrides the default net.Dial for NATS connections.
	// Set to &wsdialer.WSDialer{URL: "ws://..."} for browser WebSocket.
	CustomDialer nats.CustomDialer

	// Logger receives human-readable agent lifecycle messages.
	// Nil means no logging. In browser, pipe to the UI log panel.
	Logger func(string)
```

Add `"github.com/nats-io/nats.go"` to the imports in config.go.

- [ ] **Step 2: Use CustomDialer in New()**

In `leaf/agent/agent.go`, replace the bare `nats.Connect` call:

```go
	// Before:
	nc, err := nats.Connect(cfg.NATSUrl)

	// After:
	natsOpts := []nats.Option{nats.Name("edgesync-leaf")}
	if cfg.CustomDialer != nil {
		natsOpts = append(natsOpts, nats.SetCustomDialer(cfg.CustomDialer))
	}
	if cfg.Logger != nil {
		natsOpts = append(natsOpts, nats.DisconnectErrHandler(func(_ *nats.Conn, err error) {
			if err != nil {
				cfg.Logger("NATS disconnected: " + err.Error())
			}
		}))
		natsOpts = append(natsOpts, nats.ReconnectHandler(func(_ *nats.Conn) {
			cfg.Logger("NATS reconnected")
		}))
	}
	nc, err := nats.Connect(cfg.NATSUrl, natsOpts...)
```

- [ ] **Step 3: Add logger helper and call in pollLoop**

In `leaf/agent/agent.go`, add a helper method:

```go
func (a *Agent) logf(format string, args ...any) {
	if a.config.Logger != nil {
		a.config.Logger(fmt.Sprintf(format, args...))
	}
}
```

Update `pollLoop` to use it:

```go
func (a *Agent) pollLoop(ctx context.Context) {
	defer close(a.done)
	a.logf("agent started, poll interval %s", a.config.PollInterval)
	for {
		var ev Event
		select {
		case <-ctx.Done():
			a.logf("agent stopped")
			return
		case <-a.clock.After(a.config.PollInterval):
			ev = EventTimer
		case <-a.syncNow:
			ev = EventSyncNow
			a.logf("manual sync triggered")
		}

		act := a.Tick(ctx, ev)
		if act.Err != nil {
			a.logf("sync error: %v", act.Err)
			slog.ErrorContext(ctx, "sync error", "error", act.Err)
			continue
		}
		if act.Result != nil {
			a.logf("sync done: sent=%d recv=%d rounds=%d", act.Result.FilesSent, act.Result.FilesRecvd, act.Result.Rounds)
			slog.InfoContext(ctx, "sync done", "rounds", act.Result.Rounds, "sent", act.Result.FilesSent, "recv", act.Result.FilesRecvd, "errors", len(act.Result.Errors))
			for _, e := range act.Result.Errors {
				a.logf("sync warning: %s", e)
				slog.WarnContext(ctx, "sync protocol error", "detail", e)
			}
		}
	}
}
```

Also add logging to `New()`:

```go
	// After successful NATS connect:
	if cfg.Logger != nil {
		cfg.Logger("connected to NATS: " + cfg.NATSUrl)
	}
```

And to `Start()`:

```go
func (a *Agent) Start() error {
	a.logf("starting agent...")
	// ... existing code ...
}
```

- [ ] **Step 4: Verify builds and tests**

Run:
```bash
go build -buildvcs=false -tags ncruces ./leaf/...
go test -buildvcs=false -tags ncruces ./leaf/agent/ -count=1 -short
```
Expected: All pass. Existing tests don't set Logger, so it's nil → no-op.

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/config.go leaf/agent/agent.go
git commit -m "leaf: add CustomDialer and Logger to agent Config"
```

---

### Task 2: NATS WebSocket Config

**Files:**
- Modify: `deploy/nats.conf`

- [ ] **Step 1: Add WebSocket listener**

Append to `deploy/nats.conf`:

```
websocket {
    port: 8222
    no_tls: true
}
```

- [ ] **Step 2: Commit**

```bash
git add deploy/nats.conf
git commit -m "deploy: add NATS WebSocket listener on port 8222"
```

---

### Task 3: Wire Agent into Playground

**Files:**
- Modify: `spike/opfs-poc/main.go`
- Modify: `spike/opfs-poc/worker.js`

Replace manual sync with agent lifecycle.

- [ ] **Step 1: Add agent state and handlers to main.go**

Add import for wsdialer and agent packages:

```go
import (
	"github.com/dmestas/edgesync/leaf/agent"
	"github.com/dmestas/edgesync/leaf/wsdialer"
)
```

Add agent state variable:

```go
var currentAgent *agent.Agent
```

Add handler functions:

```go
func doStartAgent(natsWsURL string) {
	if currentAgent != nil {
		postError("agent already running")
		return
	}
	if !ensureRepo() {
		postError("no repo — clone first")
		return
	}

	// Derive NATS protocol URL from WebSocket URL for nats.go.
	// WSDialer handles the actual transport — nats.go just needs any valid URL.
	natsURL := "nats://browser"

	log(fmt.Sprintf("[agent] connecting to %s...", natsWsURL))

	a, err := agent.New(agent.Config{
		RepoPath:     repoPath,
		NATSUrl:      natsURL,
		CustomDialer: &wsdialer.WSDialer{URL: natsWsURL},
		PollInterval: 10 * time.Second,
		Push:         true,
		Pull:         true,
		Logger: func(msg string) {
			log("[agent] " + msg)
			// Post agent status updates to UI.
			postResult("agentLog", toJSON(map[string]any{"msg": msg}))
		},
	})
	if err != nil {
		postError(fmt.Sprintf("agent init failed: %v", err))
		return
	}

	if err := a.Start(); err != nil {
		postError(fmt.Sprintf("agent start failed: %v", err))
		return
	}

	currentAgent = a
	log("[agent] running — auto-sync every 10s")
	postResult("agentState", toJSON(map[string]any{"state": "running"}))
}

func doStopAgent() {
	if currentAgent == nil {
		postError("agent not running")
		return
	}
	if err := currentAgent.Stop(); err != nil {
		log(fmt.Sprintf("[agent] stop error: %v", err))
	}
	currentAgent = nil
	log("[agent] stopped")
	postResult("agentState", toJSON(map[string]any{"state": "stopped"}))
}

func doSyncNow() {
	if currentAgent == nil {
		postError("agent not running")
		return
	}
	currentAgent.SyncNow()
}
```

- [ ] **Step 2: Register JS callbacks in main()**

Add to the callback registration block:

```go
js.Global().Set("_startAgent", js.FuncOf(func(_ js.Value, args []js.Value) any {
	url := "ws://localhost:8222"
	if len(args) > 0 && args[0].String() != "" {
		url = args[0].String()
	}
	go doStartAgent(url)
	return nil
}))
js.Global().Set("_stopAgent", js.FuncOf(func(_ js.Value, _ []js.Value) any {
	go doStopAgent()
	return nil
}))
js.Global().Set("_syncNow", js.FuncOf(func(_ js.Value, _ []js.Value) any {
	go doSyncNow()
	return nil
}))
```

- [ ] **Step 3: Add agent message handlers to worker.js**

In `self.onmessage` switch, add:

```javascript
case "startAgent": _startAgent(msg.url || ""); break;
case "stopAgent":  _stopAgent(); break;
case "syncNow":    _syncNow(); break;
```

- [ ] **Step 4: Update spike/opfs-poc/go.mod**

Add leaf module dependency (resolved via go.work):

```
require github.com/dmestas/edgesync/leaf v0.0.0
```

- [ ] **Step 5: Verify WASM build**

Run:
```bash
cd /Users/dmestas/projects/EdgeSync/.worktrees/opfs-spike
GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/
```
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add spike/opfs-poc/main.go spike/opfs-poc/worker.js spike/opfs-poc/go.mod
git commit -m "spike: wire leaf agent into playground with WSDialer"
```

---

### Task 4: Update Playground UI

**Files:**
- Modify: `spike/opfs-poc/index.html`

- [ ] **Step 1: Update header and status strip**

Replace the URL input label/value from HTTP URL to NATS WebSocket URL:

```html
<input id="url" value="ws://localhost:8222" placeholder="NATS WebSocket URL" />
```

Add agent controls to the header:

```html
<button class="btn" id="agentBtn" onclick="toggleAgent()">Start Agent</button>
<button class="btn" id="syncNowBtn" onclick="cmd('syncNow')" disabled>Sync Now</button>
```

Add to status strip:

```html
<div class="metric"><span class="dot off" id="agentDot"></span> Agent: <span class="v" id="s-agent">off</span></div>
```

- [ ] **Step 2: Add JS handlers for agent state**

```javascript
let agentRunning = false;

function toggleAgent() {
    if (agentRunning) {
        cmd("stopAgent");
    } else {
        const url = document.getElementById("url").value;
        cmd("startAgent", { url });
    }
}
```

Add to `handleResult`:

```javascript
} else if (kind === "agentState") {
    agentRunning = data.state === "running";
    document.getElementById("agentBtn").textContent = agentRunning ? "Stop Agent" : "Start Agent";
    document.getElementById("agentDot").className = "dot " + (agentRunning ? "ok" : "off");
    document.getElementById("s-agent").textContent = agentRunning ? "running" : "off";
    document.getElementById("syncNowBtn").disabled = !agentRunning;
} else if (kind === "agentLog") {
    // Agent log messages already appear via the Logger callback.
    // Refresh status periodically when agent is running.
    if (agentRunning) {
        clearTimeout(window._statusTimer);
        window._statusTimer = setTimeout(() => cmd("status"), 2000);
    }
}
```

- [ ] **Step 3: Auto-start agent after clone**

In the `clone` result handler, after `cmd("status")`:

```javascript
// Auto-start agent after clone.
setTimeout(() => {
    const url = document.getElementById("url").value;
    cmd("startAgent", { url });
}, 1000);
```

- [ ] **Step 4: Verify full build and manual test**

Build: `GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces ./spike/opfs-poc/`

Manual test:
1. Start NATS with WebSocket: `nats-server -c deploy/nats.conf`
2. Start a native leaf on the same NATS: `/tmp/test-leaf-bin --repo /tmp/test-leaf.fossil --nats nats://localhost:4222`
3. Start playground: `cd spike/opfs-poc && go run -buildvcs=false -tags ncruces . -target http://localhost:9090 -repo /tmp/test-leaf.fossil`
4. Clone → agent auto-starts → connects to NATS via WebSocket
5. Wait 10s → auto-sync fires → log shows "sync done: sent=0 recv=0"
6. Edit a file on the native leaf → browser receives on next sync
7. Edit in browser → commit → next sync pushes to native leaf
8. Click "Sync Now" → immediate sync

- [ ] **Step 5: Commit**

```bash
git add spike/opfs-poc/index.html
git commit -m "spike: browser leaf agent UI — start/stop, sync now, status"
```
