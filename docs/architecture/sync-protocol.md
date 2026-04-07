# Sync Protocol & Convergence

## Transport-Agnostic Design

The sync engine (`go-libfossil/sync/`) operates entirely through a `Transport` interface with a single method:

```go
type Transport interface {
    Exchange(ctx context.Context, request *xfer.Message) (*xfer.Message, error)
}
```

Implementations: `HTTPTransport` (Fossil HTTP `/xfer`), `NATSTransport` (leaf-to-leaf), `MockTransport` (tests). The engine produces/consumes `xfer.Message` values and never touches I/O directly.

The server handler uses the same abstraction via `HandleFunc`:

```go
type HandleFunc func(ctx context.Context, r *repo.Repo, req *xfer.Message) (*xfer.Message, error)
```

Transport listeners (`ServeHTTP`, `ServeNATS`, `ServeP2P` stub) call `HandleFunc`; sync logic stays transport-agnostic. The handler is stateless per-round -- the client drives convergence.

## Xfer Card Types

### Core Cards

| Card | Direction | Purpose |
|------|-----------|---------|
| `login USER NONCE SIG` | C->S | Auth. Nonce = SHA1(payload incl. random comment). Sig = SHA1(nonce + SHA1(project/user/pass)). Empty user = no login card (unauthenticated). |
| `pragma client-version VER DATE TIME` | C->S | Protocol compat. VER must be numeric (e.g. `22800`) -- Fossil's `atoi()` rejects non-numeric. |
| `push SCODE PCODE` | Both | Announce push intent / acknowledge push permission |
| `pull SCODE PCODE` | Both | Announce pull intent |
| `file UUID SIZE [FLAGS]\nCONTENT` | Both | Full blob payload |
| `cfile UUID DELTASRC SIZE [FLAGS]\nCONTENT` | Both | Delta-compressed blob |
| `igot UUID [ISPRIVATE]` | Both | "I have this artifact." Second arg `1` = private. |
| `gimme UUID` | Both | "Send me this artifact." |
| `cookie VALUE` | Both | Session cookie, echoed back each round |
| `error MSG` | S->C | Fatal or non-fatal error |
| `message MSG` | S->C | Informational |
| `clone VER SEQNO` | C->S | Clone request (version 3 = compressed) |
| `clone_seqno NEXT` | S->C | Next rid to request; 0 = all sent |
| `reqconfig NAMES...` | C->S | Request config values |
| `config NAME SIZE\nDATA` | S->C | Config value response |
| `private` | Both | Next file/cfile is a private artifact |

### UV Cards

| Card | Direction | Purpose |
|------|-----------|---------|
| `pragma uv-hash HASH` | Both | Catalog hash short-circuit (SHA1 of sorted name/datetime/hash rows, always SHA1 even in SHA3 repos) |
| `pragma uv-push-ok` | S->C | Server accepts UV writes |
| `pragma uv-pull-only` | S->C | Server rejects UV writes |
| `uvigot NAME MTIME HASH SIZE` | Both | "I have this UV file at this version." Hash `"-"` = deletion tombstone. |
| `uvgimme NAME` | Both | "Send me this UV file." |
| `uvfile NAME MTIME HASH SIZE FLAGS\nCONTENT` | Both | UV file payload. Flags: 0=normal, 1=deletion, 4=content omitted. |

### Extension Table Cards (Remote Schema Sync)

| Card | Direction | Purpose |
|------|-----------|---------|
| `schema TABLE VER HASH MTIME SIZE\nJSON` | Both | Table definition (columns, conflict strategy). Control phase -- tables exist before data. |
| `pragma xtable-hash TABLE HASH` | Both | Per-table catalog hash short-circuit |
| `xigot TABLE PK_HASH MTIME` | Both | "I have this row at this version." |
| `xgimme TABLE PK_HASH` | Both | "Send me this row." |
| `xrow TABLE PK_HASH MTIME SIZE\nJSON` | Both | Full row payload |
| `xdelete TABLE PK_HASH MTIME SIZE\nPKDATA` | Both | Row deletion (tombstone). PKData = JSON PK column values for tombstone insertion on peers that never had the row. |

### Cluster Cards

| Card | Direction | Purpose |
|------|-----------|---------|
| `pragma req-clusters` | C->S | Request cluster igots (sent on round 2 when pulling) |

