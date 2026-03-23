# Checkout Package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `go-libfossil/checkout/` — a platform-agnostic checkout/working directory library ported from libfossil's 43-function API.

**Architecture:** All methods on `*Checkout` struct. Filesystem via `simio.Env` (no build tags). Observer interface for OTel. `Commit()` delegates to `manifest.Checkin()`. Fossil-compatible vfile/vmerge/vvar schema.

**Tech Stack:** Go, SQLite (stdlib `database/sql`), `simio.Storage`, `simio.Clock`

**Spec:** `docs/superpowers/specs/2026-03-23-checkout-package-design.md`

**Key implementation notes:**
- **SQLite driver name**: Use `db.RegisteredDriver().Name` — NOT hard-coded `"sqlite3"`. The default driver (modernc) registers as `"sqlite"`, not `"sqlite3"`. Tests import `_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"` to register the driver.
- **context.Context**: Observer methods require `context.Context`. Extract, ScanChanges, and Commit should accept `context.Context` as first parameter (or use `context.Background()` internally). Thread ctx through observer calls.
- **MemStorage.ReadDir sort order**: `os.ReadDir` returns entries sorted by name. `MemStorage.ReadDir` must sort entries by name before returning for deterministic tests.
- **Staging queue is in-memory**: `checkinQueue` is a `map[string]bool` on `*Checkout`. Lost on Close/reopen. This matches libfossil's behavior (staging is session-scoped). Fossil uses vfile.chnged flags for persistence, but the queue of *which* changed files to include in the commit is transient.
- **walkDir helper**: Implement as private helper in `vfile.go` (first needed by ScanChanges in Task 6), not deferred to Task 13.

---

## File Structure

### New files

| File | Responsibility |
|------|---------------|
| `go-libfossil/checkout/types.go` | Enums (VFileChange, FileChange, UpdateChange, RevertChange, ScanFlags, ManifestFlags), option structs, ChangeEntry, ChangeVisitor |
| `go-libfossil/checkout/observer.go` | Observer interface, event structs, nopObserver, resolveObserver |
| `go-libfossil/checkout/schema.go` | DDL constants, EnsureTables(), vvar helpers (getVVar, setVVar) |
| `go-libfossil/checkout/checkout.go` | Checkout struct, Open, Create, Close, Dir, Repo, Version, ValidateFingerprint, FindCheckoutDB, PreferredDBName, DBNames |
| `go-libfossil/checkout/vfile.go` | LoadVFile, UnloadVFile, ScanChanges |
| `go-libfossil/checkout/status.go` | HasChanges, VisitChanges |
| `go-libfossil/checkout/manage.go` | Manage, Unmanage |
| `go-libfossil/checkout/extract.go` | Extract (write files to disk from checkin) |
| `go-libfossil/checkout/checkin.go` | Enqueue, Dequeue, IsEnqueued, DiscardQueue, Commit |
| `go-libfossil/checkout/revert.go` | Revert |
| `go-libfossil/checkout/rename.go` | Rename, RevertRename |
| `go-libfossil/checkout/update.go` | Update, CalcUpdateVersion |
| `go-libfossil/checkout/util.go` | CheckFilename, IsRootedIn, FileContent, WriteManifest, walkDir helper |

### Modified files

| File | Change |
|------|--------|
| `go-libfossil/simio/storage.go` | Add `ReadDir(path string) ([]fs.DirEntry, error)` to Storage interface |
| `go-libfossil/simio/storage_mem.go` | Implement `MemStorage.ReadDir` |

### Test files

| File | Tests |
|------|-------|
| `go-libfossil/simio/storage_test.go` | ReadDir tests for OSStorage and MemStorage |
| `go-libfossil/checkout/schema_test.go` | EnsureTables idempotency |
| `go-libfossil/checkout/checkout_test.go` | Create, Open, Close, Version round-trip |
| `go-libfossil/checkout/vfile_test.go` | LoadVFile, UnloadVFile, ScanChanges |
| `go-libfossil/checkout/status_test.go` | HasChanges, VisitChanges |
| `go-libfossil/checkout/manage_test.go` | Manage, Unmanage |
| `go-libfossil/checkout/extract_test.go` | Extract files to MemStorage |
| `go-libfossil/checkout/checkin_test.go` | Enqueue, Dequeue, Commit round-trip |
| `go-libfossil/checkout/revert_test.go` | Revert changes |
| `go-libfossil/checkout/rename_test.go` | Rename, RevertRename |
| `go-libfossil/checkout/update_test.go` | Update with merge |
| `go-libfossil/checkout/observer_test.go` | Recording observer verifies hooks fire |

---

