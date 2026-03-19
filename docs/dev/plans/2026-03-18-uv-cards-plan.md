# UV Cards (Unversioned File Sync) Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement bidirectional unversioned file sync (uvfile/uvigot/uvgimme) wire-compatible with Fossil 2.28.

**Architecture:** New `go-libfossil/uv/` package for schema, CRUD, Status(), and ContentHash(). Handler and client extended with UV card dispatch. DST invariants and scenarios validate convergence. Sim tests prove Fossil interop.

**Tech Stack:** Go, SQLite (modernc/ncruces/mattn via build tags), SHA1 (`crypto/sha1`), zlib (`compress/flate`).

**Spec:** `docs/dev/specs/2026-03-18-uv-cards-design.md`

---

## Chunk 1: UV Package — Status Function

### Task 1: Status() unit tests (RED)

**Files:**
- Create: `go-libfossil/uv/status.go`
- Create: `go-libfossil/uv/status_test.go`

- [ ] **Step 1: Create uv package with Status stub**

```go
// go-libfossil/uv/status.go
package uv

// Status compares a local unversioned file against a remote one and returns
// an action code. Exact port of Fossil's unversioned_status().
//
// localHash="" means no local row (returns 0).
// "-" means deletion marker in either position.
//
// Return codes:
//   0 = not present locally (pull)
//   1 = different hash, remote newer or tiebreaker (pull)
//   2 = same hash, remote mtime older (pull mtime only)
//   3 = identical (no action)
//   4 = same hash, remote mtime newer (push mtime only)
//   5 = different hash, local newer or tiebreaker (push)
func Status(localMtime int64, localHash string, remoteMtime int64, remoteHash string) int {
	panic("not implemented")
}
```

- [ ] **Step 2: Write exhaustive table-driven tests**

```go
// go-libfossil/uv/status_test.go
package uv

import "testing"

func TestStatus(t *testing.T) {
	tests := []struct {
		name                     string
		localMtime               int64
		localHash                string
		remoteMtime              int64
		remoteHash               string
		want                     int
	}{
		// 0: no local row
		{"no-local-row", 0, "", 100, "abc123", 0},
		{"no-local-row-remote-deleted", 0, "", 100, "-", 0},

		// 1: pull — remote newer mtime, different hash
		{"remote-newer-diff-hash", 100, "aaa", 200, "bbb", 1},
		// 1: pull — same mtime, local hash < remote hash (tiebreaker)
		{"same-mtime-local-hash-less", 100, "aaa", 100, "bbb", 1},
		// 1: pull — remote deletion newer than local content
		{"remote-deletion-newer", 100, "abc123", 200, "-", 1},

		// 2: same hash, remote mtime older
		{"same-hash-remote-older", 200, "abc123", 100, "abc123", 2},

		// 3: identical
		{"identical", 100, "abc123", 100, "abc123", 3},
		{"identical-deletion", 100, "-", 100, "-", 3},

		// 4: same hash, remote mtime newer
		{"same-hash-remote-newer", 100, "abc123", 200, "abc123", 4},

		// 5: push — local newer mtime, different hash
		{"local-newer-diff-hash", 200, "bbb", 100, "aaa", 5},
		// 5: push — same mtime, local hash > remote hash (tiebreaker)
		{"same-mtime-local-hash-greater", 100, "bbb", 100, "aaa", 5},
		// 5: push — same mtime, equal hash impossible (caught by hashCmp==0)
		// 5: push — local deletion newer than remote content
		{"local-deletion-newer", 200, "-", 100, "abc123", 5},
		// 5: push — same mtime, local deletion "-" > remote hash "abc"
		{"same-mtime-local-deletion-tiebreaker", 100, "-", 100, "abc123", 5},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := Status(tt.localMtime, tt.localHash, tt.remoteMtime, tt.remoteHash)
			if got != tt.want {
				t.Errorf("Status(%d, %q, %d, %q) = %d, want %d",
					tt.localMtime, tt.localHash, tt.remoteMtime, tt.remoteHash, got, tt.want)
			}
		})
	}
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd go-libfossil && go test ./uv/ -v -run TestStatus`
Expected: FAIL — "not implemented" panic

- [ ] **Step 4: Commit test skeleton**

```bash
git add go-libfossil/uv/status.go go-libfossil/uv/status_test.go
git commit -m "uv: add Status() stub and exhaustive unit tests (RED)"
```

### Task 2: Status() implementation (GREEN)

**Files:**
- Modify: `go-libfossil/uv/status.go`

- [ ] **Step 1: Implement Status()**

Replace the panic in `go-libfossil/uv/status.go`:

```go
func Status(localMtime int64, localHash string, remoteMtime int64, remoteHash string) int {
	if localHash == "" {
		return 0
	}

	// mtimeCmp: -1 if local older, 0 if equal, +1 if local newer
	var mtimeCmp int
	switch {
	case localMtime < remoteMtime:
		mtimeCmp = -1
	case localMtime > remoteMtime:
		mtimeCmp = 1
	}

	hashCmp := cmpStr(localHash, remoteHash)

	if hashCmp == 0 {
		return 3 + mtimeCmp
	}
	if mtimeCmp < 0 || (mtimeCmp == 0 && hashCmp < 0) {
		return 1
	}
	return 5
}

// cmpStr returns -1, 0, or 1 like C's strcmp.
func cmpStr(a, b string) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	default:
		return 0
	}
}
```

- [ ] **Step 2: Run tests to verify they pass**

Run: `cd go-libfossil && go test ./uv/ -v -run TestStatus`
Expected: PASS — all 14 cases

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/uv/status.go
git commit -m "uv: implement Status() — exact port of Fossil unversioned_status()"
```

---

## Chunk 2: UV Package — Schema, CRUD, ContentHash

### Task 3: Schema and CRUD tests (RED)

**Files:**
- Create: `go-libfossil/uv/uv.go`
- Create: `go-libfossil/uv/uv_test.go`

- [ ] **Step 1: Create uv.go with types and stubs**

```go
// go-libfossil/uv/uv.go
package uv

import (
	"github.com/dmestas/edgesync/go-libfossil/db"
)

// Entry represents a row in the unversioned table.
type Entry struct {
	Name     string
	MTime    int64  // seconds since 1970
	Hash     string // "" for tombstone (NULL in DB)
	Size     int    // uncompressed size
}

// EnsureSchema creates the unversioned table if it does not exist.
func EnsureSchema(d *db.DB) error {
	panic("not implemented")
}

// Write stores an unversioned file. Hashes content, compresses if beneficial
// (zlib, 80% threshold), and REPLACE INTOs the row. Invalidates uv-hash cache.
func Write(d *db.DB, name string, content []byte, mtime int64) error {
	panic("not implemented")
}

// Delete creates a tombstone for the named file (hash=NULL, sz=0, content=NULL).
// Invalidates uv-hash cache.
func Delete(d *db.DB, name string, mtime int64) error {
	panic("not implemented")
}

// Read returns the decompressed content, mtime, and hash for the named file.
// Returns ("", 0, "", nil) if the file does not exist.
// Returns ("", mtime, "", nil) for tombstones.
func Read(d *db.DB, name string) (content []byte, mtime int64, hash string, err error) {
	panic("not implemented")
}

// List returns all entries in the unversioned table, including tombstones.
func List(d *db.DB) ([]Entry, error) {
	panic("not implemented")
}

// ContentHash computes the SHA1 catalog hash over all non-tombstone entries,
// matching Fossil's unversioned_content_hash(). Always SHA1, even in SHA3 repos.
// Caches result in config table as "uv-hash". Returns the cached value if present.
func ContentHash(d *db.DB) (string, error) {
	panic("not implemented")
}

