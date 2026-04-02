# Diff Engine Design

**Date:** 2026-04-02
**Ticket:** EDG-12
**Status:** Approved

## Problem

go-libfossil has no diff engine. The merge package has an internal LCS implementation for 3-way merge, but nothing produces unified or side-by-side diff output. This blocks `edgesync diff`, web UI diff views, and improved annotate output.

## Design Principles

- **`diff/` is pure `[]byte` in, string out.** No repo, content, or blob imports. Callers compose with `content.Expand` as needed.
- **Myers diff algorithm.** O(nd) time where d = edit distance. Industry standard (git, GNU diff, Fossil). Better than O(mn) LCS for the common case of similar files.
- **Zero external dependencies.** Consistent with go-libfossil's stdlib-only policy.

## Public API

```go
package diff

// Options configures diff output.
type Options struct {
    ContextLines int    // lines of context around changes; 0 means no context
    SrcName      string // source file label for header (e.g. "a/file.txt")
    DstName      string // destination file label for header
}

// DefaultContextLines is the standard number of context lines (matches git/fossil).
const DefaultContextLines = 3

// Unified returns a unified diff string between a and b.
// Returns "" if a and b are identical or both are binary.
func Unified(a, b []byte, opts Options) string

// SideBySide returns a side-by-side diff string.
// Width is the total column width (each side gets width/2).
// Returns "" if a and b are identical or both are binary.
func SideBySide(a, b []byte, width int, opts Options) string

// Stat returns insertion/deletion counts between a and b.
func Stat(a, b []byte) DiffStat

// DiffStat summarizes the magnitude of changes.
type DiffStat struct {
    Insertions int  // lines present only in b
    Deletions  int  // lines present only in a
    Binary     bool // true if either input contains null bytes
}
```

## Internal Structure

### `myers.go` -- Core Algorithm

Myers diff algorithm operating on lines. Input: two `[]string` (split on `\n`). Output: `[]editOp`.

```go
type opKind int

const (
    opEqual  opKind = iota
    opInsert
    opDelete
)

type editOp struct {
    kind   opKind
    srcIdx int // index in source lines (-1 for insert)
    dstIdx int // index in destination lines (-1 for delete)
    text   string
}

// myers returns the minimal edit script between src and dst.
func myers(src, dst []string) []editOp
```

The Myers algorithm works on the "edit graph" — finding the shortest path from (0,0) to (n,m) where horizontal moves are deletions, vertical moves are insertions, and diagonal moves are equal lines. Uses the classic "snakes" approach with O(nd) time and O(d^2) space (basic variant — stores V array per iteration). The linear-space Hirschberg refinement is deferred; the basic variant is adequate for files under ~100K lines.

### `hunk.go` -- Hunk Grouping

Groups consecutive edit ops into hunks separated by context lines.

```go
type hunk struct {
    srcStart, srcCount int
    dstStart, dstCount int
    ops                []editOp
}

// buildHunks groups edit ops into hunks with the given context lines.
// Adjacent hunks within 2*context lines of each other are merged.
func buildHunks(ops []editOp, contextLines int) []hunk
```

### `format_unified.go` -- Unified Format

Formats hunks as standard unified diff:

```
--- a/file.txt
+++ b/file.txt
@@ -1,4 +1,5 @@
 context line
-deleted line
+inserted line
 context line
```

### `format_sbs.go` -- Side-by-Side Format

Formats hunks as side-by-side columns:

```
 context line                    |  context line
 deleted line                    <
                                 >  inserted line
 context line                    |  context line
```

Handles line truncation when lines exceed column width. Follows GNU `diff -y` conventions: space for equal lines, `|` for changed (both sides differ), `<` for delete-only (left side), `>` for insert-only (right side).

### `diff.go` -- Public Entry Points

Wires together: `splitLines` -> `myers` -> `buildHunks` -> formatter.

```go
func splitLines(data []byte) []string {
    // Split on \n, preserve trailing content without newline
}
```

## File Layout

| File | Purpose | Lines (est.) |
|------|---------|-------------|
| `doc.go` | Package documentation | ~10 |
| `diff.go` | Public API, splitLines, wiring | ~60 |
| `myers.go` | Myers algorithm | ~120 |
| `hunk.go` | Hunk grouping with context | ~80 |
| `format_unified.go` | Unified diff formatter | ~60 |
| `format_sbs.go` | Side-by-side formatter | ~80 |
| `diff_test.go` | Unit tests | ~200 |

## Testing Strategy

**Unit tests:**
- Empty inputs (both empty, one empty, identical)
- Single line change
- Multi-line insertions, deletions, mixed changes
- Context lines: 0, 3, full file
- No trailing newline edge cases
- Binary detection (files with null bytes — return empty diff with comment)
- Large files: 10K+ lines performance sanity check

**Golden tests (pre-generated fixtures in `testdata/`):**
- Compare unified output against pre-generated `fossil diff` output
- Compare side-by-side output against pre-generated `fossil diff -y` output
- No runtime `fossil` binary dependency — fixtures committed to repo

**Stat tests:**
- Verify insertion/deletion/change counts

No DST or sim needed — pure function, no I/O.

## CLI Integration (not in scope for this ticket)

The `edgesync diff` subcommand will compose:

```go
contentA, _ := content.Expand(repo.DB(), ridA)
contentB, _ := content.Expand(repo.DB(), ridB)
fmt.Print(diff.Unified(contentA, contentB, diff.Options{
    ContextLines: 3,
    SrcName:      pathA,
    DstName:      pathB,
}))
```

This lives in `cmd/edgesync/`, not in the `diff/` package.

## Constraints

- No external dependencies
- Line-based only (no character-level or word-level diff)
- Maximum line length: no artificial limit (Go strings handle arbitrary length)
- Binary files: detect null bytes, set `DiffStat.Binary = true`, return empty diff from `Unified`/`SideBySide`
- `splitLines` normalizes `\r\n` to `\n` before comparison (strip `\r`). Output always uses `\n`. Consistent with Fossil.
- `splitLines` duplicates `merge.splitLines` — consider extracting to `internal/textutil` in a follow-up if more packages need it
