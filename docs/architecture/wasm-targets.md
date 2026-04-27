# WASM Targets

EdgeSync ships two WebAssembly builds. Both are produced by the leaf module — same agent API (`New` -> `Start` -> `Stop`) — but with different runtimes, networking models, and SQLite drivers.

| Target | GOOS | GOARCH | SQLite driver | Build entry | Output |
|---|---|---|---|---|---|
| WASI | `wasip1` | `wasm` | `ncruces` (WASM-based) | `leaf/cmd/leaf` | `bin/leaf.wasm` |
| Browser | `js` | `wasm` | `ncruces` + OPFS VFS | `leaf/cmd/wasm` | `bin/leaf-browser.wasm` + `bin/wasm_exec.js` |

## Building

Both targets are driven from the repo root:

```bash
make wasm-wasi      # produces bin/leaf.wasm
make wasm-browser   # produces bin/leaf-browser.wasm and copies wasm_exec.js
make wasm           # both
```

The Makefile `cd`s into `leaf/` because the leaf agent lives in its own Go module. The browser recipe copies `wasm_exec.js` from `$(go env GOROOT)/lib/wasm/` (Go 1.26+ path); older Go releases used `misc/wasm/`.

## SQLite driver selection

`leaf/cmd/leaf/main.go` uses build-tagged blank imports to pick the right driver:

- `driver_default.go` (`!wasip1`) — `modernc` (pure Go, the standard driver)
- `driver_wasi.go` (`wasip1`) — `ncruces` (WASM-based; `modernc.org/libc` has no `wasip1` port)

The browser entry (`leaf/cmd/wasm/main.go`) is `//go:build js` and imports `ncruces` directly.

## Runtimes

- **WASI** runs under any wasip1 host: `wasmtime`, `wasmer`, `wazero`, browser `WebAssembly.Module` instantiated with WASI shims. Full HTTP serving works (host networking).
- **Browser** runs in a Web Worker via `wasm_exec.js`. NATS connects via `nats.SetCustomDialer()` over a WebSocket. Exposes JS globals: `edgesync.newAgent`, `.start`, `.stop`, `.syncNow`. See `leaf/cmd/wasm/worker.js`.

## Known limitations

- **Browser cannot listen.** `serve_http_js.go` and `signals_js.go` are build-tagged no-ops — there is no HTTP server in browser builds.
- **`stash/` and `undo/` are excluded** (`//go:build !js`) — checkout features, not needed for sync.
- **Telemetry is a no-op** on both wasm targets (`leaf/telemetry/noop.go` covers `wasip1 || js`). OTel exporters require host networking that browser builds cannot reach.
- **Binaries are large.** `leaf.wasm` and `leaf-browser.wasm` are each ~50 MB unstripped — the SQLite driver dominates. Compression (gzip/brotli) on the serving layer is recommended.

See [`agent-deployment.md`](./agent-deployment.md) (WASM Targets section) for the agent lifecycle in WASM and [`testing-strategy.md`](./testing-strategy.md) for how WASM builds are exercised in CI.
