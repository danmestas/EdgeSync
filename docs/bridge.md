# Bridge

A proxy that translates between NATS messages and Fossil's HTTP `/xfer` sync protocol.

## Overview

The bridge subscribes to a NATS subject and forwards sync requests to a Fossil HTTP server. To the leaf agent, it looks like a sync peer. To the Fossil server, it looks like a normal Fossil client doing push/pull. Neither side knows the other exists.

## How It Works

```
Leaf agent → NATS request → Bridge → HTTP POST → Fossil server
Leaf agent ← NATS reply   ← Bridge ← HTTP response ← Fossil server
```

1. **Connects to NATS** at the configured URL
2. **Subscribes** to `<prefix>.<project-code>.sync`
3. **For each incoming message:**
   - Decodes the NATS payload as an xfer Message (`xfer.Decode`)
   - Forwards it to the Fossil server via `HTTPTransport.Exchange()` — HTTP POST to the repo root with `Content-Type: application/x-fossil`
   - Encodes the Fossil server's response (`response.Encode`)
   - Replies to the leaf via NATS (`msg.Respond`)
4. **Error handling** — if decode or HTTP fails, responds with an empty xfer Message (never leaves the leaf hanging with no reply)
5. **Graceful shutdown** — `SIGINT`/`SIGTERM` unsubscribes, drains the NATS connection, closes

## Usage

```bash
bridge --fossil http://fossil-server:8080 \
       --project abc123def456 \
       --nats nats://localhost:4222 \
       --prefix fossil
```

### Flags

| Flag | Env Var | Default | Description |
|------|---------|---------|-------------|
| `--fossil` | `BRIDGE_FOSSIL_URL` | (required) | Fossil HTTP server URL |
| `--project` | `BRIDGE_PROJECT_CODE` | (required) | Project code (determines NATS subject) |
| `--nats` | `BRIDGE_NATS_URL` | `nats://localhost:4222` | NATS server URL |
| `--prefix` | `BRIDGE_PREFIX` | `fossil` | NATS subject prefix |

### Finding the Project Code

The project code is stored in the Fossil repo's SQLite database:

```bash
fossil sql -R /path/to/repo.fossil "SELECT value FROM config WHERE name='project-code'"
```

The leaf agent and bridge must use the same project code for a given repo.

## Architecture

```
bridge/
  cmd/bridge/main.go     CLI entry point, flag parsing, signal handling
  bridge/
    config.go             Config struct with defaults and validation
    bridge.go             Bridge: New/Start/Stop, NATS subscriber, HTTP proxy
```

### Message Handler

```go
func (b *Bridge) handleMessage(msg *nats.Msg) {
    // 1. Decode NATS payload as xfer Message
    req, err := xfer.Decode(msg.Data)
    if err != nil {
        respondEmpty(msg)  // don't leave leaf hanging
        return
    }

    // 2. Forward to Fossil server via HTTP
    resp, err := HTTPTransport{URL: fossilURL}.Exchange(ctx, req)
    if err != nil {
        respondEmpty(msg)
        return
    }

    // 3. Encode and reply
    data, _ := resp.Encode()
    msg.Respond(data)
}
```

### HTTP Protocol Details

The bridge uses `sync.HTTPTransport` which speaks Fossil's wire format:

- **URL**: POST to the Fossil server's root URL (not `/xfer`)
- **Content-Type**: `application/x-fossil` — Fossil routes to the xfer handler based on this header
- **Body format**: 4-byte big-endian uncompressed size prefix + zlib-compressed xfer cards
- **Response**: same format (4-byte prefix + zlib)

## What It Does NOT Do

- No authentication/authorization on the NATS side (use NATS NKeys/JWT for that)
- No request transformation, filtering, or inspection
- No caching of artifacts or responses
- No multi-project support (one bridge instance per project/repo)
- No retry logic (the leaf's sync loop handles retries by running another round)
- No connection pooling to the Fossil server (creates a new HTTP request per round)

## Deployment

### Single Repo

```
┌─────────┐     ┌──────┐     ┌────────┐     ┌────────────────┐
│ Leaf    │────▶│ NATS │────▶│ Bridge │────▶│ Fossil Server  │
│ Agent   │◀────│      │◀────│        │◀────│ (fossil server)│
└─────────┘     └──────┘     └────────┘     └────────────────┘
```

### Multiple Repos

Run one bridge per repo, each subscribing to a different project code:

```bash
bridge --fossil http://fossil:8080 --project <project-code-1> &
bridge --fossil http://fossil:8080 --project <project-code-2> &
```

Multiple leaf agents can share the same NATS server — each publishes to its own project-specific subject.

### Multiple Leaves

```
┌─────────┐
│ Leaf A  │──┐
└─────────┘  │    ┌──────┐     ┌────────┐     ┌────────┐
             ├───▶│ NATS │────▶│ Bridge │────▶│ Fossil │
┌─────────┐  │    └──────┘     └────────┘     └────────┘
│ Leaf B  │──┘
└─────────┘
```

All leaves for the same project share the same NATS subject. The bridge handles requests sequentially (one at a time per subscription).

## Dependencies

- `libfossil` — xfer codec, sync.HTTPTransport
- `github.com/nats-io/nats.go` — NATS client
- `github.com/nats-io/nats-server/v2` — embedded NATS server (test only)

## Relationship to Leaf Agent

The bridge is the other half of the [leaf agent](leaf-agent.md). The leaf produces sync requests over NATS; the bridge consumes them and proxies to HTTP. They communicate through a shared NATS subject (`<prefix>.<project-code>.sync`) and must agree on the subject prefix.

## Future Considerations

- **JetStream persistence** — if the bridge is offline, leaf requests are lost. JetStream would queue them for replay on reconnect.
- **Multi-project** — a single bridge process handling multiple projects via config file.
- **NATS auth** — NKeys/JWT to control which leaves can push to which projects.
- **Connection pooling** — reuse HTTP connections to the Fossil server.
