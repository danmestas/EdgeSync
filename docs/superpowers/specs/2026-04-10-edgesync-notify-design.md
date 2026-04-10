# EdgeSync Notify — Bidirectional Messaging for Human-in-the-Loop AI

**Date:** 2026-04-10
**Status:** Draft
**Author:** Dan Mestas

## Problem

Claude Code (and other automated agents) need a way to notify a human operator on their Apple devices and receive steering responses — like Pushover, but bidirectional, built on EdgeSync's own infrastructure, and without third-party dependencies.

## Solution

A bidirectional messaging system built on EdgeSync's existing stack: NATS for real-time delivery, iroh for encrypted P2P transport, go-libfossil for persistent message storage. Three components: a Go CLI extension (`edgesync notify`), a Hetzner hub relay, and an Expo (React Native) app for iOS and macOS.

## Architecture

### Components

**1. CLI (`edgesync notify`)** — Go commands added to the existing edgesync binary. Sends messages, watches for replies, manages threads. This is Claude Code's interface to the system via a skill that teaches it the CLI grammar.

**2. Hetzner Hub** — the existing leaf agent on 91.99.202.69, running as a NATS hub. Routes messages between peers. Holds the canonical `notify.fossil` repo. No new infrastructure — just new NATS subjects and a new Fossil repo.

**3. Expo App (iOS + macOS)** — a single React Native codebase targeting both platforms. Connects to the hub as a NATS leaf over iroh. Reads messages from the synced go-libfossil repo. Fires local notifications via `expo-notifications`. Sends replies by committing files and publishing on NATS.

### Transport

All connections use NATS-over-iroh. The Expo app runs an iroh node and establishes a NATS leaf connection through the iroh QUIC tunnel to the hub. No WebSocket, no cleartext hops. End-to-end encrypted.

The CLI connects the same way — it starts (or attaches to) a local leaf agent that joins the iroh mesh, then uses NATS subjects for messaging.

### Identity & Trust