## Client/Server Sync Flow

### Client Convergence Loop

Each round: `buildRequest()` -> `transport.Exchange()` -> `processResponse()`. Repeats until converged or `MaxRounds` (100) exceeded.

**Request construction per round:**
1. Header cards: `pragma client-version`, `push`/`pull`, `login` (if credentials set), `cookie` (if cached)
2. `igot` for unclustered artifacts (filtered by `remoteHas`), budget-limited
3. `file`/`cfile` for `pendingSend` + `unsent` table, respecting `MaxSend` budget (default 250KB)
4. `gimme` for phantoms (max 200, scaled to `max(200, filesRecvdLastRound * 2)`)
5. UV cards if `SyncOpts.UV` is true; extension table cards for registered schemas

**Response processing:** Store received files (verify hash), track `remoteHas` from igots, queue gimme requests in `pendingSend`, cache cookies, evict stale phantoms after 3 consecutive unresolved rounds.

**Convergence criteria -- ALL must hold:**
- No files received this round
- No files sent this round
- `phantoms` empty
- `pendingSend` empty
- No new artifacts in `unsent` table
- UV loop: `nUvGimmeSent > 0 && (nUvFileRcvd > 0 || nCycle < 3)` continues

### Server Handler Card Processing

Two phases per request:
1. **Control phase:** `login`, `push`, `pull`, `clone`, `pragma`, `schema`, `reqconfig`
2. **Data phase:** `file`, `cfile`, `igot`, `gimme`, UV cards, extension table cards

Key behaviors:
- `pull` -> emit `igot` for unclustered blobs, filtered by `remoteHas` (see below)
- `push` -> enable accepting file cards (rejected without preceding push)
- `igot` for known UUID -> record in `remoteHas` (skip in igot emission later)
- `igot` for unknown UUID -> emit `gimme`
- `gimme` -> load blob, emit `file` card
- File with bad hash -> error card

### Server-Side IGot Filtering (remoteHas)

Mirrors Fossil's per-request `onremote` temp table (xfer.c:1011-1012, 1056-1057). The handler tracks UUIDs the client announced via igot cards, then filters `emitIGots()`, `emitPrivateIGots()`, and `sendAllClusters()` to skip blobs the client already has.

**Implementation:** `remoteHas map[string]remoteHasEntry` on the handler struct, where `remoteHasEntry{isPrivate bool}` records the client's announced private status. Lazily initialized (nil until first igot received). Lifecycle is per-request -- no cross-request persistence.

**Private-status-aware filtering (divergence from Fossil):** Fossil's `onremote` is a simple `rid INTEGER PRIMARY KEY` -- no private flag. Fossil doesn't need one because its server-side igot handler calls `content_make_public(rid)` / `content_make_private(rid)` to synchronize private status during igot processing (xfer.c:1472-1476). go-libfossil deliberately does NOT mutate server private status from client igots ("server is authoritative"). This means private/public transitions must propagate through igot *emission*: the filter only skips when the client's announced status matches the server's current status.

- `emitIGots()` (public blobs): skip when `ok && !e.isPrivate` -- client has it as public
- `emitPrivateIGots()` (private blobs): skip when `ok && e.isPrivate` -- client has it as private
- `sendAllClusters()` (always public): skip when `ok && !e.isPrivate`

**Cookie card:** Defined in the wire protocol but server-side is a stub (matching upstream Fossil). Client parses, caches, and echoes; server ignores. `session.cookie` field exists for forward compatibility.

## Clone Protocol

Version 3 (compressed file cards). Client creates repo, clears project-code, then enters clone loop.

**Repo creation sequence:** `repo.Create(path)` -> delete project-code -> clone rounds -> store project/server codes from server's `push` card -> return opened repo. Failure -> `os.Remove(path)`.

**Sequential delivery:** Server sends blobs in rid order. Client sends `clone 3 <seqno>`, server responds with batch + `clone_seqno <next>`. When `clone_seqno 0`, transitions to phantom-resolution mode (gimme rounds for out-of-order delta sources).

**Clone convergence:** Stop when `clone_seqno == 0` AND no phantoms AND at least 2 rounds done. No login card on round 0 (project-code unknown); login on round 1+ if credentials provided and project-code received.