// InvalidateHash removes the cached uv-hash from the config table.
func InvalidateHash(d *db.DB) error {
	panic("not implemented")
}
```

- [ ] **Step 2: Write CRUD and ContentHash tests**

```go
// go-libfossil/uv/uv_test.go
package uv

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

// openTestDB creates a temporary repo DB with schema for testing.
func openTestDB(t *testing.T) *db.DB {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := db.Open(path)
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	if err := db.CreateRepoSchema(d); err != nil {
		d.Close()
		t.Fatalf("CreateRepoSchema: %v", err)
	}
	if err := EnsureSchema(d); err != nil {
		d.Close()
		t.Fatalf("EnsureSchema: %v", err)
	}
	t.Cleanup(func() {
		d.Close()
		os.Remove(path)
	})
	return d
}

func TestEnsureSchema(t *testing.T) {
	d := openTestDB(t)
	// Calling twice should be idempotent.
	if err := EnsureSchema(d); err != nil {
		t.Fatalf("second EnsureSchema: %v", err)
	}
}

func TestWriteAndRead(t *testing.T) {
	d := openTestDB(t)

	if err := Write(d, "test.txt", []byte("hello world"), 1700000000); err != nil {
		t.Fatalf("Write: %v", err)
	}

	content, mtime, hash, err := Read(d, "test.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(content) != "hello world" {
		t.Errorf("content = %q, want %q", content, "hello world")
	}
	if mtime != 1700000000 {
		t.Errorf("mtime = %d, want %d", mtime, 1700000000)
	}
	if hash == "" {
		t.Error("hash should not be empty")
	}
}

func TestWriteOverwrite(t *testing.T) {
	d := openTestDB(t)
	Write(d, "f.txt", []byte("v1"), 100)
	Write(d, "f.txt", []byte("v2"), 200)

	content, mtime, _, err := Read(d, "f.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(content) != "v2" || mtime != 200 {
		t.Errorf("got content=%q mtime=%d, want v2/200", content, mtime)
	}
}

func TestDeleteAndRead(t *testing.T) {
	d := openTestDB(t)
	Write(d, "test.txt", []byte("hello"), 100)
	if err := Delete(d, "test.txt", 200); err != nil {
		t.Fatalf("Delete: %v", err)
	}

	content, mtime, hash, err := Read(d, "test.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if content != nil {
		t.Errorf("content should be nil for tombstone, got %q", content)
	}
	if mtime != 200 {
		t.Errorf("mtime = %d, want 200", mtime)
	}
	if hash != "" {
		t.Errorf("hash should be empty for tombstone, got %q", hash)
	}
}

func TestReadNonExistent(t *testing.T) {
	d := openTestDB(t)
	content, mtime, hash, err := Read(d, "nope.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if content != nil || mtime != 0 || hash != "" {
		t.Errorf("expected zero values for non-existent file")
	}
}

func TestList(t *testing.T) {
	d := openTestDB(t)
	Write(d, "a.txt", []byte("aaa"), 100)
	Write(d, "b.txt", []byte("bbb"), 200)
	Delete(d, "c.txt", 300)

	entries, err := List(d)
	if err != nil {
		t.Fatalf("List: %v", err)
	}
	if len(entries) != 3 {
		t.Fatalf("len(entries) = %d, want 3", len(entries))
	}
}

func TestContentHashEmpty(t *testing.T) {
	d := openTestDB(t)
	h, err := ContentHash(d)
	if err != nil {
		t.Fatalf("ContentHash: %v", err)
	}
	// SHA1 of empty string
	if h != "da39a3ee5e6b4b0d3255bfef95601890afd80709" {
		t.Errorf("empty hash = %q, want da39a3ee...", h)
	}
}

func TestContentHashDeterministic(t *testing.T) {
	d := openTestDB(t)
	Write(d, "b.txt", []byte("bbb"), 200)
	Write(d, "a.txt", []byte("aaa"), 100)

	h1, _ := ContentHash(d)
	h2, _ := ContentHash(d)
	if h1 != h2 {
		t.Errorf("ContentHash not deterministic: %q != %q", h1, h2)
	}
}

func TestContentHashExcludesTombstones(t *testing.T) {
	d := openTestDB(t)
	Write(d, "a.txt", []byte("aaa"), 100)

	h1, _ := ContentHash(d)

	Delete(d, "b.txt", 200) // tombstone should not affect hash

	h2, _ := ContentHash(d)
	if h1 != h2 {
		t.Errorf("tombstone changed hash: %q != %q", h1, h2)
	}
}

func TestContentHashChangesOnWrite(t *testing.T) {
	d := openTestDB(t)
	Write(d, "a.txt", []byte("v1"), 100)
	h1, _ := ContentHash(d)

	Write(d, "a.txt", []byte("v2"), 200)
	h2, _ := ContentHash(d)

	if h1 == h2 {
		t.Error("hash should change after write")
	}
}