## Task 1: Extend simio.Storage with ReadDir

**Files:**
- Modify: `go-libfossil/simio/storage.go`
- Modify: `go-libfossil/simio/storage_mem.go`
- Modify: `go-libfossil/simio/storage_test.go`

- [ ] **Step 1: Write failing test for OSStorage.ReadDir**

In `go-libfossil/simio/storage_test.go`:

```go
func TestOSStorageReadDir(t *testing.T) {
	dir := t.TempDir()
	s := OSStorage{}
	os.WriteFile(filepath.Join(dir, "a.txt"), []byte("hello"), 0644)
	os.MkdirAll(filepath.Join(dir, "subdir"), 0755)

	entries, err := s.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 2 {
		t.Fatalf("got %d entries, want 2", len(entries))
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./simio/ -run TestOSStorageReadDir -v`
Expected: FAIL — `OSStorage` does not implement `ReadDir`

- [ ] **Step 3: Add ReadDir to Storage interface and OSStorage**

In `go-libfossil/simio/storage.go`, add `io/fs` import and extend:

```go
import (
	"io/fs"
	"os"
)

type Storage interface {
	Stat(path string) (os.FileInfo, error)
	Remove(path string) error
	MkdirAll(path string, perm os.FileMode) error
	ReadFile(path string) ([]byte, error)
	WriteFile(path string, data []byte, perm os.FileMode) error
	ReadDir(path string) ([]fs.DirEntry, error)
}

func (OSStorage) ReadDir(path string) ([]fs.DirEntry, error) { return os.ReadDir(path) }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false ./simio/ -run TestOSStorageReadDir -v`
Expected: PASS

- [ ] **Step 5: Write failing test for MemStorage.ReadDir**

```go
func TestMemStorageReadDir(t *testing.T) {
	m := NewMemStorage()
	m.MkdirAll("/checkout/src", 0755)
	m.WriteFile("/checkout/src/main.go", []byte("package main"), 0644)
	m.WriteFile("/checkout/src/util.go", []byte("package main"), 0644)
	m.WriteFile("/checkout/README.md", []byte("# hi"), 0644)

	entries, err := m.ReadDir("/checkout")
	if err != nil {
		t.Fatal(err)
	}
	// Should see: README.md (file) + src (dir)
	if len(entries) != 2 {
		t.Fatalf("got %d entries, want 2: %v", len(entries), entries)
	}

	entries2, err := m.ReadDir("/checkout/src")
	if err != nil {
		t.Fatal(err)
	}
	if len(entries2) != 2 {
		t.Fatalf("got %d entries in src/, want 2", len(entries2))
	}
}
```

- [ ] **Step 6: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./simio/ -run TestMemStorageReadDir -v`
Expected: FAIL — `MemStorage` does not implement `ReadDir`

- [ ] **Step 7: Implement MemStorage.ReadDir**

In `go-libfossil/simio/storage_mem.go`:

```go
import (
	"io/fs"
	"sort"
)

func (m *MemStorage) ReadDir(path string) ([]fs.DirEntry, error) {
	if path == "" {
		panic("MemStorage.ReadDir: path must not be empty")
	}
	prefix := path + string(filepath.Separator)
	seen := make(map[string]bool)
	var entries []fs.DirEntry

	// Scan files for direct children
	for p, data := range m.files {
		if !strings.HasPrefix(p, prefix) {
			continue
		}
		rel := p[len(prefix):]
		if idx := strings.IndexByte(rel, filepath.Separator); idx >= 0 {
			// Subdirectory — emit dir entry for first component
			dirName := rel[:idx]
			if !seen[dirName] {
				seen[dirName] = true
				entries = append(entries, &memDirEntry{name: dirName, isDir: true})
			}
		} else {
			// Direct child file
			if !seen[rel] {
				seen[rel] = true
				entries = append(entries, &memDirEntry{name: rel, isDir: false, size: int64(len(data))})
			}
		}
	}

	// Also scan dirs map for empty subdirectories
	for d := range m.dirs {
		if !strings.HasPrefix(d, prefix) {
			continue
		}
		rel := d[len(prefix):]
		if rel == "" {
			continue
		}
		dirName := rel
		if idx := strings.IndexByte(rel, filepath.Separator); idx >= 0 {
			dirName = rel[:idx]
		}
		if !seen[dirName] {
			seen[dirName] = true
			entries = append(entries, &memDirEntry{name: dirName, isDir: true})
		}
	}

	// Sort by name for deterministic output (matching os.ReadDir behavior).
	sort.Slice(entries, func(i, j int) bool { return entries[i].Name() < entries[j].Name() })

	if len(entries) == 0 && !m.dirs[path] {
		// Check if any files exist under this path
		hasPrefix := false
		for p := range m.files {
			if strings.HasPrefix(p, prefix) {
				hasPrefix = true
				break
			}
		}
		if !hasPrefix {
			return nil, fmt.Errorf("readdir %s: %w", path, os.ErrNotExist)
		}
	}

	return entries, nil
}

