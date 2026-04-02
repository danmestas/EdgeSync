// Package diff produces unified diff output from two byte slices
// using the Myers diff algorithm.
package diff

import (
	"bytes"
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
	if len(lines) == 0 {
		return nil
	}
	return lines
}

// isBinary returns true if data contains a null byte.
func isBinary(data []byte) bool {
	return bytes.ContainsRune(data, 0)
}

type opKind int

const (
	opEqual opKind = iota
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
		v[i] = 0
	}
	v[1+max] = 0 // standard Myers: V[1] = 0

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

		// Diagonal moves (equal lines) -- walk backward.
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
