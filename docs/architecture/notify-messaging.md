# Notify — Bidirectional Messaging

## Purpose

Bidirectional notification system for human-in-the-loop AI workflows. Claude (or any app) sends messages via CLI, user replies from Apple devices. Built entirely on EdgeSync's own stack — no third-party push services.

## Architecture

Three components, phased delivery:

| Component | Stack | Status |
|-----------|-------|--------|
| Go CLI (`edgesync notify`) | go-libfossil, nats.go, Kong | **Shipped (Phase 1)** |
| Hetzner hub relay | Existing leaf agent + new NATS subjects | Planned (Phase 2) |
| Expo app (iOS + macOS) | React Native, NATS-over-iroh | Planned (Phase 3) |

## Transport: Dual Delivery

Every message travels two paths:

| Path | Mechanism | Latency | Offline? |
|------|-----------|---------|----------|
| NATS real-time | Publish on `notify.<project>.<thread-short>` | Sub-second | No — requires connection |
| Fossil sync | Commit to `notify.fossil`, sync via xfer protocol | Seconds (poll interval) | Yes — catches up on reconnect |

Deduplication: receiver checks message `id`. NATS delivers first; Fossil sync is a no-op for already-seen messages. Dedup map capped at 10K entries with FIFO eviction.

## NATS Subject Hierarchy

```
notify.<project>.<thread-id>    # Per-thread messages
notify.<project>.ack            # Delivery receipts (Phase 2)
notify.<project>.presence       # Online/typing indicators (Phase 2)
notify.<project>.*              # All threads in a project (wildcard)
notify.>                        # Firehose
```

## Data Model

### Message Format (v1)

```json
{
  "v": 1,
  "id": "msg-<32hex>",
  "thread": "thread-<32hex>",
  "project": "edgesync",
  "from": "<iroh-endpoint-id>",
  "from_name": "claude-macbook",
  "timestamp": "2026-04-10T12:00:00Z",
  "body": "Build failed. Retry?",
  "priority": "action_required",
  "actions": [{"id": "retry", "label": "Retry"}],
  "reply_to": null,
  "action_response": false
}
```

| Field | Required | Notes |
|-------|----------|-------|
| `v` | Yes | Schema version, currently 1 |
| `id` | Yes | `msg-` + 32 hex chars from crypto/rand |
| `thread` | Yes | `thread-` + 32 hex. Short form = first 8 chars after prefix |
| `project` | Yes | Scopes messages to a project |
| `from` / `from_name` | Yes | iroh endpoint ID + human-readable label |
| `priority` | No | `info` (default), `action_required`, `urgent` |
| `actions` | No | Quick-response buttons (id + label pairs) |
| `reply_to` | No | Parent message ID for replies |
| `action_response` | No | `true` when body is a tapped action ID |
| `media` | No | Filenames referencing UV attachments (Phase 2) |

### Repo Layout

```
<project>/threads/<thread-short>/
  ├── <unix-timestamp>-<msg-short>.json
  └── ...
<project>/media/           # UV files (Phase 2)
```

`notify.fossil` is a dedicated Fossil repo created via `InitNotifyRepo()`. Managed entirely by go-libfossil — no `fossil` binary dependency. Messages are versioned checkins; media attachments use UV (unversioned files).

## Identity & Trust

iroh endpoint IDs (public keys) are the sole identity mechanism. No passwords, no tokens. Trust established by adding peer IDs to known-peers list on the hub. Device pairing flow planned for Phase 2.

## Package Structure (Phase 1)

```
leaf/agent/notify/
  message.go    — Message, Action, Priority types; NewMessage, NewReply
  store.go      — Free functions on *libfossil.Repo: InitNotifyRepo, CommitMessage,
                   ReadMessage, ListThreads, ReadThread
  pubsub.go     — Publish (free function), Subscriber type (dedup, wildcard)
  notify.go     — Service (Send, Watch, FormatWatchLine)
cmd/edgesync/
  notify.go     — 7 Kong commands: init, send, ask, watch, threads, log, status
```

**Design decisions (Ousterhout review):**
- No wrapper type around `*libfossil.Repo` — free functions avoid shallow pass-throughs
- No `Publisher` type — one function doesn't justify a type
- No `NewActionReply` — callers use `NewReply` + set `ActionResponse = true`
- Service holds `*libfossil.Repo` and `*nats.Conn` directly, doesn't own their lifecycle

