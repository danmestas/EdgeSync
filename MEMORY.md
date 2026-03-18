# EdgeSync Memory

## Project Structure
- Go workspace (`go.work`) with 5 modules: root, `go-libfossil/`, `leaf/`, `bridge/`, `dst/`
- `sim/` is a package in the root module (not its own module)
- Dual VCS: git + fossil checkout — use `-buildvcs=false` for all builds
- Git repo: github.com/danmestas/EdgeSync (private)
- `fossil/` and `libfossil/` are gitignored (local-only reference, purged from history)
- `.agents/`, `.claude/skills/`, `skills-lock.json` are gitignored (tool-local)

## Entry Points
- `cmd/edgesync/` — Unified CLI (50 subcommands via kong, including `repo clone`)
- `leaf/cmd/leaf/` — Standalone leaf agent daemon
- `bridge/cmd/bridge/` — Standalone bridge daemon
- `sim/cmd/soak/` — Continuous soak test runner

## Server Handler (2026-03-18, PR#8)
- **HandleSync** in `sync/handler.go` — server-side xfer card handler. Stateless per-round.
- **HandleSyncWithOpts** — same but accepts `HandleOpts{Buggify}` for DST fault injection
- **HandleFunc** type — `func(ctx, *repo.Repo, *xfer.Message) (*xfer.Message, error)`
- Cards handled: pull→igot, push→accept files, igot→gimme, gimme→file, file→store, clone→paginated files, clone_seqno, reqconfig→config
- **ServeHTTP** in `sync/serve_http.go` — HTTP /xfer listener. `fossil clone`/`fossil sync` compatible.
- **ServeNATS** in `leaf/agent/serve_nats.go` — NATS request/reply listener
- **ServeP2P** in `leaf/agent/serve_p2p.go` — stub for libp2p
- Agent config: `ServeHTTPAddr string`, `ServeNATSEnabled bool`
- Agent.Start() launches pollLoop + server listeners concurrently
- 4 BUGGIFY sites: handleGimme.skip(5%), handleFile.reject(3%), emitIGots.truncate(10%), emitCloneBatch.smallBatch(10%)
- TigerStyle: preconditions on all handler methods, sql.ErrNoRows distinction, MaxBytesReader(50MB), decompressBounded(50MB)
- **xfer.Decode**: tries raw zlib → 4-byte prefix+zlib → uncompressed (3-way fallback)
- **parsePush/parsePull**: accept 1-arg form (Fossil omits project-code on known remotes)
- Spec: `docs/dev/specs/2026-03-18-server-handler-design.md`
- Plan: `docs/dev/plans/2026-03-18-server-handler.md`
- Obsidian note: `~/notes/Projects/EdgeSync Server Handler.md`

## DST Updates (2026-03-18)
- **MockFossil replaced with HandleSync** — `MockFossil.Exchange` delegates to `HandleSyncWithOpts`. All 37 existing DST tests pass.
- **PeerNetwork** in `dst/peer_network.go` — leaf-to-leaf DST without bridge/master. Routes Exchange→HandleSync on peer repo.
- **TestScenarioPeerSync** — 2 leaves, 10 blobs converge via PeerNetwork
- **TestScenarioClone** — fresh repo clones 250 blobs through HandleSync pagination
- DST total: 39 tests (37 existing + 2 new)

## Two Simulation Layers
- `dst/` — Deterministic single-threaded sim (NewFromParts, SimNetwork/PeerNetwork, HandleSync-backed MockFossil, event queue)
- `sim/` — Integration sim (real NATS + TCP fault proxy + real Fossil server + ServeHTTP/ServeNATS tests)
- Both share `simio/` abstractions (Clock, Rand, Env) and `sync.BuggifyChecker` interface
- sim serve tests: fossil clone, fossil sync, leaf-to-leaf NATS, leaf-to-leaf HTTP — all verified by Fossil 2.28

## Clone Implementation (2026-03-18, PR#7 merged)
- `sync.Clone()` — full repo clone via Fossil protocol v3
- `storeReceivedFile()` shared between Sync and Clone
- `ErrDeltaSourceMissing` sentinel: clone creates phantoms, sync returns error
- CLI: `edgesync repo clone <url> <path>`

## Config Locations
- agent.Config in `leaf/agent/config.go` — includes ServeHTTPAddr, ServeNATSEnabled
- bridge.Config in `bridge/bridge/config.go`
- SyncOpts in `go-libfossil/sync/session.go` — has BuggifyChecker field
- HandleOpts in `go-libfossil/sync/handler.go` — has BuggifyChecker field

## Key Patterns
- `simio.CryptoRand{}` for production callsites of `repo.Create` and `db.SeedConfig`
- `NewFromParts()` on agent/bridge bypasses NATS for testing; `New()` is production path
- `HandleSync` for simple use, `HandleSyncWithOpts` when BUGGIFY is needed (DST)

## CI & Testing
- `.github/workflows/test.yml` — unit tests + sim serve tests
- `.githooks/pre-commit` — ~8s (unit + DST 1 seed + sim unit + sim serve)
- Makefile: `make test` (CI), `make dst`/`dst-full`/`dst-hostile`/`dst-drivers`, `make sim`/`sim-full`, `make drivers`
- DST: 8 seeds normal pass, 16 seeds x hostile ~40s
- `backup/pre-filter` branch on remote — old history before fossil file purge

## Follow-ons (documented, not implemented)
- UV cards (uvfile/uvigot/uvgimme) for forum/wiki/chat
- Cookie sessions for igot optimization
- In-memory TTL sessions for libp2p connection identity
- Private branch cards
- Full config sync (tickets, skins, backoffice)
- Login verification beyond accept-all
- libp2p transport (ServeP2P stub exists)
- DST fault schedule for peer-to-peer topology
- Clone under DST event model
