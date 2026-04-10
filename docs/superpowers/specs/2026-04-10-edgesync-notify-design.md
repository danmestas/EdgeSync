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
# Send a message (new thread)
edgesync notify send --project edgesync --new-thread \
  "Build failed on commit abc123" \
  --actions "Retry,Skip,Show Logs"

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

# Initialize the notify repo
edgesync notify init

# Show connection state and unread counts
edgesync notify status
```

### Watch Output Format

Structured for machine parsing (Claude reads stdout):

```
[2026-04-10T12:01:03Z] thread:a1b2c3d4 from:dan-iphone action:retry
[2026-04-10T12:01:15Z] thread:a1b2c3d4 from:dan-iphone text:also bump the version
```

### Send Behavior

`send` is atomic: it commits the message file to `notify.fossil` via go-libfossil and publishes the JSON on the NATS subject in one operation. If the NATS publish fails (hub offline), the file is still committed — Fossil sync will deliver it eventually.

### Thread ID Shorthand

Full thread IDs are UUIDs. The CLI accepts and displays 8-character short hashes. Collision resolution: if ambiguous, the CLI prompts for more characters.

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

**Inbox** — all threads grouped by project, sorted by last activity. Unread indicators based on which files are new since last view. Each thread shows the last message preview and sender attribution.

**Thread Detail** — message bubbles with sender identity, timestamps. Quick action buttons rendered from the message's `actions` array. Media attachments rendered inline. Messages ordered by timestamp from the filename.

**Reply Composer** — text input and quick action buttons. Committing a reply writes a JSON file to the repo and publishes on NATS.

**Settings** — hub connection configuration (iroh peer ID, hub address), project subscriptions, device name (`from_name`).

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

- When to send notifications (build failures, decisions needed, task completions)
- How to create threads (`--new-thread`) vs continue existing ones
- How to define quick actions (`--actions "Yes,No,Skip"`)
- How to parse `watch` output (action vs text replies)
- Pattern: `send` then `watch` for request/response flows
- Pattern: periodic `status` checks to verify connectivity

## Testing Strategy

All backend work follows TDD (red-green-refactor).

### Test Tiers

**Unit tests:**
- Message artifact creation and validation
- JSON serialization/deserialization
- File tree path generation (`<project>/threads/<thread>/<timestamp>-<id>.json`)
- NATS subject construction
- Deduplication logic (seen message IDs)
- Thread listing and sorting
- Watch output formatting

**Integration tests:**
- Two leaf agents exchange messages via hub
- Verify NATS delivers messages in real-time
- Verify go-libfossil sync delivers messages after NATS miss
- CLI `send` → `watch` round-trip through a real hub
- Multi-project isolation (messages don't cross project boundaries)
- Multi-session attribution (distinct `from` identities)

**Sim tests (extend existing sim framework):**
- Fault injection: drop NATS messages, verify Fossil sync recovers
- Partition a peer, verify it catches up on reconnect
- Concurrent sends from multiple peers, verify ordering and dedup
- Hub restart, verify message delivery resumes

**CLI end-to-end:**
- `notify init` creates repo
- `notify send --new-thread` → verify file in repo + NATS publish
- `notify watch` receives reply within timeout
- `notify threads` lists active threads
- `notify log` shows full thread history

## Build Order

1. **Go backend (CLI + hub integration)** — TDD, fully tested
2. **Sentry integration** — error tracking on CLI and hub
3. **Claude Code skill** — teaches Claude the CLI interface
4. **Expo app** — UI over proven backend
5. **Sentry Expo integration** — crash reporting on devices

## Non-Goals

- No Apple Push Notification service (APNs) — NATS-over-iroh is the push channel
- No notification preferences in the app — iOS/macOS Focus modes handle mute/quiet hours
- No `fossil` binary dependency — all operations via go-libfossil
- No WebSocket fallback — iroh QUIC only
- No third-party push services (Pushover, Firebase, etc.)
- No local SQLite cache in the Expo app — repo files are the source of truth

## Open Questions

None — all design decisions resolved during brainstorming.
