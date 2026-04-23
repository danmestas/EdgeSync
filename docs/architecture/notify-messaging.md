# Notify — Bidirectional Messaging

## Purpose

Bidirectional notification system for human-in-the-loop AI workflows. Claude (or any app) sends messages via CLI, user replies from Apple devices. Built entirely on EdgeSync's own stack — no third-party push services.

## Architecture

Three components, phased delivery:

| Component | Stack | Status |
|-----------|-------|--------|
| Go CLI (`edgesync notify`) | libfossil, nats.go, Kong | **Shipped (Phase 1)** |
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

`notify.fossil` is a dedicated Fossil repo created via `InitNotifyRepo()`. Managed entirely by libfossil — no `fossil` binary dependency. Messages are versioned checkins; media attachments use UV (unversioned files).

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

**Store internals:** `store.go` queries Fossil's `filename`, `mlink`, `blob` tables directly and decompresses the blob format (4-byte BE size + zlib). This bypasses libfossil's public API because `r.ReadFile()` doesn't exist yet. If added, migrate.

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

**Architecture:** Go `.xcframework` starts an HTTP server on `127.0.0.1:<random-port>`. React Native talks to it via `fetch` (request/response) and `EventSource` (SSE for real-time messages). No custom native bridge — standard HTTP only.

```
┌─────────────────────────────┐
│  React Native (Expo)        │  UI layer — screens, components, gestures
│  fetch + EventSource        │
├────── localhost HTTP ────────┤
│  Go Framework (gomobile)    │  Logic layer — notify.Service, NATS, iroh
│  HTTP server + SSE          │
│  libfossil + nats.go        │
└─────────────────────────────┘
```

### Go HTTP Server Endpoints

| Method | Path | Request | Response | Purpose |
|--------|------|---------|----------|---------|
| `POST` | `/init` | `{"hub","iroh_peers","device_name"}` | `{"status":"ok"}` | Configure and connect to hub |
| `GET` | `/status` | — | `{"connected":bool,"hub","peer_id"}` | Connection state |
| `POST` | `/send` | `{"project","body","priority","actions","thread"}` | `{"message":{...}}` | Send a message |
| `GET` | `/threads?project=X` | — | `[{thread summaries}]` | List threads |
| `GET` | `/thread/:id?project=X` | — | `[{messages}]` | Read thread history |
| `GET` | `/subscribe?project=X` | — | SSE stream | Real-time incoming messages |
| `GET` | `/media/:filename?project=X` | — | binary | Serve UV attachment |
| `POST` | `/pair` | `{"token":"AXKF-9M2P-VR3T"}` | `{"status":"ok","hub":"..."}` | Complete device pairing |
| `POST` | `/stop` | — | `{"status":"ok"}` | Disconnect and clean up |

### SSE Events

```
event: message
data: {"v":1,"id":"msg-abc","thread":"thread-xyz","body":"hello","priority":"urgent",...}

event: connected
data: {"hub":"nats://...","peer_id":"endpoint-abc"}

event: disconnected
data: {"reason":"hub unreachable"}
```

`SSEManager` tracks one client connection, queues messages when disconnected, replays on reconnect.

### Framework Initialization

gomobile exports three top-level functions callable from Swift:

| Function | Signature | Behavior |
|----------|-----------|----------|
| `Start` | `Start(dataDir string) int` | Starts HTTP server on random port, writes port to `<dataDir>/server-port`, returns port. Writes error to `<dataDir>/server-error` on failure. |
| `Stop` | `Stop()` | Shuts down HTTP server, disconnects NATS/iroh |
| `IsRunning` | `IsRunning() bool` | Health check |

`Start` is called in `AppDelegate.didFinishLaunchingWithOptions`; port returned to React Native as native constant. `Stop` called in `applicationWillTerminate`. Random port avoids hard-coded cross-language dependencies.

`/init` opens/creates `notify.fossil` in the app's document directory. Succeeds if repo opens even if NATS/iroh is temporarily unreachable — connection retries in background. `GET /status` reflects actual state. (Define errors out of existence — don't fail the whole app because the hub is unreachable.)

### Server Design (Ousterhout)

