---
title: Architecture
weight: 20
---

<svg viewBox="0 0 640 280" role="img" aria-labelledby="es-arch-title es-arch-desc" preserveAspectRatio="xMidYMid meet" style="display:block;width:100%;height:auto;margin:0 auto 1.5rem;color:currentColor;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;max-width:720px">
  <title id="es-arch-title">EdgeSync architecture</title>
  <desc id="es-arch-desc">Apps (cli, daemon, mobile) call the leaf agent, which embeds libfossil and a NATS server. The agent persists to a .fossil SQLite repo and exchanges sync rounds with peer leaves over NATS, optionally tunneled through iroh QUIC, with a bridge translating to HTTP for legacy Fossil servers.</desc>
  <style>
    .a-rule { stroke: currentColor; stroke-width: 1; opacity: 0.4; }
    .a-box  { fill: none; stroke: currentColor; stroke-width: 1; opacity: 0.7; }
    .a-head { font-size: 11px; letter-spacing: 1.4px; text-transform: uppercase; opacity: 0.65; }
    .a-item { font-size: 13px; }
    .a-flow line { stroke: #c63a0f; stroke-width: 1.5; fill: none; }
    .a-arrow { fill: #c63a0f; }
    .a-flow-label { font-size: 10px; fill: #c63a0f; letter-spacing: 0.6px; text-transform: uppercase; }
  </style>
  <defs>
    <marker id="es-arch-arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">
      <path class="a-arrow" d="M0,0 L10,5 L0,10 Z"/>
    </marker>
  </defs>
  <g class="a-rule">
    <line x1="40" y1="38" x2="160" y2="38"/>
    <line x1="240" y1="38" x2="400" y2="38"/>
    <line x1="480" y1="38" x2="600" y2="38"/>
  </g>
  <g class="a-head" fill="currentColor">
    <text x="100" y="28" text-anchor="middle">apps</text>
    <text x="320" y="28" text-anchor="middle">leaf agent</text>
    <text x="540" y="28" text-anchor="middle">mesh</text>
  </g>
  <g class="a-item" fill="currentColor">
    <text x="100" y="76" text-anchor="middle">cli</text>
    <text x="100" y="100" text-anchor="middle">daemon</text>
    <text x="100" y="124" text-anchor="middle">mobile</text>
  </g>
  <rect class="a-box" x="240" y="60" width="160" height="100"/>
  <g class="a-item" fill="currentColor">
    <text x="320" y="88" text-anchor="middle">libfossil</text>
    <text x="320" y="112" text-anchor="middle">NATS server</text>
    <text x="320" y="136" text-anchor="middle">notify</text>
  </g>
  <rect class="a-box" x="490" y="60" width="100" height="100"/>
  <g class="a-item" fill="currentColor">
    <text x="540" y="88" text-anchor="middle">peer</text>
    <text x="540" y="112" text-anchor="middle">iroh</text>
    <text x="540" y="136" text-anchor="middle">bridge</text>
  </g>
  <rect class="a-box" x="240" y="200" width="160" height="40"/>
  <g class="a-item" fill="currentColor">
    <text x="320" y="225" text-anchor="middle">.fossil (SQLite)</text>
  </g>
  <g class="a-flow">
    <line x1="160" y1="100" x2="234" y2="100" marker-end="url(#es-arch-arrow)"/>
    <line x1="400" y1="88" x2="486" y2="88" marker-end="url(#es-arch-arrow)"/>
    <line x1="490" y1="136" x2="404" y2="136" marker-end="url(#es-arch-arrow)"/>
    <line x1="320" y1="160" x2="320" y2="196" marker-end="url(#es-arch-arrow)"/>
  </g>
  <g class="a-flow-label">
    <text x="197" y="92" text-anchor="middle">calls</text>
    <text x="443" y="80" text-anchor="middle">sync</text>
    <text x="447" y="154" text-anchor="middle">events</text>
    <text x="334" y="183" text-anchor="start">persist</text>
  </g>
</svg>

## Overview

EdgeSync is a thin layer on top of [libfossil](https://github.com/danmestas/libfossil) that adds NATS-based sync, an embedded NATS server, optional iroh QUIC tunneling, and a bidirectional messaging channel. The leaf agent is the only required process; the bridge is optional.

## Module layout

EdgeSync is a Go workspace (`go.work`) with four modules:

- **Root (`./`)** — hosts `cmd/edgesync/` (the unified CLI binary), `sim/` (integration simulator with real NATS + TCP fault proxy), and `dst/` shims.
- **`leaf/`** — the leaf agent. `leaf/agent/` has the daemon (`agent.go`), config (`config.go`), the embedded NATS mesh (`nats_mesh.go`), HTTP serving (`serve_http.go`), and NATS-side sync listener (`serve_nats.go`). `leaf/agent/notify/` is the messaging subsystem.
- **`bridge/`** — the NATS-to-HTTP translator. `bridge/bridge/` defines `Config`, `New()`, `Start()`, `Stop()`. Used only when interoperating with an unmodified upstream Fossil server.
- **`dst/`** — deterministic simulation tests. `SimNetwork` (bridge mode), `PeerNetwork` (leaf-to-leaf). Shares `simio/` abstractions with libfossil.

The CLI in `cmd/edgesync/` embeds libfossil's `cli.RepoCmd` (38 Fossil-compatible subcommands) plus EdgeSync-specific commands: `sync start`, `sync now`, `bridge serve`, `notify {init,send,ask,watch,threads,log,status}`, `doctor`.

## Sync wire protocol

EdgeSync reuses Fossil's `/xfer` card protocol verbatim — same blobs, same `igot`/`gimme`/`file`/`cfile` cards, same UV (unversioned) sync. The only thing EdgeSync changes is the *transport*:

| | Upstream Fossil | EdgeSync |
| --- | --- | --- |
| Wire format | xfer cards | xfer cards (identical) |
| Encoding | zlib over POST body | zlib over NATS message payload |
| Endpoint | `POST /xfer` | NATS subject (request/reply) |
| Auth | login card | login card (same) |
| Stateless rounds | yes | yes |

This is why bridge mode works: each side is just `xfer` cards in a different envelope. The bridge re-frames between an HTTP body and a NATS message without re-parsing the cards themselves.

## Embedded NATS server

Every leaf agent runs its own NATS server in-process. The role flag selects topology:

- **`peer`** — full server, accepts and initiates connections to other peers
- **`hub`** — full server, accepts inbound leaf connections (NATS leafnode protocol)
- **`leaf`** — outbound only, connects to a hub

This means a developer can run two leaves on `localhost` with no broker installed. In production, one VPS typically runs as `hub` and edge devices run as `leaf`. See the [Browser WASM key findings](https://github.com/danmestas/EdgeSync) for the embedded-server constraints (notably `DontListen` + `nats.InProcessServer` for in-browser leaves).

## Iroh transport

Iroh is opt-in. When `--iroh` is set, the leaf agent:

1. Loads or generates a node key (`--iroh-key`)
2. Establishes QUIC streams to peers from `--iroh-peer` tickets
3. Multiplexes NATS leafnode protocol over those streams

This lets two leaves behind separate NATs talk to each other without a relay server's intermediation in steady state (relays are only used for hole-punching).

## Observability

The leaf agent and sim tests emit traces, metrics, and logs via OpenTelemetry. Telemetry is **optional** — when `OTEL_EXPORTER_OTLP_ENDPOINT` is unset, the OTel observer is nil and the no-op observer (zero-cost) is used. Secrets for OTel export are managed via [Doppler](https://doppler.com).

The observer pattern is libfossil's: implement `SyncObserver` and `CheckoutObserver`, hand the implementation to the agent at construction time. EdgeSync's `leaf/telemetry/observer.go` provides an OTel-backed implementation.

## Determinism

The `dst/` module runs many leaf agents under a deterministic event loop with a seeded PRNG and a `simio.SimClock`. `BUGGIFY` (probabilistic fault injection) gates failure paths in production code so the simulator can exercise them without changing the production code path. See [libfossil's testing docs](https://libfossil-docs.daniel-mestas.workers.dev/docs/testing/) for the seed sweep workflow — EdgeSync inherits it directly.
