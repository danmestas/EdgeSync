# Iroh P2P Transport Design

**Date:** 2026-03-26
**Status:** Draft
**Linear:** TBD

## Summary

Add iroh as a peer-to-peer transport option for leaf-to-leaf sync. Iroh provides NAT traversal (holepunching + relay fallback) over QUIC, addressed by stable cryptographic identity (Ed25519 public keys) rather than IP addresses. Two leaves behind NATs can sync directly without requiring NATS or any central infrastructure.

## Motivation

EdgeSync currently supports two transports: HTTP (Fossil-compatible) and NATS (pub/sub messaging). Both require reachable infrastructure — an HTTP endpoint or a NATS server. For field deployments, disconnected peers, or mesh topologies where central infrastructure is unavailable or undesirable, a true peer-to-peer transport is needed.

Iroh was chosen over raw libp2p because it provides a focused, batteries-included networking stack (QUIC, holepunching, relay, discovery) without the complexity of libp2p's full protocol negotiation layer.

## Architecture

```
┌─────────────────────────────────────┐
│           Leaf Agent (Go)           │
│                                     │
│  ┌─────────┐ ┌──────────────────┐   │
│  │ pollLoop│ │  IrohTransport   │   │
│  │ Sync()  │→│ (HTTP to socket) │   │
│  └─────────┘ └────────┬─────────┘   │
│                        │ POST /exchange/{endpoint-id}
│              ┌─────────▼─────────┐   │
│              │  iroh-sidecar     │   │
│              │  (Rust binary)    │   │
│              │                   │   │
│              │  HTTP on Unix sock│   │
│              │  Iroh Endpoint    │   │
│              │  QUIC streams     │   │
│              └─────────┬─────────┘   │
└────────────────────────┼─────────────┘
                         │ QUIC (holepunched or relayed)
                         ▼
                    Remote Peer
```

### Design Decisions

- **Sidecar process, not cgo:** Iroh is Rust-only with no official Go bindings. A sidecar keeps the Go codebase pure — no Rust toolchain in the Go build, no cgo cross-compilation issues, clean process boundary. The Go agent spawns and manages the sidecar as a child process.
- **HTTP over Unix socket:** The sidecar exposes a local HTTP API. This matches the existing `HTTPTransport` pattern, is trivial to mock in tests, and allows debugging with curl.
- **No changes to go-libfossil:** The iroh transport lives entirely in `leaf/`. The core library remains transport-agnostic.
- **Manual peer config with DNS discovery:** Peers are configured by EndpointId. Iroh's built-in DNS discovery resolves EndpointIds to current addresses. No infrastructure to run. n0's free public relays handle fallback when holepunching fails.

## Sidecar Binary

### Location

`iroh-sidecar/` at the repo root — a standalone Rust crate, not part of the Go workspace.

### Dependencies

- `iroh` — endpoint, connections, DNS discovery
- `axum` or `hyper` — HTTP server on Unix socket
- `tokio` — async runtime
- `clap` — CLI args

### CLI Interface

```
iroh-sidecar \
  --socket /tmp/iroh-<pid>.sock \
  --key-path ./data/iroh-key \
  --callback http://127.0.0.1:8080 \
  --alpn /edgesync/xfer/1
```