- `Server` struct (not singleton) — `New(dataDir)`, `Start()`, `Stop()`, `IsRunning()`
- `Bridge.Routes()` co-locates handlers with routes — adding an endpoint is one edit in `bridge.go`
- Pass-through JSON: never redefine `notify.Message`/`notify.ThreadSummary` — struct tags on those types ARE the wire format
- `api.ts` is a deep module: SSE auto-reconnect, error normalization, connection status tracking — components never see raw HTTP; accessed via `useApi()` React context

### Repo Structure

```
edgesync-notify-app/
  go/
    go.mod              # imports libfossil + edgesync/leaf/agent/notify
    server.go           # localhost HTTP + SSE server
    bridge.go           # wraps notify.Service for HTTP handlers
    bridge_test.go      # httptest-based tests
    sse.go              # SSEManager: queuing, reconnect replay
    pair.go             # pairing token decode + hub connection
    main.go             # gomobile exports: Start, Stop, IsRunning
  app/
    (screens)/
      index.tsx         # Inbox
      thread/[id].tsx   # Thread detail
      settings.tsx      # Hub config, device name
    lib/
      api.ts            # Deep module: SSE reconnect, error normalization, connection status
      types.ts          # TypeScript types matching Go JSON
      native.ts         # Typed wrapper around NativeModules.NotifyBridge
    components/
      ThreadRow.tsx
      MessageBubble.tsx
      ActionButton.tsx
      ReplyComposer.tsx
      PriorityBadge.tsx
      PairingScreen.tsx
      ConnectionStatus.tsx
  ios/
    NotifyBridge.swift       # Native module: calls Go Start/Stop, exposes port
    NotifyBridgeModule.m     # ObjC macro to register Swift module with RN
  app.json
  package.json
  .github/workflows/ci.yml
```

### Screens

**Inbox (`index.tsx`):** Flat thread list sorted by priority (urgent > action_required > info), then last activity. Each row: thread short ID, last message preview, sender name, timestamp, priority badge. Pull-to-refresh calls `GET /threads`. SSE events update list in real-time. Shows `PairingScreen` when no hub configured.

**Thread Detail (`thread/[id].tsx`):** Message bubbles oldest-at-top, scroll to bottom on open. Sender name + body + timestamp per bubble. Self messages (matching device peer ID) right-aligned, others left-aligned. Action buttons below their message; tap sends reply with `action_response: true`. Media rendered inline via `<Image source={{uri: "http://localhost:PORT/media/..."}}/>`. Reply composer pinned at bottom.

**Settings (`settings.tsx`):** Hub address, live connection status from `GET /status`, device name (editable), project subscriptions, paired hub info with "Disconnect" option, app version.

### State Model

Minimal — Go server is source of truth. No local database, no Redux.

```typescript
connectionStatus: "connected" | "disconnected" | "connecting"
activeProject: string
threads: ThreadSummary[]     // from GET /threads + SSE updates
currentThread: Message[]     // from GET /thread/:id when viewing
replyText: string            // composer input buffer
```

Everything else derived from Go server: thread list = `GET /threads`, message list = `GET /thread/:id`, unread state inferred from SSE events.

### Background Behavior

| App State | NATS | Delivery | Notification |
|-----------|------|----------|-------------|
| Foreground | Connected (SSE live) | Real-time via SSE | None — UI updates directly |
| Background | Disconnected | Fossil sync via BGTask | Local notification if new messages |
| Killed | Dead | None until next open | None |

`expo-notifications` fires local notifications when BGTask finds new messages. No APNs — no remote push server. BGTask gives ~30 seconds every 15+ minutes (iOS-controlled). Go server queues messages missed while SSE was disconnected; replays on reconnect.

### Build Flow

```bash
# Go framework
cd go/
gomobile bind -target ios -o ../ios/NotifyBridge.xcframework .
gomobile bind -target macos -o ../macos/NotifyBridge.xcframework .

# Expo
npx expo prebuild
npx expo run:ios
```

CI runs `go test ./go/` and `npx tsc --noEmit`. Full iOS build is local only. libfossil is a public module, so no `GO_MODULE_TOKEN` or GOPRIVATE plumbing is required.

### Dependencies

| Package | Purpose |
|---------|---------|
| `libfossil` | Fossil repo operations |
| `edgesync/leaf/agent/notify` | Message types, store, pub/sub, Service |
| `nats.go` | NATS client |
| `gomobile` | Compile Go to iOS/macOS framework |
| `expo` / `expo-router` | React Native framework + file-based routing |
| `expo-notifications` | Local push notifications |
| `expo-camera` | QR code scanning for pairing |
| `@sentry/react-native` | Error tracking |

