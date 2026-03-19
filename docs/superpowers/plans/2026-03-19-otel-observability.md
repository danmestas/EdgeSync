# OpenTelemetry Observability Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add traces, metrics, and structured log correlation to the leaf agent using OpenTelemetry with a backend-agnostic observer pattern.

**Architecture:** Observer interface defined in go-libfossil/sync/ (zero deps) with hooks in Sync() and Clone(). OTel implementation lives in leaf/telemetry/ behind WASM build tags. slog replaces log.Printf in the agent for trace-correlated logs.

**Tech Stack:** Go 1.25, OpenTelemetry Go SDK (otlptracehttp, otlpmetrichttp, otlploghttp), otelslog bridge, log/slog (stdlib)

**Spec:** `docs/superpowers/specs/2026-03-19-otel-observability-design.md`

---

## File Structure

| File | Responsibility |
|------|---------------|
| `go-libfossil/sync/observer.go` | Observer interface, SessionStart, SessionEnd, nopObserver |
| `go-libfossil/sync/observer_test.go` | Test nopObserver, test observer hook calls via recording observer |
| `go-libfossil/sync/session.go` | Add Observer to SyncOpts, hook calls in Sync() |
| `go-libfossil/sync/stubs.go` | Add Observer to CloneOpts |
| `go-libfossil/sync/clone.go` | Hook calls in Clone()/run() |
| `leaf/telemetry/setup.go` | OTel SDK init: TracerProvider, MeterProvider, LoggerProvider, slog handler |
| `leaf/telemetry/observer.go` | OTelObserver: spans + metrics implementing sync.Observer |
| `leaf/telemetry/observer_test.go` | Test OTelObserver with in-memory span exporter |
| `leaf/telemetry/noop.go` | WASM build-tagged stubs |
| `leaf/agent/config.go` | Add Observer field to Config |
| `leaf/agent/agent.go` | log.Printf → slog, pass observer to SyncOpts |
| `leaf/agent/serve_nats.go` | log.Printf → slog |
| `leaf/cmd/leaf/main.go` | OTel flags, Setup() call, wire observer, shutdown sequencing |
| `leaf/go.mod` | Add OTel dependencies |

---

## Chunk 1: Observer Interface + Sync/Clone Hooks

### Task 1: Create feature branch

- [ ] **Step 1: Create and switch to feature branch**

```bash
cd /Users/dmestas/projects/edgesync
git checkout -b feature/otel-observability
```

- [ ] **Step 2: Verify clean state**

```bash
git status
```

Expected: On branch feature/otel-observability, nothing to commit.

---

### Task 2: Observer interface and nopObserver

**Files:**
- Create: `go-libfossil/sync/observer.go`
- Create: `go-libfossil/sync/observer_test.go`

- [ ] **Step 1: Write observer_test.go with a recording observer**

```go
// go-libfossil/sync/observer_test.go
package sync

import (
	"context"
	"testing"
)

// recordingObserver records all lifecycle calls for test assertions.
type recordingObserver struct {
	started        int
	roundsStarted  []int
	roundsCompleted []int
	completed      int
	lastInfo       SessionStart
	lastEnd        SessionEnd
	lastErr        error
}

func (r *recordingObserver) Started(ctx context.Context, info SessionStart) context.Context {
	r.started++
	r.lastInfo = info
	return ctx
}

func (r *recordingObserver) RoundStarted(ctx context.Context, round int) context.Context {
	r.roundsStarted = append(r.roundsStarted, round)
	return ctx
}

func (r *recordingObserver) RoundCompleted(ctx context.Context, round int, sent, recvd int) {
	r.roundsCompleted = append(r.roundsCompleted, round)
}

func (r *recordingObserver) Completed(ctx context.Context, info SessionEnd, err error) {
	r.completed++
	r.lastEnd = info
	r.lastErr = err
}

func TestNopObserverDoesNotPanic(t *testing.T) {
	var obs nopObserver
	ctx := context.Background()
	ctx = obs.Started(ctx, SessionStart{Operation: "sync"})
	ctx = obs.RoundStarted(ctx, 0)
	obs.RoundCompleted(ctx, 0, 5, 3)
	obs.Completed(ctx, SessionEnd{Operation: "sync", Rounds: 1}, nil)
}

func TestResolveObserverNil(t *testing.T) {
	obs := resolveObserver(nil)
	if obs == nil {
		t.Fatal("resolveObserver(nil) should return nopObserver, not nil")
	}
	// Should not panic
	ctx := obs.Started(context.Background(), SessionStart{})
	obs.RoundStarted(ctx, 0)
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -run TestNopObserver -v
```

Expected: FAIL — types not defined yet.

- [ ] **Step 3: Write observer.go**

```go
// go-libfossil/sync/observer.go
package sync

import "context"

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

// nopObserver is the default observer that does nothing.
type nopObserver struct{}

func (nopObserver) Started(ctx context.Context, _ SessionStart) context.Context    { return ctx }
func (nopObserver) RoundStarted(ctx context.Context, _ int) context.Context        { return ctx }
func (nopObserver) RoundCompleted(_ context.Context, _ int, _, _ int)              {}
func (nopObserver) Completed(_ context.Context, _ SessionEnd, _ error)             {}

// resolveObserver returns obs if non-nil, otherwise nopObserver{}.
func resolveObserver(obs Observer) Observer {
	if obs == nil {
		return nopObserver{}
	}
	return obs
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -run "TestNopObserver|TestResolveObserver" -v
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/sync/observer.go go-libfossil/sync/observer_test.go
git commit -m "sync: add Observer interface, SessionStart, SessionEnd, nopObserver"
```

