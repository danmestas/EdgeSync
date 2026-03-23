package branch

import (
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
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

func TestCreate(t *testing.T) {
	r := setupTestRepo(t)

	parentRid, _, err := manifest.Checkin(r, manifest.CheckinOpts{
		Files:   []manifest.File{{Name: "hello.txt", Content: []byte("hello")}},
		Comment: "initial commit",
		User:    "testuser",
		Time:    time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Checkin: %v", err)
	}

	branchRid, _, err := Create(r, CreateOpts{
		Name:   "feature-x",
		Parent: parentRid,
		User:   "testuser",
		Time:   time.Date(2024, 1, 15, 11, 0, 0, 0, time.UTC),
	})
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	if branchRid <= 0 {
		t.Fatalf("branchRid=%d, want > 0", branchRid)
	}

	// Verify branch tag.
	var branchValue string
	err = r.DB().QueryRow(
		"SELECT value FROM tagxref JOIN tag USING(tagid) WHERE tagname='branch' AND rid=?", branchRid,
	).Scan(&branchValue)
	if err != nil {
		t.Fatalf("branch tag query: %v", err)
	}
	if branchValue != "feature-x" {
		t.Errorf("branch value=%q, want %q", branchValue, "feature-x")
	}

	// Verify sym-feature-x tag.
	var symCount int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-feature-x' AND rid=?", branchRid,
	).Scan(&symCount)
	if symCount != 1 {
		t.Errorf("sym-feature-x count=%d, want 1", symCount)
	}

	// Verify old sym-trunk cancelled on the branch checkin.
	var oldSymCount int
	r.DB().QueryRow(
		"SELECT count(*) FROM tagxref JOIN tag USING(tagid) WHERE tagname='sym-trunk' AND tagtype>0 AND rid=?", branchRid,
	).Scan(&oldSymCount)
	if oldSymCount != 0 {
		t.Errorf("sym-trunk should be cancelled on branch, count=%d", oldSymCount)
	}
}
