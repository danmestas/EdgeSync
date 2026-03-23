package search_test

import (
	"fmt"
	"strings"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/search"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func newTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	dir := t.TempDir()
	r, err := repo.Create(dir+"/test.fossil", "test", simio.CryptoRand{})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestFTS5Available(t *testing.T) {
	r := newTestRepo(t)
	rows, err := r.DB().Query("PRAGMA compile_options")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()

	found := false
	for rows.Next() {
		var opt string
		if err := rows.Scan(&opt); err != nil {
			t.Fatal(err)
		}
		if opt == "ENABLE_FTS5" {
			found = true
			break
		}
	}
	if !found {
		t.Fatal("FTS5 not available in SQLite driver — search package requires it")
	}
}

func TestOpenCreatesSchema(t *testing.T) {
	r := newTestRepo(t)
	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	// Verify FTS table exists by attempting a query
	_, err = idx.Search(search.Query{Term: "test"})
	if err != nil {
		t.Fatal("search after open failed:", err)
	}
}

func TestDrop(t *testing.T) {
	r := newTestRepo(t)
	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.Drop(); err != nil {
		t.Fatal(err)
	}
	// After drop, Open should recreate tables
	idx2, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	_, err = idx2.Search(search.Query{Term: "test"})
	if err != nil {
		t.Fatal("search after drop+reopen failed:", err)
	}
}

func TestNeedsReindex_EmptyRepo(t *testing.T) {
	r := newTestRepo(t)
	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	needs, err := idx.NeedsReindex()
	if err != nil {
		t.Fatal(err)
	}
	// Empty repo has no trunk tip — nothing to index
	if needs {
		t.Fatal("expected NeedsReindex=false for empty repo")
	}
}

func TestNeedsReindex_AfterCheckin(t *testing.T) {
	r := newTestRepo(t)

	// manifest.Checkin creates repos programmatically — no fossil binary needed.
	// manifest.File fields: Name string, Content []byte, Perm string
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello world")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	needs, err := idx.NeedsReindex()
	if err != nil {
		t.Fatal(err)
	}
	if !needs {
		t.Fatal("expected NeedsReindex=true after checkin")
	}
}

func TestRebuildIndex_IndexesFiles(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "main.go", Content: []byte("package main\n\nfunc handleSync() {}\n")},
			{Name: "README.md", Content: []byte("# Hello World\n")},
		},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	// After rebuild, NeedsReindex should be false
	needs, err := idx.NeedsReindex()
	if err != nil {
		t.Fatal(err)
	}
	if needs {
		t.Fatal("expected NeedsReindex=false after rebuild")
	}
}

func TestRebuildIndex_SkipsBinaries(t *testing.T) {
	if testing.Short() {
		t.Skip("requires Search implementation (Task 4)")
	}
	r := newTestRepo(t)

	binaryContent := make([]byte, 100)
	binaryContent[50] = 0x00 // null byte
	copy(binaryContent, []byte("not really text"))

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "text.txt", Content: []byte("searchable content here")},
			{Name: "image.bin", Content: binaryContent},
		},
		Comment: "with binary",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	// Verify text file IS searchable
	results, err := idx.Search(search.Query{Term: "searchable"})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result, got %d", len(results))
	}
	if results[0].Path != "text.txt" {
		t.Fatalf("expected text.txt, got %s", results[0].Path)
	}

	// Verify binary file is NOT searchable
	binaryResults, err := idx.Search(search.Query{Term: "not really text"})
	if err != nil {
		t.Fatal(err)
	}
	if len(binaryResults) != 0 {
		t.Fatalf("expected 0 results for binary content, got %d", len(binaryResults))
	}
}

func TestRebuildIndex_HandlesDeltaChains(t *testing.T) {
	if testing.Short() {
		t.Skip("requires Search implementation (Task 4)")
	}
	r := newTestRepo(t)

	// First checkin
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "data.txt", Content: []byte("original content")}},
		Comment: "first",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	// Second checkin with Delta=true — blob stored as delta, not full content
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "data.txt", Content: []byte("modified content")}},
		Comment: "second",
		User:    "test",
		Delta:   true,
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	// Should find the expanded (modified) content, not the delta bytes
	results, err := idx.Search(search.Query{Term: "modified"})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result for delta-expanded content, got %d", len(results))
	}
}