---

### Task 3: Add Observer field to SyncOpts and hook into Sync()

**Files:**
- Modify: `go-libfossil/sync/session.go` (SyncOpts struct + Sync function)
- Modify: `go-libfossil/sync/observer_test.go` (add integration test)

- [ ] **Step 1: Write the failing test — observer receives hooks during Sync()**

Append to `go-libfossil/sync/observer_test.go`:

```go
func TestSyncCallsObserverHooks(t *testing.T) {
	// Create two repos: client and server.
	clientPath := filepath.Join(t.TempDir(), "client.fossil")
	serverPath := filepath.Join(t.TempDir(), "server.fossil")
	env := simio.RealEnv()

	server, err := repo.Create(serverPath, "test", env.Rand)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()

	client, err := repo.Create(clientPath, "test", env.Rand)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()

	var projCode, srvCode string
	client.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	client.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&srvCode)

	mt := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			resp, _ := HandleSync(context.Background(), server, req)
			return resp
		},
	}

	rec := &recordingObserver{}
	result, err := Sync(context.Background(), client, mt, SyncOpts{
		Push:        true,
		Pull:        true,
		ProjectCode: projCode,
		ServerCode:  srvCode,
		Observer:    rec,
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	if rec.started != 1 {
		t.Errorf("Started called %d times, want 1", rec.started)
	}
	if rec.completed != 1 {
		t.Errorf("Completed called %d times, want 1", rec.completed)
	}
	if len(rec.roundsStarted) != result.Rounds {
		t.Errorf("RoundStarted called %d times, want %d", len(rec.roundsStarted), result.Rounds)
	}
	if len(rec.roundsCompleted) != result.Rounds {
		t.Errorf("RoundCompleted called %d times, want %d", len(rec.roundsCompleted), result.Rounds)
	}
	if rec.lastInfo.Operation != "sync" {
		t.Errorf("Operation = %q, want %q", rec.lastInfo.Operation, "sync")
	}
	if rec.lastEnd.Rounds != result.Rounds {
		t.Errorf("SessionEnd.Rounds = %d, want %d", rec.lastEnd.Rounds, result.Rounds)
	}
	if rec.lastErr != nil {
		t.Errorf("SessionEnd err = %v, want nil", rec.lastErr)
	}
}
```

Also add these imports to the test file's import block:

```go
"path/filepath"
"github.com/dmestas/edgesync/go-libfossil/repo"
"github.com/dmestas/edgesync/go-libfossil/simio"
"github.com/dmestas/edgesync/go-libfossil/xfer"
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -run TestSyncCallsObserverHooks -v
```

Expected: FAIL — `SyncOpts` has no `Observer` field.

- [ ] **Step 3: Add Observer field to SyncOpts in session.go**

In `go-libfossil/sync/session.go`, add to `SyncOpts` struct:

```go
Observer                Observer          // nil defaults to no-op
```

- [ ] **Step 4: Hook observer into Sync() in session.go**

Replace the `Sync` function body in `go-libfossil/sync/session.go`:

```go
func Sync(ctx context.Context, r *repo.Repo, t Transport, opts SyncOpts) (result *SyncResult, err error) {
	if r == nil {
		panic("sync.Sync: r must not be nil")
	}
	if t == nil {
		panic("sync.Sync: t must not be nil")
	}
	defer func() {
		if err == nil && result == nil {
			panic("sync.Sync: result must not be nil on success")
		}
	}()

	obs := resolveObserver(opts.Observer)
	s := newSession(r, opts)

	ctx = obs.Started(ctx, SessionStart{
		Operation:   "sync",
		Push:        opts.Push,
		Pull:        opts.Pull,
		UV:          opts.UV,
		ProjectCode: opts.ProjectCode,
	})

	for cycle := 0; ; cycle++ {
		select {
		case <-ctx.Done():
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), ctx.Err())
			return &s.result, ctx.Err()
		default:
		}
		if cycle >= MaxRounds {
			err := fmt.Errorf("sync: exceeded %d rounds", MaxRounds)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, err
		}

		roundCtx := obs.RoundStarted(ctx, cycle)

		req, err := s.buildRequest(cycle)
		if err != nil {
			obs.RoundCompleted(roundCtx, cycle, 0, 0)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, fmt.Errorf("sync: buildRequest round %d: %w", cycle, err)
		}
		resp, err := t.Exchange(ctx, req)
		if err != nil {
			obs.RoundCompleted(roundCtx, cycle, 0, 0)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, fmt.Errorf("sync: exchange round %d: %w", cycle, err)
		}

		sentBefore := s.result.FilesSent
		recvdBefore := s.result.FilesRecvd

		done, err := s.processResponse(resp)
		if err != nil {
			obs.RoundCompleted(roundCtx, cycle, s.result.FilesSent-sentBefore, s.result.FilesRecvd-recvdBefore)
			obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), err)
			return &s.result, fmt.Errorf("sync: processResponse round %d: %w", cycle, err)
		}
		s.result.Rounds = cycle + 1

		obs.RoundCompleted(roundCtx, cycle, s.result.FilesSent-sentBefore, s.result.FilesRecvd-recvdBefore)

		if done {
			break
		}
	}

	obs.Completed(ctx, sessionEndFromSync(&s.result, opts.ProjectCode), nil)
	return &s.result, nil
}

// sessionEndFromSyncResult builds a SessionEnd from a SyncResult.
func sessionEndFromSync(r *SyncResult, projectCode string) SessionEnd {
	return SessionEnd{
		Operation:    "sync",
		Rounds:       r.Rounds,
		FilesSent:    r.FilesSent,
		FilesRecvd:   r.FilesRecvd,
		UVFilesSent:  r.UVFilesSent,
		UVFilesRecvd: r.UVFilesRecvd,
		UVGimmesSent: r.UVGimmesSent,
		ProjectCode:  projectCode,
		Errors:       r.Errors,
	}
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -run TestSyncCallsObserverHooks -v
```

