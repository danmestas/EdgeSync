# SQLite Driver Adapter Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the SQLite driver configurable via build tags (modernc default, ncruces for WASM, mattn for CGo performance) with zero changes to consumers.

**Architecture:** Build-tag-selected driver files provide `driverName()` and `buildDSN()`. `Open()` delegates to `OpenWith()` which normalizes pragmas and selects the driver. Existing `Open(path)` signature unchanged.

**Tech Stack:** Go 1.23 build tags, `database/sql`, modernc.org/sqlite, github.com/ncruces/go-sqlite3, github.com/mattn/go-sqlite3

**Spec:** `docs/superpowers/specs/2026-03-16-sqlite-driver-adapter-design.md`

---

## File Structure

### New Files

| File | Responsibility |
|------|---------------|
| `go-libfossil/db/driver_modernc.go` | Build tag default, blank import, driverName, buildDSN |
| `go-libfossil/db/driver_ncruces.go` | Build tag `ncruces`, two blank imports, driverName, buildDSN |
| `go-libfossil/db/driver_mattn.go` | Build tag `mattn`, blank import, driverName, buildDSN |
| `go-libfossil/db/config.go` | OpenConfig, OpenWith, defaultPragmas, driverFromEnv |

### Modified Files

| File | Change |
|------|--------|
| `go-libfossil/db/db.go` | Remove modernc import, remove PRAGMA exec, add driver field, Open delegates to OpenWith |

---

## Task 1: Create driver files and config.go

**Files:**
- Create: `go-libfossil/db/driver_modernc.go`
- Create: `go-libfossil/db/driver_ncruces.go`
- Create: `go-libfossil/db/driver_mattn.go`
- Create: `go-libfossil/db/config.go`

- [ ] **Step 1: Create driver_modernc.go**

```go
//go:build !ncruces && !mattn

package db

import (
	"fmt"
	"strings"

	_ "modernc.org/sqlite"
)

func driverName() string { return "sqlite" }

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_pragma=%s(%s)", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
```

- [ ] **Step 2: Create driver_ncruces.go**

```go
//go:build ncruces

package db

import (
	"fmt"
	"strings"

	_ "github.com/ncruces/go-sqlite3/driver"
	_ "github.com/ncruces/go-sqlite3/embed"
)

func driverName() string { return "sqlite3" }

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	// ncruces uses same _pragma=name(value) syntax as modernc.
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_pragma=%s(%s)", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
```

- [ ] **Step 3: Create driver_mattn.go**

```go
//go:build mattn

package db

import (
	"fmt"
	"strings"

	_ "github.com/mattn/go-sqlite3"
)

func driverName() string { return "sqlite3" }

func buildDSN(path string, pragmas map[string]string) string {
	if len(pragmas) == 0 {
		return path
	}
	// mattn uses _name=value syntax (underscore-prefixed pragma names).
	var parts []string
	for k, v := range pragmas {
		parts = append(parts, fmt.Sprintf("_%s=%s", k, v))
	}
	return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
```

- [ ] **Step 4: Create config.go**

```go
package db

import "os"

// OpenConfig allows callers to customize driver selection and pragmas.
type OpenConfig struct {
	Driver  string            // override driver name (empty = build-tag default or env var)
	Pragmas map[string]string // additional/override pragmas (merged with defaults)
}

func defaultPragmas() map[string]string {
	return map[string]string{
		"journal_mode": "WAL",
		"busy_timeout": "5000",
		"foreign_keys": "ON",
	}
}

func driverFromEnv() string {
	if d := os.Getenv("EDGESYNC_SQLITE_DRIVER"); d != "" {
		return d
	}
	return driverName()
}
```

- [ ] **Step 5: Verify it compiles with default tag**

Run: `go build ./go-libfossil/db/`
Expected: Success. Only `driver_modernc.go` is active (default build tags).

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/db/driver_modernc.go go-libfossil/db/driver_ncruces.go go-libfossil/db/driver_mattn.go go-libfossil/db/config.go
git commit -m "db: add build-tag driver files and OpenConfig for modernc, ncruces, mattn"
```

---

## Task 2: Refactor db.go to use driver abstraction

**Files:**
- Modify: `go-libfossil/db/db.go`

- [ ] **Step 1: Read db.go**

Read `go-libfossil/db/db.go` to see the current code.

- [ ] **Step 2: Replace db.go**

Replace the entire file. Key changes:
- Remove `_ "modernc.org/sqlite"` import (moved to driver_modernc.go)
- Remove inline `PRAGMA journal_mode=WAL` exec (moved to DSN via buildDSN)
- Add `driver` field to DB struct
- `Open()` delegates to `OpenWith()`
- Add `Driver()` accessor

```go
package db