type memDirEntry struct {
	name  string
	isDir bool
	size  int64
}

func (e *memDirEntry) Name() string               { return e.name }
func (e *memDirEntry) IsDir() bool                 { return e.isDir }
func (e *memDirEntry) Type() fs.FileMode {
	if e.isDir {
		return fs.ModeDir
	}
	return 0
}
func (e *memDirEntry) Info() (fs.FileInfo, error) {
	return &memFileInfo{name: e.name, size: e.size, isDir: e.isDir}, nil
}
```

- [ ] **Step 8: Run all simio tests**

Run: `cd go-libfossil && go test -buildvcs=false ./simio/ -v`
Expected: All pass

- [ ] **Step 9: Run full test suite to verify no breakage**

Run: `cd go-libfossil && go test -buildvcs=false ./...`
Expected: All pass (MemStorage and OSStorage both satisfy the updated interface)

- [ ] **Step 10: Commit**

```bash
git add go-libfossil/simio/
git commit -m "feat(simio): add ReadDir to Storage interface

Prerequisite for checkout package. OSStorage delegates to os.ReadDir,
MemStorage synthesizes entries from in-memory file/dir maps.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Types, Observer, and Schema

**Files:**
- Create: `go-libfossil/checkout/types.go`
- Create: `go-libfossil/checkout/observer.go`
- Create: `go-libfossil/checkout/schema.go`
- Create: `go-libfossil/checkout/schema_test.go`

- [ ] **Step 1: Create types.go with all enums and option structs**

Copy all enums and option structs from the spec's "Types & Enums" section verbatim into `go-libfossil/checkout/types.go`. Include package declaration, imports for `time`, `libfossil`, `simio`.

- [ ] **Step 2: Create observer.go with Observer interface + nopObserver**

Copy from spec's "Observer Interface" section. Follow `sync/observer.go` pattern exactly — 8 methods, nopObserver struct, resolveObserver helper. Include all event structs (ExtractStart, ExtractEnd, ScanEnd, CommitStart, CommitEnd).

- [ ] **Step 3: Create schema.go with DDL + EnsureTables + vvar helpers**

```go
package checkout

import "database/sql"

const schemaVFile = `CREATE TABLE IF NOT EXISTS vfile(
  id       INTEGER PRIMARY KEY,
  vid      INTEGER NOT NULL,
  chnged   INTEGER DEFAULT 0,
  deleted  INTEGER DEFAULT 0,
  isexe    INTEGER DEFAULT 0,
  islink   INTEGER DEFAULT 0,
  rid      INTEGER DEFAULT 0,
  mrid     INTEGER DEFAULT 0,
  mtime    INTEGER DEFAULT 0,
  pathname TEXT NOT NULL,
  origname TEXT,
  mhash    TEXT,
  UNIQUE(pathname, vid)
)`

const schemaVMerge = `CREATE TABLE IF NOT EXISTS vmerge(
  id    INTEGER REFERENCES vfile,
  merge INTEGER,
  mhash TEXT
)`

const schemaVVar = `CREATE TABLE IF NOT EXISTS vvar(
  name  TEXT PRIMARY KEY,
  value TEXT
)`

func EnsureTables(db *sql.DB) error {
	if db == nil {
		panic("checkout.EnsureTables: db must not be nil")
	}
	for _, ddl := range []string{schemaVFile, schemaVMerge, schemaVVar} {
		if _, err := db.Exec(ddl); err != nil {
			return err
		}
	}
	return nil
}

func getVVar(db *sql.DB, name string) (string, error) {
	var val string
	err := db.QueryRow("SELECT value FROM vvar WHERE name=?", name).Scan(&val)
	return val, err
}

func setVVar(db *sql.DB, name, value string) error {
	_, err := db.Exec("REPLACE INTO vvar(name, value) VALUES(?, ?)", name, value)
	return err
}
```

- [ ] **Step 4: Write schema_test.go**

