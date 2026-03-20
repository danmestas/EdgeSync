# WASM Leaf Agent Design

**Date:** 2026-03-20
**Status:** Approved

## Problem

The leaf agent only runs as a native Go binary. We want it to run in browsers (GOOS=js) and WASI runtimes (GOOS=wasip1) — same agent, same sync protocol, two targets.

## Targets

- **WASI** (wasip1/wasm): Full agent in Wasmtime/Wasmer. Real filesystem + networking via host. Portable server binary.
- **Browser** (js/wasm): Sync client + NATS subscriber in a Web Worker. OPFS-backed SQLite. No HTTP serving (can't listen).

## Design

### 1. Filesystem Abstraction (`simio/storage.go`)

Following the existing `Clock`/`Rand` pattern, add a `Storage` interface to `simio/`:

```go
type Storage interface {
    Stat(path string) (os.FileInfo, error)
    Remove(path string) error
    MkdirAll(path string, perm os.FileMode) error
    ReadFile(path string) ([]byte, error)
    WriteFile(path string, data []byte, perm os.FileMode) error
}
```

Implementations:
- **`OSStorage`** — delegates to `os.*`. Production + WASI.
- **`MemStorage`** — in-memory map. Tests + browser fallback.
- **`OPFSStorage`** (`//go:build js`) — wraps Origin Private File System via `syscall/js`.

`Env` struct gains `Storage` field. `RealEnv()` returns `OSStorage`. Callers in the sync path (`repo.Create`, `repo.Open`, `sync.Clone`) receive Storage from Env.

**Scope:** Only `repo/repo.go` and `sync/clone.go` are in the sync-critical path. `stash/` and `undo/` stay behind `//go:build !js` — they're checkout features, not needed for sync.

### 2. SQLite on WASM

The ncruces driver compiles to WASM (runs SQLite as WASM inside wazero). Current `db/` build tags handle driver selection.

- **WASI:** ncruces driver works as-is.
- **Browser:** Register OPFS VFS via `github.com/ncruces/go-sqlite3/vfs/opfs`.
- **New file:** `db/driver_ncruces_js.go` (`//go:build js`) — registers OPFS VFS at init.

No changes to `db.Open()` or `db.OpenWith()` signatures. VFS is transparent.

### 3. NATS WebSocket Transport

NATS Go client supports WebSocket natively — `wss://` URLs work with the existing client. No new transport type needed.

**Browser blocker:** `nats.go` uses `net.Dial` which doesn't work in browser WASM. Solution: NATS accepts `nats.SetCustomDialer()`. Provide a browser WebSocket dialer.

**New file:** `leaf/agent/nats_js.go` (`//go:build js`) — custom dialer wrapping `syscall/js` WebSocket. Rest of NATS code unchanged.

### 4. Agent Lifecycle

Same `New()` → `Start()` → `Stop()` API on both targets.

**WASI:** Identical to native. Full agent: poll loop, HTTP serving, NATS serving. Entry point is existing `leaf/cmd/leaf/main.go`.

**Browser:** Same core loop, build-tagged differences:
- `ServeHTTP` — no-op (browser can't `net.Listen`).
- `ServeNATS` — works (subscriber, not listener).
- Signals — replaced with JS callbacks; `SyncNow()` exposed to JS.

**New build-tagged files:**
- `leaf/agent/serve_http_js.go` (`//go:build js`) — no-op
- `leaf/agent/signals_js.go` (`//go:build js`) — no-op signals, JS-triggered sync

### 5. Browser Entry Points

**JS global exports** (`leaf/cmd/wasm/main.go`, `//go:build js`):

```go
js.Global().Set("edgesync", map[string]interface{}{
    "newAgent": js.FuncOf(jsNewAgent),
    "start":    js.FuncOf(jsStart),
    "stop":     js.FuncOf(jsStop),
    "syncNow":  js.FuncOf(jsSyncNow),
    "clone":    js.FuncOf(jsClone),
})
select {} // keep WASM module alive
```

JS usage:
```js
await edgesync.newAgent({ repoPath: "/repo.fossil", natsUrl: "wss://nats.example.com" });
await edgesync.start();
await edgesync.syncNow();
```

**Web Worker wrapper** (`leaf/cmd/wasm/worker.js`):
- Loads WASM binary via `WebAssembly.instantiateStreaming`
- Calls `edgesync.newAgent()` / `edgesync.start()` on init
- `postMessage` API: `{ type: "sync" }`, `{ type: "stop" }`
- Posts events back: `{ type: "syncComplete", rounds: 3, files: 5 }`

**WASI entry point:** Existing `leaf/cmd/leaf/main.go` — compiles to WASI as-is.

### 6. Build & Test

**Build commands:**
```bash
# WASI (full agent)
GOOS=wasip1 GOARCH=wasm go build -tags ncruces -buildvcs=false -o bin/leaf.wasm ./leaf/cmd/leaf/

# Browser
GOOS=js GOARCH=wasm go build -tags ncruces -buildvcs=false -o bin/leaf-browser.wasm ./leaf/cmd/wasm/
```

**Makefile targets:** `wasm-wasi`, `wasm-browser`, `wasm` (both).

**Testing:**
- Unit tests on WASI: `GOOS=wasip1 go test -tags ncruces ./go-libfossil/...` via Wasmtime runner.
- Browser smoke test: HTML page loads WASM, creates repo, runs sync. Manual initially.
- Existing native tests unchanged. WASM is additive.

## Files Modified

| File | Change |
|------|--------|
| `go-libfossil/simio/storage.go` | New: Storage interface + OSStorage |
| `go-libfossil/simio/storage_mem.go` | New: MemStorage |
| `go-libfossil/simio/storage_opfs.go` | New: OPFSStorage (`//go:build js`) |
| `go-libfossil/simio/env.go` | Add Storage field to Env |
| `go-libfossil/repo/repo.go` | Use Storage from Env for Stat/Remove |
| `go-libfossil/sync/clone.go` | Use Storage from Env for Stat/Remove |
| `go-libfossil/db/driver_ncruces_js.go` | New: OPFS VFS registration (`//go:build js`) |
| `leaf/agent/nats_js.go` | New: browser WebSocket dialer (`//go:build js`) |
| `leaf/agent/serve_http_js.go` | New: no-op ServeHTTP (`//go:build js`) |
| `leaf/agent/signals_js.go` | New: no-op signals (`//go:build js`) |
| `leaf/cmd/wasm/main.go` | New: browser WASM entry point |
| `leaf/cmd/wasm/worker.js` | New: Web Worker wrapper |
| `Makefile` | Add wasm-wasi, wasm-browser, wasm targets |

## What We're NOT Doing

- No filesystem abstraction for `stash/` or `undo/` (checkout features)
- No custom WASI runtime embedding
- No service worker caching
- No offline-first queue
- No browser UI
- No OTel in WASM (already stubbed via noop.go)