import (
	"database/sql"
	"fmt"
)

// DB wraps a SQLite database connection.
type DB struct {
	conn   *sql.DB
	path   string
	driver string
}

// Open opens a SQLite database with the build-tag-selected driver and default pragmas.
func Open(path string) (*DB, error) {
	return OpenWith(path, OpenConfig{})
}

// OpenWith opens a SQLite database with explicit configuration.
func OpenWith(path string, cfg OpenConfig) (*DB, error) {
	driver := cfg.Driver
	if driver == "" {
		driver = driverFromEnv()
	}

	pragmas := defaultPragmas()
	for k, v := range cfg.Pragmas {
		pragmas[k] = v
	}

	dsn := buildDSN(path, pragmas)
	conn, err := sql.Open(driver, dsn)
	if err != nil {
		return nil, fmt.Errorf("db.Open(%s): %w", driver, err)
	}

	return &DB{conn: conn, path: path, driver: driver}, nil
}

func (d *DB) Close() error {
	return d.conn.Close()
}

func (d *DB) Path() string {
	return d.path
}

func (d *DB) Driver() string {
	return d.driver
}

func (d *DB) Exec(query string, args ...any) (sql.Result, error) {
	return d.conn.Exec(query, args...)
}

func (d *DB) QueryRow(query string, args ...any) *sql.Row {
	return d.conn.QueryRow(query, args...)
}

func (d *DB) Query(query string, args ...any) (*sql.Rows, error) {
	return d.conn.Query(query, args...)
}

func (d *DB) SetApplicationID(id int32) error {
	_, err := d.conn.Exec(fmt.Sprintf("PRAGMA application_id=%d", id))
	return err
}

func (d *DB) ApplicationID() (int32, error) {
	var id int32
	err := d.conn.QueryRow("PRAGMA application_id").Scan(&id)
	return id, err
}

type Tx struct {
	tx *sql.Tx
}

func (t *Tx) Exec(query string, args ...any) (sql.Result, error) {
	return t.tx.Exec(query, args...)
}

func (t *Tx) QueryRow(query string, args ...any) *sql.Row {
	return t.tx.QueryRow(query, args...)
}

func (t *Tx) Query(query string, args ...any) (*sql.Rows, error) {
	return t.tx.Query(query, args...)
}

func (d *DB) WithTx(fn func(tx *Tx) error) error {
	sqlTx, err := d.conn.Begin()
	if err != nil {
		return fmt.Errorf("db.WithTx begin: %w", err)
	}
	if err := fn(&Tx{tx: sqlTx}); err != nil {
		sqlTx.Rollback()
		return err
	}
	return sqlTx.Commit()
}
```

- [ ] **Step 3: Verify it compiles**

Run: `go build ./go-libfossil/db/`
Expected: Success.

- [ ] **Step 4: Run all go-libfossil tests**

Run: `go test ./go-libfossil/... -count=1`
Expected: All pass. The `Open(path)` signature is unchanged so all callers compile. Pragmas are now set via DSN instead of exec, but the effect is the same.

- [ ] **Step 5: Run DST and sim tests**

Run: `go test ./dst/ -count=1 && go test ./sim/ -run "TestFaultProxy|TestGenerateSchedule|TestBuggify" -count=1`
Expected: All pass.

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/db/db.go
git commit -m "db: refactor Open() to use driver abstraction with DSN-embedded pragmas"
```

---

## Task 3: Add ncruces and mattn dependencies

**Files:**
- Modify: `go-libfossil/go.mod`

- [ ] **Step 1: Add ncruces dependency**

Run: `cd go-libfossil && go get github.com/ncruces/go-sqlite3@latest github.com/ncruces/go-sqlite3/driver@latest github.com/ncruces/go-sqlite3/embed@latest`

Note: These won't be linked in the default build (build tags exclude them), but `go.mod` needs them for the compiler to resolve imports in the tagged files.

- [ ] **Step 2: Add mattn dependency**

Run: `cd go-libfossil && go get github.com/mattn/go-sqlite3@latest`

