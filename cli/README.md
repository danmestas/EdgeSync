# cli

EdgeSync-specific Kong subcommands, embeddable into the unified `edgesync` binary alongside libfossil's repo commands.

Package: `github.com/danmestas/EdgeSync/cli`. Extracted in PR #80.

## What it does

Provides the EdgeSync-only commands that wrap leaf and bridge daemons plus environment health. libfossil supplies the Fossil-compatible repo subcommands; this package adds:

| File | Command | Purpose |
|---|---|---|
| `sync_start.go` | `edgesync sync start` | Run leaf agent in long-running daemon mode |
| `sync_now.go` | `edgesync sync now` | One-shot sync against configured peers |
| `bridge_serve.go` | `edgesync bridge serve` | Run the NATS-to-HTTP bridge |
| `notify.go` | `edgesync notify ...` | Notify init/send/ask/watch/threads/log/status |
| `doctor.go` | `edgesync doctor` | Environment + dependency health check |

Each file exports a Kong-tagged struct (e.g. `SyncStartCmd`) that the root `cmd/edgesync/main.go` mounts directly.

## Build & test

```bash
go build -buildvcs=false -o bin/edgesync ./cmd/edgesync   # `make edgesync`
go test ./cli/... -short -count=1
```

## Where it fits

`cmd/edgesync/main.go` composes `libfossil/cli` (Fossil-compatible repo commands) with this package's daemon and notify commands. Pure logic stays in [`leaf/agent`](../leaf/agent) and [`bridge/bridge`](../bridge/bridge); these files are thin Kong adapters.

See the root [`README.md`](../README.md) for the full command map and [`docs/architecture/notify-messaging.md`](../docs/architecture/notify-messaging.md) for notify semantics.
