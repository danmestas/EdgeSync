# Diff Engine Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `diff/` package to go-libfossil that produces unified diff output and diff stats from two `[]byte` inputs using the Myers algorithm.

**Architecture:** Single package (`go-libfossil/diff/`) with two files: `diff.go` (all code) and `diff_test.go`. Myers diff algorithm operates on lines, produces edit ops, groups into hunks, formats as unified diff. Pure `[]byte` in, `string` out — no repo dependencies. Side-by-side deferred per Hipp review.

**Tech Stack:** Go stdlib only. Myers diff algorithm. Line-based comparison.

**Spec:** `docs/superpowers/specs/2026-04-02-diff-engine-design.md`

---

## Chunk 1: Myers Algorithm + Unified Output

### Task 1: splitLines + binary detection + Myers algorithm

**Files:**
- Create: `go-libfossil/diff/diff.go`
- Create: `go-libfossil/diff/diff_test.go`

- [ ] **Step 1: Write failing tests for splitLines and isBinary**

Create `go-libfossil/diff/diff_test.go`:

```go
package diff

import (
	"testing"
)

func TestSplitLines(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  []string
	}{
		{"empty", "", nil},
		{"single no newline", "hello", []string{"hello"}},
		{"single with newline", "hello\n", []string{"hello"}},
		{"two lines", "a\nb\n", []string{"a", "b"}},
		{"no trailing newline", "a\nb", []string{"a", "b"}},
		{"crlf normalized", "a\r\nb\r\n", []string{"a", "b"}},
		{"mixed eol", "a\nb\r\nc\n", []string{"a", "b", "c"}},
		{"blank lines", "a\n\nb\n", []string{"a", "", "b"}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := splitLines([]byte(tt.input))
			if len(got) != len(tt.want) {
				t.Fatalf("splitLines(%q) = %v (len %d), want %v (len %d)",
					tt.input, got, len(got), tt.want, len(tt.want))
			}
			for i := range got {
				if got[i] != tt.want[i] {
					t.Errorf("line %d: got %q, want %q", i, got[i], tt.want[i])
				}
			}
		})
	}
}

func TestIsBinary(t *testing.T) {
	if isBinary([]byte("hello world")) {
		t.Fatal("text should not be binary")
	}
	if !isBinary([]byte("hello\x00world")) {
		t.Fatal("null byte should be binary")
	}
	if isBinary(nil) {
		t.Fatal("nil should not be binary")
	}
	if isBinary([]byte{}) {
		t.Fatal("empty should not be binary")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./diff/ -run "TestSplitLines|TestIsBinary" -v`
Expected: FAIL — package doesn't exist

- [ ] **Step 3: Implement splitLines and isBinary**

Create `go-libfossil/diff/diff.go`:

```go
// Package diff produces unified diff output from two byte slices
// using the Myers diff algorithm.
package diff

import (
	"bytes"
	"fmt"
	"strings"
)

// Options configures diff output.
type Options struct {
	ContextLines int    // lines of context around changes; 0 means no context
	SrcName      string // source file label for header (e.g. "a/file.txt")
	DstName      string // destination file label for header
}

// DiffStat summarizes the magnitude of changes.
type DiffStat struct {
	Insertions int  // lines present only in b
	Deletions  int  // lines present only in a
	Binary     bool // true if either input contains null bytes
}

// splitLines splits data into lines, stripping \r and trailing empty line.
func splitLines(data []byte) []string {
	if len(data) == 0 {
		return nil
	}
	s := strings.ReplaceAll(string(data), "\r\n", "\n")
	s = strings.ReplaceAll(s, "\r", "\n")
	lines := strings.Split(s, "\n")
	if len(lines) > 0 && lines[len(lines)-1] == "" {
		lines = lines[:len(lines)-1]
	}
	return lines
}

// isBinary returns true if data contains a null byte.
func isBinary(data []byte) bool {
	return bytes.ContainsRune(data, 0)
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd go-libfossil && go test -buildvcs=false ./diff/ -run "TestSplitLines|TestIsBinary" -v`
Expected: PASS

