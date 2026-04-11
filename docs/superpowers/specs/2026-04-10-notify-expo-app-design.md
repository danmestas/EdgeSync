# EdgeSync Notify Expo App

**Date:** 2026-04-10
**Status:** Draft
**Author:** Dan Mestas

## Problem

The notify backend (CLI + NATS pub/sub + Fossil repo) works, but there's no way to receive messages on Apple devices or reply from them. The Expo app closes the loop: Claude sends → phone buzzes → user replies → Claude reads.

## Solution

A React Native (Expo) app for iOS and macOS with a Go backend compiled via gomobile. The Go framework runs a localhost HTTP server inside the app. React Native talks to it via `fetch` (request/response) and `EventSource` (SSE for real-time messages). No custom native bridge — standard HTTP.

## Repo

`edgesync-notify-app` — standalone GitHub repo. The Go module imports `go-libfossil` and `edgesync/leaf/agent/notify` as published versioned dependencies. CI needs `GO_MODULE_TOKEN` for private module access.

## Architecture

```
┌─────────────────────────────┐
│  React Native (Expo)        │  UI layer — screens, components, gestures
│  fetch + EventSource        │
├────── localhost HTTP ────────┤
│  Go Framework (gomobile)    │  Logic layer — notify.Service, NATS, iroh
│  HTTP server + SSE          │
│  go-libfossil + nats.go     │
└─────────────────────────────┘
```

The Go framework compiles to an `.xcframework` via `gomobile bind`. Expo's native iOS project links it. On app launch, the framework starts an HTTP server on `127.0.0.1:<port>`. On app termination, the server dies with the process. No external server, no background daemon.

## Go HTTP Server API

| Method | Path | Request | Response | Purpose |
|--------|------|---------|----------|---------|
| `POST` | `/init` | `{"hub": "nats://...", "iroh_peers": [...], "device_name": "..."}` | `{"status": "ok"}` | Configure and connect to hub |
| `GET` | `/status` | — | `{"connected": bool, "hub": "...", "peer_id": "..."}` | Connection state |
| `POST` | `/send` | `{"project", "body", "priority", "actions", "thread"}` | `{"message": {...}}` | Send a message |
| `GET` | `/threads?project=X` | — | `[{thread summaries}]` | List threads |
| `GET` | `/thread/:id?project=X` | — | `[{messages}]` | Read thread history |
| `GET` | `/subscribe?project=X` | — | SSE stream | Real-time incoming messages |
| `GET` | `/media/:filename?project=X` | — | binary | Serve media attachment from UV |
| `POST` | `/pair` | `{"token": "AXKF-9M2P-VR3T"}` | `{"status": "ok", "hub": "..."}` | Complete device pairing |
| `POST` | `/stop` | — | `{"status": "ok"}` | Disconnect and clean up |

### SSE Events

```
event: message
data: {"v":1,"id":"msg-abc","thread":"thread-xyz","body":"hello","priority":"urgent",...}

event: connected
data: {"hub":"nats://...","peer_id":"endpoint-abc"}

event: disconnected
data: {"reason":"hub unreachable"}
```

### Go Server Internals

The server wraps `notify.Service` — same `Send`, `Watch`, `ListThreads`, `ReadThread` functions from the EdgeSync backend. No new logic.

**Type pass-through (Ousterhout: avoid change amplification):** `bridge.go` never defines its own message types. It marshals `notify.Message` directly to JSON — the existing JSON struct tags on `Message` ARE the wire format. Adding a field to `Message` in go-libfossil automatically appears in the HTTP API.

### Framework Initialization

The gomobile framework exports three top-level functions callable from Swift:
- `Start(dataDir string) int` — starts the HTTP server on `127.0.0.1:<random-port>`, writes port to `<dataDir>/server-port`, returns the port number. Uses `dataDir` for `notify.fossil` and iroh keys.
- `Stop()` — shuts down the HTTP server and disconnects NATS/iroh
- `IsRunning() bool` — health check