iroh endpoint IDs (public keys) are the sole identity mechanism. Each peer (Claude's machine, the hub, each Apple device) has a unique endpoint ID. Trust is established by adding peer IDs to a known-peers list in the hub's configuration. No passwords, no tokens, no external auth service.

Sender attribution in messages uses the `from` field (iroh endpoint ID) and `from_name` field (human-readable label like `claude-macbook` or `dan-iphone`).

## Data Model

### Message Storage

A dedicated `notify.fossil` repo, managed entirely by go-libfossil (no `fossil` binary dependency). Messages are plaintext JSON files committed to the repo.

### File Tree Layout

```
<project>/threads/<thread-id>/
  ├── <unix-timestamp>-<msg-id>.json
  ├── <unix-timestamp>-<msg-id>.json
  └── ...
<project>/media/
  └── <filename>
```

Example:

```
edgesync/threads/a1b2c3d4/
  ├── 1712750400-msg001.json    # "Build failed, retry?"
  ├── 1712750460-msg002.json    # [action: retry]
  └── 1712750520-msg003.json    # "Retrying now..."
edgesync/media/
  └── screenshot-abc123.png
```

### Message Format

```json
{
  "v": 1,
  "id": "msg-<uuid>",
  "thread": "thread-<uuid>",
  "project": "edgesync",
  "from": "<iroh-endpoint-id>",
  "from_name": "claude-macbook",
  "timestamp": "2026-04-10T12:00:00Z",
  "body": "Build failed on commit abc123. Want me to retry?",
  "priority": "action_required",
  "actions": [
    {"id": "retry", "label": "Retry"},
    {"id": "skip", "label": "Skip"},
    {"id": "logs", "label": "Show Logs"}
  ],
  "reply_to": null,
  "media": null
}
```

**Fields:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `v` | int | yes | Schema version (currently 1) |
| `id` | string | yes | Unique message ID (`msg-<uuid>`) |
| `thread` | string | yes | Thread ID (`thread-<uuid>`) |
| `project` | string | yes | Project scope (e.g., `edgesync`) |
| `from` | string | yes | Sender's iroh endpoint ID |
| `from_name` | string | yes | Human-readable sender name |
| `timestamp` | string | yes | ISO 8601 UTC timestamp |
| `body` | string | yes | Message text |
| `priority` | string | no | `info` (default), `action_required`, or `urgent`. Controls sort order and badge style in the app. |
| `actions` | array | no | Quick action buttons |
| `reply_to` | string | no | Message ID this is replying to |
| `media` | array | no | List of media filenames in `<project>/media/` |
| `action_response` | bool | no | `true` when body is an action ID from a quick action tap |

### Threads

A thread is an implicit grouping — all messages sharing the same `thread` ID. Created by the first message. The `reply_to` field links to a specific message for direct responses.

Thread IDs are full UUIDs internally. The CLI displays short hashes (first 8 chars) for human use.

### Media Attachments

Stored as unversioned files (UV) in the `notify.fossil` repo via go-libfossil's `r.UVWrite()`. Referenced by filename in the message's `media` array. UV's mtime-wins conflict resolution works for immutable media blobs.

Note: message JSON files are versioned artifacts (committed via `r.Commit()`). Media attachments are unversioned files (UV) — these are two distinct storage mechanisms in Fossil. UV is appropriate for media because attachments are immutable blobs that don't need delta compression or branching history.

## NATS Subject Hierarchy

```
notify.<project>.<thread-id>    # Per-thread messages
notify.<project>.ack            # Delivery receipts
notify.<project>.presence       # Online/typing indicators
notify.<project>.*              # All threads in a project (wildcard)
notify.>                        # All projects, all threads (firehose)
```

Messages published on NATS subjects carry the same JSON as the committed file. This is the real-time delivery path — instant when both sides are online.

## Dual Delivery & Deduplication

Every message is delivered through two paths:

1. **NATS (real-time):** Published on `notify.<project>.<thread-id>` immediately. Sub-second delivery when both sides are connected.
2. **Fossil sync (persistent):** The same message file is committed to `notify.fossil`. go-libfossil sync propagates it to all peers on the normal sync interval.

Deduplication: the receiver checks the message `id`. If it already received the message via NATS, the Fossil sync is a no-op for that file (content-addressed storage handles this naturally). If the NATS message was missed (offline, network partition), Fossil sync recovers it.

## CLI Interface

Commands added to the `edgesync` binary under the `notify` subcommand:

```bash
# Ask a question and wait for a reply (most common pattern)
# Sends a message, prints the thread ID, blocks until the first reply
edgesync notify ask --project edgesync \
  "Build failed on commit abc123. Retry?" \
  --actions "Retry,Skip,Show Logs" \
  --priority action_required

# Send a message (new thread, fire-and-forget)
edgesync notify send --project edgesync --new-thread \
  "Deploy to staging complete" \
  --priority info

# Send a message (existing thread)
edgesync notify send --project edgesync --thread a1b2c3d4 \
  "Retrying the build now"

# Watch for replies (blocks, streams to stdout)
edgesync notify watch --project edgesync --thread a1b2c3d4

# Watch all threads in a project
edgesync notify watch --project edgesync

# List threads
edgesync notify threads --project edgesync

# Read thread history
edgesync notify log --project edgesync --thread a1b2c3d4

# Initialize the notify system (creates notify.fossil repo, configures NATS subjects)
edgesync notify init

# Show connection state and unread counts
edgesync notify status

# Trace a message through the delivery pipeline
edgesync notify trace <msg-id>

# Pair a new device (generates a one-time pairing token)
edgesync notify pair --name "dan-iphone"

# Accept a pairing token (run on the new device / app)
edgesync notify pair --accept <token>
```

### Ask Command

`ask` is the primary command for Claude's request/response pattern. It combines `send --new-thread` and `watch` into a single blocking call:

1. Creates a new thread and sends the message
2. Prints the thread ID to stderr (so Claude can reference it later)
3. Blocks on the NATS subscription, waiting for the first reply
4. Prints the reply to stdout and exits

```bash
$ edgesync notify ask --project edgesync "Deploy to prod?" --actions "Yes,No"
# stderr: thread:a1b2c3d4
# (blocks until reply)
# stdout: [2026-04-10T12:01:03Z] thread:a1b2c3d4 from:dan-iphone action:yes
```

Optional `--timeout 5m` flag (default: no timeout, blocks indefinitely). On timeout, exits with code 2 so Claude can detect it.

### Watch Output Format

Structured for machine parsing (Claude reads stdout):

```
[2026-04-10T12:01:03Z] thread:a1b2c3d4 from:dan-iphone action:retry
[2026-04-10T12:01:15Z] thread:a1b2c3d4 from:dan-iphone text:also bump the version
[2026-04-10T12:01:20Z] thread:a1b2c3d4 from:dan-iphone delivered
```

The `delivered` line appears when the recipient's device acknowledges receipt (see Delivery Receipts below).

### Send Behavior

`send` is atomic: it commits the message file to `notify.fossil` via go-libfossil and publishes the JSON on the NATS subject in one operation. If the NATS publish fails (hub offline), the file is still committed — Fossil sync will deliver it eventually.

### Thread ID Shorthand

Full thread IDs are UUIDs. The CLI accepts and displays 8-character short hashes. Collision resolution: if ambiguous, the CLI prompts for more characters.

### Priority Flag

`--priority` accepts `info` (default), `action_required`, or `urgent`. Sets the `priority` field in the message JSON. The Expo app uses this for sort order, badge color, and notification sound:
- `info` — standard notification, no badge
- `action_required` — highlighted in inbox, badge on thread
- `urgent` — prominent badge, persistent notification sound

## Expo App

### Platform Targets

Single Expo (React Native) codebase targeting iOS and macOS.

### Transport Layer

The app runs an iroh node and establishes a NATS leaf connection through the iroh QUIC tunnel to the Hetzner hub. This is the same connection model as any other EdgeSync leaf peer.

### Storage

The go-libfossil `notify.fossil` repo is the single source of truth. The app reads message files directly from the synced repo. No local SQLite cache, no state duplication. The UI is a thin view over the repo's file tree.

### Notifications

`expo-notifications` fires local push notifications when a NATS message arrives while the app is backgrounded or inactive. No Apple Push Notification service (APNs) needed — the NATS-over-iroh connection IS the push channel.

### Screens

**Inbox** — all threads grouped by project. Primary sort: priority (`urgent` > `action_required` > `info`). Secondary sort: last activity. Unread indicators based on which files are new since last view. Each thread shows the last message preview, sender attribution, and priority badge. Urgent threads get a distinct visual treatment.

**Thread Detail** — message bubbles with sender identity, timestamps. Quick action buttons rendered from the message's `actions` array. Media attachments rendered inline. Messages ordered by timestamp from the filename.

**Reply Composer** — text input and quick action buttons. Committing a reply writes a JSON file to the repo and publishes on NATS.

**Settings** — hub connection configuration (iroh peer ID, hub address), project subscriptions, device name (`from_name`).

**Pairing** — accessible from Settings. Enter a pairing token (or scan QR code) to connect to a hub. Displays the device's iroh endpoint ID for manual pairing if needed.

### Offline Behavior

- App opens: reads from local repo files instantly.
- Background sync fills in missed messages when connectivity is restored.
- Replies composed offline are committed to the local repo and published on NATS reconnect.

### Minimal Frontend State

The UI holds only:
- Current connection status (connected/disconnected)
- Active screen / active thread ID
- Reply text input buffer

Everything else is derived from reading the repo file tree. Thread list = directory listing. Message list = file listing sorted by timestamp prefix. Unread state = files newer than last-viewed timestamp.

## Delivery Receipts

When a device receives a message via NATS, it publishes an acknowledgment on `notify.<project>.ack` with the message ID and the receiver's endpoint ID. The sender's `watch` (or `ask`) prints a `delivered` line when it sees the ack.

Delivery receipts are NATS-only (real-time). They are not committed to the Fossil repo — they're ephemeral signals. If the sender is offline when the ack is published, it simply doesn't see it. This is acceptable: receipts are a convenience, not a guarantee. Fossil sync is the guarantee.

Ack payload:

```json
{
  "msg_id": "msg-<uuid>",
  "received_by": "<iroh-endpoint-id>",
  "received_at": "2026-04-10T12:01:02Z"
}
```

## Device Pairing

Adding a new device to the mesh requires exchanging iroh endpoint IDs. Rather than manually editing hub config files, a pairing flow handles this:

### Pairing Flow

1. **On the hub (or any trusted peer):** `edgesync notify pair --name "dan-iphone"` generates a one-time pairing token. The token encodes the hub's iroh endpoint ID, NATS address, and a short-lived shared secret. Output: a token string (and optionally a QR code for terminal display).

2. **On the new device (Expo app):** the user enters the token (or scans the QR code) in the Settings screen. The app uses the token to connect to the hub, presents its own iroh endpoint ID, and the hub verifies the shared secret.

3. **Hub auto-adds the peer:** on successful verification, the hub adds the new device's endpoint ID to its known-peers list and persists it. No restart required.

4. **Token expires:** tokens are single-use and expire after 10 minutes. A used or expired token is rejected.

The pairing token does NOT grant long-term access — it bootstraps the initial iroh endpoint ID exchange. After pairing, trust is based solely on the iroh endpoint ID.

### CLI Pairing (non-app peers)

For adding a new CLI peer (e.g., a second Mac running Claude): `edgesync notify pair --accept <token>` on the new machine. Same flow, just terminal-based instead of app-based.

## Message Tracing

`edgesync notify trace <msg-id>` shows where a message is in the delivery pipeline:

```
$ edgesync notify trace msg-a1b2c3d4
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

Implementation: the trace command reads from the local repo (committed?), checks Sentry breadcrumbs for NATS publish/receive events, and queries delivery receipts. This is a diagnostic tool — not needed for normal operation.

## Multi-Project & Multi-Session Support

### Projects

Each project gets its own directory namespace in the repo and its own NATS subject prefix. The CLI `--project` flag scopes all operations. The Expo app shows projects as top-level groups in the inbox.

### Multiple Claude Sessions

Each Claude Code session uses a unique sender identity: the iroh endpoint ID plus a session suffix. `from_name` is human-readable (e.g., `claude-macbook-01`, `claude-macbook-02`). Threads are the organizing unit — one thread per Claude session conversation. The inbox shows which session started each thread.

## Error Tracking

Sentry for error tracking and crash reporting on all three nodes:

- **Go CLI & Hub:** Sentry Go SDK. Captures panics, sync failures, NATS connection errors. Tags: `project`, `peer_id`, `component` (cli/hub).
- **Expo App:** Sentry Expo SDK (`@sentry/react-native`). Captures JS exceptions, native crashes, navigation breadcrumbs. Tags: `project`, `device`, `peer_id`.

Sentry is the sole error tracking dependency. Existing Honeycomb OTel setup in EdgeSync remains for sync traces — Sentry is additive, not a replacement.

## Claude Code Skill

A skill that teaches Claude how to use the `edgesync notify` CLI:

- **Primary pattern:** use `ask` for request/response (blocks for reply, most common)
- **Fire-and-forget:** use `send --new-thread` for informational messages (`--priority info`)
- **Continuing threads:** use `send --thread <id>` to add to an existing conversation
- How to define quick actions (`--actions "Yes,No,Skip"`)
- How to set priority (`--priority urgent` for critical decisions, `action_required` for steering, `info` for FYI)
- How to parse `watch`/`ask` output (action vs text replies, delivered confirmations)
- How to handle `ask` timeout (exit code 2 = no response, decide whether to retry or proceed)
- Pattern: periodic `status` checks to verify connectivity

## Testing Strategy

All backend work follows TDD (red-green-refactor).

### Test Tiers

**Unit tests:**
- Message artifact creation and validation
- JSON serialization/deserialization (including priority field)
- File tree path generation (`<project>/threads/<thread>/<timestamp>-<id>.json`)
- NATS subject construction
- Deduplication logic (seen message IDs)
- Thread listing and sorting (priority-aware ordering)
- Watch output formatting (including `delivered` lines)
- Pairing token generation and validation (expiry, single-use)
- Delivery receipt creation and parsing

**Integration tests:**
- Two leaf agents exchange messages via hub
- Verify NATS delivers messages in real-time
- Verify go-libfossil sync delivers messages after NATS miss
- CLI `ask` round-trip: send + receive reply through a real hub
- CLI `send` → `watch` round-trip through a real hub
- Delivery receipt: send message, verify sender sees `delivered`
- Pairing flow: generate token, accept on new peer, verify peer added to hub
- Multi-project isolation (messages don't cross project boundaries)
- Multi-session attribution (distinct `from` identities)

**Sim tests (extend existing sim framework):**
- Fault injection: drop NATS messages, verify Fossil sync recovers
- Partition a peer, verify it catches up on reconnect
- Concurrent sends from multiple peers, verify ordering and dedup
- Hub restart, verify message delivery resumes
- `ask` timeout: verify exit code 2 when no reply within timeout

**CLI end-to-end:**
- `notify init` creates repo
- `notify send --new-thread` → verify file in repo + NATS publish
- `notify ask` → reply → verify output and exit
- `notify watch` receives reply within timeout
- `notify threads` lists active threads
- `notify log` shows full thread history
- `notify trace <msg-id>` shows pipeline status
- `notify pair` → `notify pair --accept` completes pairing

## Build Order

1. **Go backend core** — message format, repo layout, `send`/`watch`/`ask`/`threads`/`log` commands. TDD.
2. **Device pairing** — `pair` command, token generation/validation, hub auto-add. TDD.
3. **Delivery receipts & tracing** — ack subject, `delivered` output, `trace` command. TDD.
4. **Sentry integration** — error tracking on CLI and hub
5. **Claude Code skill** — teaches Claude the CLI interface
6. **Expo app** — UI over proven backend, priority-aware inbox
7. **Sentry Expo integration** — crash reporting on devices

## Non-Goals

- No Apple Push Notification service (APNs) — NATS-over-iroh is the push channel
- No notification preferences in the app — iOS/macOS Focus modes handle mute/quiet hours
- No `fossil` binary dependency — all operations via go-libfossil
- No WebSocket fallback — iroh QUIC only
- No third-party push services (Pushover, Firebase, etc.)
- No local SQLite cache in the Expo app — repo files are the source of truth

## Open Questions

None — all design decisions resolved during brainstorming.