Expected: PASS

- [ ] **Step 6: Run all existing sync tests to verify no regressions**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -v -count=1
```

Expected: All existing tests PASS (nil Observer falls back to nopObserver).

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/session.go go-libfossil/sync/observer_test.go
git commit -m "sync: hook Observer into Sync() lifecycle"
```

---

### Task 4: Add Observer field to CloneOpts and hook into Clone()

**Files:**
- Modify: `go-libfossil/sync/stubs.go` (CloneOpts)
- Modify: `go-libfossil/sync/clone.go` (Clone + run)
- Modify: `go-libfossil/sync/observer_test.go` (add clone test)

- [ ] **Step 1: Write the failing test — observer receives hooks during Clone()**

Append to `go-libfossil/sync/observer_test.go`:

```go
func TestCloneCallsObserverHooks(t *testing.T) {
	serverPath := filepath.Join(t.TempDir(), "server.fossil")
	clonePath := filepath.Join(t.TempDir(), "clone.fossil")
	env := simio.RealEnv()

	server, err := repo.Create(serverPath, "test", env.Rand)
	if err != nil {
		t.Fatal(err)
	}
	defer server.Close()

	mt := &MockTransport{
		Handler: func(req *xfer.Message) *xfer.Message {
			resp, _ := HandleSync(context.Background(), server, req)
			return resp
		},
	}

	rec := &recordingObserver{}
	r, result, err := Clone(context.Background(), clonePath, mt, CloneOpts{
		Observer: rec,
	})
	if err != nil {
		t.Fatalf("Clone: %v", err)
	}
	defer r.Close()

	if rec.started != 1 {
		t.Errorf("Started called %d times, want 1", rec.started)
	}
	if rec.completed != 1 {
		t.Errorf("Completed called %d times, want 1", rec.completed)
	}
	if len(rec.roundsStarted) != result.Rounds {
		t.Errorf("RoundStarted called %d times, want %d", len(rec.roundsStarted), result.Rounds)
	}
	if rec.lastInfo.Operation != "clone" {
		t.Errorf("Operation = %q, want %q", rec.lastInfo.Operation, "clone")
	}
	if rec.lastEnd.Rounds != result.Rounds {
		t.Errorf("SessionEnd.Rounds = %d, want %d", rec.lastEnd.Rounds, result.Rounds)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -run TestCloneCallsObserverHooks -v
```

Expected: FAIL — `CloneOpts` has no `Observer` field.

- [ ] **Step 3: Add Observer field to CloneOpts in stubs.go**

In `go-libfossil/sync/stubs.go`, add to `CloneOpts`:

```go
Observer Observer    // nil defaults to no-op
```

- [ ] **Step 4: Hook observer into Clone() and run() in clone.go**

In `go-libfossil/sync/clone.go`, modify the `Clone` function to resolve the observer and pass it into `cloneSession`:

Add `obs Observer` field to `cloneSession` struct:

```go
type cloneSession struct {
	repo        *repo.Repo
	env         *simio.Env
	opts        CloneOpts
	result      CloneResult
	phantoms    map[string]bool
	seqno       int
	projectCode string
	serverCode  string
	obs         Observer
}
```

In `Clone()`, after creating `cloneSession`, set `cs.obs = resolveObserver(opts.Observer)`.

In `run()`, add observer hooks mirroring the Sync() pattern:

```go
func (cs *cloneSession) run(ctx context.Context, t Transport) (*CloneResult, error) {
	ctx = cs.obs.Started(ctx, SessionStart{
		Operation: "clone",
		Pull:      true,
	})

	prevPhantomCount := -1

	for cycle := 0; ; cycle++ {
		select {
		case <-ctx.Done():
			cs.obs.Completed(ctx, sessionEndFromClone(&cs.result), ctx.Err())
			return &cs.result, ctx.Err()
		default:
		}
		if cycle >= MaxRounds {
			err := fmt.Errorf("sync.Clone: exceeded %d rounds", MaxRounds)
			cs.obs.Completed(ctx, sessionEndFromClone(&cs.result), err)
			return &cs.result, err
		}

		roundCtx := cs.obs.RoundStarted(ctx, cycle)

		req, err := cs.buildRequest(cycle)
		if err != nil {
			cs.obs.RoundCompleted(roundCtx, cycle, 0, 0)
			cs.obs.Completed(ctx, sessionEndFromClone(&cs.result), err)
			return &cs.result, fmt.Errorf("sync.Clone: buildRequest round %d: %w", cycle, err)
		}

		resp, err := t.Exchange(ctx, req)
		if err != nil {
			cs.obs.RoundCompleted(roundCtx, cycle, 0, 0)
			cs.obs.Completed(ctx, sessionEndFromClone(&cs.result), err)
			return &cs.result, fmt.Errorf("sync.Clone: exchange round %d: %w", cycle, err)
		}

		recvdBefore := cs.result.BlobsRecvd

		done, err := cs.processResponse(resp)
		if err != nil {
			cs.obs.RoundCompleted(roundCtx, cycle, 0, cs.result.BlobsRecvd-recvdBefore)
			cs.obs.Completed(ctx, sessionEndFromClone(&cs.result), err)
			return &cs.result, fmt.Errorf("sync.Clone: process round %d: %w", cycle, err)
		}

		cs.result.Rounds = cycle + 1
		cs.obs.RoundCompleted(roundCtx, cycle, 0, cs.result.BlobsRecvd-recvdBefore)

		if cycle >= 1 && done {
			if cs.seqno <= 0 && len(cs.phantoms) == 0 {
				break
			}
			phantomCount := len(cs.phantoms)
			if cs.seqno <= 0 && phantomCount > 0 && phantomCount >= prevPhantomCount {
				break
			}
			prevPhantomCount = phantomCount
			if cs.seqno <= 0 {
				continue
			}
		}
		if cycle >= 1 {
			prevPhantomCount = len(cs.phantoms)
		}
	}

	cs.result.ProjectCode = cs.projectCode
	cs.result.ServerCode = cs.serverCode
	cs.obs.Completed(ctx, sessionEndFromClone(&cs.result), nil)
	return &cs.result, nil
}

// sessionEndFromClone builds a SessionEnd from a CloneResult.
func sessionEndFromClone(r *CloneResult) SessionEnd {
	return SessionEnd{
		Operation:   "clone",
		Rounds:      r.Rounds,
		FilesRecvd:  r.BlobsRecvd,
		ProjectCode: r.ProjectCode,
		Errors:      r.Messages,
	}
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ -run TestCloneCallsObserverHooks -v
```

