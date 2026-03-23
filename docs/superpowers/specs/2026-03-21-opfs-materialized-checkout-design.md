# OPFS Materialized Checkout — Spike Design

**Date:** 2026-03-21
**Branch:** `spike/opfs-vfs`
**Status:** Spike (proof of concept)

## Goal

Prove that a Fossil repository's working tree can be fully materialized as individual OPFS files in the browser, with nested directory structure, and that edits to those files can be committed back as Fossil checkins.

## Context

The OPFS spike already stores the Fossil repo database in OPFS via go-sqlite3-opfs. Files are read from blob storage on demand. This design adds a second OPFS subtree — a materialized checkout — where every file from the tip checkin exists as a real OPFS file that persists across page reloads.

## OPFS Layout

```
OPFS Root
├── sqlite3-opfs/          ← repo DB (existing, sync access handles)
│   ├── repo.fossil
│   ├── repo.fossil-journal
│   └── repo.fossil-wal
└── checkout/              ← materialized working tree (new, async API)
    ├── README.md
    ├── src/
    │   └── main.go
    └── docs/
        └── guide.md
```

The repo DB uses `createSyncAccessHandle()` (Worker-only, synchronous). The checkout tree uses the async OPFS API (`getDirectoryHandle()`, `createWritable()`, `getFile()`) which can coexist in the same Worker.

## Components

### 1. OPFS File Manager (JS — worker.js)

JS functions exposed to Go via `syscall/js`:

| Function | Signature | Purpose |
|----------|-----------|---------|
| `_opfs_co_write` | `(path, Uint8Array) → Promise` | Create dirs recursively, write file |
| `_opfs_co_read` | `(path) → Promise<Uint8Array>` | Read file content |
| `_opfs_co_list` | `(dir) → Promise<[{name, isDir, size}]>` | List directory entries |
| `_opfs_co_delete` | `(path) → Promise` | Remove file |
| `_opfs_co_stat` | `(path) → Promise<{exists, isDir, size}>` | Check file/dir existence |
| `_opfs_co_clear` | `() → Promise` | Remove entire checkout tree |

All functions operate relative to the `checkout/` directory in OPFS root.