**Shared blob I/O:** `storeReceivedFile()` extracted as package-level function, used by both sync session and clone session. During clone, missing delta sources stored as phantoms (expected -- blobs arrive in rid order, not dependency order).

## UV Sync

Mtime-wins conflict resolution for unversioned files (forum posts, wiki pages, attachments).

**Storage:** `unversioned` table with `name`, `mtime`, `hash`, `sz`, `encoding`, `content`. Compression at 80% threshold. Hash `NULL` = deletion tombstone.

**Status function** (pure, no DB): compares `(localMtime, localHash)` vs `(remoteMtime, remoteHash)`. Returns 0-5: pull / pull / pull-mtime / identical / push-mtime / push. Tie-breaker: same mtime, lexically larger hash wins.

**Catalog hash short-circuit:** `pragma uv-hash HASH`. SHA1 of `"name datetime hash\n"` for all non-deleted rows, sorted by name. Always SHA1 (hardcoded), even in SHA3 repos. If hashes match, no uvigot exchange needed.

**Client flow:** Round 1 sends `pragma uv-hash`. If catalogs differ, server sends `uvigot` catalog + `pragma uv-push-ok`/`uv-pull-only`. Client compares each `uvigot` via `Status()`, sends `uvgimme`/`uvfile` as needed. Pre-populates `uvToSend` with all local UV files; entries not mentioned by server's `uvigot` get pushed (propagation of client-originated files).

## Config/Schema Sync

### Config Cards (reqconfig/config)

Client sends `reqconfig` for `project-code`, `project-name`, `server-code`. Server responds with `config` cards containing values.

### Remote Schema Sync (Extension Tables)

Generic table sync primitive using `xigot`/`xgimme`/`xrow`/`xdelete` exchange pattern (same as UV sync). All extension tables prefixed `x_` to protect Fossil core tables.

**Conflict strategies** (per-table):
- `self-write`: peer can only write/delete rows where `_owner` matches its identity
- `mtime-wins`: last mtime wins (any peer can write/delete any row)
- `owner-write`: row has `_owner` field, only owner can update/delete

**Schema propagation:** `schema` card in control phase creates table on receiving peer. Append-only evolution (ADD COLUMN only). Version monotonically increases; downgrades rejected.

**Short-circuit:** `pragma xtable-hash TABLE HASH` skips exchange when catalog hashes match. Catalog hash = SHA1 of sorted `pk_hash + " " + mtime + "\n"` for live (non-tombstone) rows only.

**Row deletion (tombstones):** Following Fossil's UV pattern, deleted rows remain in `x_<table>` with all non-PK value columns set to NULL and mtime updated to the deletion timestamp. Convention: all value columns NULL = tombstone. Tombstones are excluded from catalog hash but included in xigot exchange so deletions propagate. When a peer receives an xdelete for a row it never had, it inserts a tombstone from the PKData payload. PKData hash is verified against PKHash before insertion. Resurrection: an xrow with a newer mtime than the tombstone overwrites the NULLs.

**PK hash:** Type-aware canonical normalization — values normalized to strings by declared column type (`integer` → `strconv.FormatInt`, `text` → as-is, etc.) then SHA1'd. No JSON in the hash path. Ensures determinism across JSON round-trips that coerce `int64` → `float64`.

**Conflict resolution for deletions:** `self-write`/`owner-write` tables check `_owner` before accepting xdelete — only the row owner can delete. `mtime-wins` allows any peer to delete. Same ownership rules as xrow.

**SQL injection protection:** Table/column names validated against `^[a-z_][a-z0-9_]*$`. JSON payloads always parameterized. Non-PK columns are nullable (required for tombstone support); PK columns remain NOT NULL.

## Private Artifacts

Blobs in the `private` table are excluded from normal sync. Transfer requires both peers to have `x` capability and client opt-in via `pragma send-private`.

**Wire mechanics:**
- `igot UUID 1` -- second arg signals private; peers without `x` do not request it
- `private\n` card precedes a `file`/`cfile` card to mark it private
- Receiving a public artifact for a previously-private UUID clears private status (`MakePublic`)

