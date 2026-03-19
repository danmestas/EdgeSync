# OpenTelemetry Observability for Leaf Agent

**Date:** 2026-03-19
**Status:** Approved
**Branch:** feature/otel-observability

## Goal

Add traces, metrics, and structured log correlation to the leaf agent using OpenTelemetry. Initial backend is Honeycomb, but the design is backend-agnostic (swappable via env vars). go-libfossil stays dependency-free. WASM builds excluded via build tags.

**Out of scope:** Server-side observability (`HandleSync`, `HandleSyncWithOpts`, `ServeHTTP`). Will be addressed in a follow-up iteration.

## Observer Interface (go-libfossil/sync/)

New file: `observer.go`

```go
// SessionStart describes the beginning of a sync or clone operation.
type SessionStart struct {
    Operation   string // "sync" or "clone"
    Push, Pull  bool
    UV          bool
    ProjectCode string
}

// SessionEnd describes the result of a sync or clone operation.
type SessionEnd struct {
    Operation                     string
    Rounds                        int
    FilesSent, FilesRecvd         int
    UVFilesSent, UVFilesRecvd     int
    UVGimmesSent                  int
    ProjectCode                   string
    Errors                        []string
}

// Observer receives lifecycle callbacks during sync and clone operations.
// A single Observer instance may be shared across multiple concurrent sessions.
// Pass nil for no-op default.
type Observer interface {
    Started(ctx context.Context, info SessionStart) context.Context
    RoundStarted(ctx context.Context, round int) context.Context
    RoundCompleted(ctx context.Context, round int, sent, recvd int)
    Completed(ctx context.Context, info SessionEnd, err error)
}
```

- `Started` returns a new context (OTel impl creates root span here)
- `RoundStarted` returns a new context (OTel impl creates child span here)
- `SessionStart`/`SessionEnd` are typed structs — no `any`, no type-switching
- `nopObserver{}` is the default when nil
- Added as `Observer Observer` field on both `SyncOpts` and `CloneOpts`

## Hook Points

### sync.Sync() (session.go)

```
ctx = obs.Started(ctx, SessionStart{Operation: "sync", ...})
  ctx = obs.RoundStarted(ctx, n)
  obs.RoundCompleted(ctx, n, sent, recvd)
obs.Completed(ctx, SessionEnd{...}, err)
```

### sync.Clone() (clone.go)

```
ctx = obs.Started(ctx, SessionStart{Operation: "clone", ...})
  ctx = obs.RoundStarted(ctx, n)
  obs.RoundCompleted(ctx, n, 0, recvd)
obs.Completed(ctx, SessionEnd{...}, err)
```

## OTel Implementation (leaf/telemetry/)

### setup.go (`//go:build !wasip1 && !js`)

```go
type TelemetryConfig struct {
    ServiceName string            // default: "edgesync-leaf"
    Endpoint    string            // e.g. "https://api.honeycomb.io"
    Headers     map[string]string // e.g. {"x-honeycomb-team": "<key>"}
}

func Setup(ctx context.Context, cfg TelemetryConfig) (shutdown func(context.Context) error, error)
```

- Initializes TracerProvider, MeterProvider with OTLP HTTP exporter
- Configures slog default handler via otelslog bridge (injects trace_id, span_id)
- Falls back to standard `OTEL_*` env vars when TelemetryConfig fields are empty (including `OTEL_EXPORTER_OTLP_INSECURE` for local dev)
- When no endpoint configured: returns no-op shutdown, no providers initialized (zero overhead)

### observer.go (`//go:build !wasip1 && !js`)

`OTelObserver` implements `sync.Observer`:

- `Started` — creates root span (name from `info.Operation + ".session"`), sets attributes from `SessionStart`
- `RoundStarted` — creates child span (`info.Operation + ".round"`)
- `RoundCompleted` — ends round span, records per-round file counts
- `Completed` — records final attributes from `SessionEnd`, ends root span, records metrics

### noop.go (`//go:build wasip1 || js`)

- `Setup()` returns no-op shutdown, nil error
- `OTelObserver` is empty struct satisfying `sync.Observer` with no-op methods

## Metrics

| Name | Type | Description |
|------|------|-------------|
| `sync.sessions.total` | Counter | Total sync/clone sessions (attrs: operation, push, pull, uv) |
| `sync.errors.total` | Counter | Sessions ending with error |
| `sync.duration.seconds` | Histogram | End-to-end session duration |
| `sync.rounds` | Histogram | Rounds to convergence |
| `sync.files.sent` | Histogram | Files sent per session |
| `sync.files.received` | Histogram | Files received per session |
| `sync.uv.files.sent` | Histogram | UV files sent per session (when UV enabled) |
| `sync.uv.files.received` | Histogram | UV files received per session (when UV enabled) |

