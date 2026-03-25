# Full-Text Search Over Repo Content

**Date**: 2026-03-23
**Ticket**: CDG-135 (repurposed — original scope was Fossil's artifact metadata FTS; new scope is file content search)
**Branch**: TBD (from dev, in worktree)

## Problem

There is no way to search the contents of files stored in a Fossil repo managed by go-libfossil. Users must check out files and use external tools (grep, ripgrep) to find content. GitHub-style code search — instant, indexed, no checkout required — does not exist.

## Solution

A new `go-libfossil/search/` package that builds an FTS5 trigram index over file content in the repo DB. Searches the trunk tip manifest's files directly from blob storage. No checkout needed.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Index location | Inside repo DB | go-libfossil owns the schema; `fossil` binary won't run against these repos; FTS tables are local-only (never synced via xfer) |
| Tokenizer | FTS5 trigram | Code search needs substring matching (`handleSync`, `FslID`), not natural language stemming. Trigram enables `LIKE '%pattern%'` speed via index |
| Scope | Trunk tip only | Keeps index small. One entry per unique file in latest checkin. No historical versions |
| Binary detection | Null-byte in first 8KB | Simplified heuristic — not as robust as Git's full detection (misses UTF-16, small null-free binaries). Good enough for MVP; can improve later |
| Min query length | 3 characters | FTS5 trigram requires 3-char minimum. Sub-3 queries return empty results |
| Reindex trigger | Caller-driven | Not automatic on every query. Callers (agent, CLI, browser) decide when to rebuild |

## Schema

Two tables added to the repo DB:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS fts_content USING fts5(
    path,           -- file pathname from manifest (e.g., "src/sync/client.go")
    content,        -- full text of the file
    tokenize='trigram'
);

CREATE TABLE IF NOT EXISTS fts_meta(
    key TEXT PRIMARY KEY,
    value TEXT
);
-- Single key: 'indexed_rid' (RID of the checkin the index was built from)
```

## Package API

### Types

```go
package search

// Index manages the FTS5 index in a repo DB.
type Index struct {
    repo *repo.Repo
}

// Query configures a search request.
type Query struct {
    Term         string // search term (substring match via trigram, min 3 chars).
                        // FTS5 special chars (" * AND OR NEAR) are escaped internally.
    MaxResults   int    // 0 → default (50)
    ContextLines int    // lines of surrounding context (0 → just the match line)
}

// Result is a single search hit.
type Result struct {
    Path     string // file pathname (e.g., "src/sync/client.go")
    Line     int    // 1-based line number of match
    Column   int    // 0-based byte offset within the line
    MatchLen int    // length of matched substring
    LineText string // the matching line
    Context  string // surrounding lines including match line, newline-separated.
                    // Empty if ContextLines=0.
}
```

### Functions

```go
// Open creates an Index from an open repo. Creates FTS5 tables if they don't exist.
func Open(r *repo.Repo) (*Index, error)

// RebuildIndex walks the trunk tip manifest, expands blob content (including
// delta chains), skips binaries and phantoms, and populates fts_content.
// No-ops if already current.
func (idx *Index) RebuildIndex() error

// NeedsReindex returns true if the trunk tip has advanced past the indexed checkin.
func (idx *Index) NeedsReindex() (bool, error)

// Search executes a search and returns results ranked by relevance.
func (idx *Index) Search(q Query) ([]Result, error)

// Drop removes the FTS tables entirely.
func (idx *Index) Drop() error
```

### Trunk Tip Resolution

```go
// trunkTip returns the RID of the latest checkin on trunk.
// Queries tagxref for sym-trunk with the most recent mtime.
// Returns 0 if no trunk tip exists (empty repo).
func trunkTip(db *sql.DB) (libfossil.FslID, error)
```

SQL: `SELECT tagxref.rid FROM tagxref JOIN tag USING(tagid) WHERE tag.tagname='sym-trunk' AND tagxref.tagtype>0 ORDER BY tagxref.mtime DESC LIMIT 1`

This is unexported — internal to the search package.

## Indexing Flow

1. Call `trunkTip()` to get the current trunk tip RID
2. Check `fts_meta` for `indexed_rid` — if it matches trunk tip, return early (already current)
3. Delete all rows from `fts_content` (full replace)
4. Walk trunk tip manifest via `manifest.ListFiles(repo, trunkTipRID)`
5. For each file:
   a. Convert UUID to RID via `blob.Exists(repo.DB(), file.UUID)`
   b. If blob doesn't exist (phantom), skip and continue
   c. Expand full content via `content.Expand(repo.DB(), rid)` — handles delta chains
   d. If `content.Expand` returns a phantom error, skip and continue
   e. Check first 8KB for null bytes — if found, skip (binary)
   f. Insert `(path, content)` into `fts_content`
6. Update `fts_meta` `indexed_rid` to the trunk tip RID

## Search Flow

1. Validate term length >= 3 characters; return empty results if shorter
2. Escape FTS5 special characters in term: wrap in double quotes, escape internal quotes (`"` → `""`)
3. Execute FTS5 query: `SELECT path, content, rank FROM fts_content WHERE fts_content MATCH ? ORDER BY rank LIMIT ?`
4. For each match: split content by newlines, find lines containing the search term, compute line numbers, columns, match offsets, and surrounding context
5. Return `[]Result` sorted by FTS5 rank (lower is more relevant)

## Result Presentation

The `Result` struct supports three presentation modes (caller-side):

- **Line + highlight** (VS Code style): `LineText` + `Column` + `MatchLen`
- **Snippet + context** (GitHub style): `Context` field with surrounding lines
- **Raw API**: use the struct directly

## Integration Points

All caller-side — the `search/` package has no knowledge of agents, CLI, or browser:

| Caller | When to reindex | How to search |
|--------|----------------|---------------|
| Leaf agent | `PostSyncHook` after pulling new content | Expose via API endpoint (future) |
| CLI | `edgesync search "term"` — auto-reindex if stale | Print results to stdout |
| Browser playground | After clone or sync completes | JS bridge calls into WASM |

## Performance Expectations

- **Index size**: ~3-5x text content (trigram overhead). 500 files × 5KB avg = 2.5MB text → ~7-10MB index
- **Index build**: < 2 seconds for 500-file repo
- **Query latency**: < 10ms for trigram prefix/substring match (sub-100ms as-you-type target easily met)
- **WASM**: Works in browser via OPFS-backed SQLite. 10MB index fits comfortably in browser memory

## Testing Strategy

- Unit tests with `testutil.NewTestRepo()`: create repo, commit known files, build index, search, verify results
- Binary skip: commit a file with null bytes, verify it's excluded from index
- Phantom skip: create a phantom blob in manifest, verify indexing skips it without error
- Delta chain: commit a file, commit a delta, verify expanded content is indexed
- Trigram minimum: verify sub-3-char queries return empty results
- FTS5 escaping: search for terms with special chars (`"`, `*`, `AND`)
- Reindex idempotency: rebuild twice, verify same results
- Stale detection: commit new files, verify `NeedsReindex` returns true
- No checkout dependency: search works with only a repo open (no checkout)
- WASM FTS5 availability: verify `ENABLE_FTS5` in `pragma_compile_options` (integration test)

## Dependencies

- `go-libfossil/repo` — open repo, access DB
- `go-libfossil/content` — expand blob content (delta chain resolution)
- `go-libfossil/blob` — check blob existence (UUID → RID)
- `go-libfossil/manifest` — list files at a checkin
- SQLite FTS5 — available in all three drivers (modernc, ncruces, mattn). WASM: verify ncruces WASM build includes FTS5

No new external dependencies.

## Out of Scope

- Historical search (across all checkins)
- Fuzzy matching / typo tolerance
- Language-aware search (AST-based, symbol search)
- Automatic reindexing (callers manage lifecycle)
- Search UI (CLI formatting and browser UI are separate concerns)
- Robust binary detection (UTF-16 BOM, non-printable ratio) — future enhancement