func TestInvalidateHash(t *testing.T) {
	d := openTestDB(t)
	Write(d, "a.txt", []byte("aaa"), 100)
	h1, _ := ContentHash(d) // caches

	// Manually update without going through Write (simulating external change).
	d.Exec("UPDATE unversioned SET mtime=999 WHERE name='a.txt'")
	InvalidateHash(d)

	h2, _ := ContentHash(d)
	if h1 == h2 {
		t.Error("hash should differ after invalidate + mtime change")
	}
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `cd go-libfossil && go test ./uv/ -v -run 'TestEnsureSchema|TestWrite|TestDelete|TestRead|TestList|TestContentHash|TestInvalidateHash'`
Expected: FAIL — "not implemented" panics

- [ ] **Step 4: Commit test skeleton**

```bash
git add go-libfossil/uv/uv.go go-libfossil/uv/uv_test.go
git commit -m "uv: add CRUD and ContentHash stubs with tests (RED)"
```

### Task 4: Schema and CRUD implementation (GREEN)

**Files:**
- Modify: `go-libfossil/uv/uv.go`

- [ ] **Step 1: Implement EnsureSchema, Write, Delete, Read, List**

Replace stubs in `go-libfossil/uv/uv.go`:

```go
package uv

import (
	"bytes"
	"compress/zlib"
	"crypto/sha1"
	"database/sql"
	"encoding/hex"
	"fmt"
	"io"

	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/hash"
)

// Entry represents a row in the unversioned table.
type Entry struct {
	Name  string
	MTime int64  // seconds since 1970
	Hash  string // "" for tombstone (NULL in DB)
	Size  int    // uncompressed size
}

const schemaUV = `CREATE TABLE IF NOT EXISTS unversioned(
  uvid INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT UNIQUE,
  rcvid INTEGER,
  mtime DATETIME,
  hash TEXT,
  sz INTEGER,
  encoding INT,
  content BLOB
);`

func EnsureSchema(d *db.DB) error {
	if d == nil {
		panic("uv.EnsureSchema: d must not be nil")
	}
	_, err := d.Exec(schemaUV)
	return err
}

func Write(d *db.DB, name string, content []byte, mtime int64) error {
	if d == nil {
		panic("uv.Write: d must not be nil")
	}
	if name == "" {
		panic("uv.Write: name must not be empty")
	}
	if content == nil {
		panic("uv.Write: content must not be nil")
	}

	// Detect repo hash policy. Default to SHA1; use SHA3 if project-code is 64-char.
	var projCode string
	d.QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projCode)
	var h string
	if len(projCode) > 40 {
		h = hash.SHA3(content)
	} else {
		h = hash.SHA1(content)
	}
	sz := len(content)

	// Compress and check 80% threshold.
	var compressed bytes.Buffer
	w, err := zlib.NewWriter(&compressed)
	if err != nil {
		return fmt.Errorf("uv.Write: zlib writer: %w", err)
	}
	w.Write(content)
	w.Close()

	var encoding int
	var stored []byte
	if compressed.Len() <= sz*4/5 { // <= 80%
		encoding = 1
		stored = compressed.Bytes()
	} else {
		encoding = 0
		stored = content
	}

	_, err = d.Exec(
		`REPLACE INTO unversioned(name, rcvid, mtime, hash, sz, encoding, content)
		 VALUES(?, 1, ?, ?, ?, ?, ?)`,
		name, mtime, h, sz, encoding, stored,
	)
	if err != nil {
		return fmt.Errorf("uv.Write: %w", err)
	}
	return InvalidateHash(d)
}

func Delete(d *db.DB, name string, mtime int64) error {
	if d == nil {
		panic("uv.Delete: d must not be nil")
	}
	if name == "" {
		panic("uv.Delete: name must not be empty")
	}

	// Check if row exists; if not, insert tombstone.
	var exists int
	d.QueryRow("SELECT count(*) FROM unversioned WHERE name=?", name).Scan(&exists)
	if exists == 0 {
		_, err := d.Exec(
			`INSERT INTO unversioned(name, rcvid, mtime, hash, sz, encoding, content)
			 VALUES(?, 1, ?, NULL, 0, 0, NULL)`, name, mtime,
		)
		if err != nil {
			return fmt.Errorf("uv.Delete insert: %w", err)
		}
	} else {
		_, err := d.Exec(
			`UPDATE unversioned SET rcvid=1, mtime=?, hash=NULL, sz=0, encoding=0, content=NULL
			 WHERE name=?`, mtime, name,
		)
		if err != nil {
			return fmt.Errorf("uv.Delete update: %w", err)
		}
	}
	return InvalidateHash(d)
}

func Read(d *db.DB, name string) ([]byte, int64, string, error) {
	if d == nil {
		panic("uv.Read: d must not be nil")
	}

	var mtime int64
	var hashVal sql.NullString
	var encoding int
	var stored []byte

	err := d.QueryRow(
		"SELECT mtime, hash, encoding, content FROM unversioned WHERE name=?", name,
	).Scan(&mtime, &hashVal, &encoding, &stored)
	if err == sql.ErrNoRows {
		return nil, 0, "", nil
	}
	if err != nil {
		return nil, 0, "", fmt.Errorf("uv.Read: %w", err)
	}

	h := ""
	if hashVal.Valid {
		h = hashVal.String
	}

	// Tombstone: hash is NULL
	if !hashVal.Valid {
		return nil, mtime, "", nil
	}

	// Decompress if needed.
	if encoding == 1 && stored != nil {
		r, err := zlib.NewReader(bytes.NewReader(stored))
		if err != nil {
			return nil, 0, "", fmt.Errorf("uv.Read: zlib open: %w", err)
		}
		defer r.Close()
		data, err := io.ReadAll(r)
		if err != nil {
			return nil, 0, "", fmt.Errorf("uv.Read: zlib read: %w", err)
		}
		return data, mtime, h, nil
	}

	return stored, mtime, h, nil
}

func List(d *db.DB) ([]Entry, error) {
	if d == nil {
		panic("uv.List: d must not be nil")
	}

	rows, err := d.Query("SELECT name, mtime, hash, sz FROM unversioned ORDER BY name")
	if err != nil {
		return nil, fmt.Errorf("uv.List: %w", err)
	}
	defer rows.Close()

	var entries []Entry
	for rows.Next() {
		var e Entry
		var hashVal sql.NullString
		if err := rows.Scan(&e.Name, &e.MTime, &hashVal, &e.Size); err != nil {
			return nil, err
		}
		if hashVal.Valid {
			e.Hash = hashVal.String
		}
		entries = append(entries, e)
	}
	return entries, rows.Err()
}

// ContentHash computes the SHA1 catalog hash matching Fossil's
// unversioned_content_hash(). Always SHA1, even in SHA3 repos.
// Format: "name YYYY-MM-DD HH:MM:SS hash\n" per file, sorted by name.
func ContentHash(d *db.DB) (string, error) {
	if d == nil {
		panic("uv.ContentHash: d must not be nil")
	}

	// Check cache first.
	var cached string
	err := d.QueryRow("SELECT value FROM config WHERE name='uv-hash'").Scan(&cached)
	if err == nil && cached != "" {
		return cached, nil
	}

	// Compute.
	rows, err := d.Query(
		`SELECT name, datetime(mtime,'unixepoch'), hash
		 FROM unversioned WHERE hash IS NOT NULL ORDER BY name`,
	)
	if err != nil {
		return "", fmt.Errorf("uv.ContentHash: query: %w", err)
	}
	defer rows.Close()

	h := sha1.New()
	for rows.Next() {
		var name, dt, fileHash string
		if err := rows.Scan(&name, &dt, &fileHash); err != nil {
			return "", err
		}
		fmt.Fprintf(h, "%s %s %s\n", name, dt, fileHash)
	}
	if err := rows.Err(); err != nil {
		return "", err
	}

	result := hex.EncodeToString(h.Sum(nil))

	// Cache.
	d.Exec(
		"INSERT OR REPLACE INTO config(name, value, mtime) VALUES('uv-hash', ?, strftime('%s','now'))",
		result,
	)

	return result, nil
}

func InvalidateHash(d *db.DB) error {
	if d == nil {
		panic("uv.InvalidateHash: d must not be nil")
	}
	_, err := d.Exec("DELETE FROM config WHERE name='uv-hash'")
	return err
}
```

- [ ] **Step 2: Run all uv tests**

Run: `cd go-libfossil && go test ./uv/ -v`
Expected: PASS — all Status + CRUD + ContentHash tests

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/uv/uv.go
git commit -m "uv: implement schema, CRUD, and ContentHash (GREEN)"
```

---

## Chunk 3: Handler UV Dispatch

### Task 5: Handler UV card tests (RED)

**Files:**
- Create: `go-libfossil/sync/handler_uv_test.go`

- [ ] **Step 1: Write handler UV card tests**

```go
// go-libfossil/sync/handler_uv_test.go
package sync

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/uv"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func newTestRepo(t *testing.T) *repo.Repo {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := repo.Create(path, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("repo.Create: %v", err)
	}
	if err := uv.EnsureSchema(r.DB()); err != nil {
		t.Fatalf("EnsureSchema: %v", err)
	}
	t.Cleanup(func() {
		r.Close()
		os.Remove(path)
	})
	return r
}

func handleReq(t *testing.T, r *repo.Repo, cards ...xfer.Card) *xfer.Message {
	t.Helper()
	req := &xfer.Message{Cards: cards}
	resp, err := HandleSync(context.Background(), r, req)
	if err != nil {
		t.Fatalf("HandleSync: %v", err)
	}
	return resp
}

func TestHandlerPragmaUVHash_Match(t *testing.T) {
	r := newTestRepo(t)
	uv.Write(r.DB(), "test.txt", []byte("hello"), 100)

	h, _ := uv.ContentHash(r.DB())

	// Send matching hash — should get no uvigot back.
	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "uv-hash", Values: []string{h}},
	)

	for _, c := range resp.Cards {
		if _, ok := c.(*xfer.UVIGotCard); ok {
			t.Error("should not send uvigot when hashes match")
		}
	}
}

func TestHandlerPragmaUVHash_Mismatch(t *testing.T) {
	r := newTestRepo(t)
	uv.Write(r.DB(), "a.txt", []byte("aaa"), 100)
	uv.Write(r.DB(), "b.txt", []byte("bbb"), 200)

	// Send wrong hash — should get uvigot for each file + pragma uv-push-ok.
	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.PragmaCard{Name: "uv-hash", Values: []string{"wrong"}},
	)

	uvigots := cardsByType(resp, xfer.CardUVIGot)
	if len(uvigots) != 2 {
		t.Errorf("expected 2 uvigot cards, got %d", len(uvigots))
	}

	pragmas := cardsByType(resp, xfer.CardPragma)
	found := false
	for _, c := range pragmas {
		if c.(*xfer.PragmaCard).Name == "uv-push-ok" {
			found = true
		}
	}
	if !found {
		t.Error("expected pragma uv-push-ok")
	}
}