Expected: PASS

- [ ] **Step 6: Run ALL tests across workspace to verify no regressions**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/sync/ ./leaf/agent/ -v -count=1
```

Expected: All PASS. Agent tests use nil Observer → nopObserver.

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/sync/stubs.go go-libfossil/sync/clone.go go-libfossil/sync/observer_test.go
git commit -m "sync: hook Observer into Clone() lifecycle"
```

---

## Chunk 2: OTel Implementation in leaf/telemetry/

### Task 5: Add OTel dependencies to leaf/go.mod

**Files:**
- Modify: `leaf/go.mod`

- [ ] **Step 1: Add OTel dependencies**

```bash
cd /Users/dmestas/projects/edgesync/leaf && go get \
  go.opentelemetry.io/otel \
  go.opentelemetry.io/otel/sdk \
  go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp \
  go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp \
  go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp \
  go.opentelemetry.io/otel/sdk/metric \
  go.opentelemetry.io/otel/sdk/log \
  go.opentelemetry.io/contrib/bridges/otelslog
```

- [ ] **Step 2: Tidy**

```bash
cd /Users/dmestas/projects/edgesync/leaf && go mod tidy
```

- [ ] **Step 3: Verify build**

```bash
cd /Users/dmestas/projects/edgesync && go build -buildvcs=false ./leaf/...
```

Expected: Builds without errors.

- [ ] **Step 4: Commit**

```bash
git add leaf/go.mod leaf/go.sum
git commit -m "leaf: add OpenTelemetry dependencies"
```

---

### Task 6: Telemetry setup (leaf/telemetry/setup.go)

**Files:**
- Create: `leaf/telemetry/setup.go`

- [ ] **Step 1: Create setup.go**

```go
//go:build !wasip1 && !js

// Package telemetry provides OpenTelemetry instrumentation for the leaf agent.
package telemetry

import (
	"context"
	"errors"

	"go.opentelemetry.io/contrib/bridges/otelslog"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlplog/otlploghttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlpmetric/otlpmetrichttp"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracehttp"
	"go.opentelemetry.io/otel/log/global"
	sdklog "go.opentelemetry.io/otel/sdk/log"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"log/slog"
)

// TelemetryConfig configures the OTel SDK.
// Empty fields fall back to standard OTEL_* environment variables.
type TelemetryConfig struct {
	ServiceName string            // default: "edgesync-leaf"
	Endpoint    string            // e.g. "https://api.honeycomb.io"
	Headers     map[string]string // e.g. {"x-honeycomb-team": "<key>"}
}

// Setup initializes the OTel SDK (traces, metrics, logs) and configures
// slog with trace correlation. Returns a shutdown function that flushes
// all pending telemetry. When no endpoint is configured (and OTEL_*
// env vars are unset), returns a no-op shutdown.
func Setup(ctx context.Context, cfg TelemetryConfig) (shutdown func(context.Context) error, err error) {
	serviceName := cfg.ServiceName
	if serviceName == "" {
		serviceName = "edgesync-leaf"
	}

	res, err := resource.Merge(
		resource.Default(),
		resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName(serviceName),
		),
	)
	if err != nil {
		return nil, err
	}

	var shutdowns []func(context.Context) error

	// Trace provider
	traceOpts := []otlptracehttp.Option{}
	if cfg.Endpoint != "" {
		traceOpts = append(traceOpts, otlptracehttp.WithEndpoint(cfg.Endpoint))
	}
	if len(cfg.Headers) > 0 {
		traceOpts = append(traceOpts, otlptracehttp.WithHeaders(cfg.Headers))
	}
	traceExp, err := otlptracehttp.New(ctx, traceOpts...)
	if err != nil {
		return nil, err
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(traceExp),
		sdktrace.WithResource(res),
	)
	otel.SetTracerProvider(tp)
	shutdowns = append(shutdowns, tp.Shutdown)

	// Metric provider
	metricOpts := []otlpmetrichttp.Option{}
	if cfg.Endpoint != "" {
		metricOpts = append(metricOpts, otlpmetrichttp.WithEndpoint(cfg.Endpoint))
	}
	if len(cfg.Headers) > 0 {
		metricOpts = append(metricOpts, otlpmetrichttp.WithHeaders(cfg.Headers))
	}
	metricExp, err := otlpmetrichttp.New(ctx, metricOpts...)
	if err != nil {
		return nil, err
	}
	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithReader(sdkmetric.NewPeriodicReader(metricExp)),
		sdkmetric.WithResource(res),
	)
	otel.SetMeterProvider(mp)
	shutdowns = append(shutdowns, mp.Shutdown)

	// Log provider + slog bridge
	logOpts := []otlploghttp.Option{}
	if cfg.Endpoint != "" {
		logOpts = append(logOpts, otlploghttp.WithEndpoint(cfg.Endpoint))
	}
	if len(cfg.Headers) > 0 {
		logOpts = append(logOpts, otlploghttp.WithHeaders(cfg.Headers))
	}
	logExp, err := otlploghttp.New(ctx, logOpts...)
	if err != nil {
		return nil, err
	}
	lp := sdklog.NewLoggerProvider(
		sdklog.WithProcessor(sdklog.NewBatchProcessor(logExp)),
		sdklog.WithResource(res),
	)
	global.SetLoggerProvider(lp)
	shutdowns = append(shutdowns, lp.Shutdown)

	// Set slog default to otelslog bridge
	logger := otelslog.NewLogger("edgesync-leaf")
	slog.SetDefault(logger)

	return func(ctx context.Context) error {
		var errs []error
		for i := len(shutdowns) - 1; i >= 0; i-- {
			if err := shutdowns[i](ctx); err != nil {
				errs = append(errs, err)
			}
		}
		return errors.Join(errs...)
	}, nil
}
```