Path handling: `src/lib/utils.go` → navigate `checkout` → `src` → `lib` → `utils.go`, creating directories as needed. Paths are normalized: strip leading `/`, reject `..` and `.` components. No symlinks (OPFS doesn't support them).

`_opfs_co_list` returns a flat array for the immediate directory. Recursive traversal is handled by the Go caller:

```javascript
// JS: list one directory level
async function _opfs_co_list(id, dirPath) {
    const dir = await navigateTo(checkoutRoot, dirPath);
    const entries = [];
    for await (const [name, handle] of dir.entries()) {
        if (handle.kind === 'file') {
            const file = await handle.getFile();
            entries.push({ name, isDir: false, size: file.size });
        } else {
            entries.push({ name, isDir: true, size: 0 });
        }
    }
    _go_co_resolve(id, JSON.stringify(entries));
}
```

Go calls `_opfs_co_list` recursively to walk the full tree when needed (for Status and CommitAll).

### 2. JS ↔ Go Async Bridge

Go WASM is single-threaded. Async OPFS calls require a callback pattern:

```javascript
// JS: register Go callback, call async OPFS, resolve via callback
function _opfs_co_write(id, path, contentArray) {
    doAsyncWrite(path, contentArray).then(() => {
        _go_co_resolve(id, null);
    }).catch(e => {
        _go_co_resolve(id, e.message);
    });
}
```

```go
// Go: block goroutine on channel, unblocked by JS callback
var pending sync.Map // id → chan error

func init() {
    js.Global().Set("_go_co_resolve", js.FuncOf(resolveCallback))
}

func coWrite(path string, data []byte) error {
    id := nextID()
    ch := make(chan error, 1)
    pending.Store(id, ch)
    js.Global().Call("_opfs_co_write", id, path, toJSArray(data))
    return <-ch
}

func resolveCallback(_ js.Value, args []js.Value) any {
    id := args[0].Int()
    ch, _ := pending.LoadAndDelete(id)
    if args[1].IsNull() {
        ch.(chan error) <- nil
    } else {
        ch.(chan error) <- errors.New(args[1].String())
    }
    return nil
}
```

Each async operation gets a unique ID. Go goroutine blocks on channel. JS calls `_go_co_resolve(id, error)` to unblock.

### 3. Checkout Manager (Go — spike/opfs-poc/checkout.go)

```go
type Checkout struct {
    repo   *repo.Repo
    tipRID libfossil.FslID
}
```

**Materialize** — Extract tip checkin to OPFS:
1. `_opfs_co_clear()` — remove old checkout
2. `manifest.ListFiles(repo, tipRID)` — get file list
3. For each file: `content.Expand(repo.DB(), fileRID)` → `_opfs_co_write(name, data)`
4. Write tipRID to `checkout/.fossil-checkout` (plain text file in OPFS) for persistence across reloads

**ListDir(path)** — List OPFS directory:
- `_opfs_co_list(path)` → return entries

**ReadFile(path)** — Read from OPFS:
- `_opfs_co_read(path)` → return bytes

**WriteFile(path, content)** — Write to OPFS:
- `_opfs_co_write(path, content)` — immediate persistence

**DeleteFile(path)** — Remove from OPFS:
- `_opfs_co_delete(path)`

**Status()** — Diff OPFS working tree vs tip manifest:
1. Get tip files from `manifest.ListFiles` → build map `name → UUID`
2. Walk OPFS tree via recursive `_opfs_co_list` → build set of `name → size`
3. For each OPFS file: if not in manifest → **added**; if in manifest, compare size first (fast path), then SHA1 of content vs manifest UUID → **modified** if different
4. For each manifest file: if not in OPFS → **deleted**

Spike uses naive SHA1 recomputation (read every file). Acceptable for small repos. Production would track mtimes/sizes for fast dirty detection.

**CommitAll(comment, user)** — Commit OPFS tree as new checkin:
1. Call `Status()` to find changes
2. If no changes, return early
3. Read all files from OPFS (full tree — unchanged files too, needed for R-card)
4. `manifest.Checkin(repo, CheckinOpts{Files, Comment, User, Parent: tipRID})`
5. Update tipRID to new checkin
6. No need to re-materialize — OPFS tree is already current

### 4. Playground UI Updates

**File browser panel** — Shows OPFS tree, not manifest:
- Directory nodes expand/collapse
- Files show size
- Icons for modified/added/deleted status

**Editor** — Reads/writes OPFS directly:
- Click file → `_opfs_co_read` → display in textarea
- Auto-save on blur or explicit Save button → `_opfs_co_write`

**Status panel** — Shows working tree diff:
- Modified files (content changed)
- New files (added via UI)
- Deleted files (removed via UI)
- Count summary

**Actions:**
- "Checkout" button (after clone) — materializes tip
- "Status" button — shows changes
- "Commit" button — commits OPFS state to repo
- "New File" / "Delete" — operate on OPFS
- "Sync" — as before, push/pull with remote

### Data Flow

```
Clone → Crosslink → Checkout (tip → OPFS files)
                         ↓
              Browse OPFS tree in UI
              Edit files → auto-save to OPFS
              Create/delete files in OPFS
                         ↓
              Status (diff OPFS vs tip)
                         ↓
              CommitAll (OPFS tree → Fossil checkin)
                         ↓
              Sync (push to remote)
```

### Persistence

Both the repo DB and the checkout files persist in OPFS across:
- Page reloads
- Tab close/reopen
- Browser restart

On reload, the playground detects existing checkout by checking if `checkout/` directory exists in OPFS, and re-renders the file tree without re-materializing.

## What This Proves

- Full OPFS directory tree creation and traversal from Go WASM
- Async OPFS API alongside sync access handles in the same Worker
- Individual file persistence independent of SQLite
- Nested `getDirectoryHandle()` with recursive creation
- Working tree semantics (checkout → edit → status → commit) entirely in browser
- Coexistence of two OPFS storage strategies (sync handles for DB, async for files)

## Out of Scope

- Branch switching (single tip checkout only)
- Merge conflicts
- Binary file display (stored correctly, displayed as text)
- File permissions (Fossil's `x` bit)
- Rename tracking