- [ ] **Step 3: Tidy**

Run: `cd go-libfossil && go mod tidy`

- [ ] **Step 4: Verify default build still works**

Run: `go build ./go-libfossil/... && go test ./go-libfossil/... -count=1`
Expected: All pass. Only modernc is active.

- [ ] **Step 5: Verify ncruces build**

Run: `go build -tags ncruces ./go-libfossil/...`
Expected: Compiles. If ncruces dependency isn't in go.mod yet, this will fail — fix by running `go get` in step 1.

- [ ] **Step 6: Verify mattn build (requires CGo)**

Run: `CGO_ENABLED=1 go build -tags mattn ./go-libfossil/...`
Expected: Compiles (if C compiler available). Skip if no CGo toolchain — CI will cover it.

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/go.mod go-libfossil/go.sum
git commit -m "go-libfossil: add ncruces and mattn SQLite driver dependencies"
```

---

## Task 4: Driver config test

**Files:**
- Modify: `go-libfossil/db/db_test.go`

- [ ] **Step 1: Read existing db_test.go**

Read `go-libfossil/db/db_test.go` to see existing test patterns.

- [ ] **Step 2: Add driver config test**

Add to `go-libfossil/db/db_test.go`:

```go
func TestDriverConfig(t *testing.T) {
	name := driverName()
	t.Logf("active driver: %s", name)
	if name == "" {
		t.Fatal("driverName() returned empty string")
	}

	dsn := buildDSN("/tmp/test.db", defaultPragmas())
	t.Logf("DSN: %s", dsn)
	if !strings.Contains(dsn, "journal_mode") {
		t.Fatal("DSN missing journal_mode pragma")
	}
	if !strings.Contains(dsn, "busy_timeout") {
		t.Fatal("DSN missing busy_timeout pragma")
	}
	if !strings.Contains(dsn, "foreign_keys") {
		t.Fatal("DSN missing foreign_keys pragma")
	}
}

func TestOpenWithDefaults(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	if d.Driver() == "" {
		t.Fatal("Driver() returned empty string")
	}
	t.Logf("opened with driver: %s", d.Driver())

	// Verify WAL mode is active.
	var mode string
	d.QueryRow("PRAGMA journal_mode").Scan(&mode)
	if mode != "wal" {
		t.Fatalf("journal_mode = %q, want wal", mode)
	}
}

func TestOpenWithCustomPragmas(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := OpenWith(path, OpenConfig{
		Pragmas: map[string]string{
			"cache_size": "-2000",
		},
	})
	if err != nil {
		t.Fatalf("OpenWith: %v", err)
	}
	defer d.Close()

	// Default pragmas should still be applied.
	var mode string
	d.QueryRow("PRAGMA journal_mode").Scan(&mode)
	if mode != "wal" {
		t.Fatalf("journal_mode = %q, want wal", mode)
	}
}
```

You'll need `"strings"` and `"path/filepath"` imports — check what's already imported.

- [ ] **Step 3: Run tests**

Run: `go test ./go-libfossil/db/ -v -count=1`
Expected: All pass including new tests.

- [ ] **Step 4: Run full test suite**

Run: `go test ./go-libfossil/... -count=1`
Expected: All pass.

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/db/db_test.go
git commit -m "db: add driver config and OpenWith tests"
```

---

## Task 5: CI workflow for driver matrix

**Files:**
- Modify: `.github/workflows/dst.yml` (or create new workflow)

- [ ] **Step 1: Check existing CI workflows**

Read `.github/workflows/` to see what exists.

- [ ] **Step 2: Add driver matrix to the test workflow**

Add a job that tests all three drivers. If there's an existing test workflow, add a matrix. Otherwise create `.github/workflows/drivers.yml`:

```yaml
name: Driver Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        driver: [default, ncruces, mattn]
        include:
          - driver: default
            tags: ""
            cgo: "0"
          - driver: ncruces
            tags: "-tags ncruces"
            cgo: "0"
          - driver: mattn
            tags: "-tags mattn"
            cgo: "1"
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Run tests
        env:
          CGO_ENABLED: ${{ matrix.cgo }}
        run: go test ${{ matrix.tags }} ./go-libfossil/... -v -count=1
```

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/drivers.yml
git commit -m "ci: add driver matrix test workflow (modernc, ncruces, mattn)"
```