All metrics have `service.name` resource attribute and `project.code` dimension.

## slog Integration

- `leaf/agent/agent.go`: replace all `log.Printf` with `slog.InfoContext(ctx, ...)` / `slog.ErrorContext(ctx, ...)`
- `leaf/agent/serve_nats.go`: replace all `log.Printf` with `slog.InfoContext` / `slog.ErrorContext`
- Context carries active trace/span IDs via otelslog bridge
- JSON output in production; text for local dev

## CLI / Config (leaf/cmd/leaf/main.go)

New flags (all optional, fall back to standard OTel env vars):

- `--otel-endpoint` / `OTEL_EXPORTER_OTLP_ENDPOINT`
- `--otel-headers` / `OTEL_EXPORTER_OTLP_HEADERS`
- `--otel-service-name` / `OTEL_SERVICE_NAME`

Telemetry disabled when no endpoint configured.

### Shutdown Sequencing

1. `agent.Stop()` — cancel poll loop, wait for in-flight sync to complete, close NATS, close repo
2. `shutdown(ctx)` — flush remaining spans/metrics to backend

This ordering ensures the final sync session's telemetry is exported before the pipeline closes.

## Backend Swappability

Zero code changes to switch backends — only env vars:

| Backend | OTEL_EXPORTER_OTLP_ENDPOINT | OTEL_EXPORTER_OTLP_HEADERS |
|---------|----------------------------|---------------------------|
| Honeycomb | `https://api.honeycomb.io` | `x-honeycomb-team=<key>` |
| Grafana Cloud | `https://otlp-gateway-....grafana.net/otlp` | `Authorization=Basic <token>` |
| Jaeger (local) | `http://localhost:4318` | (none, set OTEL_EXPORTER_OTLP_INSECURE=true) |
| Axiom | `https://api.axiom.co` | `Authorization=Bearer <token>,X-Axiom-Dataset=edgesync` |

## WASM Safety

- go-libfossil has zero OTel dependencies — compiles to WASM unchanged
- `leaf/telemetry/` uses build tags: `!wasip1 && !js` for OTel files, `wasip1 || js` for no-op stubs
- slog (stdlib) compiles to WASM; handler falls back to text output

### Acceptance Criteria

- `GOOS=wasip1 GOARCH=wasm go build ./...` in `leaf/` must compile (verifies build tags are correct)

## Testing

- **nopObserver**: unit test verifying nil Observer on SyncOpts/CloneOpts doesn't panic (existing sync/clone tests already exercise this path)
- **OTelObserver**: unit test with in-memory span exporter (`sdktrace/tracetest.NewInMemoryExporter`). Verify: root span created, child spans per round, attributes populated, metrics recorded.
- **slog correlation**: verify log records contain `trace_id` and `span_id` fields when OTel is active
- **DST compatibility**: pass `nopObserver` (or nil) in simulation — existing DST tests must continue to pass with Observer field on SyncOpts/CloneOpts

## New Dependencies (leaf/go.mod only)

- `go.opentelemetry.io/otel`
- `go.opentelemetry.io/otel/sdk`
- `go.opentelemetry.io/otel/exporters/otlp/otlphttp`
- `go.opentelemetry.io/contrib/bridges/otelslog`

go-libfossil/go.mod: **unchanged**.

## Files Changed

| File | Change |
|------|--------|
| `go-libfossil/sync/observer.go` | New: Observer interface, SessionStart, SessionEnd, nopObserver |
| `go-libfossil/sync/session.go` | Add Observer to SyncOpts, 4 hook calls in Sync() |
| `go-libfossil/sync/clone.go` | Add Observer to CloneOpts, 4 hook calls in Clone()/run() |
| `leaf/telemetry/setup.go` | New: OTel SDK init (build-tagged) |
| `leaf/telemetry/observer.go` | New: OTelObserver (build-tagged) |
| `leaf/telemetry/noop.go` | New: WASM stubs (build-tagged) |
| `leaf/agent/config.go` | Add Observer field |
| `leaf/agent/agent.go` | log.Printf → slog, pass observer to SyncOpts |
| `leaf/agent/serve_nats.go` | log.Printf → slog |
| `leaf/cmd/leaf/main.go` | OTel flags, Setup() call, wire observer, shutdown sequencing |
| `leaf/go.mod` | Add OTel dependencies |