- [ ] **Step 5: Write failing tests for Myers algorithm**

Add to `diff_test.go`:

```go
func TestMyersIdentical(t *testing.T) {
	ops := myers([]string{"a", "b", "c"}, []string{"a", "b", "c"})
	for _, op := range ops {
		if op.kind != opEqual {
			t.Fatalf("identical inputs should produce only opEqual, got %v", op.kind)
		}
	}
	if len(ops) != 3 {
		t.Fatalf("got %d ops, want 3", len(ops))
	}
}

func TestMyersInsert(t *testing.T) {
	ops := myers([]string{"a", "c"}, []string{"a", "b", "c"})
	var inserts int
	for _, op := range ops {
		if op.kind == opInsert {
			inserts++
			if op.text != "b" {
				t.Fatalf("inserted text = %q, want %q", op.text, "b")
			}
		}
	}
	if inserts != 1 {
		t.Fatalf("got %d inserts, want 1", inserts)
	}
}

func TestMyersDelete(t *testing.T) {
	ops := myers([]string{"a", "b", "c"}, []string{"a", "c"})
	var deletes int
	for _, op := range ops {
		if op.kind == opDelete {
			deletes++
			if op.text != "b" {
				t.Fatalf("deleted text = %q, want %q", op.text, "b")
			}
		}
	}
	if deletes != 1 {
		t.Fatalf("got %d deletes, want 1", deletes)
	}
}

func TestMyersEmpty(t *testing.T) {
	ops := myers(nil, []string{"a", "b"})
	var inserts int
	for _, op := range ops {
		if op.kind == opInsert {
			inserts++
		}
	}
	if inserts != 2 {
		t.Fatalf("got %d inserts, want 2", inserts)
	}

	ops = myers([]string{"a", "b"}, nil)
	var deletes int
	for _, op := range ops {
		if op.kind == opDelete {
			deletes++
		}
	}
	if deletes != 2 {
		t.Fatalf("got %d deletes, want 2", deletes)
	}
}

func TestMyersMixed(t *testing.T) {
	src := []string{"a", "b", "c", "d", "e"}
	dst := []string{"a", "x", "c", "e", "f"}
	ops := myers(src, dst)

	// Verify we get a valid edit script: applying ops to src produces dst.
	var result []string
	for _, op := range ops {
		switch op.kind {
		case opEqual, opInsert:
			result = append(result, op.text)
		}
	}
	if len(result) != len(dst) {
		t.Fatalf("applying ops: got %v, want %v", result, dst)
	}
	for i := range result {
		if result[i] != dst[i] {
			t.Fatalf("line %d: got %q, want %q", i, result[i], dst[i])
		}
	}
}
```

- [ ] **Step 6: Implement Myers algorithm**

Add to `diff.go`:

```go
type opKind int

const (
	opEqual  opKind = iota
	opInsert
	opDelete
)

type editOp struct {
	kind opKind
	text string
}

// myers computes the minimal edit script between src and dst
// using Myers' O(nd) algorithm.
func myers(src, dst []string) []editOp {
	n := len(src)
	m := len(dst)

	if n == 0 && m == 0 {
		return nil
	}
	if n == 0 {
		ops := make([]editOp, m)
		for i, line := range dst {
			ops[i] = editOp{opInsert, line}
		}
		return ops
	}
	if m == 0 {
		ops := make([]editOp, n)
		for i, line := range src {
			ops[i] = editOp{opDelete, line}
		}
		return ops
	}

	max := n + m
	// V array indexed by diagonal k in [-max, max].
	// We use offset = max so V[k+max] = furthest x on diagonal k.
	size := 2*max + 1

	// Store V snapshots for backtracking.
	var trace [][]int

	v := make([]int, size)
	for i := range v {
		v[i] = -1
	}
	v[max] = 0 // diagonal 0 starts at x=0

	for d := 0; d <= max; d++ {
		// Snapshot V before mutations.
		snap := make([]int, size)
		copy(snap, v)
		trace = append(trace, snap)

		for k := -d; k <= d; k += 2 {
			var x int
			if k == -d || (k != d && v[k-1+max] < v[k+1+max]) {
				x = v[k+1+max] // move down (insert)
			} else {
				x = v[k-1+max] + 1 // move right (delete)
			}
			y := x - k

			// Follow diagonal (equal lines).
			for x < n && y < m && src[x] == dst[y] {
				x++
				y++
			}

			v[k+max] = x

			if x >= n && y >= m {
				// Reached the end. Backtrack to build edit script.
				return backtrack(trace, src, dst)
			}
		}
	}

	// Should not reach here for valid inputs.
	panic("diff.myers: failed to find edit path")
}

// backtrack reconstructs the edit script from Myers trace snapshots.
func backtrack(trace [][]int, src, dst []string) []editOp {
	n := len(src)
	m := len(dst)
	max := n + m

	x, y := n, m
	var ops []editOp

	for d := len(trace) - 1; d >= 0; d-- {
		v := trace[d]
		k := x - y

		var prevK int
		if k == -d || (k != d && v[k-1+max] < v[k+1+max]) {
			prevK = k + 1
		} else {
			prevK = k - 1
		}

		prevX := v[prevK+max]
		prevY := prevX - prevK

		// Diagonal moves (equal lines) — walk backward.
		for x > prevX && y > prevY {
			x--
			y--
			ops = append(ops, editOp{opEqual, src[x]})
		}

		if d > 0 {
			if x == prevX {
				// Vertical move: insert
				y--
				ops = append(ops, editOp{opInsert, dst[y]})
			} else {
				// Horizontal move: delete
				x--
				ops = append(ops, editOp{opDelete, src[x]})
			}
		}
	}

	// Reverse since we built it backward.
	for i, j := 0, len(ops)-1; i < j; i, j = i+1, j-1 {
		ops[i], ops[j] = ops[j], ops[i]
	}
	return ops
}
```

- [ ] **Step 7: Run Myers tests**

Run: `cd go-libfossil && go test -buildvcs=false ./diff/ -run "TestMyers" -v`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/diff/diff.go go-libfossil/diff/diff_test.go
git commit -m "feat(diff): add Myers algorithm, splitLines, binary detection

Core diff engine: Myers O(nd) algorithm operating on lines.
splitLines normalizes \r\n to \n. isBinary detects null bytes.
Part of EDG-12.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

### Task 2: Hunk Grouping + Unified Formatter + Stat

**Files:**
- Modify: `go-libfossil/diff/diff.go`
- Modify: `go-libfossil/diff/diff_test.go`

- [ ] **Step 1: Write failing tests for Unified and Stat**

Add to `diff_test.go`:

