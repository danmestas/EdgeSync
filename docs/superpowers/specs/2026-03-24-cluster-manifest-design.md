# Cluster Manifest Generation for igot Batching

**Ticket:** CDG-165
**Date:** 2026-03-24
**Status:** Draft
**Branch:** `feature/cdg-165-cluster-manifest-generation`

## Problem

Without cluster artifacts, repos with thousands of blobs send thousands of individual `igot` cards per sync round. Fossil batches these into cluster manifests (~800 blobs per cluster), dramatically reducing wire overhead. EdgeSync currently queries the `unclustered` table directly and emits one igot card per unclustered blob.

## Approach

Port Fossil's exact cluster protocol: `create_cluster()`, `send_unclustered()`, `send_all_clusters()`, `pragma req-clusters`, and crosslink processing. Full fidelity with Fossil's behavior.

## Design

### 1. Cluster Generation (`content/`)

**New function: `content.GenerateClusters(q db.Querier) error`**

Mirrors Fossil's `create_cluster()` in `src/xfer.c:914-973`:

1. Count non-phantom unclustered entries:
   ```sql
   SELECT count(*) FROM unclustered
   WHERE NOT EXISTS(SELECT 1 FROM phantom WHERE rid=unclustered.rid)
   ```
2. If count < 100, return early (no-op).
3. Query UUIDs (sorted for deterministic output):
   ```sql
   SELECT uuid FROM unclustered JOIN blob USING(rid)
   WHERE NOT EXISTS(SELECT 1 FROM phantom WHERE rid=unclustered.rid)
   ORDER BY uuid
   ```
4. Build cluster artifacts (maintain a `clusterRIDs []int` slice):
   - Accumulate `M <uuid>\n` lines into a `bytes.Buffer`.
   - At 800 entries (and if >100 remain), finalize: compute MD5 Z-card, store via `blob.Store()`, crosslink via `manifest.Crosslink()`. Append the new cluster's rid to `clusterRIDs`.
5. Finalize any remaining M-cards as a last cluster (append rid to `clusterRIDs`).
6. Delete non-cluster, non-phantom entries from unclustered. The cluster artifacts themselves stay in `unclustered` — they are "recent" and get sent as individual igots via `sendUnclustered()`. They will be clustered into meta-clusters by a future `GenerateClusters()` call when enough accumulate. The DELETE uses the tracked slice:
   ```sql
   DELETE FROM unclustered
   WHERE rid NOT IN (<clusterRIDs>)
     AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=unclustered.rid)
   ```

**Constants:** `ClusterThreshold = 100`, `ClusterMaxSize = 800`

**Dependencies:** imports `blob`, `hash` (MD5), `manifest` (crosslink). No circular deps.

**Note:** Fossil also excludes shunned and private blobs. Our schema doesn't have `shun` or `private` tables yet. CDG-166 tracks adding those exclusions when the features land.

### 2. Cluster Artifact Format

Matches Fossil exactly:

```
M <uuid-1>
M <uuid-2>
...
M <uuid-N>
Z <md5-checksum>
```

- M-cards sorted by UUID (`ORDER BY uuid` in the query)
- Z-card is MD5 hex digest of all preceding bytes (all M-lines including newlines)
- No D-card, no U-card, no T-cards — clusters are the simplest artifact type
- Max 800 M-cards per cluster; if >100 remain after a cluster, start a new one
- Stored via `blob.Store()` — gets a rid, uuid, goes into `unclustered` itself
- Uses `crypto/md5` — matches Fossil's MD5 for manifest checksums

**Parsing:** Already handled — `deck.Parse()` recognizes M-cards and `inferType()` returns `Cluster`. Z-card validation exists in the parser.

### 3. Crosslink Cluster Processing (`manifest/crosslink.go`)

**New function: `crosslinkCluster(tx *db.Tx, rid int, d *deck.Deck) error`**

Called when a cluster artifact is crosslinked (locally generated or received via sync):

1. Apply cluster singleton tag (tagid=7, pre-seeded in schema):
   ```sql
   INSERT OR REPLACE INTO tagxref(tagid, tagtype, value, mtime, rid, srcid)
   VALUES(7, 1, NULL, <mtime>, <rid>, <rid>)
   ```
2. For each M-card UUID in the deck:
   - Resolve to rid: `SELECT rid FROM blob WHERE uuid=?`
   - If rid > 0: `DELETE FROM unclustered WHERE rid=?`
   - If UUID unknown: create phantom — `INSERT OR IGNORE INTO blob(uuid,size) VALUES(?,-1)` + `INSERT OR IGNORE INTO phantom(rid) VALUES(?)`
3. The cluster artifact itself remains in `unclustered` until clustered by a future cluster or removed by `GenerateClusters()`'s bulk DELETE.

**Phantom creation** matches Fossil's `uuid_to_rid(uuid, 1)` — when receiving a cluster from a remote, some M-card members may not have arrived yet.

### 4. Sync Protocol Changes

#### Client Side (`sync/client.go`)

Refactor `buildIGotCards()` into two functions matching Fossil:

- **`sendUnclustered()`** — query `unclustered JOIN blob`, emit igot per entry, skip phantoms, filter by `remoteHas`. Returns count.
- **`sendAllClusters()`** — query `tagxref WHERE tagid=7 JOIN blob`, exclude entries still in `unclustered`, exclude phantoms, filter by `remoteHas`. Emit igot per cluster. Returns count.

