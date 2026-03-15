package manifest

import (
	"bytes"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/testutil"
)

func setupTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "testuser", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	t.Cleanup(func() { r.Close() })
	return r
}

func TestCheckinBasic(t *testing.T) {
	r := setupTestRepo(t)
	rid, uuid, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("hello world")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	if rid <= 0 {
		t.Fatalf("rid = %d", rid)
	}
	if len(uuid) != 40 && len(uuid) != 64 {
		t.Fatalf("uuid len = %d", len(uuid))
	}
	var comment string
	r.DB().QueryRow("SELECT comment FROM event WHERE objid=?", rid).Scan(&comment)
	if comment != "initial commit" {
		t.Fatalf("event comment = %q", comment)
	}
	var leafCount int
	r.DB().QueryRow("SELECT count(*) FROM leaf WHERE rid=?", rid).Scan(&leafCount)
	if leafCount != 1 {
		t.Fatalf("leaf count = %d", leafCount)
	}
}

func TestCheckinFossilRebuild(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}
	r := setupTestRepo(t)
	_, _, err := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "hello.txt", Content: []byte("hello world")},
			{Name: "src/main.go", Content: []byte("package main\n")},
		},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}
	r.Close()
	if err := testutil.FossilRebuild(r.Path()); err != nil {
		t.Fatalf("fossil rebuild: %v", err)
	}
}

func TestCheckinMultiple(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "file.txt", Content: []byte("v1")}},
		Comment: "first",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "file.txt", Content: []byte("v2")}},
		Comment: "second",
		User:    "testuser",
		Parent:  rid1,
		Time:    time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	var leafCount int
	r.DB().QueryRow("SELECT count(*) FROM leaf WHERE rid=?", rid1).Scan(&leafCount)
	if leafCount != 0 {
		t.Fatal("rid1 still a leaf")
	}
	r.DB().QueryRow("SELECT count(*) FROM leaf WHERE rid=?", rid2).Scan(&leafCount)
	if leafCount != 1 {
		t.Fatal("rid2 not a leaf")
	}
	var plinkCount int
	r.DB().QueryRow("SELECT count(*) FROM plink WHERE pid=? AND cid=?", rid1, rid2).Scan(&plinkCount)
	if plinkCount != 1 {
		t.Fatal("plink missing")
	}
}

func TestGetManifest(t *testing.T) {
	r := setupTestRepo(t)
	rid, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "test commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 30, 0, 0, time.UTC),
	})
	d, err := GetManifest(r, rid)
	if err != nil {
		t.Fatalf("GetManifest: %v", err)
	}
	if d.C != "test commit" {
		t.Fatalf("C = %q", d.C)
	}
	if d.Type != deck.Checkin {
		t.Fatalf("Type = %d", d.Type)
	}
	if len(d.F) != 1 || d.F[0].Name != "hello.txt" {
		t.Fatalf("F = %+v", d.F)
	}
}

func TestListFilesBaseline(t *testing.T) {
	r := setupTestRepo(t)
	rid, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "initial",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	files, err := ListFiles(r, rid)
	if err != nil {
		t.Fatalf("ListFiles: %v", err)
	}
	if len(files) != 2 {
		t.Fatalf("count = %d", len(files))
	}
}

func TestLogMultiple(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("v1")}},
		Comment: "first", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("v2")}},
		Comment: "second", User: "testuser", Parent: rid1,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	rid3, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("v3")}},
		Comment: "third", User: "testuser", Parent: rid2,
		Time: time.Date(2024, 1, 15, 12, 0, 0, 0, time.UTC),
	})
	entries, err := Log(r, LogOpts{Start: rid3})
	if err != nil {
		t.Fatalf("Log: %v", err)
	}
	if len(entries) != 3 {
		t.Fatalf("count = %d", len(entries))
	}
	if entries[0].Comment != "third" || entries[2].Comment != "first" {
		t.Fatalf("order: %q %q %q", entries[0].Comment, entries[1].Comment, entries[2].Comment)
	}
}

func TestLogWithLimit(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("v1")}},
		Comment: "first", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "a.txt", Content: []byte("v2")}},
		Comment: "second", User: "testuser", Parent: rid1,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	entries, _ := Log(r, LogOpts{Start: rid2, Limit: 1})
	if len(entries) != 1 {
		t.Fatalf("count = %d", len(entries))
	}
}

func TestCheckinDelta(t *testing.T) {
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "baseline", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	rid2, _, err := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa-modified")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "delta", User: "testuser", Parent: rid1, Delta: true,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin delta: %v", err)
	}
	d, _ := GetManifest(r, rid2)
	if d.B == "" {
		t.Fatal("delta should have B-card")
	}
	if len(d.F) != 1 || d.F[0].Name != "a.txt" {
		t.Fatalf("delta F = %+v, want only a.txt", d.F)
	}
	files, _ := ListFiles(r, rid2)
	if len(files) != 2 {
		t.Fatalf("ListFiles = %d, want 2", len(files))
	}
}

func TestCheckinDeltaFossilRebuild(t *testing.T) {
	if !testutil.HasFossil() {
		t.Skip("fossil not in PATH")
	}
	r := setupTestRepo(t)
	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa")},
			{Name: "b.txt", Content: []byte("bbb")},
		},
		Comment: "baseline", User: "testuser",
		Time: time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	_, _, err := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("aaa-v2")},
			{Name: "b.txt", Content: []byte("bbb")},
			{Name: "c.txt", Content: []byte("new file")},
		},
		Comment: "delta with add", User: "testuser", Parent: rid1, Delta: true,
		Time: time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("delta: %v", err)
	}
	r.Close()
	if err := testutil.FossilRebuild(r.Path()); err != nil {
		t.Fatalf("rebuild: %v", err)
	}
}

func BenchmarkCheckin(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	r, _ := repo.Create(path, "bench", simio.CryptoRand{})
	defer r.Close()
	files := make([]File, 10)
	for i := range files {
		files[i] = File{Name: fmt.Sprintf("src/file%03d.go", i), Content: bytes.Repeat([]byte("x"), 1000)}
	}
	b.ResetTimer()
	var lastRid libfossil.FslID
	for i := 0; i < b.N; i++ {
		rid, _, _ := Checkin(r, CheckinOpts{
			Files: files, Comment: "bench", User: "bench",
			Parent: lastRid, Time: time.Now().UTC(),
		})
		lastRid = rid
	}
}

func BenchmarkListFiles(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	r, _ := repo.Create(path, "bench", simio.CryptoRand{})
	defer r.Close()
	files := make([]File, 50)
	for i := range files {
		files[i] = File{Name: fmt.Sprintf("src/file%03d.go", i), Content: []byte(fmt.Sprintf("c-%d", i))}
	}
	rid, _, _ := Checkin(r, CheckinOpts{
		Files: files, Comment: "bench", User: "bench", Time: time.Now().UTC(),
	})
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ListFiles(r, rid)
	}
}
