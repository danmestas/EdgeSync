# Repository Operations & CLI

## Unified CLI

Single binary `edgesync` built with [kong](https://github.com/alecthomas/kong) struct-tag dispatch. Replaces the need for the `fossil` binary for basic repo operations. One file per command in `cmd/edgesync/`, each delegating to go-libfossil packages.

Global flags: `-R <path>` (repo file), `-C <path>` (checkout dir), `-v` (verbose). Repo discovery walks parent directories for `.fslckout` / `_FOSSIL_` when `-R` is omitted.

### Subcommands

| Command | Description |
|---------|-------------|
| `repo new <path>` | Create repo via `repo.Create` |
| `repo ci -m "..." <files>` | Checkin files (parent defaults to tip) |
| `repo co [version]` | Checkout version to `-C` dir |
| `repo ls [version]` | List files at version (`-l` for sizes/hashes) |
| `repo timeline [-n 20]` | Show history (UUID prefix, user, date, comment) |
| `repo cat <artifact>` | Output artifact content (`--raw` skips delta expansion) |
| `repo info` | Blob count, size, delta count, project-code |
| `repo verify` | Read-only integrity scan |
| `repo rebuild` | Drop and recompute all derived tables |
| `search <term>` | FTS5 trigram search over trunk tip |
| `sync start` | Start leaf agent daemon |
| `sync now` | Signal running agent (SIGUSR1) |
| `bridge serve` | Start NATS-to-Fossil bridge |
| `user add/list/update/rm/passwd` | User CRUD |
| `invite <login>` | Generate invite token with embedded credentials |

Version resolution (`resolveRID`): accepts full UUID, UUID prefix (min 4 chars), or empty string (tip). Shared across `ls`, `co`, `cat`, `timeline`.

## Tag System

Tags live in the `tagxref` table with three types:

| Type | Value | Behavior |
|------|-------|----------|
| Cancel | 0 | Deletes tag at target and propagated descendants |
| Singleton | 1 | Applied to a single artifact only |
| Propagating | 2 | Walks primary parent links (`plink.isprim=1`) to all descendants |

### Propagation Algorithm

Port of Fossil's `tag_propagate()` (tag.c). Priority queue (`container/heap`) ordered by mtime ascending. For each descendant via primary plink:

- **Propagating**: `REPLACE INTO tagxref` with `srcid=0`. Skip if descendant has a newer direct tag.
- **Cancel**: `DELETE FROM tagxref WHERE tagid=? AND rid=?`. Walk continues through previously-propagated descendants.

Special case: `bgcolor` tag also updates `event.bgcolor` at each descendant.

### Branch Management (`branch/` package)

- `Create`: builds a new checkin manifest with T-cards (`*branch`, `*sym-<name>`, `-sym-<old>`, optional `*bgcolor`), reuses parent's file set.
- `List`: queries `tagxref JOIN tag WHERE tagname='branch' AND tagtype>0`, grouped by value.
- `Close`: adds singleton `closed` tag to the branch's latest leaf checkin.

**Two-function split for tag insertion**: `AddTag()` creates a control artifact + inserts tagxref. `ApplyTag()` inserts tagxref + propagates without creating a control artifact (used by Crosslink when processing existing artifacts after clone/sync).

## Full-Text Search

Package `go-libfossil/search/` with FTS5 trigram index stored inside the repo DB.

| Decision | Choice |
|----------|--------|
| Tokenizer | FTS5 trigram (substring matching, min 3-char query) |
| Scope | Trunk tip only (one entry per unique file in latest checkin) |
| Binary detection | Null byte in first 8KB |
| Reindex trigger | Caller-driven (not automatic) |
| Index location | Repo DB (local-only, never synced) |

Schema: `fts_content` virtual table (path, content) + `fts_meta` table tracking `indexed_rid`. Full rebuild deletes all rows and re-indexes from trunk tip manifest. `NeedsReindex()` compares stored rid against current trunk tip.

Indexing expands delta chains via `content.Expand()`, skips phantoms and binaries. FTS5 special characters are escaped internally (double-quote wrapping).

## Verify & Rebuild

Package `go-libfossil/verify/` with two entry points:

- **`Verify(r)`** -- read-only, report-all (never stops early), returns structured `Report` with typed `Issue` values.
- **`Rebuild(r)`** -- drop-and-recompute all derived tables in a single transaction. Rollback on any error.

### Verify Phases

1. **Blob integrity**: expand every non-phantom blob, recompute SHA1/SHA3, compare against stored UUID.
2. **Delta chains**: verify both endpoints of every delta row exist.
3. **Phantom integrity**: verify every phantom entry has a blob row.
4. **Derived tables**: check event/mlink/plink/tagxref/filename/leaf consistency against parsed manifests.

### Rebuild Steps

1. Verify blobs (skip corrupt ones during reconstruction).
2. Delete all rows from: `event`, `mlink`, `plink`, `tagxref`, `filename`, `leaf`, `unclustered`, `unsent`.
3. **Structure pass**: walk all non-phantom blobs, parse manifests, insert event/mlink/plink/filename rows. Merge B-card baseline for delta manifests.
4. **Tag pass** (after structure): apply inline T-cards and control artifact T-cards via `tag.ApplyTag`. Must run after plinks exist since propagation walks the plink graph.
5. Compute leaf set.
6. Rebuild sync bookkeeping (`unclustered`, `unsent`).

Tables NOT rebuilt: `backlink`, `attachment`, `cherrypick` (wiki/forum/ticket types not yet implemented).

Existing `repo.Verify()` is deprecated and delegates to `verify.Verify()`, returning only the first error.

## Auth & Capabilities

Package `go-libfossil/auth/`. Repo-sovereign authentication using Fossil's existing `user` table and HMAC login scheme. No wire protocol changes.

### Login Verification

HMAC: `SHA1(nonce + SHA1(projectCode/login/password))`. Constant-time signature comparison. All failure cases return the same generic error (prevents user enumeration).

### Capability Letters

| Flag | Meaning | Sync gate |
|------|---------|-----------|
| `a` | Admin | -- |
| `c` | Append | -- |
| `d` | Delete artifacts | -- |
| `e` | View emails | -- |
| `g` | Clone | Clone |
| `h` | Hyperlinks | -- |
| `i` | Checkin (push) | Push |
| `j` | Read wiki | -- |
| `k` | Write wiki | -- |
| `n` | New ticket | -- |
| `o` | Checkout (pull) | Pull |
| `p` | Change own password | -- |
| `r` | Read tickets | -- |
| `s` | Setup (superuser) | -- |
| `t` | Manage tickets | -- |
| `w` | Write tickets | -- |
| `z` | Push UV files | UV write |

### HandleSync Integration

Two-pass card processing: (1) resolve login card, set `user`/`caps`/`authed` on handler; (2) process push/pull/clone cards with capability checks. Auth errors emit `ErrorCard` but are non-fatal.

Anonymous access governed by the `nobody` user row. `repo.Create` seeds `nobody` with full caps (`cghijknorswz`). Admins restrict by updating caps.

### Invite Tokens

Compact JSON base64url-encoded: `{"url","login","password","caps"}`. Password is 32 bytes from `crypto/rand`, hex-encoded. Used via `edgesync clone --invite <token>`. Credentials stored in local clone config for future syncs. Optional `--ttl` sets `cexpire`.

## Shun & Purge

Package `go-libfossil/shun/`. Shunning is **local state** -- does not propagate via sync. Shunned artifacts are already excluded from igot/gimme exchanges in `sync/client.go` and `sync/handler.go`.

### API

- `Add(q, uuid, comment)` -- idempotent, validates UUID format via `hash.IsValidHash`.
- `Remove(q, uuid)` -- no-op if not shunned.
- `List(q)` / `IsShunned(q, uuid)` -- read operations.
- `Purge(q)` -- physical removal in a single transaction.

### Purge Algorithm (port of `shun_artifacts()`)

1. Collect rids of shunned blobs into a temp table.
2. **Undelta dependents**: for any blob whose delta source is being purged, expand to full content and rewrite as a standalone blob.
3. Delete shunned rows from `delta` and `blob` tables.
4. Clean orphaned `private` table entries.

If undelta expansion fails (corrupt chain), the entire transaction rolls back -- no partial deletion.