**Round-aware logic in the sync loop:**

| Condition | Action |
|-----------|--------|
| Every round | `sendUnclustered()` |
| Round 2+ pushing, cumulative gimmes received > 0 | Also `sendAllClusters()` |
| Round 2, pulling | Send `pragma req-clusters` card |

"Cumulative gimmes received" means the total across all rounds (matching Fossil's `nGimmeRcvd` counter), not per-round. This ensures clusters are sent once the remote has demonstrated it needs blobs.

#### Handler Side (`sync/handler.go`)

- **On pull response:** call `content.GenerateClusters()` first, then emit unclustered igots (change `emitIGots()` to query `unclustered` instead of `blob`).
- **Handle `pragma req-clusters`:** new `sendAllClusters()` method on handler — query `tagxref WHERE tagid=7 JOIN blob`, exclude clusters still in `unclustered` and phantoms.
- **`handleIGot()`:** unchanged — already handles cluster igots correctly. When a cluster blob arrives and gets crosslinked, its M-card members are removed from `unclustered`.

#### `emitIGots()` Change

Current handler queries `SELECT uuid FROM blob WHERE size >= 0` (all blobs). This changes to query `unclustered JOIN blob` — only send igots for unclustered entries (including recently created cluster artifacts that are still in `unclustered`). Already-clustered blobs are NOT sent as individual igots; the remote discovers them by requesting clusters via `pragma req-clusters`, receiving the cluster artifact, parsing M-cards, and marking members as known.

```sql
SELECT b.uuid FROM unclustered u JOIN blob b ON b.rid=u.rid
WHERE b.size >= 0
  AND NOT EXISTS(SELECT 1 FROM phantom WHERE rid=u.rid)
```

#### Pragma Handling

We already handle pragmas for UV (`pragma uv-hash`). Add `req-clusters` as a recognized pragma value in the handler's pragma dispatch.

### 5. File Changes Summary

| File | Change |
|------|--------|
| `go-libfossil/content/cluster.go` | New — `GenerateClusters()`, constants |
| `go-libfossil/content/cluster_test.go` | New — unit tests |
| `go-libfossil/manifest/crosslink.go` | Add `crosslinkCluster()`, wire into `Crosslink()` dispatch |
| `go-libfossil/manifest/crosslink_test.go` | Add cluster crosslink tests |
| `go-libfossil/sync/client.go` | Refactor `buildIGotCards()` → `sendUnclustered()` + `sendAllClusters()`, round-aware logic, `pragma req-clusters` |
| `go-libfossil/sync/handler.go` | `emitIGots()` queries `unclustered`, add `sendAllClusters()`, handle `pragma req-clusters` |
| `go-libfossil/sync/client_test.go` | Tests for sendUnclustered, sendAllClusters, pragma |
| `go-libfossil/sync/handler_test.go` | Tests for cluster pragma, emitIGots change |
| `dst/` | New scenario: large-blob-count cluster convergence |

### 6. Testing

**Unit tests (`content/cluster_test.go`):**
- Below threshold (99 unclustered) → no clusters generated
- At threshold (100) → one cluster with 100 M-cards
- Large count (2000) → multiple clusters, max 800 each, remainder gets own cluster
- Phantoms excluded from clusters
- Cluster blob stored with correct format (M-cards sorted, valid Z-card)
- Cluster tagged with tagid=7
- Clustered blobs removed from `unclustered`
- Idempotent — calling twice with no new blobs is a no-op

**Crosslink tests (`manifest/crosslink_test.go`):**
- M-card members removed from `unclustered`
- Unknown UUIDs create phantoms
- Cluster artifact tagged with cluster tag

**Sync protocol tests:**
- `sendUnclustered()` returns only unclustered non-phantom entries
- `sendAllClusters()` returns cluster igots, excludes clusters still in `unclustered`
- `pragma req-clusters` triggers cluster igot response
- Round-trip: client sends cluster igot → server gimmes → receives → crosslinks → members marked known
- Round-aware logic: clusters not sent on round 1, sent on round 2+ when cumulative gimmes > 0
- Pull-side: `pragma req-clusters` sent on round 2, not round 1

**DST test:**
- Seed repos with 500+ blobs, sync, verify convergence with cluster igots. Assert igot count is dramatically lower than blob count after clustering.

**Existing tests:** All current sync tests pass unchanged — clustering is additive. Repos with <100 blobs won't trigger clustering.

## Dependencies

- CDG-166 (Low): Add shun/private exclusions when those tables land
- Crosslink completeness spec (`2026-03-24-crosslink-completeness-design.md`): `crosslinkCluster()` is defined there; this spec provides the concrete implementation

## References

- Fossil `create_cluster()`: `fossil/src/xfer.c:914-973`
- Fossil `send_unclustered()`: `fossil/src/xfer.c:1007-1045`
- Fossil `send_all_clusters()`: `fossil/src/xfer.c:1052-1078`
- Fossil cluster crosslink: `fossil/src/manifest.c:2431-2444`
- Fossil `pragma req-clusters`: `fossil/src/xfer.c:1877-1884` (server), `xfer.c:2289` (client)
