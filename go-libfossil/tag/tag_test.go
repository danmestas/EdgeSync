package tag

import (
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
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

func TestAddTag(t *testing.T) {
	r := setupTestRepo(t)

	// Create a checkin to tag
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Add a singleton tag
	tagRid, err := AddTag(r, TagOpts{
		TargetRID: rid,
		TagName:   "testlabel",
		TagType:   TagSingleton,
		Value:     "myvalue",
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag: %v", err)
	}
	if tagRid <= 0 {
		t.Fatalf("tagRid = %d, want > 0", tagRid)
	}

	// Verify tagxref has the correct entry
	var tagtype int
	var value string
	err = r.DB().QueryRow(
		`SELECT tagtype, value FROM tagxref
		 JOIN tag ON tag.tagid = tagxref.tagid
		 WHERE tag.tagname = ? AND tagxref.rid = ?`,
		"testlabel", rid,
	).Scan(&tagtype, &value)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagtype != TagSingleton {
		t.Fatalf("tagtype = %d, want %d (singleton)", tagtype, TagSingleton)
	}
	if value != "myvalue" {
		t.Fatalf("value = %q, want %q", value, "myvalue")
	}
}

func TestCancelTag(t *testing.T) {
	r := setupTestRepo(t)

	// Create a checkin (auto-gets sym-trunk tag via propagation in manifest)
	rid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	// Cancel the sym-trunk tag
	cancelRid, err := AddTag(r, TagOpts{
		TargetRID: rid,
		TagName:   "sym-trunk",
		TagType:   TagCancel,
		User:      "testuser",
		Time:      time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("AddTag cancel: %v", err)
	}
	if cancelRid <= 0 {
		t.Fatalf("cancelRid = %d, want > 0", cancelRid)
	}

	// Verify tagxref has tagtype=0 (cancel)
	var tagtype int
	err = r.DB().QueryRow(
		`SELECT tagtype FROM tagxref
		 JOIN tag ON tag.tagid = tagxref.tagid
		 WHERE tag.tagname = ? AND tagxref.rid = ?`,
		"sym-trunk", rid,
	).Scan(&tagtype)
	if err != nil {
		t.Fatalf("tagxref query: %v", err)
	}
	if tagtype != TagCancel {
		t.Fatalf("tagtype = %d, want %d (cancel)", tagtype, TagCancel)
	}
}
