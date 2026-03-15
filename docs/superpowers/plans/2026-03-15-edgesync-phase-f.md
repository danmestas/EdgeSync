# EdgeSync Phase F Implementation Plan: Bridge

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a NATS-to-HTTP bridge that proxies sync requests from leaf agents to a Fossil server's `/xfer` endpoint.

**Architecture:** New `bridge/` Go module. Subscribes to NATS subject, proxies via `sync.HTTPTransport`, replies. CLI binary with graceful shutdown. Also updates leaf agent with configurable subject prefix.

**Tech Stack:** Go 1.25.4, `nats.go`, go-libfossil (`sync/`, `xfer/`)

**Spec:** `docs/superpowers/specs/2026-03-15-edgesync-phase-f-design.md`

---

## Chunk 1: Bridge Module + Core

### Task 1: Module Init + Config

- Create `bridge/go.mod`, update `go.work`
- Create `bridge/bridge/config.go` — Config struct
- Create `bridge/cmd/bridge/main.go` — placeholder
- Create `bridge/bridge/bridge_test.go` — config tests

### Task 2: Bridge Core

- Create `bridge/bridge/bridge.go` — New/Start/Stop, NATS subscriber, HTTP proxy
- Tests: lifecycle, message proxying, error handling

### Task 3: CLI Binary

- Implement `bridge/cmd/bridge/main.go` — flags, env vars, signal handling

## Chunk 2: Leaf Prefix Fix + Integration + Validation

### Task 4: Leaf Agent Subject Prefix

- Add `SubjectPrefix` to leaf `Config` (default "fossil")
- Update `NATSTransport` to use prefix
- Update existing tests

### Task 5: Integration Tests

- Create `bridge/bridge/integration_test.go`
- Full end-to-end: leaf + bridge + NATS + fossil server

### Task 6: Full Validation

- vet, test, race, build for all modules