```go
package checkout

import (
	"database/sql"
	"testing"

	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func openTestCkoutDB(t *testing.T) *sql.DB {
	t.Helper()
	drv := db.RegisteredDriver()
	if drv == nil {
		t.Fatal("no SQLite driver registered")
	}
	ckdb, err := sql.Open(drv.Name, ":memory:")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ckdb.Close() })
	return ckdb
}

func TestEnsureTables(t *testing.T) {
	db := openTestCkoutDB(t)
	if err := EnsureTables(db); err != nil {
		t.Fatal(err)
	}
	// Idempotent
	if err := EnsureTables(db); err != nil {
		t.Fatal("second call failed:", err)
	}
	// Verify tables exist
	for _, table := range []string{"vfile", "vmerge", "vvar"} {
		var name string
		err := db.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name=?", table).Scan(&name)
		if err != nil {
			t.Fatalf("table %s not found: %v", table, err)
		}
	}
}

func TestVVarRoundTrip(t *testing.T) {
	db := openTestCkoutDB(t)
	EnsureTables(db)

	if err := setVVar(db, "checkout", "42"); err != nil {
		t.Fatal(err)
	}
	val, err := getVVar(db, "checkout")
	if err != nil {
		t.Fatal(err)
	}
	if val != "42" {
		t.Fatalf("got %q, want %q", val, "42")
	}
}
```

- [ ] **Step 5: Verify everything compiles and tests pass**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -v`
Expected: PASS (2 tests)

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/checkout/
git commit -m "feat(checkout): add types, observer, and schema

Enums matching Fossil's vfile states, option structs, Observer interface
following sync.Observer pattern, and Fossil-compatible DDL for
vfile/vmerge/vvar tables.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Checkout struct — Create, Open, Close, Version

**Files:**
- Create: `go-libfossil/checkout/checkout.go`
- Create: `go-libfossil/checkout/checkout_test.go`

- [ ] **Step 1: Write failing test for Create + Version round-trip**

```go
func TestCreateAndVersion(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	env := simio.SimEnv(1)
	co, err := Create(r, dir, CreateOpts{Env: env})
	if err != nil {
		t.Fatal(err)
	}
	defer co.Close()

	rid, uuid, err := co.Version()
	if err != nil {
		t.Fatal(err)
	}
	if rid <= 0 {
		t.Fatal("expected positive RID")
	}
	if uuid == "" {
		t.Fatal("expected non-empty UUID")
	}
}
```

Include a `newTestRepoWithCheckin` helper in a shared `helpers_test.go`:

```go
package checkout