**Store internals:** `store.go` queries Fossil's `filename`, `mlink`, `blob` tables directly and decompresses the blob format (4-byte BE size + zlib). This bypasses go-libfossil's public API because `r.ReadFile()` doesn't exist yet. If added, migrate.

## CLI Interface

```bash
edgesync notify init                         # Create notify.fossil
edgesync notify send --project P "body"      # Fire-and-forget
edgesync notify ask --project P "question?"  # Send + block for reply
edgesync notify watch --project P            # Stream incoming
edgesync notify threads --project P          # List threads
edgesync notify log --project P <thread>     # Thread history
edgesync notify status                       # Connection state
```

`ask` defaults to `--priority action_required`. Exit code 2 on timeout. `send`/`ask`/`watch` work in repo-only mode (no NATS) — NATS integration comes when the agent wires up the Service.

Watch output format (machine-parseable):
```
[2026-04-10T12:01:03Z] thread:a1b2c3d4 from:dan-iphone action:retry
[2026-04-10T12:01:15Z] thread:a1b2c3d4 from:dan-iphone text:also bump the version
```

## Device Pairing

Three pairing methods for adding devices to the mesh:

| Method | Mechanism | UX |
|--------|-----------|-----|
| QR Code (primary) | `edgesync notify pair --name "dan-iphone"` prints QR to terminal | Scan from app, connected in <5s |
| Token paste (fallback) | 12-char base32 token (`AXKF-9M2P-VR3T`), no ambiguous chars | Type/paste in app |
| Nearby discovery (future) | iroh mDNS on local network | Not in v1 |

**Token format:** `edgesync-pair://v1/<hub-iroh-endpoint-id>/<nats-addr>/<one-time-secret>`

**Security:** Single-use tokens, 10-minute expiry, hub stores only SHA-256 hash. Revocation via `edgesync notify unpair <name>`.

**Storage:** `_notify/devices.json` (device registry) and `_notify/pending_tokens.json` (pending tokens) in notify.fossil.

**Package structure:** `token.go` (generation, hashing, QR), `token_store.go` (`CreatePairingToken` deep function, `ValidateToken`), `device_registry.go` (`AddDevice`, `RemoveDevice`, `ListDevices`).

## Expo App (separate repo: edgesync-notify-app)

Go backend compiled via gomobile, localhost HTTP/SSE server, React Native (Expo) UI.

**Architecture:** Go `.xcframework` starts an HTTP server on `127.0.0.1:<random-port>`. React Native talks to it via `fetch` (request/response) and `EventSource` (SSE for real-time messages). No custom native bridge.

**Go server endpoints:** `/init`, `/status`, `/send`, `/threads`, `/thread/:id`, `/subscribe` (SSE), `/media/:filename`, `/pair`, `/stop`

**Server design (Ousterhout):** `Server` struct (not singleton), `Bridge.Routes()` co-locates handlers with routes, pass-through JSON (never redefine `notify.Message` types), `api.ts` is a deep module (SSE reconnection, error normalization, connection status tracking).

## Planned Phases

| Phase | Scope | Depends On |
|-------|-------|-----------|
| 1 — Go backend | CLI + repo + NATS pub/sub | **Done** |
| 2 — Device pairing | `pair`, `unpair`, `devices` commands + token infrastructure | **Done** |
| 3 — Expo Go server | Localhost HTTP/SSE wrapping notify.Service | **Done** |
| 4 — Delivery receipts | Ack subject, `delivered` output, `trace` command | Phase 1 |
| 5 — Sentry integration | Go SDK on CLI + hub | Phase 1 |
| 6 — Claude Code skill | Teaches Claude the CLI grammar | Phase 1 |
| 7 — Expo app UI | gomobile + React Native screens + pairing flow | Phases 1-3 |
| 8 — Sentry Expo | Crash reporting on devices | Phase 7 |

## Testing

26 tests across 4 test files + CLI end-to-end:

| Category | Count | What |
|----------|-------|------|
| Message format | 8 | Construction, reply, JSON round-trip, path/subject generation |
| Store operations | 4 | Init, commit, list threads, read thread |
| Blob decompression | 5 | Empty, short, valid, corrupted, truncated |
| NATS pub/sub | 3 | Publish/subscribe, wildcard, dedup |
| Service | 6 | Nil repo error, send, existing thread, watch, watch formatter |
| CLI e2e | 2 | Init, send + threads + status |

## Error Tracking

Sentry (Phase 4) — Go SDK on CLI and hub, Expo SDK on devices. Additive to existing Honeycomb OTel traces for sync operations.