- [ ] **Step 2: Verify it compiles**

```bash
cd /Users/dmestas/projects/edgesync && go build -buildvcs=false ./leaf/telemetry/
```

Expected: Builds without errors.

- [ ] **Step 3: Commit**

```bash
git add leaf/telemetry/setup.go
git commit -m "telemetry: add OTel SDK setup with OTLP HTTP exporters and slog bridge"
```

---

### Task 7: OTelObserver implementation

**Files:**
- Create: `leaf/telemetry/observer.go`
- Create: `leaf/telemetry/observer_test.go`

- [ ] **Step 1: Write observer_test.go**

```go
//go:build !wasip1 && !js

package telemetry

import (
	"context"
	"testing"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	"go.opentelemetry.io/otel/sdk/trace/tracetest"
)

func TestOTelObserverCreatesSpans(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithSyncer(exporter),
	)

	obs := NewOTelObserver(tp, nil)

	ctx := context.Background()
	ctx = obs.Started(ctx, libsync.SessionStart{
		Operation:   "sync",
		Push:        true,
		Pull:        true,
		UV:          false,
		ProjectCode: "abc123",
	})
	roundCtx := obs.RoundStarted(ctx, 0)
	obs.RoundCompleted(roundCtx, 0, 5, 3)
	obs.Completed(ctx, libsync.SessionEnd{
		Operation:  "sync",
		Rounds:     1,
		FilesSent:  5,
		FilesRecvd: 3,
	}, nil)

	spans := exporter.GetSpans()
	if len(spans) != 2 {
		t.Fatalf("got %d spans, want 2 (session + round)", len(spans))
	}

	// Round span ends first (child), then session span.
	roundSpan := spans[0]
	sessionSpan := spans[1]

	if roundSpan.Name != "sync.round" {
		t.Errorf("round span name = %q, want %q", roundSpan.Name, "sync.round")
	}
	if sessionSpan.Name != "sync.session" {
		t.Errorf("session span name = %q, want %q", sessionSpan.Name, "sync.session")
	}
}

func TestOTelObserverCloneSpanNames(t *testing.T) {
	exporter := tracetest.NewInMemoryExporter()
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithSyncer(exporter),
	)

	obs := NewOTelObserver(tp, nil)

	ctx := context.Background()
	ctx = obs.Started(ctx, libsync.SessionStart{Operation: "clone"})
	roundCtx := obs.RoundStarted(ctx, 0)
	obs.RoundCompleted(roundCtx, 0, 0, 10)
	obs.Completed(ctx, libsync.SessionEnd{Operation: "clone", Rounds: 1, FilesRecvd: 10}, nil)

	spans := exporter.GetSpans()
	if len(spans) != 2 {
		t.Fatalf("got %d spans, want 2", len(spans))
	}
	if spans[0].Name != "clone.round" {
		t.Errorf("round span name = %q, want %q", spans[0].Name, "clone.round")
	}
	if spans[1].Name != "clone.session" {
		t.Errorf("session span name = %q, want %q", spans[1].Name, "clone.session")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./leaf/telemetry/ -run TestOTelObserver -v
```

Expected: FAIL — `NewOTelObserver` not defined.

- [ ] **Step 3: Write observer.go**

