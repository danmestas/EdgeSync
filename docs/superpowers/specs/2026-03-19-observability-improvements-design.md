# Observability Improvements Design

**Date:** 2026-03-19
**Status:** Approved

## Problem

The EdgeSync codebase has OTel infrastructure in place (traces, metrics, logs) but instrumentation coverage is shallow. The sync protocol internals, server handler, bridge, and storage layer are black boxes. Errors are aggregated into counts rather than recorded individually. Bridge and HTTP server use stdlib `log` instead of slog/OTel.

## Design: Hybrid Observer + Span Events (Approach C)

Expand the `sync.Observer` interface minimally. Use span events on existing spans for card-level detail rather than creating child spans or adding per-card callbacks. Keep `nopObserver` zero-cost.

### Observer Interface Changes

**Existing methods (one signature change):**

```go
type RoundStats struct {
    FilesSent     int
    FilesReceived int
    GimmesSent    int
    IgotsSent     int
    BytesSent     int64
    BytesReceived int64
}

type HandleStart struct {
    Operation   string // "sync" or "clone"
    ProjectCode string
    RemoteAddr  string
}

type HandleEnd struct {
    CardsProcessed int
    FilesSent      int
    FilesReceived  int
    Err            error
}
```

**Full interface:**

```go
type Observer interface {
    // Session lifecycle (existing)
    Started(ctx context.Context, info SessionStart) context.Context
    RoundStarted(ctx context.Context, round int) context.Context
    RoundCompleted(ctx context.Context, round int, stats RoundStats) // CHANGED: was (round, sent, recvd int)
    Completed(ctx context.Context, info SessionEnd, err error)

    // Per-error recording (new)
    Error(ctx context.Context, err error)

    // Server-side request lifecycle (new)
    HandleStarted(ctx context.Context, info HandleStart) context.Context
    HandleCompleted(ctx context.Context, info HandleEnd)
}
```

### Performance Guarantees

- `nopObserver`: all methods are empty. Cost = one indirect call per invocation (~2ns).
- No allocations on nop path. `RoundStats` and `HandleEnd` are value types (no pointers, no maps).
- No string formatting unless observer implementation needs it.
- No per-card callbacks. Card-level detail is aggregated into `RoundStats` counts.
- `Error()` only called on actual errors (not hot path).

### Implementation by Item

#### 1. span.SetStatus(codes.Error)
- File: `leaf/telemetry/observer.go` — `Completed()` method
- Add `span.SetStatus(codes.Error, err.Error())` when err != nil

#### 2. Normalize attribute names
- File: `leaf/telemetry/observer.go` — `Completed()` method
- Change metric attribute `project.code` → `sync.project_code` to match span attributes

#### 3. Bridge log.Printf → slog
- File: `bridge/bridge/bridge.go`
- Replace 5 `log.Printf` calls with `slog.Info`/`slog.Error`
- Add `go.opentelemetry.io/contrib/bridges/otelslog` to bridge module (or just use stdlib slog)
- Decision: use stdlib slog (no OTel dep in bridge module). Bridge logs will flow through OTel only when the leaf process configures the slog default with the OTel bridge. When bridge runs standalone, logs go to stderr as normal.

#### 4. serve_http.go log.Printf → slog
- File: `go-libfossil/sync/serve_http.go`
- Replace 2 `log.Printf` calls with `slog.Error`
- No new dependencies — stdlib slog only

#### 5. Error messages as span events
- New `Error()` method on Observer, called from sync client when protocol errors occur
- OTel implementation: `span.AddEvent("sync.error", trace.WithAttributes(attribute.String("error.message", err.Error())))`
- Call sites: `client.go` processResponse error paths, `handler.go` card processing errors

#### 6. Card-level detail via RoundStats
- Change `RoundCompleted(ctx, round, sent, recvd)` → `RoundCompleted(ctx, round, RoundStats)`
- Accumulate gimme/igot/bytes counts in sync client round loop
- OTel observer adds span events in `RoundCompleted`: one event summarizing the round with all stats as attributes
- Update all callers: `session.go`, `clone.go`, DST `MockFossil`

#### 7. HTTP server request span
- Call `HandleStarted` at top of `HandleSync`/`ServeHTTP`
- Call `HandleCompleted` at end with stats
- OTel observer creates a span named `sync.handle` with server-side attributes

#### 8. Bridge tracing
- Add `Observer` field to bridge `Config`
- Call `HandleStarted`/`HandleCompleted` per NATS exchange in bridge
- Bridge gets visibility without OTel dependency (uses Observer interface)

#### 9. Sync client internals
- Accumulate `RoundStats` fields during `buildRequest` and `processResponse`
- Count gimmes sent, igots sent, bytes in files sent/received
- Call `obs.Error(ctx, err)` on individual protocol errors instead of just collecting strings
- No new methods needed — enriches existing `RoundCompleted` data

#### 10. Handler card dispatch
- Call `obs.HandleStarted`/`HandleCompleted` in `HandleSync`/`HandleSyncWithOpts`
- Count cards processed, files sent/received during handler execution
- Pass counts via `HandleEnd`

#### 11. Storage-layer metrics via RoundStats
- Track `BytesSent`/`BytesReceived` in round stats
- When storing a received file, add its size to `BytesReceived`
- When sending a file, add its size to `BytesSent`
- No separate storage observer needed — piggybacks on existing round tracking

### Files Modified

| File | Change |
|------|--------|
| `go-libfossil/sync/observer.go` | Expand interface, add types |
| `go-libfossil/sync/observer_test.go` | Update tests |
| `go-libfossil/sync/session.go` | Accumulate RoundStats, call Error() |
| `go-libfossil/sync/clone.go` | Accumulate RoundStats |
| `go-libfossil/sync/client.go` | Count gimmes/igots/bytes, call Error() |
| `go-libfossil/sync/handler.go` | Call HandleStarted/HandleCompleted |
| `go-libfossil/sync/handler_uv.go` | No changes (UV stats already tracked) |
| `go-libfossil/sync/serve_http.go` | log → slog |
| `leaf/telemetry/observer.go` | Implement new methods, fix SetStatus, normalize attrs |
| `leaf/telemetry/observer_test.go` | Test new methods |
| `bridge/bridge/bridge.go` | log → slog, add Observer field, call handle hooks |
| `bridge/bridge/config.go` | Add Observer to Config |
| `dst/mock_fossil.go` | Update RoundCompleted signature |
| `dst/simulator.go` | Update any Observer usage |

### What We're NOT Doing

- No per-card child spans (too expensive, wrong granularity)
- No blob compress/decompress tracing (not actionable)
- No delta expansion metrics (not actionable)
- No new OTel metric instruments (existing 8 + richer spans are sufficient)
- No OTel dependency in go-libfossil or bridge modules