In the Expo native module (`ios/NotifyBridge.swift`), `Start` is called in `AppDelegate.didFinishLaunchingWithOptions`. The returned port is passed to React Native as a native constant. `Stop` is called in `applicationWillTerminate`. No fixed port — Go picks a random available port to avoid conflicts (Ousterhout: eliminate obscure cross-language dependencies).

- `Init` opens/creates `notify.fossil` in the app's document directory. Succeeds if the repo opens, even if NATS/iroh connection fails — connection retries happen in background. `GET /status` reflects actual connection state. (Ousterhout: define errors out of existence — don't fail the whole app because the hub is temporarily unreachable.)
- `Subscribe` endpoint calls `svc.Watch()` and streams messages as SSE events
- `Media` endpoint reads UV files via `r.UVRead()`
- Server tracks whether the SSE client is connected. If disconnected (app backgrounded), queues messages for replay on reconnect.

## Device Pairing

### Pairing Methods

**1. QR Code (primary):** `edgesync notify pair --name "dan-iphone"` prints a QR code to the terminal. App scans it → connected in under 5 seconds. QR encodes: `edgesync-pair://v1/<hub-iroh-endpoint-id>/<nats-addr>/<one-time-secret>`.

**2. Token paste (fallback):** Same command prints a 12-char alphanumeric token (`AXKF-9M2P-VR3T`). App has "Enter Token" option. Base32-encoded, no ambiguous chars (0/O, 1/I/l excluded).

**3. Nearby discovery (future):** iroh mDNS finds hub on local network. Not in v1.

### Pairing Security

- Tokens are single-use — hub deletes after first successful use
- Tokens expire after 10 minutes
- Hub stores only a hash of the shared secret
- Pairing is mutual — hub adds device to known-peers, device stores hub's endpoint ID
- Revocation: `edgesync notify unpair <device-name>` removes a device

### Pairing CLI (hub side)

```bash
edgesync notify pair --name "dan-iphone"     # generate token + QR
edgesync notify unpair "dan-iphone"          # revoke device
edgesync notify devices                      # list paired devices
```

These commands are added to the EdgeSync CLI (not the app).

### Re-Pairing Scenarios

| Scenario | Result |
|----------|--------|
| New phone | Generate new token, scan. Old device still works. |
| Lost phone | `edgesync notify unpair` on hub. Lost device can't reconnect. |
| Hub IP changes | Auto-reconnect via iroh endpoint ID (not IP-bound). |
| App reinstalled | New iroh key → must re-pair. |

### First Launch UX

1. App opens → pairing screen (no hub configured)
2. "Scan to Pair" (primary) or "Enter Token" (fallback)
3. Instructions shown: run `edgesync notify pair --name "my-device"` on your hub
4. On success: "Connected to <hub-name>" confirmation → transition to Inbox
5. Settings shows paired hub with "Disconnect" option

## Screens

### Inbox (`index.tsx`)

- Flat list of threads sorted by: priority (urgent > action_required > info), then last activity
- Each row: thread short ID, last message preview, sender name, timestamp, priority badge (red=urgent, orange=action_required, none=info)
- Pull-to-refresh calls `GET /threads`
- SSE events update list in real-time (bump thread to top, update preview)
- Tap → thread detail

### Thread Detail (`thread/[id].tsx`)

- Messages as bubbles, oldest at top, scroll to bottom on open
- Each bubble: sender name, body text, timestamp
- Messages from self (matching device peer ID) right-aligned, others left-aligned
- Action buttons rendered below the message they belong to
- Tapping action button sends reply with `action_response: true`
- Media attachments rendered inline via `<Image source={{ uri: "http://localhost:PORT/media/..." }}/>`
- Reply composer pinned at bottom: text input + send button

### Settings (`settings.tsx`)

- Hub address and connection status (live from `GET /status`)
- Device name (editable)
- Project list (which projects to subscribe to)
- Paired hub info with "Disconnect" option
- App version

## State Model

Minimal — Go server is the source of truth.