import (
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func newTestRepoWithCheckin(t *testing.T) (*repo.Repo, func()) {
	t.Helper()
	dir := t.TempDir()
	path := dir + "/test.fossil"
	r, err := repo.CreateWithEnv(path, "test", simio.RealEnv())
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = manifest.Checkin(r, manifest.CheckinOpts{
		Files: []manifest.File{
			{Name: "hello.txt", Content: []byte("hello world\n")},
			{Name: "src/main.go", Content: []byte("package main\n")},
			{Name: "README.md", Content: []byte("# Test\n")},
		},
		Comment: "initial checkin",
		User:    "test",
		Parent:  0,
		Time:    time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC),
	})
	if err != nil {
		r.Close()
		t.Fatal(err)
	}
	return r, func() { r.Close() }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -run TestCreateAndVersion -v`
Expected: FAIL — `Create` not defined

- [ ] **Step 3: Implement checkout.go**

```go
package checkout

import (
	"database/sql"
	"fmt"
	"path/filepath"
	"runtime"
	"strconv"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
)

type Checkout struct {
	db   *sql.DB
	repo *repo.Repo
	env  *simio.Env
	obs  Observer
	dir  string
}

func Create(r *repo.Repo, dir string, opts CreateOpts) (*Checkout, error) {
	if r == nil {
		panic("checkout.Create: r must not be nil")
	}
	if dir == "" {
		panic("checkout.Create: dir must not be empty")
	}

	env := opts.Env
	if env == nil {
		env = simio.RealEnv()
	}

	drv := db.RegisteredDriver()
	if drv == nil {
		return nil, fmt.Errorf("checkout.Create: no SQLite driver registered")
	}
	dbPath := filepath.Join(dir, PreferredDBName())
	ckdb, err := sql.Open(drv.Name, dbPath)
	if err != nil {
		return nil, fmt.Errorf("checkout.Create: open db: %w", err)
	}

	if err := EnsureTables(ckdb); err != nil {
		ckdb.Close()
		return nil, fmt.Errorf("checkout.Create: schema: %w", err)
	}

	// Find tip checkin for initial version.
	var tipRID int64
	var tipUUID string
	err = r.DB().QueryRow(`
		SELECT l.rid, b.uuid FROM leaf l
		JOIN event e ON e.objid=l.rid
		JOIN blob b ON b.rid=l.rid
		WHERE e.type='ci'
		ORDER BY e.mtime DESC LIMIT 1
	`).Scan(&tipRID, &tipUUID)
	if err != nil {
		ckdb.Close()
		return nil, fmt.Errorf("checkout.Create: find tip: %w", err)
	}

	if err := setVVar(ckdb, "checkout", strconv.FormatInt(tipRID, 10)); err != nil {
		ckdb.Close()
		return nil, fmt.Errorf("checkout.Create: set checkout: %w", err)
	}
	if err := setVVar(ckdb, "checkout-hash", tipUUID); err != nil {
		ckdb.Close()
		return nil, fmt.Errorf("checkout.Create: set hash: %w", err)
	}

	return &Checkout{
		db:   ckdb,
		repo: r,
		env:  env,
		obs:  resolveObserver(opts.Observer),
		dir:  dir,
	}, nil
}

func Open(r *repo.Repo, dir string, opts OpenOpts) (*Checkout, error) {
	if r == nil {
		panic("checkout.Open: r must not be nil")
	}
	if dir == "" {
		panic("checkout.Open: dir must not be empty")
	}

	env := opts.Env
	if env == nil {
		env = simio.RealEnv()
	}

	dbName, err := FindCheckoutDB(env.Storage, dir, opts.SearchParents)
	if err != nil {
		return nil, fmt.Errorf("checkout.Open: %w", err)
	}

	drv := db.RegisteredDriver()
	if drv == nil {
		return nil, fmt.Errorf("checkout.Open: no SQLite driver registered")
	}
	ckdb, openErr := sql.Open(drv.Name, dbName)
	if openErr != nil {
		return nil, fmt.Errorf("checkout.Open: open db: %w", openErr)
	}
	// Validate the DB is usable and has checkout tables.
	if err := ckdb.Ping(); err != nil {
		ckdb.Close()
		return nil, fmt.Errorf("checkout.Open: ping: %w", err)
	}
	// Verify vvar table exists by reading checkout version.
	var checkVal string
	if qErr := ckdb.QueryRow("SELECT value FROM vvar WHERE name='checkout'").Scan(&checkVal); qErr != nil {
		ckdb.Close()
		return nil, fmt.Errorf("checkout.Open: not a valid checkout DB: %w", qErr)
	}

	return &Checkout{
		db:   ckdb,
		repo: r,
		env:  env,
		obs:  resolveObserver(opts.Observer),
		dir:  dir,
	}, nil
}

func (c *Checkout) Close() error {
	if c.db != nil {
		return c.db.Close()
	}
	return nil
}

func (c *Checkout) Dir() string      { return c.dir }
func (c *Checkout) Repo() *repo.Repo { return c.repo }

func (c *Checkout) Version() (libfossil.FslID, string, error) {
	ridStr, err := getVVar(c.db, "checkout")
	if err != nil {
		return 0, "", fmt.Errorf("checkout.Version: %w", err)
	}
	rid, err := strconv.ParseInt(ridStr, 10, 64)
	if err != nil {
		return 0, "", fmt.Errorf("checkout.Version: parse rid: %w", err)
	}
	uuid, err := getVVar(c.db, "checkout-hash")
	if err != nil {
		return 0, "", fmt.Errorf("checkout.Version: %w", err)
	}
	return libfossil.FslID(rid), uuid, nil
}

func (c *Checkout) ValidateFingerprint() error {
	// Verify the checkout-hash matches the blob uuid for checkout rid.
	rid, uuid, err := c.Version()
	if err != nil {
		return err
	}
	var storedUUID string
	err = c.repo.DB().QueryRow("SELECT uuid FROM blob WHERE rid=?", int64(rid)).Scan(&storedUUID)
	if err != nil {
		return fmt.Errorf("checkout.ValidateFingerprint: blob lookup: %w", err)
	}
	if storedUUID != uuid {
		return fmt.Errorf("checkout.ValidateFingerprint: mismatch: vvar=%s blob=%s", uuid, storedUUID)
	}
	return nil
}

func FindCheckoutDB(storage simio.Storage, dir string, searchParents bool) (string, error) {
	if storage == nil {
		panic("checkout.FindCheckoutDB: storage must not be nil")
	}
	for _, name := range DBNames() {
		path := filepath.Join(dir, name)
		if _, err := storage.Stat(path); err == nil {
			return path, nil
		}
	}
	if searchParents {
		parent := filepath.Dir(dir)
		if parent != dir {
			return FindCheckoutDB(storage, parent, true)
		}
	}
	return "", fmt.Errorf("no checkout DB found in %s", dir)
}

func PreferredDBName() string {
	if runtime.GOOS == "windows" {
		return "_FOSSIL_"
	}
	return ".fslckout"
}

func DBNames() []string {
	return []string{".fslckout", "_FOSSIL_"}
}
```

- [ ] **Step 4: Write test for Open round-trip and FindCheckoutDB**

Add to `checkout_test.go`:

```go
func TestOpenRoundTrip(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatal(err)
	}
	rid1, uuid1, _ := co.Version()
	co.Close()

	co2, err := Open(r, dir, OpenOpts{})
	if err != nil {
		t.Fatal(err)
	}
	defer co2.Close()

	rid2, uuid2, _ := co2.Version()
	if rid1 != rid2 || uuid1 != uuid2 {
		t.Fatalf("version mismatch after reopen: %d/%s vs %d/%s", rid1, uuid1, rid2, uuid2)
	}
}