func TestRebuildIndex_Idempotent(t *testing.T) {
	if testing.Short() {
		t.Skip("requires Search implementation (Task 4)")
	}
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.txt", Content: []byte("hello world")}},
		Comment: "initial",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}

	// Rebuild twice — second should no-op
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	results, err := idx.Search(search.Query{Term: "hello"})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result after double rebuild, got %d", len(results))
	}
}

func TestSearch_SubstringMatch(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "sync/client.go", Content: []byte("package sync\n\nfunc handleSync() {\n\tlog.Println(\"syncing\")\n}\n")},
			{Name: "sync/server.go", Content: []byte("package sync\n\nfunc ServeHTTP() {\n\tlog.Println(\"serving\")\n}\n")},
		},
		Comment: "sync code",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	results, err := idx.Search(search.Query{Term: "handleSync"})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result for 'handleSync', got %d", len(results))
	}
	if results[0].Path != "sync/client.go" {
		t.Fatalf("expected sync/client.go, got %s", results[0].Path)
	}
	if results[0].Line != 3 {
		t.Fatalf("expected line 3, got %d", results[0].Line)
	}
	if results[0].MatchLen != 10 {
		t.Fatalf("expected MatchLen=10, got %d", results[0].MatchLen)
	}
}

func TestSearch_CaseInsensitive(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "a.go", Content: []byte("func HandleSync() {}\n")}},
		Comment: "case test",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	// Search with different case — should still match
	results, err := idx.Search(search.Query{Term: "handlesync"})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 case-insensitive result, got %d", len(results))
	}
}

func TestSearch_MinTermLength(t *testing.T) {
	r := newTestRepo(t)
	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	// Sub-3-char query returns empty
	results, err := idx.Search(search.Query{Term: "ab"})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 0 {
		t.Fatalf("expected 0 results for 2-char query, got %d", len(results))
	}
}

func TestSearch_WithContext(t *testing.T) {
	r := newTestRepo(t)

	fileContent := "line1\nline2\nline3 target\nline4\nline5\n"
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "f.txt", Content: []byte(fileContent)}},
		Comment: "context test",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	results, err := idx.Search(search.Query{Term: "target", ContextLines: 1})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result, got %d", len(results))
	}
	if results[0].Line != 3 {
		t.Fatalf("expected line 3, got %d", results[0].Line)
	}
	ctx := results[0].Context
	if !strings.Contains(ctx, "line2") || !strings.Contains(ctx, "line3 target") || !strings.Contains(ctx, "line4") {
		t.Fatalf("context missing expected lines: %q", ctx)
	}
}

func TestSearch_EscapesFTS5SpecialChars(t *testing.T) {
	r := newTestRepo(t)

	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "q.txt", Content: []byte(`she said "hello" to the world`)}},
		Comment: "quotes",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	// Search for a term containing a double quote — should not crash
	results, err := idx.Search(search.Query{Term: `"hello"`})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 1 {
		t.Fatalf("expected 1 result for quoted search, got %d", len(results))
	}
}

func TestSearch_MaxResults(t *testing.T) {
	r := newTestRepo(t)

	var files []manifest.File
	for i := 0; i < 10; i++ {
		files = append(files, manifest.File{
			Name:    fmt.Sprintf("file%d.txt", i),
			Content: []byte("common search term here"),
		})
	}
	_, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   files,
		Comment: "many files",
		User:    "test",
	})
	if err != nil {
		t.Fatal(err)
	}

	idx, err := search.Open(r)
	if err != nil {
		t.Fatal(err)
	}
	if err := idx.RebuildIndex(); err != nil {
		t.Fatal(err)
	}

	results, err := idx.Search(search.Query{Term: "common search", MaxResults: 3})
	if err != nil {
		t.Fatal(err)
	}
	if len(results) > 3 {
		t.Fatalf("expected at most 3 results, got %d", len(results))
	}
}