func TestHandlerUVGimme(t *testing.T) {
	r := newTestRepo(t)
	uv.Write(r.DB(), "doc.txt", []byte("document content"), 100)

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.UVGimmeCard{Name: "doc.txt"},
	)

	uvfiles := cardsByType(resp, xfer.CardUVFile)
	if len(uvfiles) != 1 {
		t.Fatalf("expected 1 uvfile, got %d", len(uvfiles))
	}
	f := uvfiles[0].(*xfer.UVFileCard)
	if f.Name != "doc.txt" || string(f.Content) != "document content" {
		t.Errorf("uvfile = %+v", f)
	}
}

func TestHandlerUVFile_Accepted(t *testing.T) {
	r := newTestRepo(t)

	resp := handleReq(t, r,
		&xfer.PushCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.UVFileCard{
			Name:    "new.txt",
			MTime:   100,
			Hash:    "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d", // sha1("hello")
			Size:    5,
			Flags:   0,
			Content: []byte("hello"),
		},
	)

	// Should not have errors.
	for _, c := range resp.Cards {
		if e, ok := c.(*xfer.ErrorCard); ok {
			t.Errorf("unexpected error: %s", e.Message)
		}
	}

	// Verify stored.
	content, mtime, hash, err := uv.Read(r.DB(), "new.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(content) != "hello" || mtime != 100 || hash == "" {
		t.Errorf("stored = content=%q mtime=%d hash=%q", content, mtime, hash)
	}
}

func TestHandlerUVFile_Rejected_NoPush(t *testing.T) {
	r := newTestRepo(t)

	// No push card — uvfile should be rejected.
	resp := handleReq(t, r,
		&xfer.UVFileCard{
			Name:    "new.txt",
			MTime:   100,
			Hash:    "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d",
			Size:    5,
			Flags:   0,
			Content: []byte("hello"),
		},
	)

	errors := cardsByType(resp, xfer.CardError)
	if len(errors) == 0 {
		t.Error("expected error card for uvfile without push")
	}
}

