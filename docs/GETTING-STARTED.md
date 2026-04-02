# Getting Started with EdgeSync

This guide gets you from zero to running tests in under 5 minutes.

## Prerequisites

| Tool | Required | Version | Install |
|------|----------|---------|---------|
| Go | Yes | 1.26+ | [go.dev/dl](https://go.dev/dl/) |
| Git | Yes | 2.x | system package manager |
| Fossil | For sim/interop tests | 2.x | `brew install fossil` / `apt install fossil` |
| Doppler | For OTel export only | any | `brew install doppler` then `doppler login` |

Check your environment at any time:

```bash
bin/edgesync doctor
```

## Setup

```bash
git clone https://github.com/danmestas/EdgeSync.git
cd EdgeSync
make setup-hooks    # installs pre-commit test gate (~8s)
make build          # produces bin/edgesync, bin/leaf, bin/bridge
make test           # run CI-level tests (~15s)
```

That's it. You're ready to develop.

## Key Commands

```bash
# Daily development
make test              # unit + DST + sim serve + interop-short
make dst               # deterministic sim: 8 seeds (~2s)

# Deeper testing (requires fossil binary)
make sim               # integration sim: 1 seed (~120s)
make sim-full          # 16 seeds x 3 fault severities
make test-interop      # wire-compatible with real fossil

# SQLite driver matrix
make drivers           # test modernc, ncruces, mattn

# Build targets
make build             # all binaries
make wasm              # WASM builds (wasip1 + browser)
```

## Running a Leaf Agent

```bash
# Create a test repo
bin/edgesync repo new /tmp/test.fossil

# Start leaf agent with verbose output
bin/leaf --repo /tmp/test.fossil --serve-http :8080 --verbose

# In another terminal, clone via stock fossil:
fossil clone http://localhost:8080 /tmp/cloned.fossil
```

## Project Structure (Reading Order)

Start broad, go deep as needed:

1. **[README.md](../README.md)** -- Architecture diagram, project layout, module table
2. **[docs/leaf-agent.md](leaf-agent.md)** -- Leaf agent usage, flags, config
3. **[docs/bridge.md](bridge.md)** -- Bridge usage and deployment
4. **Architecture docs** (in `docs/architecture/`):
   - `core-library.md` -- go-libfossil package design, blob format, SQLite drivers
   - `sync-protocol.md` -- xfer card protocol, client/server flow, UV sync
   - `testing-strategy.md` -- test tiers (unit, DST, sim, interop), BUGGIFY
   - `agent-deployment.md` -- Docker, Hetzner VPS, Cloudflare Tunnel
   - `checkout-merge.md` -- checkout/checkin, merge strategies, fork prevention
   - `repo-operations.md` -- CLI, tags, FTS, auth, shun/purge

## Things to Know

- **`-buildvcs=false`** is required for raw `go build` (dual VCS: git + fossil). The Makefile handles this for you.
- **Five Go modules** in a workspace (`go.work`): root, go-libfossil, leaf, bridge, dst. Use `go test ./go-libfossil/...` etc.
- **Three SQLite drivers**: modernc (default, pure Go), ncruces (WASM-based), mattn (CGo). Switch via build tags.
- **`fossil/` and `libfossil/`** directories are gitignored -- upstream C reference checkouts for porting. They exist on some dev machines but aren't tracked.
- **Pre-commit hook** runs ~8s of tests. Skip with `git commit --no-verify` (emergency only).
- **Telemetry is optional**. Everything works without Doppler/OTel. Set `OTEL_EXPORTER_OTLP_ENDPOINT` to enable.
