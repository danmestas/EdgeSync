# Browser Leaf Agent — Level 1 (Client via NATS WebSocket)

**Date:** 2026-03-21
**Branch:** `spike/opfs-vfs`
**Status:** Spike

## Goal

Replace manual clone/sync buttons with a real `leaf/agent.Agent` running inside browser WASM. The agent connects to a remote NATS server via WebSocket, opens the OPFS-backed repo, and auto-syncs on a poll interval — same behavior as a native leaf.

## Architecture

```
Browser Worker                          VPS / Local
┌─────────────────────┐     WebSocket    ┌──────────┐
│  leaf/agent.Agent   │◄───────────────►│   NATS   │
│  ├─ NATSTransport   │  WSDialer       │  Server  │
│  ├─ OPFS SQLite     │                 └────┬─────┘
│  ├─ poll loop       │                      │
│  └─ auto-sync       │               ┌─────┴──────┐
└─────────────────────┘               │ Native Leaf │
                                      │ or Bridge   │
                                      └─────────────┘
```

No embedded NATS. No serve role. Client-only sync.

## Changes

### 1. Agent Config: CustomDialer Field

`leaf/agent/config.go` — add `CustomDialer` field:

```go
type Config struct {
    // ... existing fields ...
    CustomDialer nats.CustomDialer // optional: overrides net.Dial for NATS connection
}
```

`leaf/agent/agent.go` — in `New()`, use CustomDialer when connecting to NATS:

```go
opts := []nats.Option{nats.Name("edgesync-leaf")}
if cfg.CustomDialer != nil {
    opts = append(opts, nats.SetCustomDialer(cfg.CustomDialer))
}
nc, err := nats.Connect(cfg.NATSUrl, opts...)
```

This is a production-quality change — no build tags, no hacks. Any caller can provide a custom dialer.

### 2. Agent Event Logger

The agent's `Start()` loop processes events silently. For the browser, we need visibility into what's happening. Add a `Logger` callback to Config:

```go
type Config struct {
    // ... existing fields ...
    Logger func(msg string) // optional: receives human-readable agent lifecycle messages
}
```

The agent calls `Logger` at key points:
- `"connecting to NATS..."` / `"connected to NATS"`
- `"sync started"` / `"sync complete: sent=N recv=N rounds=N"` / `"sync failed: error"`
- `"poll timer fired"` (only at debug level — maybe skip)
- `"stopping..."` / `"stopped"`

If Logger is nil, no logging. This keeps go-libfossil and leaf/ free of browser concerns.

### 3. Playground Integration

`spike/opfs-poc/main.go` — replace manual sync with agent lifecycle:

**Flow:**
1. User clicks Clone → runs clone + crosslink + materialize (as today)
2. After clone succeeds, automatically start the agent
3. Agent connects to NATS via WSDialer, begins auto-sync
4. UI shows agent state: connecting → connected → syncing → idle
5. User can stop/restart the agent
6. Manual "Sync Now" button triggers `agent.SyncNow()`

**Config wiring:**
```go
agent.Config{
    RepoPath:     repoPath,
    NATSUrl:      "nats://nats-server:4222",  // nats:// scheme, NOT ws://
    CustomDialer: &wsdialer.WSDialer{URL: "ws://nats-server:8222"},
    PollInterval: 10 * time.Second,
    Push:         true,
    Pull:         true,
    Logger:       func(msg string) { log("[agent] " + msg) },
}
```

Key detail: `NATSUrl` uses `nats://` scheme (nats.go protocol), `WSDialer.URL` uses `ws://` scheme (WebSocket transport). The CustomDialer replaces the TCP dial with WebSocket, but nats.go still speaks NATS protocol over the connection.

### 4. NATS WebSocket Listener

`deploy/nats.conf` — add WebSocket listener:

```
websocket {
    port: 8222
    no_tls: true
}
```

For local testing, start NATS with: `nats-server -c deploy/nats.conf`

### 5. UI Updates

**Status strip additions:**
- Agent state indicator: `off` / `connecting` / `connected` / `syncing`
- Last sync timestamp
- Sync count (total syncs since page load)

**Button changes:**
- Clone stays (one-time setup)
- "Sync" button becomes "Sync Now" (triggers immediate sync via agent)
- Add "Agent" toggle button: start/stop the agent
- Remove manual sync logic — agent handles everything

**Log panel:**
- Agent lifecycle messages appear in real-time via the Logger callback
- Color-coded: green for sync success, red for errors, dim for routine

### 6. Observability

All agent activity is visible through:

1. **Logger callback** — human-readable messages in the UI log panel
2. **Status strip** — real-time agent state (connecting/connected/syncing/idle)
3. **Sync results** — each auto-sync reports sent/received/rounds in the log
4. **Error visibility** — NATS disconnects, sync failures shown immediately
5. **Connection state** — NATS connection status handlers:

```go
nats.DisconnectErrHandler(func(nc *nats.Conn, err error) {
    cfg.Logger("NATS disconnected: " + err.Error())
})
nats.ReconnectHandler(func(nc *nats.Conn) {
    cfg.Logger("NATS reconnected")
})
```

## NATS URL Configuration

The playground needs to know:
- The NATS server address for nats.go protocol: `nats://host:4222`
- The WebSocket URL for the dialer: `ws://host:8222`

For the spike, the URL input in the header accepts the WebSocket URL. The `nats://` URL is derived by replacing `ws://` with `nats://` and adjusting the port. Or we use two input fields.

Simpler: single input field for `ws://host:8222`, derive NATS URL as `nats://host:4222`. Or just hardcode the NATS port offset for the spike.

## What This Proves

- Real leaf agent running in browser WASM
- NATS connectivity from browser via WebSocket
- Auto-sync without manual intervention
- Multiple browser tabs sync with each other (via shared NATS server)
- Agent lifecycle management in browser context

## Out of Scope

- Embedded NATS server (Level 2)
- Serve role (HTTP or NATS)
- NATS authentication / TLS
- Reconnection backoff tuning
- Observer/OTel integration (Logger is sufficient for spike)
