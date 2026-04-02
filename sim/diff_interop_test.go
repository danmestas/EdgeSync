package sim

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/diff"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

// extractHunkContent strips diff headers and returns only the @@ and +/-/space lines.
func extractHunkContent(diffOutput string) string {
	var lines []string
	for _, line := range strings.Split(diffOutput, "\n") {
		if strings.HasPrefix(line, "@@") ||
			strings.HasPrefix(line, "+") ||
			strings.HasPrefix(line, "-") ||
			strings.HasPrefix(line, " ") {
			lines = append(lines, line)
		}
	}
	return strings.Join(lines, "\n")
}

func fossilDiff(t *testing.T, fossilPath, fileA, fileB string) string {
	t.Helper()
	cmd := exec.Command(fossilPath, "test-diff", fileA, fileB)
	out, err := cmd.CombinedOutput()
	if err != nil {
		// fossil test-diff returns exit code 1 when files differ -- that's normal.
		// Only fail on actual execution errors.
		if len(out) == 0 {
			t.Fatalf("fossil test-diff failed with no output: %v", err)
		}
	}
	return string(out)
}

func writeTempFile(t *testing.T, dir, name, content string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
	return path
}

// countPrefixLines counts lines starting with prefix, excluding --- and +++ headers.
func countPrefixLines(text, prefix string) int {
	count := 0
	for _, line := range strings.Split(text, "\n") {
		if strings.HasPrefix(line, prefix) &&
			!strings.HasPrefix(line, "---") &&
			!strings.HasPrefix(line, "+++") {
			count++
		}
	}
	return count
}

// assertHunkMatch compares hunk content between fossil and Go diff output.
// If exact match fails, falls back to comparing hunk count and +/- line counts
// (different Myers implementations can produce different but equally valid minimal edits).
func assertHunkMatch(t *testing.T, fossilOut, goOut string) {
	t.Helper()

	fossilHunks := extractHunkContent(fossilOut)
	goHunks := extractHunkContent(goOut)

	if fossilHunks == goHunks {
		return // exact match
	}

	// Fall back: compare hunk count and +/- line counts.
	fossilHunkCount := strings.Count(fossilOut, "@@ -")
	goHunkCount := strings.Count(goOut, "@@ -")

	fossilPlus := countPrefixLines(fossilOut, "+")
	goPlus := countPrefixLines(goOut, "+")
	fossilMinus := countPrefixLines(fossilOut, "-")
	goMinus := countPrefixLines(goOut, "-")

	if fossilHunkCount != goHunkCount {
		t.Errorf("hunk count mismatch: fossil=%d, go=%d", fossilHunkCount, goHunkCount)
	}
	if fossilPlus != goPlus {
		t.Errorf("insertion count mismatch: fossil=%d, go=%d", fossilPlus, goPlus)
	}
	if fossilMinus != goMinus {
		t.Errorf("deletion count mismatch: fossil=%d, go=%d", fossilMinus, goMinus)
	}

	if fossilHunkCount == goHunkCount && fossilPlus == goPlus && fossilMinus == goMinus {
		t.Logf("exact hunk content differs (valid alternate edit script), counts match")
	} else {
		t.Logf("--- fossil ---\n%s", fossilHunks)
		t.Logf("--- go ---\n%s", goHunks)
	}
}

func TestDiffMatchesFossil_SimpleChange(t *testing.T) {
	fossilPath := testutil.FossilBinary()
	if fossilPath == "" {
		t.Skip("fossil binary not found")
	}

	a := "line1\nline2\nline3\nline4\nline5\n"
	b := "line1\nline2\nchanged\nline4\nline5\n"

	dir := t.TempDir()
	fileA := writeTempFile(t, dir, "a.txt", a)
	fileB := writeTempFile(t, dir, "b.txt", b)

	fossilOut := fossilDiff(t, fossilPath, fileA, fileB)
	goOut := diff.Unified([]byte(a), []byte(b), diff.Options{ContextLines: 3})

	assertHunkMatch(t, fossilOut, goOut)
}