func TestValidateFingerprint(t *testing.T) {
	r, cleanup := newTestRepoWithCheckin(t)
	defer cleanup()

	dir := t.TempDir()
	co, err := Create(r, dir, CreateOpts{})
	if err != nil {
		t.Fatal(err)
	}
	defer co.Close()

	if err := co.ValidateFingerprint(); err != nil {
		t.Fatal("fingerprint should be valid:", err)
	}
}
```

- [ ] **Step 5: Run tests**

Run: `cd go-libfossil && go test -buildvcs=false ./checkout/ -v`
Expected: All pass

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/checkout/
git commit -m "feat(checkout): Create, Open, Close, Version, ValidateFingerprint

Core Checkout struct with lifecycle management. Both Open and Create
take *repo.Repo (caller owns repo lifecycle). FindCheckoutDB searches
for .fslckout/_FOSSIL_ via simio.Storage.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: LoadVFile and UnloadVFile

**Files:**
- Create: `go-libfossil/checkout/vfile.go`
- Create: `go-libfossil/checkout/vfile_test.go`

- [ ] **Step 1: Write failing test for LoadVFile**

Test creates a repo with a checkin containing 3 files, then calls `LoadVFile` and verifies vfile has 3 rows with correct pathnames and RIDs.

- [ ] **Step 2: Run test — FAIL**

- [ ] **Step 3: Implement LoadVFile**

Uses `manifest.ListFiles(r, rid)` to get file entries, inserts vfile rows with `vid`, `pathname`, `rid` (from `blob.Exists`), `mhash` (UUID). If `clear=true`, deletes existing vfile rows for other versions first.

- [ ] **Step 4: Run test — PASS**

- [ ] **Step 5: Write failing test for UnloadVFile**

Load, then unload, verify vfile is empty for that vid.

- [ ] **Step 6: Implement UnloadVFile**

`DELETE FROM vfile WHERE vid=?`

- [ ] **Step 7: Run all checkout tests — PASS**

- [ ] **Step 8: Commit**

```bash
git commit -m "feat(checkout): LoadVFile and UnloadVFile

Populate vfile from manifest, clear by version. Uses manifest.ListFiles
and blob.Exists for RID resolution.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Extract — write files to disk

**Files:**
- Create: `go-libfossil/checkout/extract.go`
- Create: `go-libfossil/checkout/extract_test.go`

- [ ] **Step 1: Write failing test**

Create repo with checkin, create checkout with `simio.MemStorage`, call `Extract(rid, ExtractOpts{})`, verify files exist in MemStorage with correct content. Verify vfile rows match. Verify observer ExtractStarted/ExtractCompleted fire.

- [ ] **Step 2: Run test — FAIL**

- [ ] **Step 3: Implement Extract**

1. Call observer `ExtractStarted`
2. Call `LoadVFile(rid, true)` to populate vfile
3. Query vfile rows, for each: `content.Expand(repo.DB(), fileRid)` → `env.Storage.WriteFile(path, data, perm)`
4. Update vvar `checkout` and `checkout-hash`
5. Call observer `ExtractCompleted`
6. Handle `opts.DryRun` (skip writes), `opts.Force` (overwrite existing), `opts.Callback` per file

- [ ] **Step 4: Run tests — PASS**

- [ ] **Step 5: Test with callback and DryRun**

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(checkout): Extract — materialize files to disk

Writes checkin files to disk via simio.Storage. Populates vfile,
updates vvar checkout version. Supports DryRun, Force, per-file Callback.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: ScanChanges

**Files:**
- Modify: `go-libfossil/checkout/vfile.go`
- Modify: `go-libfossil/checkout/vfile_test.go`

- [ ] **Step 1: Write failing test**

Extract files to MemStorage, modify one file's content, call `ScanChanges(ScanHash)`, verify `vfile.chnged=1` for modified file.

- [ ] **Step 2: Run test — FAIL**

