# NATS-over-iroh: P2P Presence and Messaging

**Date:** 2026-04-08
**Target repo:** EdgeSync (agent + sidecar), no go-libfossil changes
**Linear:** EDG-59

## Problem

Iroh P2P transport today is sync-only — two peers exchange xfer payloads via HTTP through the sidecar. No presence, no broadcast, no peer discovery, no messaging. NATS provides all of these, but requires central infrastructure.

Running NATS leaf node connections over iroh QUIC streams combines iroh's NAT traversal with NATS's pub/sub, presence, and messaging. The agent already speaks NATS — no new application protocol needed.

## Design Decisions

**Embedded NATS always runs.** Every agent starts an in-process NATS server. If `--nats` URL is also provided, the embedded server joins external NATS as a leaf node. This unifies the architecture: the agent always connects to its local embedded NATS, regardless of whether peers are reached via iroh tunnels, external NATS, or both.

**Sidecar is a dumb TCP pipe.** The iroh sidecar gains a new ALPN (`/edgesync/nats-leaf/1`) and a TCP-to-QUIC tunnel. It has zero NATS protocol awareness — it pipes bytes between a local TCP socket (embedded NATS) and an iroh QUIC bidirectional stream. This supports the full NATS feature set: leaf node handshake, subscriptions, pub/sub, JetStream, etc.

**Role-based topology.** Each agent declares a NATS role that determines connection direction:

| Role | Accepts leaf connections | Solicits leaf connections | Use case |
|------|------------------------|--------------------------|----------|
| `peer` (default) | Yes | Yes (lower EndpointId solicits) | Native P2P mesh |
| `hub` | Yes | No | Dedicated server, cluster node |
| `leaf` | No | Yes (always solicits outward) | WASM browser, lightweight client |

When both sides are `peer`, the one with the lexicographically lower iroh EndpointId initiates the tunnel. This prevents duplicate connections deterministically with no negotiation.

**No changes to iroh upstream.** All Rust changes are in the EdgeSync-owned `iroh-sidecar/` binary. iroh is used as a library dependency only.

## Iroh Sidecar Changes

New module: `iroh-sidecar/src/tunnel.rs`

### New ALPN

`/edgesync/nats-leaf/1` registered alongside existing `/edgesync/xfer/1` at endpoint creation. The acceptor dispatches by ALPN — xfer connections go to the existing callback handler, NATS connections go to the tunnel handler.

### New CLI arg

`--nats-addr <host:port>` — address of the local embedded NATS leaf node port. If omitted, NATS tunneling is disabled (backwards compatible). Passed by the Go agent after starting the embedded NATS server.

### Inbound tunnel (peer connects to us)

1. Acceptor receives QUIC connection, matches NATS ALPN
2. `tunnel::handle_stream(stream, nats_addr)` opens TCP to local NATS
3. Spawns two tasks: QUIC read → TCP write, TCP read → QUIC write
4. Both run until either side closes or errors
5. Cleanup: close both sides on any error

### Outbound tunnel (we connect to peer)

New HTTP endpoint: `POST /nats-tunnel/{endpoint_id}`

1. Opens QUIC connection to remote peer (reuses connection cache) on NATS ALPN
2. Opens bidirectional stream
3. Opens TCP connection to local NATS (`nats_addr`)
4. Same bidirectional pipe as inbound
5. Returns 200 once tunnel is established
6. Tunnel is long-lived — runs in a spawned task, not blocking the HTTP response

### Acceptor dispatch

Modify `acceptor.rs` to match ALPN:

```rust
let alpn = connection.alpn();
if alpn == XFER_ALPN {
    // existing xfer callback handler
} else if alpn == NATS_ALPN {
    tunnel::handle_connection(connection, nats_addr).await;
} else {
    // unknown ALPN, close connection
}
```

### Files changed

| File | Change |
|------|--------|
| `iroh-sidecar/src/tunnel.rs` | New — bidirectional TCP-QUIC pipe |
| `iroh-sidecar/src/acceptor.rs` | ALPN dispatch (xfer vs NATS) |
| `iroh-sidecar/src/server.rs` | New endpoint `POST /nats-tunnel/{endpoint_id}` |
| `iroh-sidecar/src/main.rs` | New CLI arg `--nats-addr`, register second ALPN |
| `iroh-sidecar/Cargo.toml` | No new dependencies — tokio TCP + iroh QUIC already available |

## Go Agent Changes

### NATS Mesh Module

New file: `leaf/agent/nats_mesh.go`

Single module that owns both the embedded NATS server and tunnel establishment. This keeps lifecycle ordering constraints hidden inside one module instead of spread across the agent. Interface to the agent:

```go
type NATSMesh struct { ... }

func NewNATSMesh(cfg NATSMeshConfig, sidecar *Sidecar) *NATSMesh
func (m *NATSMesh) Start() (clientURL string, err error)
func (m *NATSMesh) Stop()
```

The agent calls `mesh.Start()` and gets back a NATS client URL. Everything else — embedded server config, leaf node ports, tunnel establishment, startup ordering — is internal to the mesh module.

**Embedded NATS configuration by role:**

- `peer` / `hub`: `LeafNode.Port` set to ephemeral port. This is the address passed to the sidecar via `--nats-addr`. Incoming iroh tunnels connect here.
- `leaf`: `LeafNode.Port` disabled. `LeafNode.Remotes` populated after tunnel establishment — embedded NATS solicits outward through the tunnel to a remote peer's NATS.
- All roles: if `NATSUpstream` URL provided, added to `LeafNode.Remotes` as an upstream (embedded NATS joins external cluster as a leaf).

**Tunnel establishment (internal to mesh):**

