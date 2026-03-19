# Observability Improvements — Implementation Plan

**Spec:** `docs/superpowers/specs/2026-03-19-observability-improvements-design.md`

## Phase 1: Observer Interface & Core (must be sequential)

### Step 1: Expand Observer interface
- `go-libfossil/sync/observer.go` — Add `RoundStats`, `HandleStart`, `HandleEnd` types. Add `Error()`, `HandleStarted()`, `HandleCompleted()` methods. Update `RoundCompleted` signature. Update `nopObserver`.
- `go-libfossil/sync/observer_test.go` — Update tests for new signature.

### Step 2: Update all Observer callers
- `go-libfossil/sync/session.go` — Build `RoundStats`, call new signature
- `go-libfossil/sync/clone.go` — Build `RoundStats`, call new signature
- `dst/mock_fossil.go` — Update `RoundCompleted` call
- `dst/simulator.go` — Update if needed

### Step 3: Enrich sync client with RoundStats data
- `go-libfossil/sync/client.go` — Count gimmes/igots/bytes during buildRequest and processResponse
- `go-libfossil/sync/session.go` — Pass enriched stats to RoundCompleted
- `go-libfossil/sync/clone.go` — Same for clone path

### Step 4: Add Error() calls in protocol error paths
- `go-libfossil/sync/client.go` — Call obs.Error() on processResponse errors
- `go-libfossil/sync/session.go` — Call obs.Error() on round errors

### Step 5: Add HandleStarted/HandleCompleted to server handler
- `go-libfossil/sync/handler.go` — Accept Observer in HandleOpts, call handle lifecycle
- `go-libfossil/sync/serve_http.go` — Pass observer through

## Phase 2: OTel Implementation (depends on Phase 1)

### Step 6: Update OTel observer
- `leaf/telemetry/observer.go` — Implement Error(), HandleStarted(), HandleCompleted(). Fix span.SetStatus. Normalize attributes. Add span events in RoundCompleted.
- `leaf/telemetry/observer_test.go` — Test new methods.

## Phase 3: Log Migration (independent of Phase 1-2)

### Step 7: Migrate bridge logs
- `bridge/bridge/bridge.go` — Replace log.Printf → slog. Add Observer to Config. Call handle hooks.
- `bridge/bridge/config.go` — Add Observer field.

### Step 8: Migrate serve_http logs
- `go-libfossil/sync/serve_http.go` — Replace log.Printf → slog.

## Phase 4: Verify

### Step 9: Run all tests
- `make test` — unit + DST + sim serve
- `make dst` — 8 seeds
- Run sim with OTel to verify new spans/events appear in Honeycomb

## Checkpoints
- After Step 2: `go build ./...` must compile across all modules
- After Step 5: `make test` must pass
- After Step 6: `make test` must pass
- After Step 8: `make test` must pass, run sim with OTel