```go
func TestUnifiedIdentical(t *testing.T) {
	a := []byte("hello\nworld\n")
	got := Unified(a, a, Options{ContextLines: 3})
	if got != "" {
		t.Fatalf("identical inputs should return empty string, got:\n%s", got)
	}
}

func TestUnifiedSimpleChange(t *testing.T) {
	a := []byte("line1\nline2\nline3\n")
	b := []byte("line1\nchanged\nline3\n")
	got := Unified(a, b, Options{
		ContextLines: 3,
		SrcName:      "a/file.txt",
		DstName:      "b/file.txt",
	})
	if got == "" {
		t.Fatal("expected non-empty diff")
	}
	if !strings.Contains(got, "--- a/file.txt") {
		t.Fatalf("missing src header in:\n%s", got)
	}
	if !strings.Contains(got, "+++ b/file.txt") {
		t.Fatalf("missing dst header in:\n%s", got)
	}
	if !strings.Contains(got, "-line2") {
		t.Fatalf("missing deletion in:\n%s", got)
	}
	if !strings.Contains(got, "+changed") {
		t.Fatalf("missing insertion in:\n%s", got)
	}
	if !strings.Contains(got, " line1") {
		t.Fatalf("missing context in:\n%s", got)
	}
	if !strings.Contains(got, "@@") {
		t.Fatalf("missing hunk header in:\n%s", got)
	}
}

func TestUnifiedInsertOnly(t *testing.T) {
	a := []byte("a\nc\n")
	b := []byte("a\nb\nc\n")
	got := Unified(a, b, Options{ContextLines: 3})
	if !strings.Contains(got, "+b") {
		t.Fatalf("expected +b in:\n%s", got)
	}
}

func TestUnifiedDeleteOnly(t *testing.T) {
	a := []byte("a\nb\nc\n")
	b := []byte("a\nc\n")
	got := Unified(a, b, Options{ContextLines: 3})
	if !strings.Contains(got, "-b") {
		t.Fatalf("expected -b in:\n%s", got)
	}
}

func TestUnifiedZeroContext(t *testing.T) {
	a := []byte("a\nb\nc\nd\ne\n")
	b := []byte("a\nB\nc\nd\nE\n")
	got := Unified(a, b, Options{ContextLines: 0})
	// Two changes separated by 2 unchanged lines — with 0 context, must be 2 hunks.
	hunkCount := strings.Count(got, "\n@@ ") + 1 // first hunk has no leading \n
	if !strings.HasPrefix(got, "---") {
		t.Fatalf("expected diff header, got:\n%s", got)
	}
	if hunkCount != 2 {
		t.Fatalf("expected 2 hunks with 0 context, got %d:\n%s", hunkCount, got)
	}
	// No context lines should appear.
	if strings.Contains(got, " a") || strings.Contains(got, " c") || strings.Contains(got, " d") {
		t.Fatalf("zero-context diff should not have context lines:\n%s", got)
	}
}

func TestUnifiedBinary(t *testing.T) {
	a := []byte("hello\x00world")
	b := []byte("different")
	got := Unified(a, b, Options{})
	if got != "" {
		t.Fatalf("binary input should return empty string, got:\n%s", got)
	}
}

func TestUnifiedBothEmpty(t *testing.T) {
	got := Unified(nil, nil, Options{})
	if got != "" {
		t.Fatalf("both nil should return empty, got: %q", got)
	}
}

func TestUnifiedOneEmpty(t *testing.T) {
	a := []byte("hello\n")
	got := Unified(nil, a, Options{SrcName: "old", DstName: "new"})
	if !strings.Contains(got, "+hello") {
		t.Fatalf("expected insertion in:\n%s", got)
	}

	got2 := Unified(a, nil, Options{SrcName: "old", DstName: "new"})
	if !strings.Contains(got2, "-hello") {
		t.Fatalf("expected deletion in:\n%s", got2)
	}
}

func TestStatSimple(t *testing.T) {
	a := []byte("a\nb\nc\n")
	b := []byte("a\nB\nc\nd\n")
	stat := Stat(a, b)
	// b changed -> 1 deletion + 1 insertion, d added -> 1 insertion
	if stat.Insertions < 1 {
		t.Fatalf("Insertions = %d, want >= 1", stat.Insertions)
	}
	if stat.Deletions < 1 {
		t.Fatalf("Deletions = %d, want >= 1", stat.Deletions)
	}
	if stat.Binary {
		t.Fatal("should not be binary")
	}
}

func TestStatBinary(t *testing.T) {
	stat := Stat([]byte("a\x00b"), []byte("c"))
	if !stat.Binary {
		t.Fatal("should detect binary")
	}
}

func TestStatIdentical(t *testing.T) {
	a := []byte("same\n")
	stat := Stat(a, a)
	if stat.Insertions != 0 || stat.Deletions != 0 {
		t.Fatalf("identical: got %+v", stat)
	}
}
```