```go
//go:build !wasip1 && !js

package telemetry

import (
	"context"
	"time"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"
	"go.opentelemetry.io/otel/trace"
)

const instrumentationName = "edgesync-leaf"

type operationKey struct{}
type startTimeKey struct{}

func withOperation(ctx context.Context, op string) context.Context {
	return context.WithValue(ctx, operationKey{}, op)
}

func operationFromContext(ctx context.Context) string {
	if op, ok := ctx.Value(operationKey{}).(string); ok {
		return op
	}
	return "sync"
}

// OTelObserver implements sync.Observer using OpenTelemetry spans and metrics.
type OTelObserver struct {
	tracer trace.Tracer

	sessionsTotal metric.Int64Counter
	errorsTotal   metric.Int64Counter
	duration      metric.Float64Histogram
	rounds        metric.Int64Histogram
	filesSent     metric.Int64Histogram
	filesRecvd    metric.Int64Histogram
	uvFilesSent   metric.Int64Histogram
	uvFilesRecvd  metric.Int64Histogram
}

// NewOTelObserver creates an OTelObserver from the given providers.
// Pass nil for either to use the globally registered provider (set by Setup).
func NewOTelObserver(tp trace.TracerProvider, mp metric.MeterProvider) *OTelObserver {
	if tp == nil {
		tp = otel.GetTracerProvider()
	}
	if mp == nil {
		mp = otel.GetMeterProvider()
	}
	m := mp.Meter(instrumentationName)
	obs := &OTelObserver{
		tracer: tp.Tracer(instrumentationName),
	}
	obs.sessionsTotal, _ = m.Int64Counter("sync.sessions.total",
		metric.WithDescription("Total sync/clone sessions"))
	obs.errorsTotal, _ = m.Int64Counter("sync.errors.total",
		metric.WithDescription("Sessions ending with error"))
	obs.duration, _ = m.Float64Histogram("sync.duration.seconds",
		metric.WithDescription("End-to-end session duration"),
		metric.WithUnit("s"))
	obs.rounds, _ = m.Int64Histogram("sync.rounds",
		metric.WithDescription("Rounds to convergence"))
	obs.filesSent, _ = m.Int64Histogram("sync.files.sent",
		metric.WithDescription("Files sent per session"))
	obs.filesRecvd, _ = m.Int64Histogram("sync.files.received",
		metric.WithDescription("Files received per session"))
	obs.uvFilesSent, _ = m.Int64Histogram("sync.uv.files.sent",
		metric.WithDescription("UV files sent per session"))
	obs.uvFilesRecvd, _ = m.Int64Histogram("sync.uv.files.received",
		metric.WithDescription("UV files received per session"))
	return obs
}

func (o *OTelObserver) Started(ctx context.Context, info libsync.SessionStart) context.Context {
	ctx = withOperation(ctx, info.Operation)
	ctx = context.WithValue(ctx, startTimeKey{}, time.Now())
	ctx, _ = o.tracer.Start(ctx, info.Operation+".session",
		trace.WithAttributes(
			attribute.String("sync.operation", info.Operation),
			attribute.Bool("sync.push", info.Push),
			attribute.Bool("sync.pull", info.Pull),
			attribute.Bool("sync.uv", info.UV),
			attribute.String("sync.project_code", info.ProjectCode),
		),
	)
	return ctx
}

func (o *OTelObserver) RoundStarted(ctx context.Context, round int) context.Context {
	op := operationFromContext(ctx)
	ctx, _ = o.tracer.Start(ctx, op+".round",
		trace.WithAttributes(
			attribute.Int("sync.round", round),
		),
	)
	return ctx
}

func (o *OTelObserver) RoundCompleted(ctx context.Context, round int, sent, recvd int) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("sync.round.files_sent", sent),
		attribute.Int("sync.round.files_received", recvd),
	)
	span.End()
}

func (o *OTelObserver) Completed(ctx context.Context, info libsync.SessionEnd, err error) {
	span := trace.SpanFromContext(ctx)
	span.SetAttributes(
		attribute.Int("sync.rounds", info.Rounds),
		attribute.Int("sync.files_sent", info.FilesSent),
		attribute.Int("sync.files_received", info.FilesRecvd),
		attribute.Int("sync.uv_files_sent", info.UVFilesSent),
		attribute.Int("sync.uv_files_received", info.UVFilesRecvd),
		attribute.Int("sync.errors_count", len(info.Errors)),
	)
	if err != nil {
		span.RecordError(err)
	}
	span.End()

	attrs := metric.WithAttributes(
		attribute.String("sync.operation", info.Operation),
		attribute.String("project.code", info.ProjectCode),
	)
	o.sessionsTotal.Add(ctx, 1, attrs)
	if err != nil {
		o.errorsTotal.Add(ctx, 1, attrs)
	}
	if startTime, ok := ctx.Value(startTimeKey{}).(time.Time); ok {
		o.duration.Record(ctx, time.Since(startTime).Seconds(), attrs)
	}
	o.rounds.Record(ctx, int64(info.Rounds), attrs)
	o.filesSent.Record(ctx, int64(info.FilesSent), attrs)
	o.filesRecvd.Record(ctx, int64(info.FilesRecvd), attrs)
	o.uvFilesSent.Record(ctx, int64(info.UVFilesSent), attrs)
	o.uvFilesRecvd.Record(ctx, int64(info.UVFilesRecvd), attrs)
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./leaf/telemetry/ -run TestOTelObserver -v
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add leaf/telemetry/observer.go leaf/telemetry/observer_test.go
git commit -m "telemetry: add OTelObserver with spans and metrics"
```

---

### Task 8: WASM no-op stubs

**Files:**
- Create: `leaf/telemetry/noop.go`

- [ ] **Step 1: Create noop.go**

```go
//go:build wasip1 || js

package telemetry

import (
	"context"

	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
)

// TelemetryConfig is a stub for WASM builds.
type TelemetryConfig struct {
	ServiceName string
	Endpoint    string
	Headers     map[string]string
}

// Setup is a no-op on WASM — returns a no-op shutdown function.
func Setup(_ context.Context, _ TelemetryConfig) (func(context.Context) error, error) {
	return func(context.Context) error { return nil }, nil
}

// OTelObserver is a no-op on WASM builds.
type OTelObserver struct{}

// NewOTelObserver returns a no-op observer on WASM builds.
func NewOTelObserver(_ any, _ any) *OTelObserver {
	return &OTelObserver{}
}

func (*OTelObserver) Started(ctx context.Context, _ libsync.SessionStart) context.Context {
	return ctx
}

func (*OTelObserver) RoundStarted(ctx context.Context, _ int) context.Context {
	return ctx
}

func (*OTelObserver) RoundCompleted(_ context.Context, _ int, _, _ int) {}

func (*OTelObserver) Completed(_ context.Context, _ libsync.SessionEnd, _ error) {}
```

