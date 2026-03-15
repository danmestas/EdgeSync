# Leaf Agent

A daemon process that sits next to a Fossil repo and keeps it synced over NATS.

## Overview

The leaf agent watches a local Fossil repository for new artifacts and syncs them to a remote Fossil server through a NATS message bus. It implements the client side of Fossil's sync protocol without needing the `fossil` binary installed.

## How It Works

```
fossil commit → repo SQLite updated → leaf polls unsent/unclustered tables
    → sync.Sync() via NATSTransport → NATS request/reply → bridge → fossil server
```

1. **Opens a Fossil repo** via `repo.Open(path)` — reads `project-code` and `server-code` from the SQLite config table
2. **Connects to NATS** at the configured URL
3. **Runs a poll loop** — every N seconds (default 5s):
   - Queries `unsent` and `unclustered` tables for new local artifacts
   - If anything found (or pull is enabled), calls `sync.Sync()` through the NATS transport
4. **Syncs via NATS request/reply** — encodes an xfer Message, publishes to `<prefix>.<project-code>.sync`, waits for the bridge's reply, decodes the response
5. **Handles signals**:
   - `SIGUSR1` — triggers an immediate sync (skips the poll timer)
   - `SIGINT` / `SIGTERM` — graceful shutdown (stops loop, closes NATS, closes repo)

## Usage

```bash
leaf-agent --repo /path/to/repo.fossil \
           --nats nats://localhost:4222 \
           --poll 5s \
           --user anonymous \
           --push \
           --pull
```

### Flags

| Flag | Env Var | Default | Description |
|------|---------|---------|-------------|
| `--repo` | `LEAF_REPO` | (required) | Path to `.fossil` repo file |
| `--nats` | `LEAF_NATS_URL` | `nats://localhost:4222` | NATS server URL |
| `--poll` | — | `5s` | Poll interval |
| `--user` | `LEAF_USER` | `anonymous` | Sync user |
| `--password` | `LEAF_PASSWORD` | (empty) | Sync password |
| `--push` | — | `true` | Enable pushing local artifacts |
| `--pull` | — | `true` | Enable pulling remote artifacts |
| `--prefix` | — | `fossil` | NATS subject prefix |

### Manual Sync

```bash
# Trigger immediate sync without waiting for poll
kill -USR1 $(pgrep leaf-agent)
```

## Architecture

```
leaf/
  cmd/leaf/main.go      CLI entry point, flag parsing, signal handling
  agent/
    config.go            Config struct with defaults and validation
    agent.go             Agent: New/Start/Stop/SyncNow, poll loop
    nats.go              NATSTransport implementing sync.Transport
```

### NATSTransport

Implements `sync.Transport` over NATS request/reply:

```go
type NATSTransport struct {
    conn    *nats.Conn
    subject string        // "<prefix>.<project-code>.sync"
    timeout time.Duration // default 30s
}

func (t *NATSTransport) Exchange(ctx, req) (*xfer.Message, error) {
    // 1. req.Encode() → zlib bytes with 4-byte size prefix
    // 2. conn.RequestWithContext(ctx, subject, bytes)
    // 3. xfer.Decode(reply.Data) → *xfer.Message
}
```

### Poll Loop

```go
for {
    select {
    case <-ctx.Done():
        return  // shutdown
    case <-time.After(pollInterval):
        doSync()
    case <-syncNow:
        doSync()  // SIGUSR1 triggered
    }
}
```

The poll goroutine is the sole executor of `doSync()` — `SyncNow()` only sends on a buffered channel, avoiding concurrent SQLite access.

### doSync

1. **Optimization check** — if `unsent` and `unclustered` tables are both empty and pull is disabled, skip this round
2. Call `sync.Sync(ctx, repo, transport, opts)`
3. Log results (rounds, files sent/received, errors)
4. On error: log and continue (don't crash)

## What It Does NOT Do

- No web UI or HTTP server
- No multi-repo support (one agent per repo)
- No JetStream persistence (request/reply only — JetStream is a future upgrade)
- No peer discovery or P2P sync
- No filesystem watching (polls SQLite tables, not the filesystem)
- No authentication computation (uses `nobody` permissions or sends login card if user/password provided)

## Dependencies

- `go-libfossil` — sync engine, repo access, xfer codec
- `github.com/nats-io/nats.go` — NATS client
- `github.com/nats-io/nats-server/v2` — embedded NATS server (test only)

## Relationship to Bridge

The leaf agent sends sync requests over NATS. It does not know or care what's on the other end. In production, a [bridge](bridge.md) subscribes to those requests and proxies them to a Fossil HTTP server. In tests, a mock subscriber plays the same role.
