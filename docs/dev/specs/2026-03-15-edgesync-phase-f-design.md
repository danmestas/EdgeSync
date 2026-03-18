# EdgeSync Phase F Design Spec: Bridge

NATS-to-HTTP bridge that translates sync requests from leaf agents into Fossil's HTTP `/xfer` protocol. Subscribes to a NATS subject, proxies each request to a Fossil server, and replies with the response. Single project per instance.

## Scope

Extract the test bridge responder (from Phase E integration tests) into a production binary with proper config, logging, lifecycle, and error handling.

**In scope:** NATS subscriber, HTTP proxy to fossil server via `sync.HTTPTransport`, CLI binary, configurable NATS subject prefix, graceful shutdown.
**Out of scope:** Multi-project (future), authentication/authorization on NATS side (future — use NATS NKeys/JWT), request transformation, caching.

## Constraints

- Go 1.25.4
- External dependency: `github.com/nats-io/nats.go` (runtime), `github.com/nats-io/nats-server/v2` (test only)
- Same testing standards as all phases: TDD, race clean
- `fossil server` as test dependency for integration tests

## Project Structure

```
EdgeSync/
  go-libfossil/           # existing
  leaf/                    # existing (Phase E)
  bridge/                  # NEW (own go.mod)
    cmd/bridge/
      main.go              # CLI entry point
    bridge/
      bridge.go            # Bridge struct: New, Start, Stop
      config.go            # Config struct, validation
      bridge_test.go       # Unit tests (embedded NATS)
      integration_test.go  # End-to-end with leaf + fossil server
    go.mod
    go.sum
  go.work                 # MODIFY: add ./bridge
```

### go.work update

```
go 1.25.4

use (
    ./go-libfossil
    ./leaf
    ./bridge
)
```

### bridge/go.mod

```
module github.com/dmestas/edgesync/bridge

go 1.25.4

require (
    github.com/dmestas/edgesync/go-libfossil v0.0.0
    github.com/nats-io/nats.go v1.49.0
    github.com/nats-io/nats-server/v2 v2.12.5 // test only, _test.go imports only
)
```

## NATS Subject Prefix

The NATS subject for sync traffic is: `<prefix>.<project-code>.sync`

Default prefix: `fossil`. Configurable via `--prefix` flag or `BRIDGE_PREFIX` env var. Both the leaf agent and bridge must use the same prefix for a given project.

**Phase E follow-up:** Add `SubjectPrefix` field to leaf's `Config` struct (default `"fossil"`), pass it to `NATSTransport`. Non-breaking — existing behavior unchanged when prefix is empty/default.

## Bridge Core

```go
package bridge

type Config struct {
    NATSUrl       string // default "nats://localhost:4222"
    FossilURL     string // required: e.g. "http://localhost:8080"
    ProjectCode   string // required: determines NATS subject
    SubjectPrefix string // default "fossil"
}

type Bridge struct {
    config Config
    conn   *nats.Conn
    sub    *nats.Subscription
    cancel context.CancelFunc
    done   chan struct{}
}

func New(cfg Config) (*Bridge, error)
func (b *Bridge) Start() error
func (b *Bridge) Stop() error
```

### New(cfg)

1. Apply defaults, validate (FossilURL and ProjectCode required)
2. Connect to NATS
3. Return `*Bridge`

### Start()

1. Create context
2. Subscribe to `<prefix>.<project-code>.sync`
3. Each message handler:
   - `xfer.Decode(msg.Data)` → request
   - `sync.HTTPTransport{URL: fossilURL}.Exchange(ctx, request)` → response
   - `response.Encode()` → reply bytes
   - `msg.Respond(reply)`
   - On decode/exchange error: respond with empty message (don't leave leaf hanging)
4. Log subscription start

### Stop()

1. Unsubscribe from NATS
2. Drain connection
3. Close NATS
4. Log shutdown

## CLI

```
bridge --fossil http://fossil-server:8080 --project <project-code> [--nats nats://localhost:4222] [--prefix fossil]
```

Flags + env vars:
- `--fossil` / `BRIDGE_FOSSIL_URL` (required)
- `--project` / `BRIDGE_PROJECT_CODE` (required)
- `--nats` / `BRIDGE_NATS_URL` (default nats://localhost:4222)
- `--prefix` / `BRIDGE_PREFIX` (default fossil)

Signal handling: SIGINT/SIGTERM → graceful Stop.

## Testing

### Unit Tests (embedded NATS, mock HTTP)

- Bridge New + Stop lifecycle
- Bridge receives NATS message, proxies to HTTP, returns response
- Bridge handles decode error gracefully (bad payload → empty response, not crash)
- Bridge handles HTTP error gracefully (fossil server down → empty response)
- Config validation (FossilURL and ProjectCode required)
- Custom subject prefix works

### Integration Tests (NATS + leaf agent + fossil server)

Full end-to-end: the first test where leaf and bridge run together.

1. Start embedded NATS
2. Start `fossil server` subprocess
3. Create repo with `repo.Create` + `manifest.Checkin`
4. Clone to create matching remote
5. Start Bridge (NATS → fossil server)
6. Start leaf Agent (repo → NATS)
7. Trigger SyncNow on leaf
8. Wait, verify sync completed (check logs / result)
9. Stop both, cleanup

### Leaf Agent Subject Prefix Fix

As part of Phase F, update `leaf/agent/nats.go` to accept a configurable subject prefix (default "fossil"). Add `SubjectPrefix` to leaf's Config. This ensures leaf and bridge agree on the subject.

## Phase F Exit Criteria

1. Bridge binary builds: `go build ./bridge/cmd/bridge/`
2. Bridge subscribes to configurable NATS subject and proxies to fossil server
3. Decode/HTTP errors produce empty response (don't crash or leave leaf hanging)
4. SIGINT/SIGTERM graceful shutdown
5. Custom subject prefix works (non-default prefix, leaf + bridge agree)
6. Integration test: leaf + bridge + fossil server end-to-end
7. All tests green, race clean
8. Leaf agent updated with configurable SubjectPrefix
