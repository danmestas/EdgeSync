# Leaf Agent

A daemon that syncs a local Fossil repo via NATS messaging and/or direct peer-to-peer over iroh.

## Overview

The leaf agent opens a Fossil repository, runs an embedded NATS server, and syncs artifacts on a poll loop. It implements both the client and server sides of Fossil's sync protocol — a stock `fossil clone`/`fossil sync` can talk to it directly via HTTP.

## Transport Modes

| Mode | Transport | Infrastructure | Use case |
|------|-----------|---------------|----------|
| Bridged | NATS → Bridge → HTTP | External NATS + Fossil server | Production with central server |
| Leaf-to-leaf (NATS) | NATS subject request/reply | External NATS | Peer-to-peer via shared NATS |
| Leaf-to-leaf (HTTP) | HTTP `/xfer` | None | Direct sync, `fossil clone` compat |
| Leaf-to-leaf (iroh) | NATS over QUIC tunnel | iroh sidecar | P2P with NAT traversal, no central infra |

## Flags

| Flag | Env Var | Default | Description |
|------|---------|---------|-------------|
| `--repo` | `LEAF_REPO` | (required) | Path to `.fossil` repo file |
| `--nats` | `LEAF_NATS_URL` | `""` | Optional upstream NATS URL (embedded server joins as leaf) |
| `--nats-client-port` | `LEAF_NATS_CLIENT_PORT` | `0` | Embedded NATS client listener port (`0` chooses a random localhost port) |
| `--nats-role` | | `peer` | Mesh role: `peer`, `hub`, or `leaf` |
| `--poll` | | `5s` | Poll interval |
| `--user` | `LEAF_USER` | (anonymous) | Sync user |
| `--password` | `LEAF_PASSWORD` | (empty) | Sync password |
| `--push` / `--no-push` | | `true` | Enable pushing local artifacts |
| `--pull` / `--no-pull` | | `true` | Enable pulling remote artifacts |
| `--serve-http` | `LEAF_SERVE_HTTP` | (disabled) | HTTP listen address for `/xfer` + `/healthz` |
| `--serve-nats` | | `false` | Enable NATS request/reply listener |
| `--uv` | | `false` | Sync unversioned files (wiki, forum, attachments) |
| `--prefix` | | `fossil` | NATS subject prefix |
| `--iroh` | `LEAF_IROH` | `false` | Enable iroh sidecar for P2P |
| `--iroh-peer` | | (none) | Remote EndpointId (repeatable) |
| `--iroh-key` | `LEAF_IROH_KEY` | `<repo>.iroh-key` | Ed25519 keypair path |
| `--verbose` / `-v` | | `false` | Verbose logging |

## Embedded NATS

Every agent runs an in-process NATS server on a random localhost port. The agent's NATS client connects to this local server. This is transparent — the agent doesn't need to know whether peers are reached via external NATS, iroh tunnels, or both.

If `--nats` is provided, the embedded server joins the external NATS as a leaf node. Messages flow between external and embedded NATS automatically.

## Connecting Two Nodes over iroh

iroh enables peer-to-peer sync over QUIC with automatic NAT traversal. No central NATS server or VPN needed — just two machines with internet access.

### Prerequisites

- Both machines have the `leaf` and `iroh-sidecar` binaries
- Both repos have matching `project-code` and `server-code`

### Step 1: Start both agents

On machine A:
```bash
leaf --repo project.fossil --iroh --serve-nats --poll 5s
```

On machine B:
```bash
leaf --repo project.fossil --iroh --serve-nats --poll 5s
```

Both agents will start their iroh sidecars and log their EndpointIds:
```
iroh sidecar ready, endpoint_id=abc123def456...
```

### Step 2: Exchange EndpointIds

Copy each machine's EndpointId and restart with `--iroh-peer`:

On machine A:
```bash
leaf --repo project.fossil --iroh --serve-nats \
     --iroh-peer <machine-B-endpoint-id>
```

On machine B:
```bash
leaf --repo project.fossil --iroh --serve-nats \
     --iroh-peer <machine-A-endpoint-id>
```

The agents will establish a NATS leaf node tunnel over iroh QUIC. Sync happens automatically on the next poll cycle.

### How it works

```
Machine A                              Machine B
┌─────────────────┐                   ┌─────────────────┐
│ Leaf Agent      │                   │ Leaf Agent      │
│  ↕ nats://      │                   │  ↕ nats://      │
│ Embedded NATS   │                   │ Embedded NATS   │
│  ↕ TCP          │                   │  ↕ TCP          │
│ iroh sidecar    │◄═══ QUIC ════════►│ iroh sidecar    │
│ (NAT traversal) │   (encrypted)     │ (NAT traversal) │
└─────────────────┘                   └─────────────────┘
```

1. Each agent runs an embedded NATS server
2. The iroh sidecar tunnels NATS leaf node connections over QUIC
3. The lower EndpointId peer initiates the tunnel (deterministic, no config needed)
4. NATS handles message routing, subscriptions, and reconnection automatically
5. Sync happens over NATS subjects — the agent's `NATSTransport` doesn't know it's going over iroh

### Roles

For simple two-node sync, the default `peer` role works. For more complex topologies:

- **`--nats-role hub`** — Dedicated server that only accepts connections. Use when one machine is always-on and others connect to it.
- **`--nats-role leaf`** — Lightweight client that always solicits outward. Use for WASM browsers or ephemeral machines.
- **`--nats-role peer`** (default) — Both accepts and solicits. The peer with the lexicographically lower EndpointId initiates the connection.

### With an existing NATS server

You can combine iroh P2P with an external NATS server. The embedded NATS joins the external server as a leaf:

```bash
leaf --repo project.fossil --iroh --serve-nats \
     --nats nats://central-server:4222 \
     --iroh-peer <peer-endpoint-id>
```

This gives you:
- Central NATS for peers that can reach it directly
- iroh tunnels for peers behind NAT or firewalls
- All peers see each other through the NATS leaf node graph

## Architecture

```
leaf/
  cmd/leaf/main.go        CLI entry point, flag parsing, signal handling
  agent/
    config.go             Config struct with defaults and validation
    agent.go              Agent: New/Start/Stop/SyncNow, poll loop
    nats_mesh.go          NATSMesh: embedded NATS + iroh tunnel lifecycle
    nats.go               NATSTransport implementing libfossil.Transport
    iroh.go               IrohTransport for direct xfer-over-QUIC
    sidecar.go            iroh-sidecar process management
    serve_http.go         HTTP server (/healthz + /xfer)
    serve_nats.go         NATS request/reply handler
    peer_registry.go      Peer discovery metadata table
```

## What It Does NOT Do

- No web UI
- No multi-repo support (one agent per repo)
- No filesystem watching (polls SQLite tables)
- No peer discovery (manual `--iroh-peer` config for now)
- No JetStream persistence (request/reply only)
