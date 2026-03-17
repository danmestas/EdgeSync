# 3-Way Merge with Swappable Strategies

**Date:** 2026-03-16
**Status:** Draft
**Scope:** `go-libfossil/merge/` package, CLI commands, sync integration

## Problem

When a leaf node is offline and both it and another node modify the same file, sync delivers both versions but there's no way to reconcile them. The offline-first architecture (edge config, personal sync, disconnected ops) makes this a common scenario, not an edge case.

## Goals

1. Detect divergent checkins by walking the plink DAG
2. Provide swappable merge strategies (3-way text, last-writer-wins, binary)
3. Support per-file-pattern strategy assignment via version-controlled config
4. Handle conflicts the way Fossil does — no custom tables, idiomatic storage

## Design Principles

**Idiomatic Fossil storage.** All database writes follow Fossil's conventions: INTEGER PRIMARY KEY, REFERENCES blob for rids, julianday for timestamps — the same patterns used by plink, event, tagxref, and other Fossil tables.

**Strategy-dependent conflict storage:**

- **Most strategies** (three-way, last-writer-wins, binary) use Fossil's native conflict handling: `vfile.chnged=5` in the checkout DB, sidecar files (`.LOCAL`, `.BASELINE`, `.MERGE`), conflict markers in working files. The `fossil` CLI sees these conflicts natively.

- **The conflict-fork strategy** (offline-first, keep all versions until resolved) uses a `conflict` table in the repo DB. This table follows Fossil's conventions and is only created/written when the conflict-fork strategy is active. It preserves all divergent versions as blob references so nothing is lost during extended offline periods with multiple writers. The `fossil` CLI ignores this table (Fossil skips unknown tables).

## Package Structure

```
go-libfossil/merge/
    strategy.go     Strategy interface, Result, Conflict types
    threeway.go     ThreeWayText — Myers diff + line-level 3-way merge
    lastwriter.go   LastWriterWins — pick newer version by timestamp
    binary.go       Binary — always conflict, never auto-merge
    fork.go         ConflictFork — offline-first, preserve all versions in conflict table
    ancestor.go     FindCommonAncestor — BFS on plink table
    detect.go       DetectForks — find divergent leaves in plink DAG
    resolve.go      Resolver — read .edgesync-merge + config, match patterns to strategies
```

## Core Types

```go
// Strategy merges three versions of content.
type Strategy interface {
    Name() string
    Merge(base, local, remote []byte) (*Result, error)
}

type Result struct {
    Content   []byte      // merged output (with conflict markers if any)
    Conflicts []Conflict  // conflict regions (for reporting)
    Clean     bool        // true if no unresolved conflicts
}

type Conflict struct {
    StartLine int
    EndLine   int
    Local     []byte
    Remote    []byte
    Base      []byte
}

// Fork represents two divergent checkins sharing a common ancestor.
type Fork struct {
    Ancestor libfossil.FslID
    LocalTip libfossil.FslID
    RemoteTip libfossil.FslID
}
```

No `ConflictRecord` type — conflicts are tracked via Fossil's native `vfile.chnged=5` mechanism.

## Fork Detection

`DetectForks(r *repo.Repo) ([]Fork, error)` finds divergent history:

1. Query the `leaf` table for all leaf checkins (Fossil already maintains this table — checkins with no children)
2. If only one leaf, no fork — repo is linear
3. If multiple leaves, find common ancestor for each pair via BFS on plink
4. Return Fork structs with ancestor + both tips

`FindCommonAncestor(r *repo.Repo, ridA, ridB libfossil.FslID) (libfossil.FslID, error)` walks plink backwards from both nodes via BFS. First rid appearing in both visited sets is the ancestor.

Detection is SQL-only — no file content is read.

## Conflict Handling (Fossil-Idiomatic)

When a file has unresolvable conflicts during merge:

### 1. Write conflict markers into the working file

```
<<<<<<< LOCAL
your changes here
=======
their changes here
>>>>>>> REMOTE
```

### 2. Write sidecar files

For a conflicted `config.yaml`:
- `config.yaml` — merged content with conflict markers
- `config.yaml.LOCAL` — the local (checkout) version
- `config.yaml.BASELINE` — the common ancestor version
- `config.yaml.MERGE` — the incoming (remote) version

### 3. Flag in checkout DB

```sql
UPDATE vfile SET chnged=5 WHERE pathname='config.yaml' AND vid=?
```

`chnged=5` is Fossil's code for "merge conflict". This is readable by both `edgesync repo conflicts` and `fossil changes`.

### 4. Resolution

When the user edits the file to resolve conflicts and runs `edgesync repo merge resolve <file>`:
- Reset `vfile.chnged` to 1 (modified, not conflicted)
- Delete the sidecar files (`.LOCAL`, `.BASELINE`, `.MERGE`)

## Strategy Resolution

Three sources, checked in priority order:

1. **CLI flag** — `--strategy <name>` overrides everything
2. **`.edgesync-merge` file** — version-controlled, checked into repo, syncs across nodes
3. **Config table** — `merge-strategy` key as default fallback (default: `three-way`)

### .edgesync-merge file format

```
# pattern      strategy
*.yaml         last-writer-wins
*.json         last-writer-wins
*.go           three-way
*.md           three-way
*.png          binary
*              three-way
```

Glob matching, first match wins, processed top to bottom.

```go
type Resolver struct {
    patterns []PatternRule
    default_ string
}

type PatternRule struct {
    Glob     string
    Strategy string
}

func LoadResolver(r *repo.Repo, tipRid libfossil.FslID) (*Resolver, error)
func (res *Resolver) Resolve(filename string) string
func StrategyByName(name string) (Strategy, error)
```

## Strategy Implementations

