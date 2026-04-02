package shun_test

import (
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/shun"

	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func setupDB(t *testing.T) *db.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := db.Open(path)
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	if err := db.CreateRepoSchema(d); err != nil {
		t.Fatalf("CreateRepoSchema: %v", err)
	}
	t.Cleanup(func() { d.Close() })
	return d
}

const testUUID = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"

func TestAddAndIsShunned(t *testing.T) {
	d := setupDB(t)

	ok, err := shun.IsShunned(d, testUUID)
	if err != nil {
		t.Fatalf("IsShunned: %v", err)
	}
	if ok {
		t.Fatal("expected not shunned before Add")
	}

	if err := shun.Add(d, testUUID, "test shun"); err != nil {
		t.Fatalf("Add: %v", err)
	}

	ok, err = shun.IsShunned(d, testUUID)
	if err != nil {
		t.Fatalf("IsShunned: %v", err)
	}
	if !ok {
		t.Fatal("expected shunned after Add")
	}
}

func TestRemove(t *testing.T) {
	d := setupDB(t)

	if err := shun.Add(d, testUUID, "to remove"); err != nil {
		t.Fatalf("Add: %v", err)
	}
	if err := shun.Remove(d, testUUID); err != nil {
		t.Fatalf("Remove: %v", err)
	}

	ok, err := shun.IsShunned(d, testUUID)
	if err != nil {
		t.Fatalf("IsShunned: %v", err)
	}
	if ok {
		t.Fatal("expected not shunned after Remove")
	}
}

func TestRemoveNoop(t *testing.T) {
	d := setupDB(t)
	// Remove non-existent UUID should not error.
	if err := shun.Remove(d, testUUID); err != nil {
		t.Fatalf("Remove (noop): %v", err)
	}
}

func TestAddInvalidUUID(t *testing.T) {
	d := setupDB(t)
	if err := shun.Add(d, "not-a-valid-hash", "bad"); err == nil {
		t.Fatal("expected error for invalid UUID")
	}
}

func TestAddIdempotent(t *testing.T) {
	d := setupDB(t)

	if err := shun.Add(d, testUUID, "first"); err != nil {
		t.Fatalf("Add first: %v", err)
	}
	if err := shun.Add(d, testUUID, "second"); err != nil {
		t.Fatalf("Add second: %v", err)
	}

	entries, err := shun.List(d)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(entries) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(entries))
	}
	if entries[0].Comment != "second" {
		t.Errorf("comment = %q, want %q", entries[0].Comment, "second")
	}
}

func TestPurgeStandaloneBlob(t *testing.T) {
	d := setupDB(t)

	// Store a blob and shun it.
	rid, uuid, err := blob.Store(d, []byte("secret content"))
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	if err := shun.Add(d, uuid, "purge test"); err != nil {
		t.Fatalf("Add: %v", err)
	}

	result, err := shun.Purge(d)
	if err != nil {
		t.Fatalf("Purge: %v", err)
	}

	if result.BlobsDeleted != 1 {
		t.Errorf("BlobsDeleted = %d, want 1", result.BlobsDeleted)
	}

	// Verify blob is gone.
	var count int
	d.QueryRow("SELECT COUNT(*) FROM blob WHERE rid=?", rid).Scan(&count)
	if count != 0 {
		t.Error("blob row still exists after purge")
	}
}

func TestPurgeEmpty(t *testing.T) {
	d := setupDB(t)

	result, err := shun.Purge(d)
	if err != nil {
		t.Fatalf("Purge: %v", err)
	}
	if result.BlobsDeleted != 0 || result.DeltasExpanded != 0 || result.PrivateCleaned != 0 {
		t.Errorf("expected zero counts, got %+v", result)
	}
}