```typescript
connectionStatus: "connected" | "disconnected" | "connecting"
activeProject: string
threads: ThreadSummary[]     // from GET /threads + SSE updates
currentThread: Message[]     // from GET /thread/:id when viewing
replyText: string            // composer input buffer
```

No local database. No Redux. No complex state management. The Go server owns all state via the Fossil repo.

## Notifications & Background

| App State | NATS | Delivery | Notification |
|-----------|------|----------|-------------|
| Foreground | Connected (SSE live) | Real-time via SSE | None — UI updates directly |
| Background | Disconnected | Fossil sync via BGTask | Local notification if new messages |
| Killed | Dead | None until next open | None |

- `expo-notifications` fires local notifications when BGTask finds new messages
- No APNs — no remote push server, no Apple developer portal push config
- BGTask gives ~30 seconds every 15+ minutes (iOS-controlled frequency)
- Go server queues messages missed while SSE was disconnected, replays on reconnect

## Project Structure

```
edgesync-notify-app/
  go/
    go.mod                    # imports go-libfossil + edgesync/leaf/agent/notify
    server.go                 # localhost HTTP + SSE server
    bridge.go                 # wraps notify.Service for HTTP handlers
    bridge_test.go            # httptest-based tests
    pair.go                   # pairing token decode + hub connection
    main.go                   # gomobile exports: Init, Start, Stop
  app/
    (screens)/
      index.tsx               # Inbox
      thread/[id].tsx         # Thread detail
      settings.tsx            # Hub config, device name
    lib/
      api.ts                  # Deep module: SSE reconnection, error normalization, connection status tracking. Components never see raw HTTP.
      types.ts                # TypeScript types matching Go JSON
    components/
      ThreadRow.tsx
      MessageBubble.tsx
      ActionButton.tsx
      ReplyComposer.tsx
      PriorityBadge.tsx
      PairingScreen.tsx
  ios/                        # Expo-generated, links NotifyBridge.xcframework
  app.json                    # Expo config (iOS + macOS targets)
  package.json
  .github/workflows/ci.yml
```

## Build

```bash
# Go framework
cd go/
gomobile bind -target ios -o ../ios/NotifyBridge.xcframework .
gomobile bind -target macos -o ../macos/NotifyBridge.xcframework .

# Expo
npx expo prebuild
npx expo run:ios
```

CI runs `go test ./go/` and `npx tsc --noEmit`. Full iOS build is local only.

## Testing

| Layer | Tool | What |
|-------|------|------|
| Go HTTP server | `go test` + `httptest` | Endpoint responses, SSE streaming, pairing token decode |
| Go notify logic | Already tested (26 tests in EdgeSync) | Message format, store, pub/sub, dedup |
| React Native | Expo test runner | Screens render, SSE events update state, actions send replies |
| Integration | iOS Simulator | Full loop: Go server → SSE → UI → tap reply → Go sends |

## Error Tracking

- Go: `github.com/getsentry/sentry-go` — panics, connection errors, NATS failures
- React Native: `@sentry/react-native` — JS exceptions, native crashes, navigation breadcrumbs

## Design Aesthetic

Minimal/clean for v1. Lots of whitespace, subtle colors, content-first. Polish comes later.

## Non-Goals

- No APNs / remote push notifications
- No notification preferences in the app (iOS Focus modes handle this)
- No `fossil` binary dependency
- No local SQLite cache (Fossil repo is the source of truth)
- No account system (iroh endpoint ID is identity)
- No nearby discovery in v1

## Dependencies

| Package | Purpose |
|---------|---------|
| `go-libfossil` | Fossil repo operations |
| `edgesync/leaf/agent/notify` | Message types, store, pub/sub, Service |
| `nats.go` | NATS client |
| `gomobile` | Compile Go to iOS/macOS framework |
| `expo` | React Native framework |
| `expo-notifications` | Local push notifications |
| `expo-camera` | QR code scanning for pairing |
| `@sentry/react-native` | Error tracking |
