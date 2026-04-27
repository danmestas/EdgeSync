# bridge

NATS-to-HTTP translator for talking to an unmodified Fossil server. One project per bridge instance.

Module path: `github.com/danmestas/EdgeSync/bridge`

## What it does

Subscribes to `<prefix>.<project-code>.sync`, decodes incoming xfer messages, proxies them to a Fossil HTTP server via `libfossil.NewHTTPTransport`, and replies on the NATS request/reply channel.

The bridge is the *legacy* path: leaf agents only need it to interoperate with a stock Fossil binary. Pure leaf-to-leaf deployments don't run a bridge at all.

Decode failures and HTTP errors produce an empty response — never leave the leaf hanging, never crash.

## Public API surface

- `bridge.Config` — `FossilURL`, `ProjectCode`, `NATSUrl`, `SubjectPrefix`
- `bridge.New(Config)`, `(*Bridge).Start()`, `Stop()`

| Config Field | Default | CLI Flag | Env Var |
|---|---|---|---|
| `FossilURL` | (required) | `--fossil` | `BRIDGE_FOSSIL_URL` |
| `ProjectCode` | (required) | `--project` | `BRIDGE_PROJECT_CODE` |
| `NATSUrl` | `nats://localhost:4222` | `--nats` | `BRIDGE_NATS_URL` |
| `SubjectPrefix` | `"fossil"` | `--prefix` | `BRIDGE_PREFIX` |

CLI: `cmd/bridge/main.go`.

## Build & test

```bash
cd bridge && go build -buildvcs=false -o ../bin/bridge ./cmd/bridge  # or `make bridge`
cd bridge && go test ./... -short -count=1
```

## Where it fits

Optional middle tier. The leaf agent and the bridge must agree on `SubjectPrefix` for a given project. JetStream is a drop-in upgrade — replace `Request` with publish/consume, the `Exchange` signature is unchanged.

See [`docs/architecture/agent-deployment.md`](../docs/architecture/agent-deployment.md) (Bridge Architecture section) for full details.