- [ ] **Step 2: Verify native build still works**

```bash
cd /Users/dmestas/projects/edgesync && go build -buildvcs=false ./leaf/telemetry/
```

Expected: Builds (noop.go excluded by build tags on native).

- [ ] **Step 3: Commit**

```bash
git add leaf/telemetry/noop.go
git commit -m "telemetry: add WASM no-op stubs"
```

---

## Chunk 3: Agent Wiring + CLI

### Task 9: Add Observer to agent Config and migrate to slog

**Files:**
- Modify: `leaf/agent/config.go`
- Modify: `leaf/agent/agent.go`
- Modify: `leaf/agent/serve_nats.go`

- [ ] **Step 1: Add Observer field to Config in config.go**

Add to the `Config` struct in `leaf/agent/config.go`, after the `Buggify` field:

```go
// Observer receives telemetry callbacks during sync operations.
// Nil defaults to no-op (no telemetry).
Observer libsync.Observer
```

Update the import to include the libsync alias (it's already imported as `libsync`).

- [ ] **Step 2: Pass Observer into SyncOpts in agent.go runSync()**

In `leaf/agent/agent.go`, modify `runSync()` to include the Observer:

```go
func (a *Agent) runSync(ctx context.Context) (*sync.SyncResult, error) {
	opts := sync.SyncOpts{
		Push:        a.config.Push,
		Pull:        a.config.Pull,
		ProjectCode: a.projectCode,
		ServerCode:  a.serverCode,
		User:        a.config.User,
		Password:    a.config.Password,
		Buggify:     a.config.Buggify,
		UV:          a.config.UV,
		Observer:    a.config.Observer,
	}
	return sync.Sync(ctx, a.repo, a.transport, opts)
}
```

- [ ] **Step 3: Migrate agent.go from log.Printf to slog**

Replace the import of `"log"` with `"log/slog"` in `leaf/agent/agent.go`.

Replace all `log.Printf` calls in `pollLoop()`:

```go
// Before:
log.Printf("agent: sync error: %v", act.Err)
// After:
slog.ErrorContext(ctx, "sync error", "error", act.Err)

// Before:
log.Printf("agent: sync done: rounds=%d sent=%d recv=%d errors=%d",
    act.Result.Rounds, act.Result.FilesSent, act.Result.FilesRecvd, len(act.Result.Errors))
// After:
slog.InfoContext(ctx, "sync done",
    "rounds", act.Result.Rounds,
    "sent", act.Result.FilesSent,
    "recv", act.Result.FilesRecvd,
    "errors", len(act.Result.Errors))

// Before:
for _, e := range act.Result.Errors {
    log.Printf("agent: sync: %s", e)
}
// After:
for _, e := range act.Result.Errors {
    slog.WarnContext(ctx, "sync protocol error", "detail", e)
}
```

Also replace the `log.Printf` calls in `Start()`:

```go
// Before:
log.Printf("agent: serve-http stopped: %v", err)
// After:
slog.Error("serve-http stopped", "error", err)

// Before:
log.Printf("agent: serve-nats stopped: %v", err)
// After:
slog.Error("serve-nats stopped", "error", err)
```

- [ ] **Step 4: Migrate serve_nats.go from log.Printf to slog**

In `leaf/agent/serve_nats.go`, replace `"log"` import with `"log/slog"`:

```go
// Before:
log.Printf("serve-nats: decode error: %v", err)
// After:
slog.ErrorContext(ctx, "serve-nats: decode error", "error", err)

// Before:
log.Printf("serve-nats: handler error: %v", err)
// After:
slog.ErrorContext(ctx, "serve-nats: handler error", "error", err)

// Before:
log.Printf("serve-nats: encode error: %v", err)
// After:
slog.ErrorContext(ctx, "serve-nats: encode error", "error", err)

// Before:
log.Printf("serve-nats: respond error: %v", err)
// After:
slog.ErrorContext(ctx, "serve-nats: respond error", "error", err)
```

Note: The `ctx` in the NATS subscribe callback comes from the outer `ServeNATS` function parameter — it's already in scope.

- [ ] **Step 5: Run agent tests to verify no regressions**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./leaf/agent/ -v -count=1
```

Expected: All PASS.

- [ ] **Step 6: Commit**

```bash
git add leaf/agent/config.go leaf/agent/agent.go leaf/agent/serve_nats.go
git commit -m "agent: add Observer to Config, migrate log.Printf to slog"
```

---

### Task 10: CLI wiring and shutdown sequencing

**Files:**
- Modify: `leaf/cmd/leaf/main.go`

- [ ] **Step 1: Update main.go with OTel flags, Setup(), and shutdown**

Replace the full content of `leaf/cmd/leaf/main.go`:

```go
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/dmestas/edgesync/leaf/agent"
	"github.com/dmestas/edgesync/leaf/telemetry"
)

