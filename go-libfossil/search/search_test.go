package search_test

import (
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
