# Auto-Save via UV Drafts — Design Spec

**Date:** 2026-03-23
**Branch:** `feature/browser-fossil-playground`
**Status:** Design

## Goal

Enable real-time collaborative editing in the browser playground. When multiple peers have the same file open, edits propagate instantly via Fossil's UV (unversioned) file sync. When collaboration ends, UV drafts auto-commit to the versioned timeline.

## Core Concept

UV files act as a "draft layer" — mutable, mtime-wins, sync automatically. They propagate edits between peers without creating versioned commits. When the user explicitly commits (or collaboration ends), UV drafts promote to proper Fossil checkins and are deleted.

```
Keystroke → 500ms debounce → UV write "draft/<path>"
                                ↓
                          UV sync to peers (automatic)
                                ↓
                          Peer receives → update OPFS → refresh editor
                                ↓
                          Commit (manual or auto) → Checkin → delete UV draft
```

## Activation Rule

Auto-save is **per-file, presence-gated**:

- **Off** when you're the only peer with a file open
- **On** when 2+ peers have the same file open
- **Deactivate** when co-editors leave → auto-commit remaining UV drafts

No wasted sync traffic for solo editing.

## File-Level Presence

### Heartbeat Format

The existing presence heartbeat on `edgesync.presence` is extended with the currently open file:

```json
{
  "id": "a3f2",
  "user": "browser-a3f2",
  "file": "README.md",
  "time": "2026-03-23T12:00:00Z"
}
```

`file` is `""` when no file is open in the editor.

### Presence State

Each peer tracks:
- `peerFiles map[string]string` — peerID → currently open file path
- Derived: `fileEditors map[string][]string` — file path → list of peer IDs

### UI Indicators

- **File tree**: peer count badge next to files with multiple editors (e.g., `README.md 👤2`)
- **Editor header**: "Editing with browser-a3f2" when co-editing
- **Presence panel**: show which file each peer has open

## UV Draft Mechanism

### Naming Convention

UV draft files use the prefix `draft/`:

| Versioned file | UV draft name |
|----------------|---------------|
| `README.md` | `draft/README.md` |
| `src/main.go` | `draft/src/main.go` |

The `draft/` prefix prevents collisions with actual UV files (wiki, forum).

### Write Path (sender)

1. Editor `input` event fires
2. JS debounces for 500ms
3. If file has co-editors (presence count > 1 for this file):
   - Call `_saveDraft(path, content)` → Go writes UV entry via `uv.Write(db, "draft/"+path, content, now)`
   - Agent's UV sync pushes it on next round (or immediately via notify)
4. If file has no co-editors:
   - Normal OPFS save only (no UV draft)

### Read Path (receiver)

1. Agent pulls UV files during sync (`SyncOpts{UV: true}`)
2. `PostSyncHook` checks for new `draft/*` UV entries
3. For each draft:
   - Write content to OPFS checkout file at the corresponding path
   - If that file is currently open in the editor, refresh the editor content
   - Post `draftReceived` result to UI with path and peer info

### Conflict Resolution

UV sync uses **mtime-wins** — the most recent write wins. This is correct for real-time collaborative editing: if two people edit the same file, the last keystroke wins. This matches Google Docs / Notion behavior for the "draft" layer.

For the versioned layer (commits), Fossil's 3-way merge applies if needed.

## Commit Flow (De-draft)

### Manual Commit

When the user clicks Commit:

1. List all UV entries matching `draft/*`
2. For each draft: read content from UV
3. Create `manifest.Checkin` with all files (drafts + unchanged files from OPFS)
4. Delete all `draft/*` UV entries: `uv.Delete(db, "draft/"+path, now)`
5. UV sync propagates the deletions to peers
6. Publish NATS notify so peers pull the new commit

### Auto-Commit on Deactivation

When co-editors leave (presence drops to 1 for all files):

1. Wait 5 seconds (grace period for reconnection)
2. If still solo, commit all remaining UV drafts
3. Log: `"auto-commit: collaboration ended, N files committed"`

### Auto-Commit Timer (optional, configurable)

If enabled, commit UV drafts every N seconds while collaborating. Creates periodic checkpoints without manual action. Default: off (manual commit only).

## Agent Configuration

Enable UV sync in the agent config:

```go
cfg := agent.Config{
    // ... existing fields ...
    UV: true,  // Enable unversioned file sync for drafts
}
```

The agent already supports UV sync — just not enabled in the playground.

## Components to Modify

| File | Change |
|------|--------|
| `social.go` | Extend presence heartbeat with `file` field. Add `peerFiles` tracking. Add `fileEditors()` helper. |
| `main.go` | Add `_saveDraft` handler. Enable `UV: true` in agent config. |
| `checkout.go` | Add `WriteDraftToOPFS(path, content)` for receiving drafts. |
| `index.html` | Debounced `input` handler. Per-file presence indicators. Co-editing header. |
| `worker.js` | Add `saveDraft` message handler. |

## Data Flow Diagram

```
Tab A (editing README.md)          NATS            Tab B (viewing README.md)
        │                           │                        │
  keystroke                         │                        │
  500ms debounce                    │                        │
  uv.Write("draft/README.md")      │                        │
        │                           │                        │
  agent sync ──── uvfile card ────►│──── uvfile card ────► agent sync
        │                           │                  uv stored
        │                           │              PostSyncHook:
        │                           │                read draft/README.md
        │                           │                write to OPFS
        │                           │                refresh editor
        │                           │                        │
  Commit clicked                    │                        │
  read draft/README.md              │                        │
  manifest.Checkin                  │                        │
  uv.Delete("draft/README.md")     │                        │
  notify ──────────────────────────►│──────────────────────► SyncNow
        │                           │              pull commit + UV deletion
```

## Out of Scope

- Character-level CRDTs (future — for true concurrent editing)
- Operational transforms
- Cursor position sharing
- Undo/redo across peers
- Binary file drafts
- Draft conflict UI (mtime-wins is sufficient)

## Edge Cases

- **Rapid typing**: 500ms debounce batches keystrokes. UV write happens at most twice per second.
- **Large files**: UV syncs full content (no delta). Acceptable for text files in a browser editor. Future optimization: delta UV.
- **Page reload during collaboration**: OPFS checkout has the latest content. UV drafts from peers will re-arrive on next sync.
- **Both peers commit simultaneously**: Fossil handles this — creates a fork in the DAG, next sync merges or reports conflict.
- **Peer crashes without deactivation**: Presence timeout (15s) evicts the peer. Auto-commit fires after 5s grace period.