- [ ] **Step 3: Implement ScanChanges**

For each vfile row: read file via Storage, hash content, compare to `mhash`. If different, update `vfile.chnged`. If file missing, mark appropriately. Uses `env.Storage.ReadDir` + recursive walk via helper `walkDir` to detect extra files.

- [ ] **Step 4: Run tests — PASS**

- [ ] **Step 5: Test missing file detection and mtime-only mode**

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(checkout): ScanChanges — detect modified/missing files

Walks checkout dir via Storage.ReadDir, hashes file content, updates
vfile.chnged. Supports ScanHash, ScanSetMTime flags.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: HasChanges and VisitChanges

**Files:**
- Create: `go-libfossil/checkout/status.go`
- Create: `go-libfossil/checkout/status_test.go`

- [ ] **Step 1: Write failing tests**

Test `HasChanges` returns false on clean checkout, true after modifying a file + scan. Test `VisitChanges` calls visitor with correct entries for modified, added (extra), deleted (missing) files.

- [ ] **Step 2: Run tests — FAIL**

- [ ] **Step 3: Implement HasChanges and VisitChanges**

`HasChanges`: `SELECT count(*) FROM vfile WHERE chnged>0 OR deleted>0`
`VisitChanges`: optionally call `ScanChanges`, then query vfile for changed/deleted rows, call visitor per entry.

- [ ] **Step 4: Run tests — PASS**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(checkout): HasChanges and VisitChanges

DB-only quick check and visitor-pattern change iteration.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: Manage and Unmanage

**Files:**
- Create: `go-libfossil/checkout/manage.go`
- Create: `go-libfossil/checkout/manage_test.go`

- [ ] **Step 1: Write failing tests**

Test Manage adds a new file to vfile with `rid=0`, `chnged=1`. Test Unmanage marks file as deleted or removes the vfile row if `rid=0`.

- [ ] **Step 2: Run tests — FAIL**

- [ ] **Step 3: Implement Manage and Unmanage**

Manage: for each path, check if already in vfile. If not, insert with `rid=0, chnged=1`. Hash file via Storage read. Call callback.
Unmanage: for `rid=0` rows, DELETE. For existing rows, set `deleted=1`. Call callback.

- [ ] **Step 4: Run tests — PASS**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(checkout): Manage and Unmanage — add/remove files from tracking

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 9: Enqueue, Dequeue, Commit

**Files:**
- Create: `go-libfossil/checkout/checkin.go`
- Create: `go-libfossil/checkout/checkin_test.go`

- [ ] **Step 1: Write failing test for full cycle**

Create checkout, extract, modify a file in MemStorage, scan, enqueue, commit. Verify new blob in repo, verify parent linkage, verify checkout version updated to new RID.

- [ ] **Step 2: Run test — FAIL**

- [ ] **Step 3: Implement Enqueue/Dequeue/IsEnqueued/DiscardQueue**

Staging uses a `checkinQueue map[string]bool` field on `*Checkout` (in-memory, not persisted — matches libfossil). Enqueue adds paths, Dequeue removes, IsEnqueued checks, DiscardQueue clears.

- [ ] **Step 4: Implement Commit**

1. Call observer `CommitStarted`
2. If queue empty, use all changed files (implicit enqueue-all, matching libfossil)
3. For each enqueued file: read from Storage, build `manifest.File`
4. Call `manifest.Checkin(repo, CheckinOpts{...})` with parent = current version
5. Update vvar `checkout` and `checkout-hash` to new RID/UUID
6. Reload vfile for new version
7. Call observer `CommitCompleted`

- [ ] **Step 5: Run tests — PASS**

- [ ] **Step 6: Test Dequeue and DiscardQueue**

- [ ] **Step 7: Commit**