func TestHandlerUVIGot_ServerPulls(t *testing.T) {
	r := newTestRepo(t)
	// Server has nothing — client announces a file via uvigot.

	resp := handleReq(t, r,
		&xfer.PullCard{ServerCode: "s", ProjectCode: "p"},
		&xfer.UVIGotCard{Name: "client.txt", MTime: 100, Hash: "abc", Size: 10},
	)

	gimmes := cardsByType(resp, xfer.CardUVGimme)
	if len(gimmes) != 1 {
		t.Fatalf("expected 1 uvgimme, got %d", len(gimmes))
	}
	if gimmes[0].(*xfer.UVGimmeCard).Name != "client.txt" {
		t.Errorf("wrong gimme name")
	}
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test ./sync/ -v -run 'TestHandler.*UV'`
Expected: FAIL — UV cards not dispatched by handler

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/sync/handler_uv_test.go
git commit -m "sync: add handler UV card dispatch tests (RED)"
```

### Task 6: Handler UV card implementation (GREEN)

**Files:**
- Modify: `go-libfossil/sync/handler.go`
- Create: `go-libfossil/sync/handler_uv.go`

- [ ] **Step 1: Add UV state to handler struct**

In `go-libfossil/sync/handler.go`, add to the handler struct:

```go
type handler struct {
	repo           *repo.Repo
	buggify        BuggifyChecker
	resp           []xfer.Card
	pushOK         bool
	pullOK         bool
	cloneMode      bool
	cloneSeq       int
	uvCatalogSent  bool // true after sending UV catalog
}
```

- [ ] **Step 2: Dispatch pragma uv-hash in handleControlCard**

In `go-libfossil/sync/handler.go`, update the PragmaCard case:

```go
case *xfer.PragmaCard:
	if c.Name == "uv-hash" && len(c.Values) >= 1 {
		h.handlePragmaUVHash(c.Values[0])
	}
```

- [ ] **Step 3: Dispatch UV data cards in handleDataCard**

In `go-libfossil/sync/handler.go`, add cases:

```go
case *xfer.UVIGotCard:
	return h.handleUVIGot(c)
case *xfer.UVGimmeCard:
	return h.handleUVGimme(c)
case *xfer.UVFileCard:
	return h.handleUVFile(c)
```

- [ ] **Step 4: Create handler_uv.go with UV handlers**

```go
// go-libfossil/sync/handler_uv.go
package sync

import (
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/uv"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func (h *handler) handlePragmaUVHash(clientHash string) {
	if h.uvCatalogSent {
		return
	}
	uv.EnsureSchema(h.repo.DB())

	localHash, err := uv.ContentHash(h.repo.DB())
	if err != nil {
		return // non-fatal
	}
	if localHash == clientHash {
		return // already in sync
	}

	h.uvCatalogSent = true
	h.resp = append(h.resp, &xfer.PragmaCard{Name: "uv-push-ok"})

	entries, err := uv.List(h.repo.DB())
	if err != nil {
		return
	}
	for _, e := range entries {
		hashVal := e.Hash
		if hashVal == "" {
			hashVal = "-"
		}
		h.resp = append(h.resp, &xfer.UVIGotCard{
			Name:  e.Name,
			MTime: e.MTime,
			Hash:  hashVal,
			Size:  e.Size,
		})
	}
}

func (h *handler) handleUVIGot(c *xfer.UVIGotCard) error {
	if c == nil {
		panic("handler.handleUVIGot: c must not be nil")
	}
	uv.EnsureSchema(h.repo.DB())

	// Look up local file.
	_, localMtime, localHash, err := uv.Read(h.repo.DB(), c.Name)
	if err != nil {
		return fmt.Errorf("handler.handleUVIGot: read %q: %w", c.Name, err)
	}

	remoteHash := c.Hash
	if remoteHash == "-" {
		remoteHash = "-"
	}

	status := uv.Status(localMtime, localHash, c.MTime, remoteHash)

	switch {
	case status == 0 || status == 1:
		// We want the client's version.
		h.resp = append(h.resp, &xfer.UVGimmeCard{Name: c.Name})
	case status == 2:
		// Same hash, update mtime.
		h.repo.DB().Exec("UPDATE unversioned SET mtime=? WHERE name=?", c.MTime, c.Name)
		uv.InvalidateHash(h.repo.DB())
	case status == 4 || status == 5:
		// Send our version to client.
		h.sendUVFile(c.Name)
	}
	// status == 3: identical, no action
	return nil
}

func (h *handler) handleUVGimme(c *xfer.UVGimmeCard) error {
	if c == nil {
		panic("handler.handleUVGimme: c must not be nil")
	}
	// BUGGIFY: 5% chance skip sending UV file.
	if h.buggify != nil && h.buggify.Check("handler.handleUVGimme.skip", 0.05) {
		return nil
	}
	h.sendUVFile(c.Name)
	return nil
}

func (h *handler) sendUVFile(name string) {
	content, mtime, fileHash, err := uv.Read(h.repo.DB(), name)
	if err != nil {
		return
	}

	if fileHash == "" {
		// Tombstone.
		h.resp = append(h.resp, &xfer.UVFileCard{
			Name:  name,
			MTime: mtime,
			Hash:  "-",
			Size:  0,
			Flags: 1,
		})
		return
	}

	h.resp = append(h.resp, &xfer.UVFileCard{
		Name:    name,
		MTime:   mtime,
		Hash:    fileHash,
		Size:    len(content),
		Flags:   0,
		Content: content,
	})
}

func (h *handler) handleUVFile(c *xfer.UVFileCard) error {
	if c == nil {
		panic("handler.handleUVFile: c must not be nil")
	}
	if !h.pushOK {
		h.resp = append(h.resp, &xfer.ErrorCard{
			Message: fmt.Sprintf("uvfile %s rejected: no push card", c.Name),
		})
		return nil
	}

	// BUGGIFY: 3% chance drop uvfile.
	if h.buggify != nil && h.buggify.Check("handler.handleUVFile.drop", 0.03) {
		return nil
	}

	uv.EnsureSchema(h.repo.DB())

	// Validate hash if content present.
	if c.Flags&0x0005 == 0 && c.Content != nil {
		computed := hash.SHA1(c.Content)
		if len(c.Hash) > 40 {
			computed = hash.SHA3(c.Content)
		}
		if computed != c.Hash {
			h.resp = append(h.resp, &xfer.ErrorCard{
				Message: fmt.Sprintf("uvfile %s: hash mismatch", c.Name),
			})
			return nil
		}
	}

	// Double-check status.
	_, localMtime, localHash, err := uv.Read(h.repo.DB(), c.Name)
	if err != nil {
		return fmt.Errorf("handler.handleUVFile: read %q: %w", c.Name, err)
	}

	remoteHash := c.Hash
	status := uv.Status(localMtime, localHash, c.MTime, remoteHash)
	// Status >= 2 means local is same-or-newer. On the receiving end of a uvfile,
	// we should only accept if the incoming file is strictly newer (status 0 or 1).
	if status >= 2 {
		return nil
	}

	// Apply.
	if c.Hash == "-" {
		return uv.Delete(h.repo.DB(), c.Name, c.MTime)
	}
	if c.Content != nil {
		return uv.Write(h.repo.DB(), c.Name, c.Content, c.MTime)
	}
	return nil
}
```

- [ ] **Step 5: Add import for uv package in handler.go**

Add `"github.com/dmestas/edgesync/go-libfossil/uv"` to the imports in `handler.go` (only needed if referenced directly; the handler_uv.go file handles its own imports since it's in the same package).

Note: `handler_uv.go` is in package `sync` and accesses `h.repo.DB()` directly. No import of `uv` needed in `handler.go` itself — just the struct field addition and dispatch changes.

- [ ] **Step 6: Run handler UV tests**

Run: `cd go-libfossil && go test ./sync/ -v -run 'TestHandler.*UV'`
Expected: PASS

- [ ] **Step 7: Run all sync tests to check no regressions**

Run: `cd go-libfossil && go test ./sync/ -v`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add go-libfossil/sync/handler.go go-libfossil/sync/handler_uv.go
git commit -m "sync: implement handler UV card dispatch (pragma uv-hash, uvigot, uvgimme, uvfile)"
```

---

## Chunk 4: Client UV Sync

### Task 7: Client UV sync tests (RED)

**Files:**
- Create: `go-libfossil/sync/client_uv_test.go`

- [ ] **Step 1: Write client UV sync integration tests**

These test the full Sync() loop with a MockTransport that exercises UV cards.

```go
// go-libfossil/sync/client_uv_test.go
package sync

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/uv"
)

// newTestRepoPair creates a server and client repo, both with UV schema.
func newTestRepoPair(t *testing.T) (server, client *repo.Repo) {
	t.Helper()
	dir := t.TempDir()

	sPath := filepath.Join(dir, "server.fossil")
	s, err := repo.Create(sPath, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("server repo: %v", err)
	}
	uv.EnsureSchema(s.DB())

	cPath := filepath.Join(dir, "client.fossil")
	c, err := repo.Create(cPath, "test", simio.CryptoRand{})
	if err != nil {
		t.Fatalf("client repo: %v", err)
	}
	uv.EnsureSchema(c.DB())

	t.Cleanup(func() {
		s.Close()
		c.Close()
		os.Remove(sPath)
		os.Remove(cPath)
	})
	return s, c
}

func TestSyncUV_PullFromServer(t *testing.T) {
	serverRepo, clientRepo := newTestRepoPair(t)

	// Server has a UV file.
	uv.Write(serverRepo.DB(), "wiki/page.txt", []byte("hello wiki"), 100)

	// Client syncs with UV enabled. Uses existing MockTransport with Handler wrapper.
	transport := &MockTransport{Handler: func(req *xfer.Message) *xfer.Message {
		resp, _ := HandleSync(context.Background(), serverRepo, req)
		return resp
	}}
	_, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true, Push: true, UV: true,
		ProjectCode: "p", ServerCode: "s",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Client should now have the file.
	content, mtime, hash, err := uv.Read(clientRepo.DB(), "wiki/page.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(content) != "hello wiki" || mtime != 100 || hash == "" {
		t.Errorf("got content=%q mtime=%d hash=%q", content, mtime, hash)
	}
}

func TestSyncUV_PushToServer(t *testing.T) {
	serverRepo, clientRepo := newTestRepoPair(t)

	// Client has a UV file.
	uv.Write(clientRepo.DB(), "data/config.json", []byte(`{"key":"val"}`), 200)

	transport := &MockTransport{Handler: func(req *xfer.Message) *xfer.Message {
		resp, _ := HandleSync(context.Background(), serverRepo, req)
		return resp
	}}
	_, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true, Push: true, UV: true,
		ProjectCode: "p", ServerCode: "s",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Server should now have the file.
	content, _, _, err := uv.Read(serverRepo.DB(), "data/config.json")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(content) != `{"key":"val"}` {
		t.Errorf("server content = %q", content)
	}
}

func TestSyncUV_Bidirectional(t *testing.T) {
	serverRepo, clientRepo := newTestRepoPair(t)

	uv.Write(serverRepo.DB(), "server-file.txt", []byte("from server"), 100)
	uv.Write(clientRepo.DB(), "client-file.txt", []byte("from client"), 200)

	transport := &MockTransport{Handler: func(req *xfer.Message) *xfer.Message {
		resp, _ := HandleSync(context.Background(), serverRepo, req)
		return resp
	}}
	_, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true, Push: true, UV: true,
		ProjectCode: "p", ServerCode: "s",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Both should have both files.
	sc, _, _, _ := uv.Read(serverRepo.DB(), "client-file.txt")
	cc, _, _, _ := uv.Read(clientRepo.DB(), "server-file.txt")
	if string(sc) != "from client" {
		t.Errorf("server missing client file: %q", sc)
	}
	if string(cc) != "from server" {
		t.Errorf("client missing server file: %q", cc)
	}
}

func TestSyncUV_Deletion(t *testing.T) {
	serverRepo, clientRepo := newTestRepoPair(t)

	// Both have the file initially.
	uv.Write(serverRepo.DB(), "old.txt", []byte("data"), 100)
	uv.Write(clientRepo.DB(), "old.txt", []byte("data"), 100)

	// Server deletes it (newer mtime).
	uv.Delete(serverRepo.DB(), "old.txt", 200)

	transport := &MockTransport{Handler: func(req *xfer.Message) *xfer.Message {
		resp, _ := HandleSync(context.Background(), serverRepo, req)
		return resp
	}}
	_, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true, Push: true, UV: true,
		ProjectCode: "p", ServerCode: "s",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Client should have tombstone.
	_, mtime, hash, _ := uv.Read(clientRepo.DB(), "old.txt")
	if hash != "" {
		t.Errorf("expected tombstone, got hash=%q", hash)
	}
	if mtime != 200 {
		t.Errorf("mtime = %d, want 200", mtime)
	}
}

func TestSyncUV_NoUVFlag_SkipsUV(t *testing.T) {
	serverRepo, clientRepo := newTestRepoPair(t)
	uv.Write(serverRepo.DB(), "test.txt", []byte("data"), 100)

	transport := &MockTransport{Handler: func(req *xfer.Message) *xfer.Message {
		resp, _ := HandleSync(context.Background(), serverRepo, req)
		return resp
	}}
	_, err := Sync(context.Background(), clientRepo, transport, SyncOpts{
		Pull: true, Push: true, UV: false, // UV disabled
		ProjectCode: "p", ServerCode: "s",
	})
	if err != nil {
		t.Fatalf("Sync: %v", err)
	}

	// Client should NOT have the UV file.
	content, _, _, _ := uv.Read(clientRepo.DB(), "test.txt")
	if content != nil {
		t.Error("UV file should not sync when UV=false")
	}
}

// Note: uses existing MockTransport from transport.go — no new type needed.
```

Wait — there's already a MockTransport pattern. Let me check if there's one in the sync package already.

Note to implementer: the `sync` package already has `MockTransport` in `transport.go` with a `Handler func(req *xfer.Message) *xfer.Message` field. The tests above use it via a wrapper that delegates to `HandleSync`. The `xfer` import will be needed:

```go
import "github.com/dmestas/edgesync/go-libfossil/xfer"
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd go-libfossil && go test ./sync/ -v -run 'TestSyncUV'`
Expected: FAIL — `UV` field doesn't exist on SyncOpts, or pragma uv-hash not sent

- [ ] **Step 3: Commit**

```bash
git add go-libfossil/sync/client_uv_test.go
git commit -m "sync: add client UV sync integration tests (RED)"
```

### Task 8: Client UV sync implementation (GREEN)

**Files:**
- Modify: `go-libfossil/sync/session.go`
- Modify: `go-libfossil/sync/client.go`

- [ ] **Step 1: Add UV field to SyncOpts**

In `go-libfossil/sync/session.go`, add to SyncOpts:

```go
type SyncOpts struct {
	Push, Pull              bool
	ProjectCode, ServerCode string
	User, Password          string
	MaxSend                 int
	Env                     *simio.Env
	Buggify                 BuggifyChecker
	UV                      bool // sync unversioned files
}
```

- [ ] **Step 2: Add UV state to session struct**

In `go-libfossil/sync/session.go`, add fields to session:

```go
type session struct {
	// ... existing fields ...
	uvHashSent  bool
	uvPushOK    bool
	uvPullOnly  bool
	uvToSend    map[string]bool // name -> true=full content, false=mtime-only
	nUvGimmeSent int
	nUvFileRcvd  int
}
```

- [ ] **Step 3: Initialize uvToSend in newSession**

In `newSession()`, after existing initialization, add:

```go
s := &session{
	// ... existing fields ...
}

// Pre-populate uvToSend with all local non-tombstone UV files.
if opts.UV {
	uv.EnsureSchema(r.DB())
	entries, err := uv.List(r.DB())
	if err == nil {
		s.uvToSend = make(map[string]bool)
		for _, e := range entries {
			if e.Hash != "" { // skip tombstones
				s.uvToSend[e.Name] = true
			}
		}
	}
}

return s
```

- [ ] **Step 4: Add UV cards to buildRequest**

In `go-libfossil/sync/client.go`, in `buildRequest()`, before the login card section (step 7), add:

```go
// UV: pragma uv-hash on first round
if s.opts.UV && !s.uvHashSent {
	uv.EnsureSchema(s.repo.DB())
	h, err := uv.ContentHash(s.repo.DB())
	if err == nil {
		cards = append(cards, &xfer.PragmaCard{
			Name:   "uv-hash",
			Values: []string{h},
		})
		s.uvHashSent = true
	}
}

// UV: send uvfile cards from uvToSend (only after uvPushOK)
if s.opts.UV && s.uvPushOK {
	uvCards, err := s.buildUVFileCards()
	if err != nil {
		return nil, fmt.Errorf("buildRequest uvfile: %w", err)
	}
	cards = append(cards, uvCards...)
}
```

- [ ] **Step 5: Add buildUVFileCards method**

In `go-libfossil/sync/client.go`:

```go
func (s *session) buildUVFileCards() ([]xfer.Card, error) {
	var cards []xfer.Card
	budget := s.maxSend

	for name, fullContent := range s.uvToSend {
		if budget <= 0 {
			break
		}

		content, mtime, fileHash, err := uv.Read(s.repo.DB(), name)
		if err != nil {
			continue
		}

		if fullContent && content != nil {
			cards = append(cards, &xfer.UVFileCard{
				Name:    name,
				MTime:   mtime,
				Hash:    fileHash,
				Size:    len(content),
				Flags:   0,
				Content: content,
			})
			budget -= len(content)
		} else {
			// mtime-only
			cards = append(cards, &xfer.UVFileCard{
				Name:  name,
				MTime: mtime,
				Hash:  fileHash,
				Size:  len(content),
				Flags: 4,
			})
		}
		delete(s.uvToSend, name)
	}
	return cards, nil
}
```

- [ ] **Step 6: Add UV card handling to processResponse**

In `go-libfossil/sync/client.go`, in the `processResponse()` switch, add cases:

```go
case *xfer.PragmaCard:
	if c.Name == "uv-push-ok" {
		s.uvPushOK = true
	} else if c.Name == "uv-pull-only" {
		s.uvPullOnly = true
	}

case *xfer.UVIGotCard:
	if s.opts.UV {
		s.handleUVIGotCard(c)
	}

case *xfer.UVFileCard:
	if s.opts.UV {
		if err := s.handleUVFileCard(c); err != nil {
			return false, err
		}
		s.nUvFileRcvd++
	}

case *xfer.UVGimmeCard:
	if s.opts.UV {
		// Server is requesting a file from us — mark for next round.
		if s.uvToSend == nil {
			s.uvToSend = make(map[string]bool)
		}
		s.uvToSend[c.Name] = true
	}
```

- [ ] **Step 7: Add uvGimmes field to session and handleUVIGotCard method**

Add `uvGimmes map[string]bool` to the session struct (initialized in newSession as `make(map[string]bool)` when `opts.UV`). UV gimme cards are queued here and emitted in `buildRequest`.

In `buildRequest`, add before the login card section:

```go
// UV: gimme cards for requested UV files
if s.opts.UV {
	for name := range s.uvGimmes {
		cards = append(cards, &xfer.UVGimmeCard{Name: name})
		delete(s.uvGimmes, name)
	}
}
```

The handler method:

```go
func (s *session) handleUVIGotCard(c *xfer.UVIGotCard) {
	uv.EnsureSchema(s.repo.DB())

	_, localMtime, localHash, err := uv.Read(s.repo.DB(), c.Name)
	if err != nil {
		return
	}

	remoteHash := c.Hash
	status := uv.Status(localMtime, localHash, c.MTime, remoteHash)

	switch {
	case status == 0 || status == 1:
		if remoteHash != "-" {
			// Request the file.
			s.uvGimmes[c.Name] = true
			s.nUvGimmeSent++
			// Delete local row if exists (no-op for status 0).
			s.repo.DB().Exec("DELETE FROM unversioned WHERE name=?", c.Name)
			uv.InvalidateHash(s.repo.DB())
		} else if status == 1 {
			// Remote deletion is newer — apply tombstone.
			uv.Delete(s.repo.DB(), c.Name, c.MTime)
		}
	case status == 2:
		s.repo.DB().Exec("UPDATE unversioned SET mtime=? WHERE name=?", c.MTime, c.Name)
		uv.InvalidateHash(s.repo.DB())
	case status == 3:
		delete(s.uvToSend, c.Name) // already in sync
	case status == 4:
		if s.uvPullOnly {
			delete(s.uvToSend, c.Name)
		} else {
			s.uvToSend[c.Name] = false // mtime-only
		}
	case status == 5:
		if s.uvPullOnly {
			delete(s.uvToSend, c.Name)
		} else {
			s.uvToSend[c.Name] = true // full content
		}
	}
}
```

- [ ] **Step 8: Add handleUVFileCard method**

```go
func (s *session) handleUVFileCard(c *xfer.UVFileCard) error {
	uv.EnsureSchema(s.repo.DB())

	// Validate hash if content present.
	if c.Flags&0x0005 == 0 && c.Content != nil {
		computed := hash.SHA1(c.Content)
		if len(c.Hash) > 40 {
			computed = hash.SHA3(c.Content)
		}
		if computed != c.Hash {
			return fmt.Errorf("uvfile %s: hash mismatch", c.Name)
		}
	}

	// Double-check status.
	_, localMtime, localHash, err := uv.Read(s.repo.DB(), c.Name)
	if err != nil {
		return err
	}
	status := uv.Status(localMtime, localHash, c.MTime, c.Hash)
	// Only accept if incoming is strictly newer (status 0 or 1).
	if status >= 2 {
		return nil
	}

	if c.Hash == "-" {
		return uv.Delete(s.repo.DB(), c.Name, c.MTime)
	}
	if c.Content != nil {
		return uv.Write(s.repo.DB(), c.Name, c.Content, c.MTime)
	}
	return nil
}
```

- [ ] **Step 9: Update convergence check to account for UV**

In `processResponse()`, update the convergence logic near the end. After existing convergence checks, add:

```go
// UV convergence: still going if we sent gimmes and expect files.
if s.opts.UV && s.nUvGimmeSent > 0 && (s.nUvFileRcvd > 0 || s.result.Rounds < 3) {
	s.nUvGimmeSent = 0
	s.nUvFileRcvd = 0
	return false, nil
}
if s.opts.UV && len(s.uvToSend) > 0 {
	return false, nil
}
if s.opts.UV && len(s.uvGimmes) > 0 {
	return false, nil
}
// Reset UV counters for next round.
s.nUvGimmeSent = 0
s.nUvFileRcvd = 0
```

- [ ] **Step 10: Run client UV tests**

Run: `cd go-libfossil && go test ./sync/ -v -run 'TestSyncUV'`
Expected: PASS

- [ ] **Step 11: Run all sync tests**

Run: `cd go-libfossil && go test ./sync/ -v`
Expected: PASS — no regressions

- [ ] **Step 12: Commit**

```bash
git add go-libfossil/sync/session.go go-libfossil/sync/client.go
git commit -m "sync: implement client UV sync — pragma uv-hash, uvigot/uvgimme/uvfile handling"
```

---

## Chunk 5: DST Invariants and Scenarios

### Task 9: DST UV invariants

**Files:**
- Modify: `dst/invariants.go`

- [ ] **Step 1: Add CheckUVIntegrity invariant**

```go
// CheckUVIntegrity verifies that every non-tombstone entry in the
// unversioned table has a hash matching the SHA1 of its decompressed
// content, and sz matches the content length.
func CheckUVIntegrity(nodeID string, r *repo.Repo) error {
	uv.EnsureSchema(r.DB())
	entries, err := uv.List(r.DB())
	if err != nil {
		return fmt.Errorf("CheckUVIntegrity(%s): list: %w", nodeID, err)
	}
	for _, e := range entries {
		if e.Hash == "" {
			continue // tombstone
		}
		content, _, storedHash, err := uv.Read(r.DB(), e.Name)
		if err != nil {
			return &InvariantError{
				Invariant: "uv-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("read %q: %v", e.Name, err),
			}
		}
		computed := hash.SHA1(content)
		if computed != storedHash {
			return &InvariantError{
				Invariant: "uv-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("file %q: hash mismatch stored=%s computed=%s", e.Name, storedHash, computed),
			}
		}
		if len(content) != e.Size {
			return &InvariantError{
				Invariant: "uv-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("file %q: sz=%d but content len=%d", e.Name, e.Size, len(content)),
			}
		}
	}
	return nil
}
```

- [ ] **Step 2: Add CheckUVConvergence invariant**

```go
// CheckUVConvergence verifies that all repos have identical UV content hashes
// and the same set of entries.
func CheckUVConvergence(master *repo.Repo, leaves map[NodeID]*repo.Repo) error {
	uv.EnsureSchema(master.DB())
	masterHash, err := uv.ContentHash(master.DB())
	if err != nil {
		return fmt.Errorf("CheckUVConvergence: master hash: %w", err)
	}

	for id, leafRepo := range leaves {
		uv.EnsureSchema(leafRepo.DB())
		leafHash, err := uv.ContentHash(leafRepo.DB())
		if err != nil {
			return fmt.Errorf("CheckUVConvergence: leaf %s hash: %w", id, err)
		}
		if leafHash != masterHash {
			return &InvariantError{
				Invariant: "uv-convergence",
				NodeID:    string(id),
				Detail:    fmt.Sprintf("master=%s leaf=%s", masterHash, leafHash),
			}
		}
	}
	return nil
}
```

- [ ] **Step 3: Add UV checks to Simulator.CheckSafety**

In the `CheckSafety` method, add after existing checks:

```go
if err := CheckUVIntegrity(string(id), r); err != nil {
	return err
}
```

- [ ] **Step 4: Add CheckAllUVConverged method to Simulator**

```go
func (s *Simulator) CheckAllUVConverged(master *repo.Repo) error {
	leaves := make(map[NodeID]*repo.Repo, len(s.leaves))
	for id, a := range s.leaves {
		leaves[id] = a.Repo()
	}
	return CheckUVConvergence(master, leaves)
}
```

- [ ] **Step 5: Run existing DST tests to verify no regressions**

Run: `cd dst && go test -v -short`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add dst/invariants.go
git commit -m "dst: add UV integrity and convergence invariants"
```

### Task 10: DST UV scenarios

**Files:**
- Modify: `dst/scenario_test.go`

- [ ] **Step 1: Add TestUVCleanSync scenario**

```go
func TestUVCleanSync(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(10)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "wiki/intro.txt", []byte("Welcome to the wiki"), 100)
	uv.Write(masterRepo.DB(), "wiki/faq.txt", []byte("Frequently asked questions"), 200)
	uv.Write(masterRepo.DB(), "data/config.json", []byte(`{"version":1}`), 300)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	steps := stepsFor(200)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	if sev.DropRate == 0 && !sev.Buggify {
		if err := sim.CheckAllUVConverged(masterRepo); err != nil {
			t.Fatalf("UV Convergence: %v", err)
		}
	}
}
```

Note to implementer: `SimConfig` needs a `UV bool` field. The simulator must pass `UV: true` in the `SyncOpts` it creates for each leaf agent. Check `dst/simulator.go` for where `SyncOpts` is constructed and add the `UV` field there.

- [ ] **Step 2: Add TestUVBidirectional scenario**

```go
func TestUVBidirectional(t *testing.T) {
	sev := parseSeverity()
	seed := seedFor(11)

	masterRepo := createMasterRepo(t)
	uv.EnsureSchema(masterRepo.DB())
	uv.Write(masterRepo.DB(), "wiki/page1.txt", []byte("page one"), 100)

	mf := NewMockFossil(masterRepo)

	sim, err := New(SimConfig{
		Seed:         seed,
		NumLeaves:    2,
		PollInterval: 5 * time.Second,
		TmpDir:       t.TempDir(),
		Upstream:     mf,
		Buggify:      sev.Buggify,
		UV:           true,
	})
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	defer sim.Close()

	// Write UV file directly into first leaf.
	leaf0 := sim.Leaf(sim.LeafIDs()[0])
	uv.EnsureSchema(leaf0.Repo().DB())
	uv.Write(leaf0.Repo().DB(), "wiki/page2.txt", []byte("page two"), 200)

	steps := stepsFor(300)
	if err := sim.Run(steps); err != nil {
		t.Fatalf("Run: %v", err)
	}

	if err := sim.CheckSafety(); err != nil {
		t.Fatalf("Safety: %v", err)
	}

	if sev.DropRate == 0 && !sev.Buggify {
		if err := sim.CheckAllUVConverged(masterRepo); err != nil {
			t.Fatalf("UV Convergence: %v", err)
		}
	}
}
```

- [ ] **Step 3: Add remaining UV scenarios**

Add the following tests, all following the same SimConfig + Run + CheckSafety + CheckAllUVConverged pattern:

**TestUVConflictMtimeWins**: Same filename `conflict.txt` on master (mtime=200, content="master") and leaf (mtime=100, content="leaf"). After sync, master version wins everywhere (newer mtime).

**TestUVConflictHashTiebreaker**: Same filename, same mtime=100. Master has content producing hash "aaa...", leaf has content producing hash "bbb...". The lexically larger hash wins (leaf keeps its version, master pulls from leaf since master's hash < leaf's hash → status 1).

**TestUVDeletion**: Master has file, both sync. Then master deletes (tombstone mtime=200). Re-sync. Assert tombstone propagates to all leaves.

**TestUVDeletionRevival**: Master deletes file (mtime=100). Leaf creates same-named file with mtime=200. Sync. Assert leaf version wins (newer mtime beats tombstone).

**TestUVPartitionHeal**: Create 2 leaves. Set `sim.Network().SetDropRate(1.0)`. Mutate UV files independently on master and each leaf. Then `SetDropRate(0)`. Run more steps. Assert convergence.

**TestUVMtimeOnlyUpdate**: Master and leaf have same file content but different mtimes. After sync, assert mtimes converge without full content retransmit. Verify by checking that the uvfile cards in the exchange use flags=4.

**TestUVContentOmitted**: Create a UV file larger than `DefaultMaxSend`. Sync. Assert convergence takes multiple rounds (content-omitted → retry).

**TestUVCatalogHashSkip**: Master and leaf have identical UV files. Sync. Assert no uvigot cards sent (pragma uv-hash match short-circuits). Check by intercepting exchange or verifying single round.

- [ ] **Step 4: Run DST UV scenarios**

Run: `cd dst && go test -v -run 'TestUV'`
Expected: PASS (or RED initially until simulator is plumbed)

- [ ] **Step 5: Plumb UV through simulator**

If tests fail because `SimConfig.UV` doesn't exist yet, add the field to `SimConfig` and pass it through to `SyncOpts` in the agent construction. The exact location depends on how the simulator creates leaf agents — check `dst/simulator.go`.

- [ ] **Step 6: Run all DST tests**

Run: `cd dst && go test -v -short`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add dst/scenario_test.go dst/simulator.go
git commit -m "dst: add UV sync scenarios (clean sync, bidirectional, conflict, deletion, partition)"
```

---

## Chunk 6: Sim Integration (Real Fossil)

### Task 11: Sim UV tests against real Fossil

**Files:**
- Modify: `sim/serve_test.go`

- [ ] **Step 1: Add TestSimUVSyncPull**

```go
func TestSimUVSyncPull(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()

	// 1. Create a Fossil repo and add UV files via fossil CLI.
	repoPath := filepath.Join(dir, "source.fossil")
	run(t, "fossil", "new", repoPath)

	// Add UV file.
	uvFile := filepath.Join(dir, "wiki.txt")
	os.WriteFile(uvFile, []byte("hello from fossil"), 0644)
	run(t, "fossil", "uv", "add", uvFile, "--as", "wiki.txt", "-R", repoPath)

	// 2. Open repo with EdgeSync and start ServeHTTP.
	srcRepo, err := repo.Open(repoPath)
	if err != nil {
		t.Fatalf("repo.Open: %v", err)
	}
	uv.EnsureSchema(srcRepo.DB())
	defer srcRepo.Close()

	// Verify UV file is readable.
	content, _, _, err := uv.Read(srcRepo.DB(), "wiki.txt")
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(content) != "hello from fossil" {
		t.Fatalf("UV content mismatch: %q", content)
	}
	t.Logf("UV file verified in source repo")
}
```

Note to implementer: The `fossil uv add` command syntax may vary. Check `fossil help uv` on the local install. The test should also verify `ContentHash` matches `fossil uv hash -R repoPath`.

- [ ] **Step 2: Add TestSimUVCatalogHashCompat**

```go
func TestSimUVCatalogHashCompat(t *testing.T) {
	if !hasFossil() {
		t.Skip("fossil binary not found in PATH")
	}
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dir := t.TempDir()
	repoPath := filepath.Join(dir, "test.fossil")
	run(t, "fossil", "new", repoPath)

	// Add UV files.
	for i, content := range []string{"aaa", "bbb", "ccc"} {
		f := filepath.Join(dir, fmt.Sprintf("file%d.txt", i))
		os.WriteFile(f, []byte(content), 0644)
		run(t, "fossil", "uv", "add", f, "--as", fmt.Sprintf("file%d.txt", i), "-R", repoPath)
	}

	// Get Fossil's hash.
	out := runOutput(t, "fossil", "uv", "hash", "-R", repoPath)
	fossilHash := strings.TrimSpace(string(out))

	// Get our hash.
	r, _ := repo.Open(repoPath)
	uv.EnsureSchema(r.DB())
	defer r.Close()
	ourHash, _ := uv.ContentHash(r.DB())

	if ourHash != fossilHash {
		t.Errorf("hash mismatch: ours=%q fossil=%q", ourHash, fossilHash)
	}
	t.Logf("Catalog hash match: %s", ourHash)
}
```

- [ ] **Step 3: Add helper functions if needed**

```go
func run(t *testing.T, name string, args ...string) {
	t.Helper()
	cmd := exec.Command(name, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%s %v failed: %v\n%s", name, args, err, out)
	}
}

func runOutput(t *testing.T, name string, args ...string) string {
	t.Helper()
	cmd := exec.Command(name, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("%s %v failed: %v\n%s", name, args, err, out)
	}
	return string(out)
}
```

- [ ] **Step 4: Add remaining sim tests**

Add the following tests following the same pattern:

**TestSimUVSyncPush**: Create EdgeSync repo with UV files, start ServeHTTP, use `fossil clone` + `fossil sync --uv` to pull, verify `fossil uv list` shows the files.

**TestSimUVSyncBidirectional**: Fossil server has UV file A, EdgeSync repo has UV file B. Sync. Verify both sides have both files.

**TestSimUVDeletion**: Add UV file via `fossil uv add`, sync to EdgeSync. Then `fossil uv rm`, sync again. Verify tombstone propagates. Reverse: EdgeSync deletes, sync, verify `fossil uv list` shows deletion.

**TestSimUVRoundTrip**: Fossil creates files → EdgeSync pulls → EdgeSync modifies content → pushes back → verify with `fossil uv cat`.

- [ ] **Step 5: Run sim UV tests**

Run: `cd sim && go test -v -run 'TestSimUV'`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add sim/serve_test.go
git commit -m "sim: add UV sync integration tests against real Fossil 2.28"
```

### Task 12: Final verification

- [ ] **Step 1: Run full test suite**

```bash
make test
```

Expected: PASS — all existing tests still pass, all new UV tests pass.

- [ ] **Step 2: Run DST with multiple seeds**

```bash
cd dst && go test -v -run 'TestUV' -count=5
```

Expected: PASS across all seeds.

- [ ] **Step 3: Final commit with CLAUDE.md update**

Update the package table in `CLAUDE.md` to include `uv/` package, and update `MEMORY.md` to mark UV cards as implemented.

```bash
git add CLAUDE.md MEMORY.md
git commit -m "docs: add uv/ package to CLAUDE.md, mark UV cards implemented in MEMORY.md"
```