func main() {
	repoPath := flag.String("repo", envOrDefault("LEAF_REPO", ""), "path to Fossil repository file (required)")
	natsURL := flag.String("nats", envOrDefault("LEAF_NATS_URL", "nats://localhost:4222"), "NATS server URL")
	poll := flag.Duration("poll", 5*time.Second, "poll interval")
	user := flag.String("user", envOrDefault("LEAF_USER", ""), "Fossil user name")
	password := flag.String("password", envOrDefault("LEAF_PASSWORD", ""), "Fossil user password")
	push := flag.Bool("push", true, "enable push")
	pull := flag.Bool("pull", true, "enable pull")

	// OTel flags (fall back to standard OTEL_* env vars)
	otelEndpoint := flag.String("otel-endpoint", envOrDefault("OTEL_EXPORTER_OTLP_ENDPOINT", ""), "OTel OTLP endpoint")
	otelHeaders := flag.String("otel-headers", envOrDefault("OTEL_EXPORTER_OTLP_HEADERS", ""), "OTel OTLP headers (key=value,key=value)")
	otelServiceName := flag.String("otel-service-name", envOrDefault("OTEL_SERVICE_NAME", "edgesync-leaf"), "OTel service name")

	flag.Parse()

	if *repoPath == "" {
		fmt.Fprintln(os.Stderr, "error: --repo is required (or set LEAF_REPO)")
		flag.Usage()
		os.Exit(1)
	}

	// Setup telemetry
	ctx := context.Background()
	telCfg := telemetry.TelemetryConfig{
		ServiceName: *otelServiceName,
		Endpoint:    *otelEndpoint,
		Headers:     parseHeaders(*otelHeaders),
	}
	shutdown, err := telemetry.Setup(ctx, telCfg)
	if err != nil {
		slog.Error("telemetry setup failed", "error", err)
		os.Exit(1)
	}

	// Create observer (nil-safe: if no endpoint, OTel uses no-op providers)
	obs := telemetry.NewOTelObserver(nil, nil)

	cfg := agent.Config{
		RepoPath:     *repoPath,
		NATSUrl:      *natsURL,
		PollInterval: *poll,
		User:         *user,
		Password:     *password,
		Push:         *push,
		Pull:         *pull,
		Observer:     obs,
	}

	a, err := agent.New(cfg)
	if err != nil {
		slog.Error("agent init failed", "error", err)
		os.Exit(1)
	}

	if err := a.Start(); err != nil {
		slog.Error("agent start failed", "error", err)
		os.Exit(1)
	}

	slog.Info("leaf-agent started", "repo", *repoPath, "nats", *natsURL, "poll", *poll)

	// Signal handling
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM, syscall.SIGUSR1)

	for sig := range sigCh {
		switch sig {
		case syscall.SIGUSR1:
			slog.Info("SIGUSR1 received, triggering sync")
			a.SyncNow()
		case syscall.SIGINT, syscall.SIGTERM:
			slog.Info("shutdown signal received", "signal", sig.String())
			// 1. Stop agent (waits for in-flight sync)
			if err := a.Stop(); err != nil {
				slog.Error("agent stop error", "error", err)
			}
			// 2. Flush telemetry
			shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			if err := shutdown(shutdownCtx); err != nil {
				slog.Error("telemetry shutdown error", "error", err)
			}
			cancel()
			os.Exit(0)
		}
	}
}

// parseHeaders parses "key=value,key=value" into a map.
func parseHeaders(s string) map[string]string {
	if s == "" {
		return nil
	}
	headers := make(map[string]string)
	for _, pair := range strings.Split(s, ",") {
		k, v, ok := strings.Cut(pair, "=")
		if ok {
			headers[strings.TrimSpace(k)] = strings.TrimSpace(v)
		}
	}
	return headers
}

func envOrDefault(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
```

- [ ] **Step 2: Verify build**

```bash
cd /Users/dmestas/projects/edgesync && go build -buildvcs=false ./leaf/cmd/leaf/
```

Expected: Builds without errors.

- [ ] **Step 3: Run all leaf tests**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./leaf/... -v -count=1
```

Expected: All PASS.

- [ ] **Step 4: Commit**

```bash
git add leaf/cmd/leaf/main.go leaf/telemetry/observer.go
git commit -m "cli: add OTel flags, telemetry setup, and shutdown sequencing"
```

---

### Task 11: Full verification

- [ ] **Step 1: Run all tests across the workspace**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./go-libfossil/... ./leaf/... -count=1
```

Expected: All PASS.

- [ ] **Step 2: Build all binaries**

```bash
cd /Users/dmestas/projects/edgesync && make build
```

Expected: All binaries build.

- [ ] **Step 3: Verify WASM build (acceptance criterion)**

```bash
cd /Users/dmestas/projects/edgesync/leaf && GOOS=wasip1 GOARCH=wasm go build -buildvcs=false ./...
```

Expected: Builds without errors. This verifies build tags are correct and OTel deps are excluded from WASM.

- [ ] **Step 4: Verify DST still passes (if applicable)**

```bash
cd /Users/dmestas/projects/edgesync && go test -buildvcs=false ./dst/... -count=1 -timeout 60s
```

Expected: All PASS (DST uses nil Observer → nopObserver).

- [ ] **Step 5: Commit spec and plan**

```bash
git add docs/superpowers/specs/2026-03-19-otel-observability-design.md docs/superpowers/plans/2026-03-19-otel-observability.md
git commit -m "docs: add OTel observability spec and implementation plan"
```

---

**Note on spec discrepancy:** The spec lists `go.opentelemetry.io/otel/exporters/otlp/otlphttp` as a dependency, but the actual packages are the more specific `otlptrace/otlptracehttp`, `otlpmetric/otlpmetrichttp`, and `otlplog/otlploghttp`. The plan uses the correct packages.

**Note on WASM noop.go:** The `NewOTelObserver(_ any, _ any)` signature in `noop.go` differs from the typed `(trace.TracerProvider, metric.MeterProvider)` in `observer.go`. This is intentional — build tags ensure only one is compiled — but if parameters are added to one side, both must be updated.