### ThreeWayText

The core merge algorithm using `gotextdiff/myers`:

1. Split base, local, remote into lines
2. Diff base→local (edits A) and base→remote (edits B)
3. Walk base line by line:
   - Neither changed: copy from base
   - Only A changed: take A's version
   - Only B changed: take B's version
   - Both changed identically: take either
   - Both changed differently: emit conflict markers
4. Return Result with merged content + conflict list

### LastWriterWins

Compare event.mtime for the two divergent checkins. Return the content from whichever is newer. Always clean — no conflicts possible.

### Binary

Always returns a conflict. Cannot auto-merge binary files. Writes sidecar files so user can pick manually.

### ConflictFork (offline-first)

The strategy for extended offline scenarios with multiple writers. Instead of merging immediately, preserves all divergent versions as blob references in a `conflict` table. Nothing is lost — the user resolves when convenient.

The `conflict` table follows Fossil's conventions:

```sql
CREATE TABLE IF NOT EXISTS conflict(
    cid INTEGER PRIMARY KEY,                  -- conflict ID
    filename TEXT NOT NULL,                    -- file path
    base_rid INTEGER REFERENCES blob,         -- common ancestor blob
    local_rid INTEGER REFERENCES blob,        -- local version blob
    remote_rid INTEGER REFERENCES blob,       -- remote version blob
    mtime REAL NOT NULL                       -- julianday timestamp
);
```

This matches Fossil's patterns: `REFERENCES blob` for rid foreign keys (like plink.pid, plink.cid), `REAL` julianday for timestamps (like event.mtime, plink.mtime), `INTEGER PRIMARY KEY` for the row ID.

The table is only created when the conflict-fork strategy is first used. It is only written to by the conflict-fork strategy — other strategies never touch it.

When the user resolves a conflict-fork entry, the row is deleted (not flagged — Fossil deletes resolved state rather than marking it, as seen in the phantom table pattern where resolved phantoms are removed).

### Built-in Strategy Registry

```go
var strategies = map[string]Strategy{
    "three-way":        &ThreeWayText{},
    "last-writer-wins": &LastWriterWins{},
    "binary":           &Binary{},
    "conflict-fork":    &ConflictFork{},
}
```

## CLI Commands

### repo merge \<version\>

Merge a divergent version into the current checkout:

1. Open repo + checkout DB, resolve version to rid
2. Find common ancestor via `FindCommonAncestor`
3. Load `.edgesync-merge` file + config for strategy resolution
4. For each file differing between the two versions:
   - Pick strategy via resolver
   - Load base, local, remote content via `content.Expand()`
   - Call `strategy.Merge(base, local, remote)`
   - If clean: write merged content to checkout, set `vfile.chnged=1`
   - If conflicts: write merged content with markers + sidecar files, set `vfile.chnged=5`
5. Report summary: N files merged, M conflicts

### repo conflicts

Checks both conflict sources:

1. Checkout DB `vfile.chnged=5` — standard merge conflicts (three-way, binary)
2. Repo DB `conflict` table — conflict-fork entries (if table exists)

```sql
-- Standard conflicts (from three-way/binary strategies)
SELECT pathname FROM vfile WHERE chnged=5 AND vid=?

-- Conflict-fork entries (from conflict-fork strategy)
SELECT filename, base_rid, local_rid, remote_rid FROM conflict
```

Output:
```
CONFLICT  config.yaml  (three-way, 2 conflict regions)
FORK      data.json    (conflict-fork, 3 versions preserved)
```

Standard conflicts are also visible via `fossil changes`.

### repo merge resolve \<file\>

Mark a conflict as resolved. Handles both conflict types:

**Standard conflicts** (vfile.chnged=5):
1. Update `vfile SET chnged=1` (modified, no longer conflicted)
2. Delete sidecar files: `file.LOCAL`, `file.BASELINE`, `file.MERGE`

**Conflict-fork entries** (conflict table):
1. Delete the row from the `conflict` table (Fossil convention: delete resolved state, like phantom table)
2. The chosen version is already in the working directory

## Sync Integration

After a successful sync cycle, the leaf agent can optionally detect forks:

```go
forks, _ := merge.DetectForks(r)
if len(forks) > 0 {
    log.Printf("sync: %d divergent branches detected — run 'edgesync repo merge' to resolve", len(forks))
}
```

Advisory only — logs a message. No automatic merging. The user decides when to merge.

## Testing

1. **Unit tests for ThreeWayText**: non-overlapping edits, overlapping same, overlapping different (conflict), empty files, single-line files
2. **Unit tests for LastWriterWins**: newer local wins, newer remote wins
3. **Unit tests for FindCommonAncestor**: linear history, branched history, no common ancestor
4. **Unit tests for DetectForks**: single leaf (no fork), multiple leaves (fork found)
5. **Unit tests for Resolver**: glob matching, file fallback, config fallback
6. **Integration test**: create repo, make divergent checkins, run merge, verify markers + sidecar files + vfile.chnged=5
7. **Fossil compat test**: after merge with conflicts, run `fossil changes` and verify it sees the conflicts

## Dependencies

- `github.com/hexops/gotextdiff` (already added for repo diff)
- Existing go-libfossil packages: repo, blob, content, manifest, db

## Implementation Order

1. Core types + Strategy interface (`strategy.go`)
2. FindCommonAncestor (`ancestor.go`)
3. DetectForks (`detect.go`)
4. ThreeWayText (`threeway.go`) — the hard part
5. LastWriterWins + Binary (`lastwriter.go`, `binary.go`)
6. Resolver (`resolve.go`)
7. CLI: `repo merge`, `repo conflicts`, `repo merge resolve`
8. Sync integration (advisory fork detection logging)