**Exclusion filters** (when `Private: false`): `sendUnclustered`, `sendAllClusters`, `buildFileCards`, `buildGimmeCards` all add `NOT EXISTS(SELECT 1 FROM private WHERE rid=...)`.

**Delta safety:** Never send a delta against a private base unless both peers are in private sync mode.

## Cluster Batching

Repos with many blobs batch `igot` announcements into cluster manifests (~800 UUIDs per cluster) to reduce wire overhead.

**Cluster artifact format:** Sorted `M <uuid>` lines + `Z <md5-checksum>`. Stored as blobs, tagged with `tagid=7`.

**Generation:** `content.GenerateClusters()` fires when unclustered count >= 100. Creates clusters of up to 800, removes clustered blobs from `unclustered` table. Cluster blobs themselves stay in `unclustered` for announcement.

**Protocol flow:**
- Every round: `sendUnclustered()` emits igots for unclustered entries only
- Round 2+ (pushing, cumulative gimmes > 0): also `sendAllClusters()` for already-clustered batches
- Round 2 (pulling): client sends `pragma req-clusters`

**Crosslink:** When a cluster blob is received and parsed, its M-card members are removed from `unclustered`. Unknown UUIDs create phantoms.

## Crosslink

Two-pass architecture matching Fossil's `manifest_crosslink_begin`/`manifest_crosslink`/`manifest_crosslink_end`:

**Pass 1 -- Link artifacts into tables:** Parse each uncrosslinked blob, dispatch by type. Tag insertion and `tag_propagate_all` run immediately per artifact. Collect deferred work (wiki backlinks, ticket rebuilds).

**Pass 2 -- Deferred processing:** Wiki backlink refresh, ticket entry rebuild, TAG_PARENT reparenting.

**Per-type handlers:** Checkin (event/plink/leaf/mlink + cherrypick), Wiki (event type='w', wiki-tag, plink chain), Ticket (tkt-tag, deferred rebuild), Event/Technote (event type='e'), Attachment (attachment table, isLatest, type detection), Cluster (cluster tag, unclustered cleanup), Forum (forumpost table, thread references), Control (event type='g').

**Dephantomize hook:** `AfterDephantomize(rid)` fires when a phantom receives real content -- crosslinks the blob, checks orphan table for delta manifests waiting on this baseline, recursively dephantomizes delta chains. Hook is per-session (not global) to avoid data races.

**Auto-crosslink in sync:** Dephantomize hook set at session start for incremental crosslinking. `manifest.Crosslink()` called as catch-all after convergence. Clone skips incremental hook; bulk crosslink at end.

## Convergence Criteria Summary

| Mode | Continue When | Stop When |
|------|--------------|-----------|
| Sync | Files exchanged, phantoms remain, unsent artifacts exist | Zero files exchanged, no phantoms, no pending, no unsent |
| Clone | `clone_seqno > 0`, or phantoms exist and files received, or `nCycle == 1` | `clone_seqno == 0` AND no phantoms AND >= 2 rounds |
| UV | `nUvGimmeSent > 0 && (nUvFileRcvd > 0 \|\| nCycle < 3)` | No outstanding UV gimmes or 3 rounds without progress |
| Extension tables | `xigot`/`xgimme` exchange pending | Catalog hashes match or no rows exchanged |
| Safety | -- | `MaxRounds` (100) or context cancellation |

## Key Constraints

- **Blob format:** `[4-byte BE uncompressed size][zlib data]`. If `len(content) == declared size`, content is uncompressed.
- **Wire format:** Raw zlib (no size prefix). `xfer.Decode` auto-detects: raw zlib, prefix+zlib, or uncompressed.
- **SHA3 UUIDs:** 64-char = SHA3-256 (Fossil 2.0+), 40-char = SHA1 (legacy).
- **Performance targets:** Compute within 3x of C, I/O within 5x.
- **No CGo:** Pure Go, behavioral equivalence validated by Fossil CLI as test oracle.
- **go-libfossil is transport-agnostic:** No operational endpoints (healthz, metrics). Operational concerns live in `leaf/agent/serve_http.go`.
- **Stale phantom eviction:** After 3 consecutive unresolved rounds, phantoms are evicted to prevent convergence deadlock.
- **Auth contract:** User + Password both set = login card with SHA1 auth. Either empty = no login card (unauthenticated "nobody").
