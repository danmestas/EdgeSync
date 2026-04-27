# sim

Integration simulation tests for EdgeSync. Real NATS, real Fossil, real `leaf` and `bridge` binaries — wrapped in a deterministic harness with a TCP fault-injection proxy.

Lives in the root module: `github.com/danmestas/EdgeSync/sim`. (Deterministic single-threaded sim tests live in [libfossil's `dst/`](https://github.com/danmestas/libfossil) — see [ADR notes in the root `README.md`](../README.md).)

## What it does

- **TCP fault proxy** (`proxy.go`) — drops, delays, partitions, and corrupts traffic between NATS, leaf, and bridge processes. Seeded RNG drives the schedule.
- **Harness** (`harness.go`) — starts processes, points them at the proxy, asserts invariants on convergence.
- **Serve tests** (`serve_test.go`) — exercise `fossil clone` and `fossil sync` against the leaf's `ServeHTTP`; verify a stock Fossil binary is a valid client.
- **Interop tests** (`interop_test.go`, `diff_interop_test.go`, `finfo_interop_test.go`) — Tier-1 byte-equivalence and Tier-2 sampling against reference Fossil checkouts.
- **BUGGIFY** (`buggify.go`) — stochastic fault injection inside libfossil's sync loop, gated by `sync.BuggifyChecker`.
- **Iroh tests** (`iroh_test.go`, `nats_iroh_test.go`) — leaf-to-leaf over the iroh sidecar; full QUIC + Unix-socket lifecycle.
- **Soak runner** (`cmd/soak/`) — continuous random-seed runner for catching rare schedules.

## How to run

```bash
make sim         # 1 seed, normal severity, ~1m
make sim-full    # 16 seeds × 3 severities (normal/adversarial/hostile), ~30m
make test-interop  # Tier-1 + Tier-2, requires `fossil` binary on PATH
make test-iroh   # build iroh-sidecar then run iroh tests
```

Direct: `cd sim && go test . -run TestSimulation -sim.seed=N -sim.severity=hostile -timeout=120s`.

## Where it fits

Sim is the integration-tier test bench: it boots real binaries with fault injection. Faster deterministic single-threaded tests (DST) live in libfossil. Per-module unit tests live alongside their code (`leaf/...`, `bridge/...`).

See [`docs/architecture/testing-strategy.md`](../docs/architecture/testing-strategy.md) for the test pyramid.