| Flag | Purpose |
|------|---------|
| `--socket` | Unix socket path for the HTTP API (Go agent generates a unique path per instance) |
| `--key-path` | Path to persist/load the Ed25519 keypair |
| `--callback` | URL to forward incoming peer requests to (the Go agent's HTTP handler) |
| `--alpn` | ALPN protocol identifier for iroh connections |

### Connection Management

The sidecar maintains a `HashMap<EndpointId, Connection>` — reuses QUIC connections across sync rounds. Connections are evicted on close or error. New connections are established on demand via `endpoint.connect()`.

## Sidecar HTTP API

### `POST /exchange/{endpoint-id}`

Send an xfer round to a remote peer.

- **Request body:** xfer-encoded bytes (zlib compressed, same as Fossil wire format)
- **Response body:** xfer-encoded bytes from the remote peer
- **Flow:** sidecar connects to the remote peer (by EndpointId), opens a bidirectional QUIC stream on ALPN `/edgesync/xfer/1`, writes the request, reads the response, returns it
- **Connection reuse:** subsequent exchanges with the same peer reuse the cached QUIC connection

### `GET /status`

Health check and peer info.

- **Response:** `{ "endpoint_id": "...", "peers": [...], "relay_url": "..." }`
- Used by the Go agent for startup readiness checks and logging the local EndpointId

### `POST /shutdown`

Graceful shutdown — close all connections, unbind the endpoint, exit.

### Incoming Connections (Serving Side)

When a remote peer opens a QUIC stream on ALPN `/edgesync/xfer/1`:

1. Sidecar accepts the stream, reads the request bytes
2. Sidecar forwards the request to the Go agent via HTTP POST to the `--callback` URL (the same `ServeHTTP` / `XferHandler` that handles Fossil HTTP clients)
3. Sidecar reads the response from the Go agent, writes it back to the QUIC stream

This reverse-proxy pattern means `HandleSync` works for iroh peers exactly as it does for HTTP and NATS — no new server-side code in `go-libfossil/`.

## Go Integration

### New Files

| File | Purpose |
|------|---------|
| `leaf/agent/iroh.go` | `IrohTransport` implementing `sync.Transport`, sidecar process management (spawn, health check, shutdown) |
| `leaf/agent/serve_iroh.go` | Wiring for incoming iroh connections (reverse-proxy path to `HandleSync`) |

### IrohTransport

```go
// IrohTransport implements sync.Transport over the iroh sidecar.
type IrohTransport struct {
    socketPath string // Unix socket to the sidecar
    endpointID string // remote peer's EndpointId
    client     *http.Client
}

func (t *IrohTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
    // Encode xfer message
    // POST to http://unix/exchange/{endpointID} via Unix socket
    // Decode and return response
}
```

Structurally identical to `HTTPTransport` — POST bytes, get bytes back. The only difference is the URL path includes the remote EndpointId, and the HTTP client dials a Unix socket instead of TCP.

### Config Additions

```go
// IrohEnabled starts the iroh sidecar for peer-to-peer sync.
IrohEnabled bool

// IrohPeers is a list of remote EndpointIds to sync with.
IrohPeers []string

// IrohKeyPath is the path to the persistent Ed25519 keypair.
// Defaults to "<repo-dir>/.iroh-key".
IrohKeyPath string
```

### CLI Flags

| Flag | Type | Description |
|------|------|-------------|
| `--iroh` | bool | Enable the iroh sidecar |
| `--iroh-peer` | string (repeatable) | Remote EndpointId to sync with |
| `--iroh-key` | string | Path to iroh keypair file |

### Agent Lifecycle

**`Start()`:**
1. If `IrohEnabled`, resolve the sidecar binary path (same directory as the leaf binary)
2. Generate a unique Unix socket path: `/tmp/iroh-<pid>.sock`
3. Spawn `iroh-sidecar` as a child process with `--socket`, `--key-path`, `--callback` flags
4. Poll `GET /status` until the sidecar is ready (timeout after 10s)
5. Log the local EndpointId from the status response

**`pollLoop`:**
- For each configured iroh peer, create an `IrohTransport{socketPath, endpointID}` and call `Sync()` — same as the NATS sync path

**`Stop()`:**
1. `POST /shutdown` to the sidecar
2. Wait up to 5s for the process to exit
3. `SIGTERM` if still running, then `SIGKILL` after 2s
4. Remove the Unix socket file

## Peer Discovery

**Phase 1 (this spec):** Manual configuration only. Peers are specified by EndpointId via `--iroh-peer` flags or config. Iroh's built-in DNS discovery (queries `dns.iroh.link`) resolves EndpointIds to current network addresses. n0's free public relays handle NAT traversal fallback.

**Future phases (not in scope):**
- NATS-assisted discovery (peers announce EndpointIds on a NATS subject)
- mDNS for local network discovery
- Self-hosted relay and DNS discovery servers

## Testing

### Unit Tests (Go)

- `IrohTransport` tested with a mock HTTP server on a Unix socket — no real sidecar needed. Verifies the `Exchange` method encodes/decodes correctly and handles errors.
- Sidecar process management tested with a stub binary that responds to `/status` and `/shutdown`. Verifies spawn, readiness polling, and graceful shutdown.

### Integration Tests (sim/)

- New test: two leaf agents with `IrohEnabled`, each spawning a real sidecar, syncing blobs peer-to-peer on localhost. Verifies convergence the same way existing NATS leaf-to-leaf tests do.
- Sidecar processes run on localhost, so holepunching is trivial — this tests the full plumbing without needing real NAT conditions.

### DST

- No DST integration for the iroh transport. The sidecar is an external process that doesn't fit the deterministic single-threaded model.
- Iroh transport under DST uses a `MockTransport` backed by `HandleSync` — same as today's peer network tests. The transport layer is already proven correct; DST tests the sync protocol logic.

### Out of Scope for Testing

- Real NAT traversal and relay fallback (requires multiple networks)
- Iroh connection failure modes under hostile conditions

## Deployment

### Local Development

`make iroh-sidecar` builds the Rust binary into `bin/`. Requires Rust toolchain. The Go build has no dependency on the sidecar — iroh is opt-in via `--iroh`.

### Docker

The `deploy/Dockerfile` gets an additional Rust builder stage:

```dockerfile
FROM rust:1-bookworm AS iroh-builder
WORKDIR /src
COPY iroh-sidecar/ ./iroh-sidecar/
RUN cargo build --release --manifest-path iroh-sidecar/Cargo.toml
```

The final stage copies the sidecar binary alongside the leaf binary.

### Hetzner VPS

No changes to the existing NATS-based deployment. Iroh is an additional transport. A leaf can run NATS (for server-connected sync) and iroh (for direct peer sync) simultaneously.

### Binary Distribution

The sidecar is a ~5-10MB static Rust binary. Pre-built binaries via GitHub releases is a follow-on concern.

## Scope

| Component | Change |
|-----------|--------|
| `iroh-sidecar/` | New Rust crate — HTTP-over-Unix-socket proxy to iroh QUIC |
| `leaf/agent/iroh.go` | `IrohTransport` + sidecar process management |
| `leaf/agent/serve_iroh.go` | Incoming connection reverse-proxy wiring |
| `leaf/agent/config.go` | 3 new config fields |
| `leaf/cmd/leaf/` | 3 new CLI flags |
| `Makefile` | `iroh-sidecar` build target |
| `sim/` | Integration test for iroh peer-to-peer sync |

### Not Changed

- `go-libfossil/` — no changes, stays transport-agnostic
- `bridge/` — no changes
- `dst/` — no changes (iroh tested via MockTransport in DST)
- Existing HTTP and NATS transports — unchanged, iroh is additive

## Open Questions

None — all decisions resolved during design review.
