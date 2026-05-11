# Hub Package

## Purpose

In-process host for an EdgeSync hub: a fossil hub repo, an embedded NATS server with JetStream, auto-port allocation, and an HTTP server exposing the fossil timeline UI / xfer endpoint. Lets consumers (notably the bones project) host a hub without rolling their own libfossil + nats-server bootstrap.

Before this package, every adopter that needed a hub fossil — a central shared repo that leaves push to and that serves the timeline — had to write the same ~900-line wrapper around `libfossil.Create`/`Open`, `natsserver.NewServer`, JetStream config, port allocation, PID files, and shutdown supervision.

## Public API

All types are libfossil-free and nats-server-free. Consumers import only `github.com/danmestas/EdgeSync/hub`.

```go
type Config struct {
    RepoPath           string        // fossil repo path; created if absent
    ServerName         string        // NATS server identity; empty = nats-server auto-generated random
    BootstrapUser      string        // default "hub"
    NATSStoreDir       string        // default <repo dir>/messaging
    FossilHTTPPort     int           // 0 = auto-pick
    NATSClientPort     int           // 0 = auto-pick
    NATSLeafPort       int           // 0 = auto-pick
    LeafUpstream       string        // optional "nats-leaf://host:port" to solicit upstream; empty = root hub
    CheckpointInterval time.Duration // 0 = 10s (PASSIVE WAL checkpoint cadence)
}

type Hub struct{ /* opaque */ }

func NewHub(ctx context.Context, cfg Config) (*Hub, error)
func (h *Hub) ServeHTTP(ctx context.Context) error
func (h *Hub) Stop() error

func (h *Hub) NATSURL() string       // "nats://127.0.0.1:NNNN"
func (h *Hub) LeafUpstream() string  // "nats-leaf://127.0.0.1:LLLL" (this hub's leafnode listener)
func (h *Hub) HTTPAddr() string      // "127.0.0.1:HHHH"
func (h *Hub) ServerName() string    // live NATS server identity (Config.ServerName or auto-generated)

type User struct{ Login, Caps string }

func (h *Hub) AddUser(User) error
func (h *Hub) GetUser(login string) (User, error)
func (h *Hub) HasUser(login string) bool
func (h *Hub) ListUsers() ([]User, error)
func (h *Hub) RemoveUser(login string) error

type RevID string
type FileToCommit struct{ Name string; Content []byte }
type CommitOpts struct{ Files []FileToCommit; Message, Author string }

func (h *Hub) Commit(ctx context.Context, opts CommitOpts) (RevID, error)
func (h *Hub) Read(ctx context.Context, path string) ([]byte, error)
func (h *Hub) ReadAt(ctx context.Context, rev RevID, path string) ([]byte, error)
```

## Lifecycle

`NewHub` does the bootstrap-or-open: opens an existing `RepoPath` if present, calls `libfossil.Create` if absent. It then binds the HTTP listener (so `HTTPAddr()` is immediately usable for downstream consumers wiring leaves to the hub) and starts the embedded NATS server, waiting for `ReadyForConnections`. After `NewHub` returns successfully, all three address accessors return live, reachable URLs.

`ServeHTTP` is a blocking goroutine — typical pattern is `go h.ServeHTTP(ctx)`. It runs the fossil HTTP server (`h.repo.XferHandler()`) against the listener bound by `NewHub` until ctx is cancelled or `Stop` is called. `Stop` is idempotent.

```go
ctx, cancel := context.WithCancel(context.Background())
defer cancel()

h, err := hub.NewHub(ctx, hub.Config{RepoPath: "/data/hub.repo"})
if err != nil { return err }
defer h.Stop()

go h.ServeHTTP(ctx)
// Hub now serving at h.HTTPAddr(), NATS at h.NATSURL(), leaf upstream h.LeafUpstream()
```

## SQLite Tuning

`busy_timeout = 30000` is applied at hub bootstrap so concurrent operations retry on `SQLITE_BUSY` rather than failing fast.

The hub deliberately does **not** cap `MaxOpenConns`. An earlier iteration set `SetMaxOpenConns(1)` (matching what bones did before this package existed), but libfossil's clone path has internal goroutines that all need a DB connection — capping the pool deadlocked every concurrent clone (issue #120). `busy_timeout` alone is the right primitive for concurrent-write safety.

## Vanilla Fossil Interop

The hub repo file is opened in WAL journal mode (libfossil's default). Without periodic checkpoints, the on-disk file would stay as a ~4 KiB stub plus a multi-hundred-KiB `*-wal` sidecar for the lifetime of the hub — and vanilla `fossil ui` / `fossil info` / third-party SQLite tooling would refuse to open it because their validity probe stats the main file before consulting the WAL.

The hub guards both ends of that contract:

- **While serving:** a background goroutine runs `PRAGMA wal_checkpoint(PASSIVE)` every `Config.CheckpointInterval` (default 10s). PASSIVE never blocks readers or writers, so vanilla fossil can read the repo file at any time without coordinating with the hub.
- **After Stop:** `Hub.Stop` halts the checkpoint goroutine, then closes the libfossil handle. libfossil's own `(*Repo).Close` runs `PRAGMA wal_checkpoint(TRUNCATE)` before releasing the connection (libfossil v0.6.0+), so the on-disk repo file is self-contained — no `-wal` / `-shm` sidecar required.

There is no "disable" knob: vanilla-fossil readability is the contract this package exists to uphold. Set a long `CheckpointInterval` if you really want to dilute the cadence.

## Layout: in-root vs. separate module

The `hub/` directory lives inside the EdgeSync **root module** rather than as a separate `go.mod` sibling of `leaf/`/`bridge/`. This was a deliberate choice to ship the package without a chicken-and-egg release: the package can evolve in lockstep with the leaf agent, and root tags pick it up.

If a downstream consumer needs to depend on `hub` independently of EdgeSync's full transitive deps, the package can be promoted to its own module without changing its public API. That migration is tracked in issue #118; triggers worth waiting for include consumers wanting to embed hub but skip the rest of EdgeSync's deps, or hub picking up dependencies the rest of EdgeSync doesn't want.

## Out of Scope

- **Multi-hub federation / cross-host hub replication.** Out of band.
- **Hub-side authentication beyond fossil user management.** Caller passes raw `Caps` strings; everything else is fossil's user table.
- **Embedded `fossil` binary support.** In-process libfossil only.
- **Public OTel / metrics surface.** If `leaf/telemetry` is wired in, hub hooks in; otherwise no-op. No new telemetry interface added at this stage.

## Integration with the Leaf Agent

A leaf agent boots with `NATSUpstream: hub.NATSURL()` (or pulls the leaf-node URL via `hub.LeafUpstream()` for the leaf-node path). Sync goes over the HTTP-xfer transport — leaves construct an HTTP transport against `hub.HTTPAddr()` and call `Agent.SyncTo(...)`. End-to-end coverage of this path is locked by `sim/hub_leaf_test.go::TestHubLeafE2E` (see [testing-strategy.md](./testing-strategy.md)).

## Cross-references

- [agent-deployment.md](./agent-deployment.md) — leaf agent lifecycle and the NATS mesh roles that consume `hub.LeafUpstream()`.
- [sync-protocol.md](./sync-protocol.md) — the xfer wire format the hub serves at `/`.
- [core-library.md](./core-library.md) — libfossil's opaque-handle API the hub wraps internally.