func TestDiffMatchesFossil_MultipleChanges(t *testing.T) {
	fossilPath := testutil.FossilBinary()
	if fossilPath == "" {
		t.Skip("fossil binary not found")
	}

	var a, b strings.Builder
	for i := 0; i < 100; i++ {
		fmt.Fprintf(&a, "line %d\n", i)
		if i == 20 || i == 50 || i == 80 {
			fmt.Fprintf(&b, "CHANGED %d\n", i)
		} else {
			fmt.Fprintf(&b, "line %d\n", i)
		}
	}

	dir := t.TempDir()
	fileA := writeTempFile(t, dir, "a.txt", a.String())
	fileB := writeTempFile(t, dir, "b.txt", b.String())

	fossilOut := fossilDiff(t, fossilPath, fileA, fileB)
	goOut := diff.Unified([]byte(a.String()), []byte(b.String()), diff.Options{ContextLines: 3})

	assertHunkMatch(t, fossilOut, goOut)
}

func TestDiffMatchesFossil_InsertOnly(t *testing.T) {
	fossilPath := testutil.FossilBinary()
	if fossilPath == "" {
		t.Skip("fossil binary not found")
	}

	a := "line1\nline3\n"
	b := "line1\nline2\nline3\n"

	dir := t.TempDir()
	fileA := writeTempFile(t, dir, "a.txt", a)
	fileB := writeTempFile(t, dir, "b.txt", b)

	fossilOut := fossilDiff(t, fossilPath, fileA, fileB)
	goOut := diff.Unified([]byte(a), []byte(b), diff.Options{ContextLines: 3})

	assertHunkMatch(t, fossilOut, goOut)
}

func TestDiffMatchesFossil_DeleteOnly(t *testing.T) {
	fossilPath := testutil.FossilBinary()
	if fossilPath == "" {
		t.Skip("fossil binary not found")
	}

	a := "line1\nline2\nline3\n"
	b := "line1\nline3\n"

	dir := t.TempDir()
	fileA := writeTempFile(t, dir, "a.txt", a)
	fileB := writeTempFile(t, dir, "b.txt", b)

	fossilOut := fossilDiff(t, fossilPath, fileA, fileB)
	goOut := diff.Unified([]byte(a), []byte(b), diff.Options{ContextLines: 3})

	assertHunkMatch(t, fossilOut, goOut)
}

func TestDiffMatchesFossil_LargeFile(t *testing.T) {
	fossilPath := testutil.FossilBinary()
	if fossilPath == "" {
		t.Skip("fossil binary not found")
	}

	var a, b strings.Builder
	for i := 0; i < 1000; i++ {
		fmt.Fprintf(&a, "line %d original content\n", i)
		if i%100 == 50 {
			fmt.Fprintf(&b, "line %d MODIFIED content\n", i)
		} else {
			fmt.Fprintf(&b, "line %d original content\n", i)
		}
	}

	dir := t.TempDir()
	fileA := writeTempFile(t, dir, "a.txt", a.String())
	fileB := writeTempFile(t, dir, "b.txt", b.String())

	fossilOut := fossilDiff(t, fossilPath, fileA, fileB)
	goOut := diff.Unified([]byte(a.String()), []byte(b.String()), diff.Options{ContextLines: 3})

	// For large files, compare hunk count and stat rather than exact content
	// (minor algorithmic differences in optimal edit path are acceptable).
	fossilHunkCount := strings.Count(fossilOut, "@@ -")
	goHunkCount := strings.Count(goOut, "@@ -")

	if fossilHunkCount != goHunkCount {
		t.Fatalf("hunk count mismatch: fossil=%d, go=%d", fossilHunkCount, goHunkCount)
	}

	// Verify same number of + and - lines.
	fossilPlus := countPrefixLines(fossilOut, "+")
	goPlus := countPrefixLines(goOut, "+")
	fossilMinus := countPrefixLines(fossilOut, "-")
	goMinus := countPrefixLines(goOut, "-")

	if fossilPlus != goPlus {
		t.Fatalf("insertion count: fossil=%d, go=%d", fossilPlus, goPlus)
	}
	if fossilMinus != goMinus {
		t.Fatalf("deletion count: fossil=%d, go=%d", fossilMinus, goMinus)
	}
}