Add `"strings"` to the test imports.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test -buildvcs=false ./diff/ -run "TestUnified|TestStat" -v`
Expected: FAIL — `Unified` and `Stat` undefined

- [ ] **Step 3: Implement hunk grouping, unified formatter, Stat, and public API**

Add to `diff.go`:

```go
// hunk represents a group of changes with surrounding context.
type hunk struct {
	srcStart, srcCount int
	dstStart, dstCount int
	ops                []editOp
}

// buildHunks groups edit ops into hunks with context lines.
// Adjacent hunks within 2*contextLines of each other are merged.
func buildHunks(ops []editOp, contextLines int) []hunk {
	if len(ops) == 0 {
		return nil
	}

	// Find change positions.
	type changeRange struct{ start, end int }
	var changes []changeRange
	for i, op := range ops {
		if op.kind != opEqual {
			if len(changes) == 0 || i > changes[len(changes)-1].end {
				changes = append(changes, changeRange{i, i + 1})
			} else {
				changes[len(changes)-1].end = i + 1
			}
		}
	}

	if len(changes) == 0 {
		return nil
	}

	// Merge adjacent changes that are within 2*contextLines of each other.
	merged := []changeRange{changes[0]}
	for _, c := range changes[1:] {
		prev := &merged[len(merged)-1]
		gap := c.start - prev.end
		if gap <= 2*contextLines {
			prev.end = c.end
		} else {
			merged = append(merged, c)
		}
	}

	// Build hunks with context.
	var hunks []hunk
	srcLine, dstLine := 0, 0

	// Track src/dst line positions through ops.
	srcPos := make([]int, len(ops))
	dstPos := make([]int, len(ops))
	si, di := 0, 0
	for i, op := range ops {
		srcPos[i] = si
		dstPos[i] = di
		switch op.kind {
		case opEqual:
			si++
			di++
		case opDelete:
			si++
		case opInsert:
			di++
		}
	}

	for _, cr := range merged {
		start := cr.start - contextLines
		if start < 0 {
			start = 0
		}
		end := cr.end + contextLines
		if end > len(ops) {
			end = len(ops)
		}

		h := hunk{
			srcStart: srcPos[start] + 1, // 1-indexed
			dstStart: dstPos[start] + 1,
			ops:      ops[start:end],
		}

		// Count src and dst lines in this hunk.
		for _, op := range h.ops {
			switch op.kind {
			case opEqual:
				h.srcCount++
				h.dstCount++
			case opDelete:
				h.srcCount++
			case opInsert:
				h.dstCount++
			}
		}

		hunks = append(hunks, h)
	}

	return hunks
}

// formatUnified formats hunks as a unified diff string.
func formatUnified(hunks []hunk, srcName, dstName string) string {
	if len(hunks) == 0 {
		return ""
	}

	var buf strings.Builder
	if srcName == "" {
		srcName = "a"
	}
	if dstName == "" {
		dstName = "b"
	}
	fmt.Fprintf(&buf, "--- %s\n", srcName)
	fmt.Fprintf(&buf, "+++ %s\n", dstName)

	for _, h := range hunks {
		fmt.Fprintf(&buf, "@@ -%d,%d +%d,%d @@\n",
			h.srcStart, h.srcCount, h.dstStart, h.dstCount)
		for _, op := range h.ops {
			switch op.kind {
			case opEqual:
				fmt.Fprintf(&buf, " %s\n", op.text)
			case opDelete:
				fmt.Fprintf(&buf, "-%s\n", op.text)
			case opInsert:
				fmt.Fprintf(&buf, "+%s\n", op.text)
			}
		}
	}

	return buf.String()
}