```bash
git commit -m "feat(checkout): Enqueue, Dequeue, Commit

Staging queue + commit delegates to manifest.Checkin(). Updates
checkout version and reloads vfile after commit.

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 10: Revert

**Files:**
- Create: `go-libfossil/checkout/revert.go`
- Create: `go-libfossil/checkout/revert_test.go`

- [ ] **Step 1: Write failing test**

Modify file, scan, revert, verify file restored to original content via Storage and vfile.chnged reset to 0.

- [ ] **Step 2: Run test — FAIL**

- [ ] **Step 3: Implement Revert**

Empty paths = revert all. For each path: if `rid=0` (newly added), remove from vfile and delete from Storage. If `rid>0`, expand original content from repo, write to Storage, reset `vfile.chnged=0, deleted=0`. Call callback per file.

- [ ] **Step 4: Run tests — PASS**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(checkout): Revert — restore files to checkout version

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 11: Rename and RevertRename

**Files:**
- Create: `go-libfossil/checkout/rename.go`
- Create: `go-libfossil/checkout/rename_test.go`

- [ ] **Step 1: Write failing tests**

Test Rename updates vfile.pathname and sets origname. Test RevertRename restores original name. Test DoFsMove actually moves file in Storage.

- [ ] **Step 2: Run tests — FAIL**

- [ ] **Step 3: Implement Rename and RevertRename**

Rename: verify target doesn't exist in vfile, update pathname, set origname to old name, set chnged=1. If DoFsMove, read old file from Storage, write to new path, remove old.
RevertRename: if origname set, swap pathname back, clear origname, optionally move on disk.

- [ ] **Step 4: Run tests — PASS**

- [ ] **Step 5: Commit**

```bash
git commit -m "feat(checkout): Rename and RevertRename

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 12: Update with merge + CalcUpdateVersion

**Files:**
- Create: `go-libfossil/checkout/update.go`
- Create: `go-libfossil/checkout/update_test.go`

- [ ] **Step 1: Write failing test for CalcUpdateVersion**

Create repo with two checkins (linear), checkout first, verify CalcUpdateVersion returns the second.

- [ ] **Step 2: Implement CalcUpdateVersion**

Query: find latest leaf checkin on the same branch as current checkout. Walk plink/tagxref to find branch tip.

- [ ] **Step 3: Write failing test for Update**

Create two divergent checkins from same parent, checkout one, modify a file, call Update to the other. Verify 3-way merge applied, files on disk correct, vfile updated.

- [ ] **Step 4: Implement Update**

1. Determine target (opts.TargetRID or CalcUpdateVersion)
2. Find common ancestor via `merge.FindCommonAncestor` or `path.Shortest`
3. For each file: compare ancestor vs target vs working. Apply `merge.ThreeWayText` where needed
4. Write merged files to Storage, update vfile
5. Track conflicts, report via observer and callback

- [ ] **Step 5: Run tests — PASS**

- [ ] **Step 6: Commit**

```bash
git commit -m "feat(checkout): Update with 3-way merge + CalcUpdateVersion

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 13: Utilities — FileContent, WriteManifest, CheckFilename, IsRootedIn

**Files:**
- Create: `go-libfossil/checkout/util.go`
- Add tests to existing test files or create `go-libfossil/checkout/util_test.go`

- [ ] **Step 1: Write failing tests**

Test FileContent reads file from Storage. Test WriteManifest writes manifest/manifest.uuid. Test CheckFilename rejects paths outside checkout. Test IsRootedIn.

- [ ] **Step 2: Implement all four**

FileContent: `c.env.Storage.ReadFile(filepath.Join(c.dir, name))`
WriteManifest: expand current version manifest, write to `c.dir/manifest` and/or `c.dir/manifest.uuid`
CheckFilename: clean path, verify within c.dir, return relative
IsRootedIn: `strings.HasPrefix(filepath.Clean(absPath), filepath.Clean(c.dir))`

- [ ] **Step 3: Run tests — PASS**

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(checkout): FileContent, WriteManifest, CheckFilename, IsRootedIn

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 14: Observer integration test

**Files:**
- Create: `go-libfossil/checkout/observer_test.go`

- [ ] **Step 1: Write recording observer**

```go
type recordingObserver struct {
	events []string
}
```

Record each hook call as a string. Verify Extract, Scan, Commit lifecycle hooks fire in correct order.

- [ ] **Step 2: Write test exercising full lifecycle**

Create → Extract → ScanChanges → Commit. Verify observer saw: ExtractStarted, ExtractFileCompleted (per file), ExtractCompleted, ScanStarted, ScanCompleted, CommitStarted, CommitCompleted.

- [ ] **Step 3: Run test — PASS**

- [ ] **Step 4: Commit**

```bash
git commit -m "test(checkout): observer integration test with recording observer

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```

---

## Task 15: TigerStyle audit + full test run

**Files:**
- Modify: all `go-libfossil/checkout/*.go` files

- [ ] **Step 1: Audit all public methods for precondition panics**

Every public method should panic on nil receiver args, empty required strings. Follow patterns from `go-libfossil/stash/stash.go` and `go-libfossil/sync/handler.go`.

- [ ] **Step 2: Run full test suite**

Run: `cd go-libfossil && go test -buildvcs=false ./...`
Expected: All packages pass

- [ ] **Step 3: Run DST**

Run: `make dst`
Expected: All seeds pass

- [ ] **Step 4: Commit**

```bash
git commit -m "style: TigerStyle audit — precondition panics for checkout package

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
```
