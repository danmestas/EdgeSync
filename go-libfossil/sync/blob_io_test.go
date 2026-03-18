package sync

import (
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/hash"
)

func TestStoreBlobFull(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("hello world")
	uuid := hash.SHA1(data)

	if err := StoreBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("StoreBlob: %v", err)
	}

	rid, ok := blob.Exists(r.DB(), uuid)
	if !ok {
		t.Fatal("blob not found after store")
	}
	if rid <= 0 {
		t.Fatalf("expected positive rid, got %d", rid)
	}
}

func TestStoreBlobBadHash(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("hello world")
	badUUID := "0000000000000000000000000000000000000000"

	err := StoreBlob(r.DB(), badUUID, "", data)
	if err == nil {
		t.Fatal("expected error for bad hash")
	}
}

func TestStoreBlobDuplicate(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("hello world")
	uuid := hash.SHA1(data)

	if err := StoreBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("first store: %v", err)
	}
	if err := StoreBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("duplicate store should succeed: %v", err)
	}
}

func TestLoadBlob(t *testing.T) {
	r := setupSyncTestRepo(t)
	data := []byte("load me")
	uuid := hash.SHA1(data)

	if err := StoreBlob(r.DB(), uuid, "", data); err != nil {
		t.Fatalf("store: %v", err)
	}

	card, size, err := LoadBlob(r.DB(), uuid)
	if err != nil {
		t.Fatalf("LoadBlob: %v", err)
	}
	if card.UUID != uuid {
		t.Fatalf("UUID mismatch: got %s", card.UUID)
	}
	if size != len(data) {
		t.Fatalf("size: got %d, want %d", size, len(data))
	}
	if string(card.Content) != string(data) {
		t.Fatalf("content mismatch")
	}
}

func TestLoadBlobNotFound(t *testing.T) {
	r := setupSyncTestRepo(t)
	_, _, err := LoadBlob(r.DB(), "0000000000000000000000000000000000000000")
	if err == nil {
		t.Fatal("expected error for missing blob")
	}
}

func TestListBlobUUIDs(t *testing.T) {
	r := setupSyncTestRepo(t)
	data1 := []byte("blob one")
	data2 := []byte("blob two")
	uuid1 := hash.SHA1(data1)
	uuid2 := hash.SHA1(data2)

	StoreBlob(r.DB(), uuid1, "", data1)
	StoreBlob(r.DB(), uuid2, "", data2)

	uuids, err := ListBlobUUIDs(r.DB())
	if err != nil {
		t.Fatalf("ListBlobUUIDs: %v", err)
	}
	found := map[string]bool{}
	for _, u := range uuids {
		found[u] = true
	}
	if !found[uuid1] || !found[uuid2] {
		t.Fatalf("missing blobs: found=%v", found)
	}
}

func TestListBlobsFromRID(t *testing.T) {
	r := setupSyncTestRepo(t)
	for i := 0; i < 5; i++ {
		data := []byte(fmt.Sprintf("clone blob %d", i))
		uuid := hash.SHA1(data)
		StoreBlob(r.DB(), uuid, "", data)
	}

	// Page 1: limit 3
	cards, lastRID, more, err := ListBlobsFromRID(r.DB(), 0, 3)
	if err != nil {
		t.Fatalf("page 1: %v", err)
	}
	if len(cards) != 3 {
		t.Fatalf("page 1: got %d cards, want 3", len(cards))
	}
	if !more {
		t.Fatal("page 1: expected more=true")
	}
	if lastRID <= 0 {
		t.Fatalf("page 1: bad lastRID %d", lastRID)
	}

	// Page 2: from lastRID
	cards2, _, more2, err := ListBlobsFromRID(r.DB(), lastRID, 100)
	if err != nil {
		t.Fatalf("page 2: %v", err)
	}
	if len(cards2) < 2 {
		t.Fatalf("page 2: got %d cards, want >= 2", len(cards2))
	}
	if more2 {
		t.Fatal("page 2: expected more=false")
	}
}