Called after sidecar is ready and EndpointIds are known. For each iroh peer:

```
if my_role == "leaf":
    POST /nats-tunnel/{peer_endpoint_id}  (I always solicit)
elif my_role == "hub":
    skip  (wait for them to connect)
elif my_role == "peer":
    if my_endpoint_id < peer_endpoint_id:
        POST /nats-tunnel/{peer_endpoint_id}  (I solicit)
    else:
        skip  (they will connect to me)
```

The sidecar handles the QUIC stream lifecycle. The mesh module only triggers establishment.

**Internal startup sequence** (hidden inside `mesh.Start()`):

1. Create embedded NATS server (role-based config)
2. Start embedded NATS
3. Start iroh sidecar (with `--nats-addr localhost:<leaf-port>`)
4. Wait for sidecar ready
5. Establish NATS tunnels to iroh peers (role + EndpointId logic)
6. Return `nats://127.0.0.1:<client-port>` to caller

The agent then connects its NATS client to the returned URL. Ordering constraints are encapsulated — the agent never sees steps 1-5.

### Transport simplification

With NATS-over-iroh, the `syncTargets` list collapses. Instead of iterating NATS + iroh targets separately, there's one `NATSTransport` connected to the local embedded server. Iroh peers are reached via NATS leaf node routing — not direct xfer exchange.

`NATSTransport` and `ServeNATS` work unchanged — they just talk to the local embedded server.

### Config changes

| Field | Change |
|-------|--------|
| `NATSRole` | New — `"peer"` (default), `"hub"`, `"leaf"` |
| `NATSUrl` | Renamed to `NATSUpstream`. No longer required. If set, embedded NATS joins it as a leaf |
| `IrohEnabled` | Still controls whether sidecar starts. Now also controls NATS tunnel establishment |
| `IrohPeers` | Still lists remote EndpointIds. Tunnel logic uses these for NATS connections too |

### Agent lifecycle (updated)

```
Start():
  1. mesh.Start()  → returns clientURL (mesh handles embedded NATS + sidecar + tunnels internally)
  2. Connect agent's NATS client to clientURL
  3. Start ServeNATS (subscribe to sync subject on embedded NATS)
  4. Start HTTP server (if configured)
  5. Start poll loop

Stop():
  1. Cancel poll loop
  2. Drain NATS client
  3. mesh.Stop()  (shuts down sidecar + embedded NATS internally)
  4. Close repo
```

### Failure handling

- **Tunnel drops** (QUIC stream closes): sidecar detects EOF, closes TCP side. NATS sees leaf node disconnect. Agent's NATS client has `MaxReconnects(-1)` — reconnect is automatic once tunnel is re-established.
- **Sidecar crash**: agent's liveness monitor detects exit. Can restart sidecar and re-establish tunnels.
- **No explicit tunnel health polling** — NATS's built-in disconnect/reconnect handles detection. Tunnel failure is defined out of existence from the agent's perspective.

### Files changed

| File | Change |
|------|--------|
| `leaf/agent/nats_mesh.go` | New — embedded NATS server, role-based config, tunnel establishment, startup sequencing |
| `leaf/agent/agent.go` | Simplified lifecycle: `mesh.Start()` / `mesh.Stop()`, single NATSTransport |
| `leaf/agent/config.go` | New field `NATSRole`, rename `NATSUrl` → `NATSUpstream` |
| `leaf/agent/sidecar.go` | Accept `--nats-addr` passthrough from mesh |

## What Does NOT Change

- **go-libfossil** — transport-agnostic, no changes
- **NATSTransport** (`leaf/agent/nats.go`) — still does request/reply on subjects, connects to localhost
- **ServeNATS** (`leaf/agent/serve_nats.go`) — still subscribes and handles sync requests
- **IrohTransport** (`leaf/agent/iroh.go`) — kept for backwards compat with peers that don't have embedded NATS. Deprecate once NATS-over-iroh is stable (follow-up ticket) — having two paths to reach iroh peers increases cognitive load
- **serve_http.go** — HTTP serving unchanged
- **Wire protocol** — same xfer cards, same NATS subjects
- **Bridge** — still works with external NATS (embedded NATS joins as leaf)
- **Existing `--iroh-peer` xfer path** — still works, tunnel is additive

## Testing

### Unit tests (Go)

- Embedded NATS starts, accepts client connections, shuts down cleanly
- Role config produces correct NATS server options (leaf port, remotes)
- Tunnel establishment: correct peers solicited based on role + EndpointId comparison
- `NATSRole` validation (only "peer", "hub", "leaf" accepted)

### Integration test

Two agents with `--nats-role peer`, iroh sidecars, no external NATS:
1. Seed blobs on agent A
2. Verify convergence on agent B
3. Proves: embedded NATS + iroh tunnel + sync over NATS subjects = working P2P

### Manual verification

- `nats sub '>'` on one peer's embedded server — messages from remote peer arrive
- Subscribe to `edgesync.presence.*`, publish heartbeats, confirm cross-tunnel visibility

## Scope Boundary

**In scope (this spike):**
- Iroh sidecar NATS tunnel (Rust)
- Embedded NATS server in agent (Go)
- Role-based topology (peer/hub/leaf)
- Tunnel establishment with EndpointId tie-breaking
- Integration test proving P2P sync over NATS-over-iroh

**Out of scope:**
- Presence protocol (subjects, heartbeat format, timeout semantics)
- Messaging/chat layer
- Peer discovery via NATS (replacing `--iroh-peer` manual config)
- JetStream configuration on embedded NATS
- WASM leaf node mode (future — uses same architecture, different config)
- Cluster/supercluster topologies (future — hub role enables this)