## Delivery Receipts

NATS-only ephemeral signals. Not committed to Fossil — if sender is offline when ack is published, it simply doesn't see it. Fossil sync is the durability guarantee; receipts are a convenience.

**Subject:** `notify.<project>.ack`

**Ack payload:**
```json
{"msg_id": "msg-<uuid>", "received_by": "<iroh-endpoint-id>", "received_at": "2026-04-10T12:01:02Z"}
```

`watch`/`ask` output prints a `delivered` line when an ack is seen:
```
[2026-04-10T12:01:20Z] thread:a1b2c3d4 from:dan-iphone delivered
```

## Message Tracing

`edgesync notify trace <msg-id>` — diagnostic tool showing pipeline status:

```
Message: msg-a1b2c3d4
Thread:  thread-f5e6d7c8
Project: edgesync

Pipeline:
  ✓ Committed to notify.fossil    2026-04-10T12:00:00Z
  ✓ NATS published                2026-04-10T12:00:00Z
  ✓ Hub received                  2026-04-10T12:00:01Z
  ✓ Delivered to dan-iphone       2026-04-10T12:00:01Z
  ✗ Delivered to dan-macbook      (not yet)
```

Reads from local repo (committed?), checks delivery receipts for NATS delivery events.

## Planned Phases

| Phase | Scope | Status |
|-------|-------|--------|
| 1 — Go backend | CLI + repo + NATS pub/sub | **Done** |
| 2 — Device pairing | `pair`, `unpair`, `devices` commands + token infrastructure | **Done** |
| 3 — Expo Go server | Localhost HTTP/SSE wrapping notify.Service in `edgesync-notify-app` | **Done** |
| 4 — gomobile + Expo scaffold | `gomobile bind`, Expo Router, native module, screens, `api.ts` deep module | Planned |
| 5 — Integration | Wire xcframework into iOS app, `PairingFlow`, e2e tests | Planned |
| 6 — Notifications | `expo-notifications` local push, BGTask background refresh | Planned |
| 7 — Sentry | Go SDK on CLI + hub; `@sentry/react-native` on devices | Planned |
| 8 — Claude Code skill | Teaches Claude the CLI grammar and `ask`/`watch` patterns | Planned |

## Testing

### EdgeSync backend (26 tests)

| Category | Count | What |
|----------|-------|------|
| Message format | 8 | Construction, reply, JSON round-trip, path/subject generation |
| Store operations | 4 | Init, commit, list threads, read thread |
| Blob decompression | 5 | Empty, short, valid, corrupted, truncated |
| NATS pub/sub | 3 | Publish/subscribe, wildcard, dedup |
| Service | 6 | Nil repo error, send, existing thread, watch, watch formatter |
| CLI e2e | 2 | Init, send + threads + status |

### Expo app layers

| Layer | Tool | What |
|-------|------|------|
| Go HTTP server | `go test` + `httptest` | Endpoint responses, SSE streaming, pairing token decode, reconnect replay |
| Go notify logic | Already tested (EdgeSync) | Message format, store, pub/sub, dedup |
| React Native | Expo test runner | Screens render, SSE events update state, action taps send replies |
| Integration | iOS Simulator | Full loop: Go server → SSE → UI → tap reply → Go sends |

### Integration + sim tests (planned)

- Two leaf agents exchange messages via hub — NATS real-time + Fossil catch-up after NATS miss
- CLI `ask` round-trip through real hub; timeout exits code 2
- Pairing flow: generate token → accept on new peer → hub auto-adds
- Fault injection (sim): drop NATS messages, verify Fossil sync recovers; hub restart; concurrent sends + dedup

## Error Tracking

Sentry (Phase 7) — additive to existing Honeycomb OTel traces for sync operations.

| Component | SDK | Captures |
|-----------|-----|---------|
| Go CLI + hub | `github.com/getsentry/sentry-go` | Panics, sync failures, NATS connection errors; tags: `project`, `peer_id`, `component` (cli/hub) |
| Expo app | `@sentry/react-native` | JS exceptions, native crashes, navigation breadcrumbs; tags: `project`, `device`, `peer_id` |
