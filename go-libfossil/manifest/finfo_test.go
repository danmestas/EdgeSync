package manifest

import (
	"testing"
	"time"

	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func TestFileHistory_Basic(t *testing.T) {
	r := setupTestRepo(t)
	ts := time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC)

	// Commit 1: add hello.txt
	rid1, _, err := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("v1")}},
		Comment: "add hello",
		User:    "alice",
		Time:    ts,
	})
	if err != nil {
		t.Fatal(err)
	}

	// Commit 2: modify hello.txt
	_, _, err = Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("v2")}},
		Comment: "update hello",
		User:    "bob",
		Time:    ts.Add(time.Hour),
		Parent:  rid1,
	})
	if err != nil {
		t.Fatal(err)
	}

	versions, err := FileHistory(r, FileHistoryOpts{Path: "hello.txt"})
	if err != nil {
		t.Fatal(err)
	}

	if len(versions) != 2 {
		t.Fatalf("expected 2 versions, got %d", len(versions))
	}

	// Most recent first
	if versions[0].Action != FileModified {
		t.Errorf("v[0] action = %v, want Modified", versions[0].Action)
	}
	if versions[0].User != "bob" {
		t.Errorf("v[0] user = %q, want bob", versions[0].User)
	}
	if versions[1].Action != FileAdded {
		t.Errorf("v[1] action = %v, want Added", versions[1].Action)
	}
	if versions[1].User != "alice" {
		t.Errorf("v[1] user = %q, want alice", versions[1].User)
	}
}

func TestFileHistory_MultipleFiles(t *testing.T) {
	r := setupTestRepo(t)
	ts := time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC)

	rid1, _, _ := Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("a1")},
			{Name: "b.txt", Content: []byte("b1")},
		},
		Comment: "add both",
		User:    "alice",
		Time:    ts,
	})

	// Only modify a.txt
	Checkin(r, CheckinOpts{
		Files: []File{
			{Name: "a.txt", Content: []byte("a2")},
			{Name: "b.txt", Content: []byte("b1")},
		},
		Comment: "update a only",
		User:    "bob",
		Time:    ts.Add(time.Hour),
		Parent:  rid1,
	})

	histA, _ := FileHistory(r, FileHistoryOpts{Path: "a.txt"})
	histB, _ := FileHistory(r, FileHistoryOpts{Path: "b.txt"})

	if len(histA) != 2 {
		t.Fatalf("a.txt: expected 2 versions, got %d", len(histA))
	}
	// b.txt should only show the initial add — mlink only records changes
	if len(histB) != 1 {
		t.Fatalf("b.txt: expected 1 version (add only, no change in commit 2), got %d", len(histB))
	}
}

func TestFileHistory_Limit(t *testing.T) {
	r := setupTestRepo(t)
	ts := time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC)

	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "f.txt", Content: []byte("v1")}},
		Comment: "c1",
		User:    "u",
		Time:    ts,
	})

	rid2, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "f.txt", Content: []byte("v2")}},
		Comment: "c2",
		User:    "u",
		Time:    ts.Add(time.Hour),
		Parent:  rid1,
	})

	Checkin(r, CheckinOpts{
		Files:   []File{{Name: "f.txt", Content: []byte("v3")}},
		Comment: "c3",
		User:    "u",
		Time:    ts.Add(2 * time.Hour),
		Parent:  rid2,
	})

	versions, _ := FileHistory(r, FileHistoryOpts{Path: "f.txt", Limit: 2})
	if len(versions) != 2 {
		t.Fatalf("expected 2 versions with limit, got %d", len(versions))
	}
}

func TestFileHistory_NotFound(t *testing.T) {
	r := setupTestRepo(t)

	_, err := FileHistory(r, FileHistoryOpts{Path: "nonexistent.txt"})
	if err == nil {
		t.Fatal("expected error for nonexistent file")
	}
}

func TestFileHistory_EmptyPath(t *testing.T) {
	r := setupTestRepo(t)

	_, err := FileHistory(r, FileHistoryOpts{Path: ""})
	if err == nil {
		t.Fatal("expected error for empty path")
	}
}

func TestFileHistory_FileUUID(t *testing.T) {
	r := setupTestRepo(t)
	ts := time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC)

	Checkin(r, CheckinOpts{
		Files:   []File{{Name: "data.txt", Content: []byte("content")}},
		Comment: "add data",
		User:    "u",
		Time:    ts,
	})

	versions, _ := FileHistory(r, FileHistoryOpts{Path: "data.txt"})
	if len(versions) != 1 {
		t.Fatalf("expected 1 version, got %d", len(versions))
	}
	if versions[0].FileUUID == "" {
		t.Error("FileUUID should be set for non-deleted file")
	}
	if versions[0].CheckinUUID == "" {
		t.Error("CheckinUUID should be set")
	}
	if versions[0].FileRID <= 0 {
		t.Error("FileRID should be positive")
	}
}

func TestFileAt_Basic(t *testing.T) {
	r := setupTestRepo(t)
	ts := time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC)

	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("v1")}},
		Comment: "c1",
		User:    "u",
		Time:    ts,
	})

	rid2, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("v2")}},
		Comment: "c2",
		User:    "u",
		Time:    ts.Add(time.Hour),
		Parent:  rid1,
	})

	fid1, ok1 := FileAt(r, rid1, "hello.txt")
	fid2, ok2 := FileAt(r, rid2, "hello.txt")

	if !ok1 || fid1 <= 0 {
		t.Fatal("expected file at commit 1")
	}
	if !ok2 || fid2 <= 0 {
		t.Fatal("expected file at commit 2")
	}
	if fid1 == fid2 {
		t.Fatal("file rids should differ between versions")
	}
}

func TestFileAt_NotFound(t *testing.T) {
	r := setupTestRepo(t)
	ts := time.Date(2024, 1, 15, 10, 0, 0, 0, time.UTC)

	rid1, _, _ := Checkin(r, CheckinOpts{
		Files:   []File{{Name: "hello.txt", Content: []byte("v1")}},
		Comment: "c1",
		User:    "u",
		Time:    ts,
	})

	_, ok := FileAt(r, rid1, "nonexistent.txt")
	if ok {
		t.Fatal("expected false for nonexistent file")
	}
}