// Unified returns a unified diff string between a and b.
// Returns "" if a and b are identical or either is binary.
func Unified(a, b []byte, opts Options) string {
	if isBinary(a) || isBinary(b) {
		return ""
	}
	srcLines := splitLines(a)
	dstLines := splitLines(b)
	ops := myers(srcLines, dstLines)

	allEqual := true
	for _, op := range ops {
		if op.kind != opEqual {
			allEqual = false
			break
		}
	}
	if allEqual {
		return ""
	}

	hunks := buildHunks(ops, opts.ContextLines)
	return formatUnified(hunks, opts.SrcName, opts.DstName)
}

// Stat returns insertion/deletion counts between a and b.
func Stat(a, b []byte) DiffStat {
	if isBinary(a) || isBinary(b) {
		return DiffStat{Binary: true}
	}
	srcLines := splitLines(a)
	dstLines := splitLines(b)
	ops := myers(srcLines, dstLines)

	var stat DiffStat
	for _, op := range ops {
		switch op.kind {
		case opInsert:
			stat.Insertions++
		case opDelete:
			stat.Deletions++
		}
	}
	return stat
}
```

- [ ] **Step 4: Run all tests**

Run: `cd go-libfossil && go test -buildvcs=false ./diff/ -v`
Expected: All PASS

- [ ] **Step 5: Fix any edge cases and run again**

If any tests fail, debug and fix. Common issues:
- Hunk line numbering (1-indexed vs 0-indexed)
- Empty file edge cases in hunk building
- Context line overflow at start/end of file

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/diff/diff.go go-libfossil/diff/diff_test.go
git commit -m "feat(diff): add Unified() and Stat() with hunk grouping

Unified diff output with configurable context lines.
DiffStat reports insertions, deletions, and binary detection.
Part of EDG-12.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

### Task 3: Performance Test + Full Verification

**Files:**
- Modify: `go-libfossil/diff/diff_test.go`

- [ ] **Step 1: Add performance sanity test**

Add to `diff_test.go`:

```go
func TestUnifiedLargeFile(t *testing.T) {
	// 10K lines, change every 100th line.
	var a, b strings.Builder
	for i := 0; i < 10000; i++ {
		fmt.Fprintf(&a, "line %d original\n", i)
		if i%100 == 50 {
			fmt.Fprintf(&b, "line %d CHANGED\n", i)
		} else {
			fmt.Fprintf(&b, "line %d original\n", i)
		}
	}

	got := Unified([]byte(a.String()), []byte(b.String()), Options{ContextLines: 3})
	if got == "" {
		t.Fatal("expected non-empty diff for large file")
	}

	stat := Stat([]byte(a.String()), []byte(b.String()))
	// 100 lines changed = 100 deletions + 100 insertions
	if stat.Insertions != 100 {
		t.Fatalf("Insertions = %d, want 100", stat.Insertions)
	}
	if stat.Deletions != 100 {
		t.Fatalf("Deletions = %d, want 100", stat.Deletions)
	}
}

func BenchmarkMyers(b *testing.B) {
	var src, dst []string
	for i := 0; i < 1000; i++ {
		src = append(src, fmt.Sprintf("line %d", i))
		if i%10 == 5 {
			dst = append(dst, fmt.Sprintf("line %d changed", i))
		} else {
			dst = append(dst, fmt.Sprintf("line %d", i))
		}
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		myers(src, dst)
	}
}
```

Add `"fmt"` to test imports if not already present.

- [ ] **Step 2: Run all tests including performance**

Run: `cd go-libfossil && go test -buildvcs=false ./diff/ -v -count=1`
Expected: All PASS

- [ ] **Step 3: Run full project test suite**

Run: `make test`
Expected: All PASS (no regressions)

- [ ] **Step 4: Run build**

Run: `make build`
Expected: All binaries build

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/diff/diff_test.go
git commit -m "test(diff): add large file test and benchmark

10K-line file with 100 changes verifies correctness at scale.
Benchmark for Myers on 1K lines with 10% changes.
Part of EDG-12.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```
