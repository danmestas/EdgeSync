# go-libfossil Phase A Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the repository fundamentals layer — types, database, hashing, delta codec, blob storage, content expansion, and repo lifecycle — so that Go can create `.fossil` files that `fossil rebuild --verify` accepts.

**Architecture:** Bottom-up package construction with strict TDD. Each package is built and fully tested before its dependents. The `testutil` package provides fossil CLI oracle wrappers used by integration tests throughout. All code lives in `~/projects/EdgeSync/go-libfossil/` as a standalone Go module.

**Tech Stack:** Go 1.23+, `modernc.org/sqlite` (pure Go SQLite with FTS5), `golang.org/x/crypto/sha3`, standard library (`crypto/sha1`, `compress/zlib`, `encoding/hex`)

**Spec:** `docs/superpowers/specs/2026-03-14-go-libfossil-design.md`

---

## Chunk 1: Project Scaffold, Types, Errors, Julian, and testutil

### Task 1: Initialize Go Module

**Files:**
- Create: `go-libfossil/go.mod`
- Create: `go-libfossil/Makefile`

- [ ] **Step 1: Create module directory and go.mod**

```bash
cd ~/projects/EdgeSync
mkdir -p go-libfossil
cd go-libfossil
go mod init github.com/dmestas/edgesync/go-libfossil
```

- [ ] **Step 2: Add modernc sqlite dependency**

```bash
cd ~/projects/EdgeSync/go-libfossil
go get modernc.org/sqlite
```

- [ ] **Step 3: Create Makefile**

Create `go-libfossil/Makefile`:

```makefile
.PHONY: test test-race vet cover bench validate

test:
	go test ./...

test-race:
	go test -race ./...

vet:
	go vet ./...

cover:
	go test -cover ./...

bench:
	go test -bench=. -benchmem ./...

# Full validation protocol (per-generation)
validate: vet test test-race
```

- [ ] **Step 4: Verify module compiles**

```bash
cd ~/projects/EdgeSync/go-libfossil
go build ./...
```

Expected: no errors (empty module builds fine)

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/go.mod go-libfossil/go.sum go-libfossil/Makefile
fossil commit -m "Initialize go-libfossil Go module with modernc sqlite dependency"
```

---

### Task 2: Root Package — Types (types.go)

**Files:**
- Create: `go-libfossil/types.go`
- Create: `go-libfossil/types_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/types_test.go`:

```go
package libfossil

import "testing"

func TestFslIDIsInt64(t *testing.T) {
	var id FslID = -1
	if id != -1 {
		t.Fatal("FslID should support negative values")
	}
	// Verify it can hold values larger than int32 max
	var big FslID = 1<<33
	if big <= 0 {
		t.Fatal("FslID should support values > int32 max")
	}
}

func TestFslSizePhantom(t *testing.T) {
	if PhantomSize != -1 {
		t.Fatalf("PhantomSize = %d, want -1", PhantomSize)
	}
	var s FslSize = PhantomSize
	if s >= 0 {
		t.Fatal("FslSize should be able to represent -1 for phantoms")
	}
}

func TestFossilAppID(t *testing.T) {
	if FossilApplicationID != 252006673 {
		t.Fatalf("FossilApplicationID = %d, want 252006673", FossilApplicationID)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./...
```

Expected: FAIL — `FslID`, `FslSize`, `PhantomSize`, `FossilApplicationID` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/types.go`:

```go
// Package libfossil is a pure-Go reimplementation of the libfossil C library
// for reading and writing Fossil SCM repository files.
package libfossil

// FslID is a signed 64-bit integer for database record IDs.
// C libfossil uses int32 (fsl_id_t) but we widen to int64 because
// SQLite natively returns int64 and it avoids artificial limits.
type FslID int64

// FslSize is a signed 64-bit integer for content sizes.
// C libfossil uses uint64 (fsl_size_t) but we use int64 because
// the blob.size column stores -1 for phantom blobs.
type FslSize int64

const (
	// PhantomSize is the size value stored for phantom blobs
	// (blobs whose UUID is known but content has not arrived).
	PhantomSize FslSize = -1

	// FossilApplicationID is the SQLite application_id that identifies
	// a database as a Fossil repository. Set via PRAGMA application_id.
	FossilApplicationID int32 = 252006673
)
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/types.go go-libfossil/types_test.go
fossil commit -m "Add root types: FslID, FslSize, PhantomSize, FossilApplicationID"
```

---

### Task 3: Root Package — Errors (errors.go)

**Files:**
- Create: `go-libfossil/errors.go`
- Create: `go-libfossil/errors_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/errors_test.go`:

```go
package libfossil

import (
	"errors"
	"testing"
)

func TestFslErrorCode(t *testing.T) {
	err := &FslError{Code: RCNotARepo, Msg: "not a repo"}
	if err.Code != RCNotARepo {
		t.Fatalf("Code = %d, want %d", err.Code, RCNotARepo)
	}
}

func TestFslErrorImplementsError(t *testing.T) {
	var err error = &FslError{Code: RCDB, Msg: "db error"}
	got := err.Error()
	want := "fossil(db): db error"
	if got != want {
		t.Fatalf("Error() = %q, want %q", got, want)
	}
}

func TestFslErrorUnwrap(t *testing.T) {
	inner := errors.New("sqlite: constraint")
	err := &FslError{Code: RCDB, Msg: "insert failed", Cause: inner}
	if !errors.Is(err, inner) {
		t.Fatal("FslError should unwrap to its Cause")
	}
}

func TestRCCodeString(t *testing.T) {
	tests := []struct {
		code RC
		want string
	}{
		{RCOK, "ok"},
		{RCError, "error"},
		{RCOOM, "oom"},
		{RCMisuse, "misuse"},
		{RCRange, "range"},
		{RCAccess, "access"},
		{RCIO, "io"},
		{RCNotFound, "not_found"},
		{RCAlreadyExists, "already_exists"},
		{RCConsistency, "consistency"},
		{RCNotARepo, "not_a_repo"},
		{RCDB, "db"},
		{RCChecksumMismatch, "checksum_mismatch"},
	}
	for _, tt := range tests {
		if got := tt.code.String(); got != tt.want {
			t.Errorf("RC(%d).String() = %q, want %q", tt.code, got, tt.want)
		}
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./...
```

Expected: FAIL — `FslError`, `RC`, `RCOK`, etc. undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/errors.go`:

```go
package libfossil

import "fmt"

// RC represents a Fossil result code, matching the C FSL_RC_* enum.
type RC int

const (
	RCOK               RC = 0
	RCError            RC = 100
	RCNYI              RC = 101
	RCOOM              RC = 102
	RCMisuse           RC = 103
	RCRange            RC = 104
	RCAccess           RC = 105
	RCIO               RC = 106
	RCNotFound         RC = 107
	RCAlreadyExists    RC = 108
	RCConsistency      RC = 109
	RCRepoNeedsRebuild RC = 110
	RCNotARepo         RC = 111
	RCRepoVersion      RC = 112
	RCDB               RC = 113
	RCBreak            RC = 114
	RCStepRow          RC = 115
	RCStepDone         RC = 116
	RCStepError        RC = 117
	RCType             RC = 118
	RCNotACkout        RC = 119
	RCRepoMismatch     RC = 120
	RCChecksumMismatch RC = 121
	RCLocked           RC = 122
	RCConflict         RC = 123
	RCSizeMismatch     RC = 124
	RCPhantom          RC = 125
	RCUnsupported      RC = 126
)

var rcNames = map[RC]string{
	RCOK:               "ok",
	RCError:            "error",
	RCNYI:              "nyi",
	RCOOM:              "oom",
	RCMisuse:           "misuse",
	RCRange:            "range",
	RCAccess:           "access",
	RCIO:               "io",
	RCNotFound:         "not_found",
	RCAlreadyExists:    "already_exists",
	RCConsistency:      "consistency",
	RCRepoNeedsRebuild: "repo_needs_rebuild",
	RCNotARepo:         "not_a_repo",
	RCRepoVersion:      "repo_version",
	RCDB:               "db",
	RCBreak:            "break",
	RCStepRow:          "step_row",
	RCStepDone:         "step_done",
	RCStepError:        "step_error",
	RCType:             "type",
	RCNotACkout:        "not_a_ckout",
	RCRepoMismatch:     "repo_mismatch",
	RCChecksumMismatch: "checksum_mismatch",
	RCLocked:           "locked",
	RCConflict:         "conflict",
	RCSizeMismatch:     "size_mismatch",
	RCPhantom:          "phantom",
	RCUnsupported:      "unsupported",
}

func (rc RC) String() string {
	if s, ok := rcNames[rc]; ok {
		return s
	}
	return fmt.Sprintf("rc_%d", int(rc))
}

// FslError represents an error from the libfossil library.
type FslError struct {
	Code  RC
	Msg   string
	Cause error
}

func (e *FslError) Error() string {
	return fmt.Sprintf("fossil(%s): %s", e.Code, e.Msg)
}

func (e *FslError) Unwrap() error {
	return e.Cause
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/errors.go go-libfossil/errors_test.go
fossil commit -m "Add error types and result codes matching FSL_RC_* enum"
```

---

### Task 4: Root Package — Julian Day Conversion (julian.go)

**Files:**
- Create: `go-libfossil/julian.go`
- Create: `go-libfossil/julian_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/julian_test.go`:

```go
package libfossil

import (
	"math"
	"testing"
	"time"
)

func TestTimeToJulian(t *testing.T) {
	// Unix epoch (1970-01-01T00:00:00Z) = Julian day 2440587.5
	epoch := time.Date(1970, 1, 1, 0, 0, 0, 0, time.UTC)
	got := TimeToJulian(epoch)
	if math.Abs(got-2440587.5) > 0.0001 {
		t.Fatalf("TimeToJulian(epoch) = %f, want 2440587.5", got)
	}
}

func TestJulianToTime(t *testing.T) {
	// Julian day 2440587.5 = Unix epoch
	got := JulianToTime(2440587.5)
	epoch := time.Date(1970, 1, 1, 0, 0, 0, 0, time.UTC)
	diff := got.Sub(epoch)
	if diff < -time.Second || diff > time.Second {
		t.Fatalf("JulianToTime(2440587.5) = %v, want %v (diff=%v)", got, epoch, diff)
	}
}

func TestJulianRoundTrip(t *testing.T) {
	now := time.Now().UTC().Truncate(time.Millisecond)
	julian := TimeToJulian(now)
	back := JulianToTime(julian)
	diff := now.Sub(back)
	if diff < -time.Millisecond || diff > time.Millisecond {
		t.Fatalf("Round-trip failed: %v -> %f -> %v (diff=%v)", now, julian, back, diff)
	}
}

func TestKnownJulianDates(t *testing.T) {
	tests := []struct {
		name   string
		t      time.Time
		julian float64
	}{
		{"J2000", time.Date(2000, 1, 1, 12, 0, 0, 0, time.UTC), 2451545.0},
		{"2026-03-14 noon", time.Date(2026, 3, 14, 12, 0, 0, 0, time.UTC), 2461114.0},
	}
	for _, tt := range tests {
		got := TimeToJulian(tt.t)
		if math.Abs(got-tt.julian) > 0.001 {
			t.Errorf("%s: TimeToJulian = %f, want %f", tt.name, got, tt.julian)
		}
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./...
```

Expected: FAIL — `TimeToJulian`, `JulianToTime` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/julian.go`:

```go
package libfossil

import "time"

// julianEpoch is the Julian day number for the Unix epoch (1970-01-01T00:00:00Z).
const julianEpoch = 2440587.5

// TimeToJulian converts a time.Time to a Julian day number (float64).
// Fossil stores all timestamps as Julian day numbers.
func TimeToJulian(t time.Time) float64 {
	return julianEpoch + float64(t.UTC().UnixMilli())/(86400.0*1000.0)
}

// JulianToTime converts a Julian day number to a time.Time in UTC.
func JulianToTime(j float64) time.Time {
	millis := int64((j - julianEpoch) * 86400.0 * 1000.0)
	return time.UnixMilli(millis).UTC()
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/julian.go go-libfossil/julian_test.go
fossil commit -m "Add Julian day <-> time.Time conversion functions"
```

---

### Task 5: testutil Package — Fossil CLI Oracle

**Files:**
- Create: `go-libfossil/testutil/testutil.go`
- Create: `go-libfossil/testutil/testutil_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/testutil/testutil_test.go`:

```go
package testutil

import (
	"os"
	"testing"
)

func TestNewTestRepo(t *testing.T) {
	tr := NewTestRepo(t)
	// File should exist
	if _, err := os.Stat(tr.Path); err != nil {
		t.Fatalf("repo file does not exist: %v", err)
	}
}

func TestFossilRebuild(t *testing.T) {
	tr := NewTestRepo(t)
	// A fresh repo created by fossil should pass rebuild
	tr.FossilRebuild(t)
}

func TestFossilSQL(t *testing.T) {
	tr := NewTestRepo(t)
	out := tr.FossilSQL(t, "SELECT count(*) FROM blob;")
	if out != "0" {
		t.Fatalf("FossilSQL count(*) = %q, want %q", out, "0")
	}
}

func TestFossilBinary(t *testing.T) {
	path := FossilBinary()
	if path == "" {
		t.Fatal("FossilBinary() returned empty string")
	}
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("fossil binary not found at %q: %v", path, err)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./testutil/...
```

Expected: FAIL — package/functions undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/testutil/testutil.go`:

```go
// Package testutil provides fossil CLI oracle helpers for integration tests.
// It wraps the fossil binary to create test repos and validate that
// Go-produced .fossil files are compatible with the fossil CLI.
package testutil

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// TestRepo wraps a temporary .fossil file for testing.
type TestRepo struct {
	Path string // absolute path to the .fossil file
	Dir  string // temp directory (cleaned up by t.Cleanup)
}

// FossilBinary returns the path to the fossil CLI binary.
// It checks FOSSIL_BIN env var first, then falls back to PATH lookup.
func FossilBinary() string {
	if bin := os.Getenv("FOSSIL_BIN"); bin != "" {
		return bin
	}
	path, err := exec.LookPath("fossil")
	if err != nil {
		return ""
	}
	return path
}

// NewTestRepo creates a new temporary .fossil repository using `fossil new`.
// The repo file and temp directory are cleaned up when the test finishes.
func NewTestRepo(t *testing.T) *TestRepo {
	t.Helper()
	bin := FossilBinary()
	if bin == "" {
		t.Skip("fossil binary not found; skipping integration test")
	}

	dir := t.TempDir()
	path := filepath.Join(dir, "test.fossil")

	cmd := exec.Command(bin, "new", path)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil new failed: %v\n%s", err, out)
	}

	return &TestRepo{Path: path, Dir: dir}
}

// NewTestRepoFromPath wraps an existing .fossil file for validation.
// Does not manage the file's lifecycle — caller is responsible for cleanup.
func NewTestRepoFromPath(t *testing.T, path string) *TestRepo {
	t.Helper()
	abs, err := filepath.Abs(path)
	if err != nil {
		t.Fatalf("cannot resolve path %q: %v", path, err)
	}
	return &TestRepo{Path: abs, Dir: filepath.Dir(abs)}
}

// FossilRebuild runs `fossil rebuild --verify` on the repo.
// Fails the test if the rebuild reports errors.
func (r *TestRepo) FossilRebuild(t *testing.T) {
	t.Helper()
	cmd := exec.Command(FossilBinary(), "rebuild", "--verify", r.Path)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild --verify failed: %v\n%s", err, out)
	}
}

// FossilArtifact retrieves an artifact by UUID using `fossil artifact`.
func (r *TestRepo) FossilArtifact(t *testing.T, uuid string) []byte {
	t.Helper()
	cmd := exec.Command(FossilBinary(), "artifact", uuid, "-R", r.Path)
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("fossil artifact %s failed: %v", uuid, err)
	}
	return out
}

// FossilSQL runs a SQL query against the repo using `fossil sql -R`.
// Returns the trimmed output.
func (r *TestRepo) FossilSQL(t *testing.T, sql string) string {
	t.Helper()
	cmd := exec.Command(FossilBinary(), "sql", "-R", r.Path, sql)
	out, err := cmd.Output()
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok {
			t.Fatalf("fossil sql failed: %v\nstderr: %s", err, exitErr.Stderr)
		}
		t.Fatalf("fossil sql failed: %v", err)
	}
	return strings.TrimSpace(string(out))
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./testutil/...
```

Expected: PASS (or SKIP if fossil binary not available — but it is at `/opt/homebrew/bin/fossil`)

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/testutil/testutil.go go-libfossil/testutil/testutil_test.go
fossil commit -m "Add testutil package with fossil CLI oracle wrappers"
```

- [ ] **Step 6: Run full validation**

```bash
cd ~/projects/EdgeSync/go-libfossil && make validate
```

Expected: all vet, test, and race checks pass

### Task 6: db Package — Open, Close, Exec

**Files:**
- Create: `go-libfossil/db/db.go`
- Create: `go-libfossil/db/db_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/db/db_test.go`:

```go
package db

import (
	"fmt"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestOpenClose(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.db")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	// Should be able to execute a simple query
	var result int
	err = d.QueryRow("SELECT 1+1").Scan(&result)
	if err != nil {
		t.Fatalf("QueryRow: %v", err)
	}
	if result != 2 {
		t.Fatalf("got %d, want 2", result)
	}
}

func TestExec(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.db")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	_, err = d.Exec("CREATE TABLE test(id INTEGER PRIMARY KEY, val TEXT)")
	if err != nil {
		t.Fatalf("Exec CREATE: %v", err)
	}

	_, err = d.Exec("INSERT INTO test(val) VALUES(?)", "hello")
	if err != nil {
		t.Fatalf("Exec INSERT: %v", err)
	}

	var val string
	err = d.QueryRow("SELECT val FROM test WHERE id=1").Scan(&val)
	if err != nil {
		t.Fatalf("QueryRow: %v", err)
	}
	if val != "hello" {
		t.Fatalf("val = %q, want %q", val, "hello")
	}
}

func TestApplicationID(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.db")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	err = d.SetApplicationID(252006673)
	if err != nil {
		t.Fatalf("SetApplicationID: %v", err)
	}

	id, err := d.ApplicationID()
	if err != nil {
		t.Fatalf("ApplicationID: %v", err)
	}
	if id != 252006673 {
		t.Fatalf("application_id = %d, want 252006673", id)
	}
}

func TestTransaction(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.db")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	d.Exec("CREATE TABLE test(id INTEGER PRIMARY KEY, val TEXT)")

	// Commit case
	err = d.WithTx(func(tx *Tx) error {
		_, err := tx.Exec("INSERT INTO test(val) VALUES(?)", "committed")
		return err
	})
	if err != nil {
		t.Fatalf("WithTx commit: %v", err)
	}

	var count int
	d.QueryRow("SELECT count(*) FROM test").Scan(&count)
	if count != 1 {
		t.Fatalf("count after commit = %d, want 1", count)
	}

	// Rollback case
	err = d.WithTx(func(tx *Tx) error {
		tx.Exec("INSERT INTO test(val) VALUES(?)", "rolled-back")
		return fmt.Errorf("deliberate error")
	})
	if err == nil {
		t.Fatal("WithTx should return error")
	}

	d.QueryRow("SELECT count(*) FROM test").Scan(&count)
	if count != 1 {
		t.Fatalf("count after rollback = %d, want 1", count)
	}
}

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./db/...
```

Expected: FAIL — package/functions undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/db/db.go`:

```go
// Package db provides a thin SQLite wrapper for Fossil repository databases.
// It uses modernc.org/sqlite (pure Go, no CGo).
package db

import (
	"database/sql"
	"fmt"

	_ "modernc.org/sqlite"
)

// DB wraps a SQLite database connection for a Fossil repository.
type DB struct {
	conn *sql.DB
	path string
}

// Open opens or creates a SQLite database at the given path.
func Open(path string) (*DB, error) {
	conn, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("db.Open: %w", err)
	}
	// Enable WAL mode for better concurrent read performance
	if _, err := conn.Exec("PRAGMA journal_mode=WAL"); err != nil {
		conn.Close()
		return nil, fmt.Errorf("db.Open WAL: %w", err)
	}
	return &DB{conn: conn, path: path}, nil
}

// Close closes the database connection.
func (d *DB) Close() error {
	return d.conn.Close()
}

// Path returns the filesystem path of this database.
func (d *DB) Path() string {
	return d.path
}

// Exec executes a SQL statement with optional arguments.
func (d *DB) Exec(query string, args ...any) (sql.Result, error) {
	return d.conn.Exec(query, args...)
}

// QueryRow executes a query that returns at most one row.
func (d *DB) QueryRow(query string, args ...any) *sql.Row {
	return d.conn.QueryRow(query, args...)
}

// Query executes a query that returns rows.
func (d *DB) Query(query string, args ...any) (*sql.Rows, error) {
	return d.conn.Query(query, args...)
}

// SetApplicationID sets the SQLite application_id pragma.
func (d *DB) SetApplicationID(id int32) error {
	_, err := d.conn.Exec(fmt.Sprintf("PRAGMA application_id=%d", id))
	return err
}

// ApplicationID reads the current application_id pragma value.
func (d *DB) ApplicationID() (int32, error) {
	var id int32
	err := d.conn.QueryRow("PRAGMA application_id").Scan(&id)
	return id, err
}

// Tx wraps a *sql.Tx so that Exec/QueryRow/Query route through the transaction.
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

// WithTx executes fn inside a transaction. Commits on success, rolls back on error.
// The Tx handle must be used for all operations that should be transactional.
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

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./db/...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/db/db.go go-libfossil/db/db_test.go
fossil commit -m "Add db package: SQLite wrapper with open, exec, transactions, application_id"
```

---

### Task 7: db Package — Repo Schema Creation

**Files:**
- Create: `go-libfossil/db/schema.go`
- Modify: `go-libfossil/db/db_test.go` (add schema tests)

The schema SQL is extracted verbatim from the C source files `schema_repo1_cstr.c` and `schema_repo2_cstr.c`. We embed them as Go string constants. The key difference from the C source: we strip the `repo.` prefix from table names since the schema is applied to the main database (not an attached "repo" database).

- [ ] **Step 1: Write the failing test**

Add to `go-libfossil/db/db_test.go`:

```go
func TestCreateRepoSchema(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer d.Close()

	err = CreateRepoSchema(d)
	if err != nil {
		t.Fatalf("CreateRepoSchema: %v", err)
	}

	// Verify repo1 (static) tables exist
	repo1Tables := []string{"blob", "delta", "rcvfrom", "user", "config", "shun", "private", "reportfmt", "concealed"}
	for _, table := range repo1Tables {
		var name string
		err := d.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name=?", table).Scan(&name)
		if err != nil {
			t.Errorf("repo1 table %q not found: %v", table, err)
		}
	}

	// Verify repo2 (transient) tables exist
	repo2Tables := []string{"filename", "mlink", "plink", "leaf", "event", "phantom", "orphan", "unclustered", "unsent", "tag", "tagxref", "backlink", "attachment", "cherrypick"}
	for _, table := range repo2Tables {
		var name string
		err := d.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name=?", table).Scan(&name)
		if err != nil {
			t.Errorf("repo2 table %q not found: %v", table, err)
		}
	}

	// Verify application_id
	id, err := d.ApplicationID()
	if err != nil {
		t.Fatalf("ApplicationID: %v", err)
	}
	if id != 252006673 {
		t.Fatalf("application_id = %d, want 252006673", id)
	}

	// Verify seed rcvfrom row
	var rcvid int
	err = d.QueryRow("SELECT rcvid FROM rcvfrom WHERE rcvid=1").Scan(&rcvid)
	if err != nil {
		t.Fatalf("seed rcvfrom row missing: %v", err)
	}

	// Verify seed tag rows (1-11)
	var tagCount int
	err = d.QueryRow("SELECT count(*) FROM tag").Scan(&tagCount)
	if err != nil {
		t.Fatalf("tag count: %v", err)
	}
	if tagCount != 11 {
		t.Fatalf("tag count = %d, want 11", tagCount)
	}

	// Verify specific seed tags
	var tagName string
	err = d.QueryRow("SELECT tagname FROM tag WHERE tagid=8").Scan(&tagName)
	if err != nil {
		t.Fatalf("tag 8: %v", err)
	}
	if tagName != "branch" {
		t.Fatalf("tag 8 name = %q, want %q", tagName, "branch")
	}
}

func TestCreateRepoSchema_FossilValidation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping fossil CLI validation in short mode")
	}

	path := filepath.Join(t.TempDir(), "test.fossil")
	d, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}

	err = CreateRepoSchema(d)
	if err != nil {
		t.Fatalf("CreateRepoSchema: %v", err)
	}

	// Must also seed config rows that fossil expects
	err = SeedConfig(d)
	if err != nil {
		t.Fatalf("SeedConfig: %v", err)
	}

	// Seed a user row (rcvfrom references user.uid=1)
	err = SeedUser(d, "testuser")
	if err != nil {
		t.Fatalf("SeedUser: %v", err)
	}

	d.Close()

	// fossil rebuild --verify should pass
	cmd := exec.Command("fossil", "rebuild", "--verify", path)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild --verify failed: %v\n%s", err, out)
	}
}
```

Add `"os/exec"` to imports.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./db/...
```

Expected: FAIL — `CreateRepoSchema`, `SeedConfig`, `SeedUser` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/db/schema.go`:

```go
package db

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
)

// schemaRepo1 is the static repo schema (tables that survive rebuild).
// Sourced from libfossil's schema_repo1_cstr.c, with "repo." prefix stripped.
const schemaRepo1 = `
CREATE TABLE blob(
  rid INTEGER PRIMARY KEY,
  rcvid INTEGER,
  size INTEGER,
  uuid TEXT UNIQUE NOT NULL,
  content BLOB,
  CHECK( length(uuid)>=40 AND rid>0 )
);
CREATE TABLE delta(
  rid INTEGER PRIMARY KEY,
  srcid INTEGER NOT NULL REFERENCES blob
);
CREATE INDEX delta_i1 ON delta(srcid);
CREATE TABLE rcvfrom(
  rcvid INTEGER PRIMARY KEY,
  uid INTEGER REFERENCES user,
  mtime DATETIME,
  nonce TEXT UNIQUE,
  ipaddr TEXT
);
CREATE TABLE user(
  uid INTEGER PRIMARY KEY,
  login TEXT UNIQUE,
  pw TEXT,
  cap TEXT,
  cookie TEXT,
  ipaddr TEXT,
  cexpire DATETIME,
  info TEXT,
  mtime DATE,
  photo BLOB
);
CREATE TABLE config(
  name TEXT PRIMARY KEY NOT NULL,
  value CLOB,
  mtime DATE,
  CHECK( typeof(name)='text' AND length(name)>=1 )
) WITHOUT ROWID;
CREATE TABLE shun(
  uuid TEXT PRIMARY KEY,
  mtime DATE,
  scom TEXT
) WITHOUT ROWID;
CREATE TABLE private(rid INTEGER PRIMARY KEY);
CREATE TABLE reportfmt(
   rn INTEGER PRIMARY KEY,
   owner TEXT,
   title TEXT UNIQUE,
   mtime DATE,
   cols TEXT,
   sqlcode TEXT
);
CREATE TABLE concealed(
  hash TEXT PRIMARY KEY,
  mtime DATE,
  content TEXT
) WITHOUT ROWID;
PRAGMA application_id=252006673;
`

// schemaRepo2 is the transient repo schema (tables rebuilt from artifacts).
// Sourced from libfossil's schema_repo2_cstr.c, with "repo." prefix stripped.
const schemaRepo2 = `
CREATE TABLE filename(
  fnid INTEGER PRIMARY KEY,
  name TEXT UNIQUE
);
CREATE TABLE mlink(
  mid INTEGER,
  fid INTEGER,
  pmid INTEGER,
  pid INTEGER,
  fnid INTEGER REFERENCES filename,
  pfnid INTEGER,
  mperm INTEGER,
  isaux BOOLEAN DEFAULT 0
);
CREATE INDEX mlink_i1 ON mlink(mid);
CREATE INDEX mlink_i2 ON mlink(fnid);
CREATE INDEX mlink_i3 ON mlink(fid);
CREATE INDEX mlink_i4 ON mlink(pid);
CREATE TABLE plink(
  pid INTEGER REFERENCES blob,
  cid INTEGER REFERENCES blob,
  isprim BOOLEAN,
  mtime DATETIME,
  baseid INTEGER REFERENCES blob,
  UNIQUE(pid, cid)
);
CREATE INDEX plink_i2 ON plink(cid,pid);
CREATE TABLE leaf(rid INTEGER PRIMARY KEY);
CREATE TABLE event(
  type TEXT,
  mtime DATETIME,
  objid INTEGER PRIMARY KEY,
  tagid INTEGER,
  uid INTEGER REFERENCES user,
  bgcolor TEXT,
  euser TEXT,
  user TEXT,
  ecomment TEXT,
  comment TEXT,
  brief TEXT,
  omtime DATETIME
);
CREATE INDEX event_i1 ON event(mtime);
CREATE TABLE phantom(
  rid INTEGER PRIMARY KEY
);
CREATE TABLE orphan(
  rid INTEGER PRIMARY KEY,
  baseline INTEGER
);
CREATE INDEX orphan_baseline ON orphan(baseline);
CREATE TABLE unclustered(
  rid INTEGER PRIMARY KEY
);
CREATE TABLE unsent(
  rid INTEGER PRIMARY KEY
);
CREATE TABLE tag(
  tagid INTEGER PRIMARY KEY,
  tagname TEXT UNIQUE
);
INSERT INTO tag VALUES(1, 'bgcolor');
INSERT INTO tag VALUES(2, 'comment');
INSERT INTO tag VALUES(3, 'user');
INSERT INTO tag VALUES(4, 'date');
INSERT INTO tag VALUES(5, 'hidden');
INSERT INTO tag VALUES(6, 'private');
INSERT INTO tag VALUES(7, 'cluster');
INSERT INTO tag VALUES(8, 'branch');
INSERT INTO tag VALUES(9, 'closed');
INSERT INTO tag VALUES(10,'parent');
INSERT INTO tag VALUES(11,'note');
CREATE TABLE tagxref(
  tagid INTEGER REFERENCES tag,
  tagtype INTEGER,
  srcid INTEGER REFERENCES blob,
  origid INTEGER REFERENCES blob,
  value TEXT,
  mtime TIMESTAMP,
  rid INTEGER REFERENCE blob,
  UNIQUE(rid, tagid)
);
CREATE INDEX tagxref_i1 ON tagxref(tagid, mtime);
CREATE TABLE backlink(
  target TEXT,
  srctype INT,
  srcid INT,
  mtime TIMESTAMP,
  UNIQUE(target, srctype, srcid)
);
CREATE INDEX backlink_src ON backlink(srcid, srctype);
CREATE TABLE attachment(
  attachid INTEGER PRIMARY KEY,
  isLatest BOOLEAN DEFAULT 0,
  mtime TIMESTAMP,
  src TEXT,
  target TEXT,
  filename TEXT,
  comment TEXT,
  user TEXT
);
CREATE INDEX attachment_idx1 ON attachment(target, filename, mtime);
CREATE INDEX attachment_idx2 ON attachment(src);
CREATE TABLE cherrypick(
  parentid INT,
  childid INT,
  isExclude BOOLEAN DEFAULT false,
  PRIMARY KEY(parentid, childid)
) WITHOUT ROWID;
CREATE INDEX cherrypick_cid ON cherrypick(childid);
`

// CreateRepoSchema creates the full Fossil repository schema in the database.
// This includes both static (repo1) and transient (repo2) tables,
// seed tag rows, and the application_id pragma.
func CreateRepoSchema(d *DB) error {
	_, err := d.Exec(schemaRepo1)
	if err != nil {
		return fmt.Errorf("schema repo1: %w", err)
	}
	_, err = d.Exec(schemaRepo2)
	if err != nil {
		return fmt.Errorf("schema repo2: %w", err)
	}
	return nil
}

// SeedUser inserts the initial user row (uid=1) needed by the rcvfrom seed.
func SeedUser(d *DB, login string) error {
	_, err := d.Exec(
		"INSERT OR IGNORE INTO user(uid, login, pw, cap, info) VALUES(1, ?, '', 's', '')",
		login,
	)
	return err
}

// SeedConfig inserts the minimum config rows that fossil expects:
// project-code, server-code, and aux-schema version.
func SeedConfig(d *DB) error {
	projCode, err := randomHex(20)
	if err != nil {
		return fmt.Errorf("generating project-code: %w", err)
	}
	serverCode, err := randomHex(20)
	if err != nil {
		return fmt.Errorf("generating server-code: %w", err)
	}
	stmts := []struct {
		name, value string
	}{
		{"project-code", projCode},
		{"server-code", serverCode},
		{"aux-schema", "2015-01-24"},
	}
	for _, s := range stmts {
		_, err := d.Exec(
			"INSERT INTO config(name, value, mtime) VALUES(?, ?, strftime('%s','now'))",
			s.name, s.value,
		)
		if err != nil {
			return fmt.Errorf("seed config %q: %w", s.name, err)
		}
	}
	// Seed rcvfrom row (references user uid=1)
	_, err = d.Exec("INSERT INTO rcvfrom(rcvid, uid, mtime, nonce, ipaddr) VALUES(1, 1, julianday('now'), NULL, NULL)")
	if err != nil {
		return fmt.Errorf("seed rcvfrom: %w", err)
	}
	return nil
}

func randomHex(nBytes int) (string, error) {
	b := make([]byte, nBytes)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./db/...
```

Expected: PASS (including the `_FossilValidation` test with `fossil rebuild --verify`)

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/db/schema.go
fossil commit -m "Add repo schema creation: all 23 tables, seed data, application_id"
```

---

### Task 8: hash Package — SHA1 and SHA3-256

**Files:**
- Create: `go-libfossil/hash/hash.go`
- Create: `go-libfossil/hash/hash_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/hash/hash_test.go`:

```go
package hash

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestSHA1(t *testing.T) {
	tests := []struct {
		input string
		want  string
	}{
		{"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
		{"hello", "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d"},
		{"Fossil SCM", "a4304aff1fcb8a78d973db242692175b2e579612"},
	}
	for _, tt := range tests {
		got := SHA1([]byte(tt.input))
		if got != tt.want {
			t.Errorf("SHA1(%q) = %q, want %q", tt.input, got, tt.want)
		}
	}
}

func TestSHA3(t *testing.T) {
	tests := []struct {
		input string
		want  string
	}{
		{"", "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"},
		{"hello", "3338be694f50c5f338814986cdf0686453a888b84f424d792af4b9202398f392"},
	}
	for _, tt := range tests {
		got := SHA3([]byte(tt.input))
		if got != tt.want {
			t.Errorf("SHA3(%q) = %q, want %q", tt.input, got, tt.want)
		}
	}
}

func TestSHA1_FossilValidation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping fossil CLI validation in short mode")
	}
	// Write test content to a temp file, hash with both Go and fossil
	input := []byte("test content for hashing")
	goHash := SHA1(input)

	tmpFile := filepath.Join(t.TempDir(), "testfile")
	os.WriteFile(tmpFile, input, 0644)

	cmd := exec.Command("fossil", "sha1sum", tmpFile)
	out, err := cmd.Output()
	if err != nil {
		t.Skipf("fossil sha1sum not available: %v", err)
	}
	// fossil sha1sum outputs: "hash  filename\n"
	fossilHash := strings.Fields(string(out))[0]
	if goHash != fossilHash {
		t.Fatalf("SHA1 mismatch: go=%q fossil=%q", goHash, fossilHash)
	}
}

func TestSHA3Format(t *testing.T) {
	got := SHA3([]byte("test"))
	if len(got) != 64 {
		t.Fatalf("SHA3 length = %d, want 64", len(got))
	}
	for _, c := range got {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			t.Fatalf("SHA3 contains non-lowercase-hex char: %c", c)
		}
	}
}

func BenchmarkSHA1(b *testing.B) {
	data := make([]byte, 10000)
	for i := range data {
		data[i] = byte(i)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		SHA1(data)
	}
}

func BenchmarkSHA3(b *testing.B) {
	data := make([]byte, 10000)
	for i := range data {
		data[i] = byte(i)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		SHA3(data)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./hash/...
```

Expected: FAIL — `SHA1`, `SHA3` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/hash/hash.go`:

```go
// Package hash provides content-addressing hash functions for Fossil artifacts.
// Fossil uses SHA1 (legacy) and SHA3-256 for artifact UUIDs.
package hash

import (
	"crypto/sha1"
	"encoding/hex"

	"golang.org/x/crypto/sha3"
)

// SHA1 returns the lowercase hex SHA1 hash of data.
// This is the legacy hash used by older Fossil repositories.
func SHA1(data []byte) string {
	h := sha1.Sum(data)
	return hex.EncodeToString(h[:])
}

// SHA3 returns the lowercase hex SHA3-256 hash of data.
// This is the modern hash used by Fossil v2+ repositories.
func SHA3(data []byte) string {
	h := sha3.Sum256(data)
	return hex.EncodeToString(h[:])
}

// HashSize returns the expected hex string length for the given hash type.
func HashSize(hashType string) int {
	switch hashType {
	case "sha1":
		return sha1.Size * 2 // 40
	case "sha3":
		return 64 // SHA3-256 = 32 bytes = 64 hex chars
	default:
		return 0
	}
}

// IsValidHash checks if a hex string looks like a valid Fossil hash
// (40 chars for SHA1 or 64 chars for SHA3-256, lowercase hex).
func IsValidHash(h string) bool {
	if len(h) != 40 && len(h) != 64 {
		return false
	}
	for _, c := range h {
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}
```

Note: This requires adding `golang.org/x/crypto` dependency:

```bash
cd ~/projects/EdgeSync/go-libfossil
go get golang.org/x/crypto
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./hash/...
```

Expected: PASS

- [ ] **Step 5: Run benchmarks**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test -bench=. -benchmem ./hash/...
```

Record baseline results.

- [ ] **Step 6: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/hash/hash.go go-libfossil/hash/hash_test.go
fossil commit -m "Add hash package: SHA1 and SHA3-256 content addressing"
```

- [ ] **Step 7: Run full validation**

```bash
cd ~/projects/EdgeSync/go-libfossil && make validate
```

Expected: all vet, test, and race checks pass across all packages so far

### Task 9: delta Package — Apply

**Files:**
- Create: `go-libfossil/delta/delta.go`
- Create: `go-libfossil/delta/delta_test.go`

The delta codec is copied-and-adapted from `~/projects/EdgeSync/pkg/delta/delta.go`. The existing implementation has `Apply` working but `Create` is not yet implemented. We port `Apply` first, then `Create` in the next task.

Reference: `~/projects/EdgeSync/libfossil/checkout/src/delta.c` and `~/fossil/libfossil-amalgamation/libfossil.c` (search for `fsl_delta_apply`).

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/delta/delta_test.go`:

```go
package delta

import (
	"bytes"
	"testing"
)

func TestApply_InsertOnly(t *testing.T) {
	// Delta that inserts "hello" (no copy from source):
	// "5\n5:hello0;" means target_len=5, insert 5 bytes "hello", checksum=0, end
	// We need the real checksum. The Fossil checksum of "hello" is computed below.
	source := []byte{}
	target := []byte("hello")

	// Build a manual delta: targetLen \n cnt :literal checksum ;
	// Fossil base-64 encodes integers. For small numbers:
	// 5 = '5', 0 = '0'
	cs := Checksum(target)
	delta := encodeDelta(uint64(len(target)), nil, target, cs)

	got, err := Apply(source, delta)
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if !bytes.Equal(got, target) {
		t.Fatalf("Apply = %q, want %q", got, target)
	}
}

func TestApply_CopyFromSource(t *testing.T) {
	source := []byte("hello world")
	target := []byte("hello Go")

	// We'll test with a real delta once Create is implemented.
	// For now, build a manual delta:
	// copy 6 bytes from source offset 0 ("hello "), then insert "Go"
	cs := Checksum(target)
	delta := manualDelta(uint64(len(target)), []deltaOp{
		{opType: '@', offset: 0, length: 6},          // copy "hello "
		{opType: ':', data: []byte("Go")},             // insert "Go"
	}, cs)

	got, err := Apply(source, delta)
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if !bytes.Equal(got, target) {
		t.Fatalf("Apply = %q, want %q", got, target)
	}
}

func TestApply_ChecksumMismatch(t *testing.T) {
	source := []byte{}
	target := []byte("hello")
	badChecksum := uint32(999999)
	delta := encodeDelta(uint64(len(target)), nil, target, badChecksum)

	_, err := Apply(source, delta)
	if err == nil {
		t.Fatal("expected checksum error")
	}
}

func TestApply_InvalidDelta(t *testing.T) {
	_, err := Apply(nil, []byte{})
	if err == nil {
		t.Fatal("expected error on empty delta")
	}
}

func TestChecksum(t *testing.T) {
	// Checksum should be deterministic
	data := []byte("hello")
	c1 := Checksum(data)
	c2 := Checksum(data)
	if c1 != c2 {
		t.Fatalf("Checksum not deterministic: %d != %d", c1, c2)
	}
	// Empty data
	c0 := Checksum(nil)
	if c0 != 0 {
		t.Fatalf("Checksum(nil) = %d, want 0", c0)
	}
}

func BenchmarkApply(b *testing.B) {
	source := bytes.Repeat([]byte("abcdefghij"), 1000) // 10KB
	target := append(bytes.Repeat([]byte("abcdefghij"), 999), []byte("CHANGED!")...)
	// Once Create is available, we'll use it. For now, benchmark with insert-only.
	cs := Checksum(target)
	delta := encodeDelta(uint64(len(target)), nil, target, cs)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Apply(source, delta)
	}
}

// --- Test helpers ---

type deltaOp struct {
	opType byte   // '@' for copy, ':' for insert
	offset uint64 // for copy
	length uint64 // for copy
	data   []byte // for insert
}

func manualDelta(targetLen uint64, ops []deltaOp, checksum uint32) []byte {
	var buf bytes.Buffer
	writeInt(&buf, targetLen)
	buf.WriteByte('\n')
	for _, op := range ops {
		switch op.opType {
		case '@':
			writeInt(&buf, op.offset)
			buf.WriteByte('@')
			writeInt(&buf, op.length)
			buf.WriteByte(',')
		case ':':
			writeInt(&buf, uint64(len(op.data)))
			buf.WriteByte(':')
			buf.Write(op.data)
		}
	}
	writeInt(&buf, uint64(checksum))
	buf.WriteByte(';')
	return buf.Bytes()
}

func encodeDelta(targetLen uint64, source, literal []byte, checksum uint32) []byte {
	var buf bytes.Buffer
	writeInt(&buf, targetLen)
	buf.WriteByte('\n')
	if len(literal) > 0 {
		writeInt(&buf, uint64(len(literal)))
		buf.WriteByte(':')
		buf.Write(literal)
	}
	writeInt(&buf, uint64(checksum))
	buf.WriteByte(';')
	return buf.Bytes()
}

// writeInt writes a Fossil base-64 encoded integer.
const zDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"

func writeInt(buf *bytes.Buffer, v uint64) {
	if v == 0 {
		buf.WriteByte('0')
		return
	}
	var tmp [13]byte
	i := len(tmp)
	for v > 0 {
		i--
		tmp[i] = zDigits[v&0x3f]
		v >>= 6
	}
	buf.Write(tmp[i:])
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./delta/...
```

Expected: FAIL — `Apply`, `Checksum` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/delta/delta.go` by copying and adapting from `~/projects/EdgeSync/pkg/delta/delta.go`:

```go
// Package delta implements the Fossil delta compression algorithm.
//
// The Fossil delta format encodes differences between two blobs (source
// and target) as a sequence of copy and insert commands. This is a pure
// Go port of Fossil's delta.c.
//
// Reference: https://fossil-scm.org/home/doc/tip/www/delta_format.wiki
package delta

import (
	"errors"
	"fmt"
)

var (
	ErrInvalidDelta = errors.New("delta: invalid format")
	ErrChecksum     = errors.New("delta: checksum mismatch")
)

// digits maps Fossil's base-64 characters to their numeric values.
var digits = [128]int{
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
	-1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
	25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, -1, -1, -1, -1, 36,
	-1, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, -1, -1, -1, 63, -1,
}

type reader struct {
	data []byte
	pos  int
}

func (r *reader) getInt() (uint64, error) {
	var v uint64
	started := false
	for r.pos < len(r.data) {
		c := r.data[r.pos]
		if c >= 128 || digits[c] < 0 {
			break
		}
		v = v*64 + uint64(digits[c])
		r.pos++
		started = true
	}
	if !started {
		return 0, fmt.Errorf("%w: expected integer at pos %d", ErrInvalidDelta, r.pos)
	}
	return v, nil
}

func (r *reader) getChar() (byte, error) {
	if r.pos >= len(r.data) {
		return 0, fmt.Errorf("%w: unexpected end at pos %d", ErrInvalidDelta, r.pos)
	}
	c := r.data[r.pos]
	r.pos++
	return c, nil
}

// Apply applies a delta to a source blob, producing the target blob.
// The output is byte-exact given the same source and delta inputs.
func Apply(source, delta []byte) ([]byte, error) {
	if len(delta) == 0 {
		return nil, fmt.Errorf("%w: empty delta", ErrInvalidDelta)
	}

	r := &reader{data: delta}

	targetLen, err := r.getInt()
	if err != nil {
		return nil, err
	}
	term, err := r.getChar()
	if err != nil {
		return nil, err
	}
	if term != '\n' {
		return nil, fmt.Errorf("%w: expected newline after target length", ErrInvalidDelta)
	}

	output := make([]byte, 0, targetLen)

	for r.pos < len(r.data) {
		cnt, err := r.getInt()
		if err != nil {
			return nil, err
		}
		cmd, err := r.getChar()
		if err != nil {
			return nil, err
		}

		switch cmd {
		case '@':
			offset := cnt
			cnt, err = r.getInt()
			if err != nil {
				return nil, err
			}
			term, err = r.getChar()
			if err != nil {
				return nil, err
			}
			if term != ',' {
				return nil, fmt.Errorf("%w: expected comma in copy command", ErrInvalidDelta)
			}
			if int(offset+cnt) > len(source) {
				return nil, fmt.Errorf("%w: copy exceeds source bounds (offset=%d, cnt=%d, srclen=%d)",
					ErrInvalidDelta, offset, cnt, len(source))
			}
			output = append(output, source[offset:offset+cnt]...)

		case ':':
			if r.pos+int(cnt) > len(r.data) {
				return nil, fmt.Errorf("%w: insert exceeds delta bounds", ErrInvalidDelta)
			}
			output = append(output, r.data[r.pos:r.pos+int(cnt)]...)
			r.pos += int(cnt)

		case ';':
			if uint64(len(output)) != targetLen {
				return nil, fmt.Errorf("%w: output size %d != target size %d",
					ErrInvalidDelta, len(output), targetLen)
			}
			if cnt != uint64(Checksum(output)) {
				return nil, fmt.Errorf("%w: expected %d, got %d",
					ErrChecksum, Checksum(output), cnt)
			}
			return output, nil

		default:
			return nil, fmt.Errorf("%w: unknown command '%c' at pos %d",
				ErrInvalidDelta, cmd, r.pos-1)
		}
	}

	return nil, fmt.Errorf("%w: missing terminator", ErrInvalidDelta)
}

// Checksum computes the Fossil delta checksum for a byte slice.
// This matches the checksum algorithm in fossil's delta.c.
func Checksum(data []byte) uint32 {
	var sum0, sum1 uint16
	for _, b := range data {
		sum0 += uint16(b)
		sum1 += sum0
	}
	return uint32(sum0) | (uint32(sum1) << 16)
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./delta/...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/delta/delta.go go-libfossil/delta/delta_test.go
fossil commit -m "Add delta package: Apply and Checksum ported from EdgeSync"
```

---

### Task 10: delta Package — Create

**Files:**
- Modify: `go-libfossil/delta/delta.go` (add Create function)
- Modify: `go-libfossil/delta/delta_test.go` (add Create tests)

Reference: `~/projects/EdgeSync/libfossil/checkout/src/delta.c` function `fsl_delta_create`.

- [ ] **Step 1: Write the failing test**

Add to `go-libfossil/delta/delta_test.go`:

```go
func TestCreate_SmallInputs(t *testing.T) {
	tests := []struct {
		name   string
		source string
		target string
	}{
		{"identical", "hello", "hello"},
		{"append", "hello", "hello world"},
		{"prepend", "world", "hello world"},
		{"replace", "aaaa", "bbbb"},
		{"empty_source", "", "new content"},
		{"empty_target", "old content", ""},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			src := []byte(tt.source)
			tgt := []byte(tt.target)

			d := Create(src, tgt)
			if len(d) == 0 {
				t.Fatal("Create returned empty delta")
			}

			// Apply must reproduce target exactly
			got, err := Apply(src, d)
			if err != nil {
				t.Fatalf("Apply failed: %v", err)
			}
			if !bytes.Equal(got, tgt) {
				t.Fatalf("round-trip failed: got %q, want %q", got, tgt)
			}
		})
	}
}

func TestCreate_LargeInput(t *testing.T) {
	// 100KB source with small change
	source := bytes.Repeat([]byte("The quick brown fox jumps. "), 4000)
	target := make([]byte, len(source))
	copy(target, source)
	copy(target[50000:], []byte("CHANGED CONTENT HERE!"))

	d := Create(source, target)

	// Delta should be much smaller than target
	if len(d) > len(target)/2 {
		t.Fatalf("delta too large: %d bytes for %d byte target", len(d), len(target))
	}

	got, err := Apply(source, d)
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if !bytes.Equal(got, target) {
		t.Fatal("round-trip failed for large input")
	}
}

func TestCreate_RoundTrip_FossilValidation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping fossil validation in short mode")
	}
	// Create delta with Go, verify round-trip
	source := []byte("original content of the file\nwith multiple lines\nand some data\n")
	target := []byte("original content of the file\nwith MODIFIED lines\nand some data\nplus new stuff\n")

	d := Create(source, target)
	got, err := Apply(source, d)
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if !bytes.Equal(got, target) {
		t.Fatalf("round-trip failed")
	}
}

func BenchmarkCreate(b *testing.B) {
	source := bytes.Repeat([]byte("abcdefghij"), 1000) // 10KB
	target := make([]byte, len(source))
	copy(target, source)
	copy(target[5000:], []byte("XXXXXXXXXXXX"))
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Create(source, target)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./delta/...
```

Expected: FAIL — `Create` undefined

- [ ] **Step 3: Write the Create implementation**

Add to `go-libfossil/delta/delta.go`:

```go
// zDigits is the Fossil base-64 encoding alphabet.
const zDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz~"

// Create computes a delta that transforms source into target.
// The delta can be applied with Apply(source, delta) to reproduce target.
//
// The algorithm uses a rolling hash to find matching blocks between source
// and target, then emits copy commands for matches and insert commands for
// new content.
func Create(source, target []byte) []byte {
	if len(target) == 0 {
		// Empty target: just encode length 0 + checksum + terminator
		var buf []byte
		buf = appendInt(buf, 0)
		buf = append(buf, '\n')
		buf = appendInt(buf, uint64(Checksum(target)))
		buf = append(buf, ';')
		return buf
	}

	// For very small inputs or empty source, just emit insert-all
	if len(source) < 16 {
		return createInsertAll(target)
	}

	// Build hash table from source
	const nHash = 16 // minimum match length
	type hashEntry struct {
		offset int
		next   int // index into entries, -1 = end
	}

	tableSize := len(source) / nHash
	if tableSize < 64 {
		tableSize = 64
	}
	// Round up to power of 2
	for tableSize&(tableSize-1) != 0 {
		tableSize &= tableSize - 1
	}
	tableSize <<= 1
	mask := tableSize - 1

	heads := make([]int, tableSize) // index into entries, 0 = empty (offset by 1)
	entries := make([]hashEntry, 0, len(source)/nHash)

	// Populate hash table with source blocks
	for i := 0; i+nHash <= len(source); i += nHash {
		h := rollingHash(source[i : i+nHash])
		idx := int(h) & mask
		entries = append(entries, hashEntry{offset: i, next: heads[idx] - 1})
		heads[idx] = len(entries) // 1-based
	}

	var buf []byte
	buf = appendInt(buf, uint64(len(target)))
	buf = append(buf, '\n')

	var pendingInsert []byte
	tPos := 0

	flushInsert := func() {
		if len(pendingInsert) > 0 {
			buf = appendInt(buf, uint64(len(pendingInsert)))
			buf = append(buf, ':')
			buf = append(buf, pendingInsert...)
			pendingInsert = pendingInsert[:0]
		}
	}

	for tPos < len(target) {
		bestLen := 0
		bestOff := 0

		if tPos+nHash <= len(target) {
			h := rollingHash(target[tPos : tPos+nHash])
			idx := int(h) & mask
			ei := heads[idx]
			for ei > 0 {
				e := entries[ei-1]
				sOff := e.offset

				// Verify the hash match
				if sOff+nHash <= len(source) && matchLen(source[sOff:], target[tPos:]) >= nHash {
					ml := matchLen(source[sOff:], target[tPos:])
					if ml > bestLen {
						bestLen = ml
						bestOff = sOff
					}
				}
				ei = e.next + 1
			}
		}

		if bestLen >= nHash {
			flushInsert()
			buf = appendInt(buf, uint64(bestOff))
			buf = append(buf, '@')
			buf = appendInt(buf, uint64(bestLen))
			buf = append(buf, ',')
			tPos += bestLen
		} else {
			pendingInsert = append(pendingInsert, target[tPos])
			tPos++
		}
	}

	flushInsert()
	buf = appendInt(buf, uint64(Checksum(target)))
	buf = append(buf, ';')
	return buf
}

func createInsertAll(target []byte) []byte {
	var buf []byte
	buf = appendInt(buf, uint64(len(target)))
	buf = append(buf, '\n')
	buf = appendInt(buf, uint64(len(target)))
	buf = append(buf, ':')
	buf = append(buf, target...)
	buf = appendInt(buf, uint64(Checksum(target)))
	buf = append(buf, ';')
	return buf
}

func appendInt(buf []byte, v uint64) []byte {
	if v == 0 {
		return append(buf, '0')
	}
	var tmp [13]byte
	i := len(tmp)
	for v > 0 {
		i--
		tmp[i] = zDigits[v&0x3f]
		v >>= 6
	}
	return append(buf, tmp[i:]...)
}

func rollingHash(data []byte) uint32 {
	var h uint32
	for _, b := range data {
		h = h*37 + uint32(b)
	}
	return h
}

func matchLen(a, b []byte) int {
	n := len(a)
	if len(b) < n {
		n = len(b)
	}
	for i := 0; i < n; i++ {
		if a[i] != b[i] {
			return i
		}
	}
	return n
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./delta/...
```

Expected: PASS

- [ ] **Step 5: Run benchmarks**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test -bench=. -benchmem ./delta/...
```

Record baseline results.

- [ ] **Step 6: Commit**

```bash
cd ~/projects/EdgeSync
fossil commit -m "Add delta.Create: rolling hash block matching with copy/insert encoding"
```

---

### Task 11: blob Package — Store, Load, Exists

**Files:**
- Create: `go-libfossil/blob/blob.go`
- Create: `go-libfossil/blob/compress.go`
- Create: `go-libfossil/blob/blob_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/blob/blob_test.go`:

```go
package blob

import (
	"bytes"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

func setupTestDB(t *testing.T) *db.DB {
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

func TestStoreAndLoad(t *testing.T) {
	d := setupTestDB(t)
	content := []byte("hello fossil world")

	rid, uuid, err := Store(d, content)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}
	if rid <= 0 {
		t.Fatalf("rid = %d, want > 0", rid)
	}
	if len(uuid) != 40 && len(uuid) != 64 {
		t.Fatalf("uuid length = %d, want 40 or 64", len(uuid))
	}

	got, err := Load(d, rid)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if !bytes.Equal(got, content) {
		t.Fatalf("Load = %q, want %q", got, content)
	}
}

func TestExists(t *testing.T) {
	d := setupTestDB(t)
	content := []byte("existence test")

	_, uuid, err := Store(d, content)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	rid, ok := Exists(d, uuid)
	if !ok {
		t.Fatal("Exists returned false for stored blob")
	}
	if rid <= 0 {
		t.Fatalf("Exists rid = %d, want > 0", rid)
	}

	_, ok = Exists(d, "0000000000000000000000000000000000000000")
	if ok {
		t.Fatal("Exists returned true for non-existent blob")
	}
}

func TestStorePhantom(t *testing.T) {
	d := setupTestDB(t)
	uuid := "da39a3ee5e6b4b0d3255bfef95601890afd80709"

	rid, err := StorePhantom(d, uuid)
	if err != nil {
		t.Fatalf("StorePhantom: %v", err)
	}
	if rid <= 0 {
		t.Fatalf("phantom rid = %d, want > 0", rid)
	}

	// Phantom should be in the phantom table
	var count int
	d.QueryRow("SELECT count(*) FROM phantom WHERE rid=?", rid).Scan(&count)
	if count != 1 {
		t.Fatalf("phantom table count = %d, want 1", count)
	}

	// blob.size should be -1
	var size int64
	d.QueryRow("SELECT size FROM blob WHERE rid=?", rid).Scan(&size)
	if size != -1 {
		t.Fatalf("phantom size = %d, want -1", size)
	}
}

func TestStoreDelta(t *testing.T) {
	d := setupTestDB(t)
	source := []byte("original content here")
	target := []byte("original content modified")

	srcRid, _, err := Store(d, source)
	if err != nil {
		t.Fatalf("Store source: %v", err)
	}

	tgtRid, _, err := StoreDelta(d, target, srcRid)
	if err != nil {
		t.Fatalf("StoreDelta: %v", err)
	}
	if tgtRid <= 0 {
		t.Fatalf("tgtRid = %d, want > 0", tgtRid)
	}

	// Should have a delta table entry
	var srcid int64
	err = d.QueryRow("SELECT srcid FROM delta WHERE rid=?", tgtRid).Scan(&srcid)
	if err != nil {
		t.Fatalf("delta row missing: %v", err)
	}
	if srcid != int64(srcRid) {
		t.Fatalf("delta.srcid = %d, want %d", srcid, srcRid)
	}
}

func BenchmarkStore(b *testing.B) {
	d := func() *db.DB {
		path := filepath.Join(b.TempDir(), "bench.fossil")
		d, _ := db.Open(path)
		db.CreateRepoSchema(d)
		return d
	}()
	defer d.Close()

	data := bytes.Repeat([]byte("benchmark data"), 100)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		// Each iteration stores unique content to avoid UUID conflicts
		content := append(data, byte(i), byte(i>>8))
		Store(d, content)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./blob/...
```

Expected: FAIL — `Store`, `Load`, `Exists`, `StorePhantom`, `StoreDelta` undefined

- [ ] **Step 3: Write compress.go**

Create `go-libfossil/blob/compress.go`:

```go
package blob

import (
	"bytes"
	"compress/zlib"
	"fmt"
	"io"
)

// Compress compresses data using zlib (Fossil's blob storage format).
func Compress(data []byte) ([]byte, error) {
	var buf bytes.Buffer
	w := zlib.NewWriter(&buf)
	if _, err := w.Write(data); err != nil {
		return nil, fmt.Errorf("zlib compress: %w", err)
	}
	if err := w.Close(); err != nil {
		return nil, fmt.Errorf("zlib close: %w", err)
	}
	return buf.Bytes(), nil
}

// Decompress decompresses zlib-compressed data.
func Decompress(data []byte) ([]byte, error) {
	r, err := zlib.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, fmt.Errorf("zlib decompress: %w", err)
	}
	defer r.Close()
	out, err := io.ReadAll(r)
	if err != nil {
		return nil, fmt.Errorf("zlib read: %w", err)
	}
	return out, nil
}
```

- [ ] **Step 4: Write blob.go**

Create `go-libfossil/blob/blob.go`:

```go
// Package blob provides content-addressed blob storage for Fossil repositories.
// Blobs are zlib-compressed and stored in the blob table, identified by their
// SHA1 or SHA3-256 hash.
package blob

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
)

// Store compresses and stores content in the blob table.
// Returns the record ID and the content's SHA1 hash (UUID).
func Store(d *db.DB, content []byte) (libfossil.FslID, string, error) {
	uuid := hash.SHA1(content)

	// Check if already exists
	if rid, ok := Exists(d, uuid); ok {
		return rid, uuid, nil
	}

	compressed, err := Compress(content)
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store compress: %w", err)
	}

	result, err := d.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		uuid, len(content), compressed,
	)
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store insert: %w", err)
	}

	rid, err := result.LastInsertId()
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store lastid: %w", err)
	}

	return libfossil.FslID(rid), uuid, nil
}

// StoreDelta stores content as a delta against a source blob.
// The delta is computed, compressed, and stored. A delta table entry is created.
func StoreDelta(d *db.DB, content []byte, srcRid libfossil.FslID) (libfossil.FslID, string, error) {
	uuid := hash.SHA1(content)

	if rid, ok := Exists(d, uuid); ok {
		return rid, uuid, nil
	}

	// Load source content to compute delta
	srcContent, err := Load(d, srcRid)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta load source: %w", err)
	}

	deltaBytes := delta.Create(srcContent, content)
	compressed, err := Compress(deltaBytes)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta compress: %w", err)
	}

	result, err := d.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		uuid, len(content), compressed,
	)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta insert blob: %w", err)
	}

	rid, err := result.LastInsertId()
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta lastid: %w", err)
	}

	_, err = d.Exec("INSERT INTO delta(rid, srcid) VALUES(?, ?)", rid, srcRid)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta insert delta: %w", err)
	}

	return libfossil.FslID(rid), uuid, nil
}

// StorePhantom creates a phantom blob entry — a blob whose UUID is known
// but whose content has not yet arrived. Size is set to -1.
func StorePhantom(d *db.DB, uuid string) (libfossil.FslID, error) {
	if rid, ok := Exists(d, uuid); ok {
		return rid, nil
	}

	result, err := d.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, -1, NULL, 0)",
		uuid,
	)
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom: %w", err)
	}

	rid, err := result.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom lastid: %w", err)
	}

	_, err = d.Exec("INSERT INTO phantom(rid) VALUES(?)", rid)
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom phantom table: %w", err)
	}

	return libfossil.FslID(rid), nil
}

// Load retrieves and decompresses a blob by record ID.
// For delta-stored blobs, this returns the raw (still-delta) content.
// Use content.Expand() to resolve delta chains.
func Load(d *db.DB, rid libfossil.FslID) ([]byte, error) {
	var compressed []byte
	var size int64
	err := d.QueryRow("SELECT content, size FROM blob WHERE rid=?", rid).Scan(&compressed, &size)
	if err != nil {
		return nil, fmt.Errorf("blob.Load query: %w", err)
	}

	if size == -1 {
		return nil, fmt.Errorf("blob.Load: rid %d is a phantom", rid)
	}

	if compressed == nil {
		return nil, fmt.Errorf("blob.Load: rid %d has NULL content", rid)
	}

	return Decompress(compressed)
}

// Exists checks whether a blob with the given UUID exists in the repository.
// Returns the record ID and true if found, or 0 and false otherwise.
func Exists(d *db.DB, uuid string) (libfossil.FslID, bool) {
	var rid int64
	err := d.QueryRow("SELECT rid FROM blob WHERE uuid=?", uuid).Scan(&rid)
	if err != nil {
		return 0, false
	}
	return libfossil.FslID(rid), true
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./blob/...
```

Expected: PASS

- [ ] **Step 6: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/blob/blob.go go-libfossil/blob/compress.go go-libfossil/blob/blob_test.go
fossil commit -m "Add blob package: Store, Load, Exists, StorePhantom, StoreDelta with zlib compression"
```

- [ ] **Step 7: Run full validation**

```bash
cd ~/projects/EdgeSync/go-libfossil && make validate
```

Expected: all vet, test, and race checks pass across all packages

### Task 12: content Package — Delta Chain Expansion and Verification

**Files:**
- Create: `go-libfossil/content/content.go`
- Create: `go-libfossil/content/content_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/content/content_test.go`:

```go
package content

import (
	"bytes"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
)

func setupTestDB(t *testing.T) *db.DB {
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

func TestExpand_FullText(t *testing.T) {
	d := setupTestDB(t)
	content := []byte("full text content, no delta")

	rid, _, err := blob.Store(d, content)
	if err != nil {
		t.Fatalf("Store: %v", err)
	}

	got, err := Expand(d, rid)
	if err != nil {
		t.Fatalf("Expand: %v", err)
	}
	if !bytes.Equal(got, content) {
		t.Fatalf("Expand = %q, want %q", got, content)
	}
}

func TestExpand_SingleDelta(t *testing.T) {
	d := setupTestDB(t)
	source := []byte("the original source content for delta testing purposes here")
	target := []byte("the original source content for MODIFIED testing purposes here")

	srcRid, _, err := blob.Store(d, source)
	if err != nil {
		t.Fatalf("Store source: %v", err)
	}

	tgtRid, _, err := blob.StoreDelta(d, target, srcRid)
	if err != nil {
		t.Fatalf("StoreDelta: %v", err)
	}

	got, err := Expand(d, tgtRid)
	if err != nil {
		t.Fatalf("Expand: %v", err)
	}
	if !bytes.Equal(got, target) {
		t.Fatalf("Expand = %q, want %q", got, target)
	}
}

func TestExpand_DeltaChain(t *testing.T) {
	d := setupTestDB(t)
	v1 := []byte("version one of the content with enough data to make deltas work well")
	v2 := []byte("version TWO of the content with enough data to make deltas work well")
	v3 := []byte("version THREE of the content with enough data to make deltas work well")

	rid1, _, _ := blob.Store(d, v1)
	rid2, _, _ := blob.StoreDelta(d, v2, rid1)
	rid3, _, _ := blob.StoreDelta(d, v3, rid2)

	// Expanding v3 should follow: v3 -> v2 -> v1, apply deltas in reverse
	got, err := Expand(d, rid3)
	if err != nil {
		t.Fatalf("Expand chain: %v", err)
	}
	if !bytes.Equal(got, v3) {
		t.Fatalf("Expand chain = %q, want %q", got, v3)
	}
}

func TestExpand_Phantom(t *testing.T) {
	d := setupTestDB(t)
	_, err := blob.StorePhantom(d, "da39a3ee5e6b4b0d3255bfef95601890afd80709")
	if err != nil {
		t.Fatalf("StorePhantom: %v", err)
	}

	// Expanding a phantom referenced by a delta should fail
	// (but expanding the phantom itself should also fail)
	var rid int64
	d.QueryRow("SELECT rid FROM phantom LIMIT 1").Scan(&rid)

	_, err = Expand(d, 0) // invalid rid
	if err == nil {
		t.Fatal("expected error expanding invalid rid")
	}
}

func TestVerify(t *testing.T) {
	d := setupTestDB(t)
	content := []byte("content to verify")
	rid, _, _ := blob.Store(d, content)

	err := Verify(d, rid)
	if err != nil {
		t.Fatalf("Verify: %v", err)
	}
}

func BenchmarkExpand_DeltaChain(b *testing.B) {
	path := filepath.Join(b.TempDir(), "bench.fossil")
	d, _ := db.Open(path)
	db.CreateRepoSchema(d)
	defer d.Close()

	base := bytes.Repeat([]byte("base content for benchmark "), 100)
	rid, _, _ := blob.Store(d, base)

	// Build a chain of 5 deltas
	for i := 0; i < 5; i++ {
		next := make([]byte, len(base))
		copy(next, base)
		copy(next[i*50:], []byte("CHANGED!"))
		newRid, _, _ := blob.StoreDelta(d, next, rid)
		rid = newRid
		base = next
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Expand(d, rid)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./content/...
```

Expected: FAIL — `Expand`, `Verify` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/content/content.go`:

```go
// Package content provides higher-level content operations on Fossil blobs,
// including delta chain expansion, phantom tracking, and content verification.
package content

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
)

// Expand retrieves the full content for a blob, resolving delta chains.
// If the blob is stored as full-text, it returns the content directly.
// If stored as a delta, it walks the delta chain back to a full-text blob,
// then applies deltas forward to reconstruct the target content.
func Expand(d *db.DB, rid libfossil.FslID) ([]byte, error) {
	if rid <= 0 {
		return nil, fmt.Errorf("content.Expand: invalid rid %d", rid)
	}

	// Walk the delta chain to find the root (non-delta) blob
	chain, err := walkDeltaChain(d, rid)
	if err != nil {
		return nil, fmt.Errorf("content.Expand: %w", err)
	}

	// Load the root blob (full-text)
	content, err := blob.Load(d, chain[0])
	if err != nil {
		return nil, fmt.Errorf("content.Expand load root rid=%d: %w", chain[0], err)
	}

	// Apply deltas forward through the chain
	for i := 1; i < len(chain); i++ {
		deltaBytes, err := blob.Load(d, chain[i])
		if err != nil {
			return nil, fmt.Errorf("content.Expand load delta rid=%d: %w", chain[i], err)
		}
		content, err = delta.Apply(content, deltaBytes)
		if err != nil {
			return nil, fmt.Errorf("content.Expand apply delta rid=%d: %w", chain[i], err)
		}
	}

	return content, nil
}

// walkDeltaChain returns the chain of RIDs from root to the target rid.
// The first element is the root (full-text blob), and the last is the target.
// For a non-delta blob, returns just [rid].
func walkDeltaChain(d *db.DB, rid libfossil.FslID) ([]libfossil.FslID, error) {
	var chain []libfossil.FslID
	current := rid
	seen := make(map[libfossil.FslID]bool)

	for {
		if seen[current] {
			return nil, fmt.Errorf("delta chain cycle detected at rid=%d", current)
		}
		seen[current] = true
		chain = append(chain, current)

		var srcid int64
		err := d.QueryRow("SELECT srcid FROM delta WHERE rid=?", current).Scan(&srcid)
		if err != nil {
			// No delta entry — this is the root (full-text blob)
			break
		}
		current = libfossil.FslID(srcid)
	}

	// Reverse so chain goes root -> ... -> target
	for i, j := 0, len(chain)-1; i < j; i, j = i+1, j-1 {
		chain[i], chain[j] = chain[j], chain[i]
	}
	return chain, nil
}

// Verify checks that a blob's stored content matches its UUID hash.
// Expands delta chains if necessary.
func Verify(d *db.DB, rid libfossil.FslID) error {
	var uuid string
	err := d.QueryRow("SELECT uuid FROM blob WHERE rid=?", rid).Scan(&uuid)
	if err != nil {
		return fmt.Errorf("content.Verify query uuid: %w", err)
	}

	content, err := Expand(d, rid)
	if err != nil {
		return fmt.Errorf("content.Verify expand: %w", err)
	}

	var computed string
	if len(uuid) == 64 {
		computed = hash.SHA3(content)
	} else {
		computed = hash.SHA1(content)
	}

	if computed != uuid {
		return fmt.Errorf("content.Verify: hash mismatch for rid=%d: stored=%s computed=%s", rid, uuid, computed)
	}
	return nil
}

// IsPhantom checks if a blob RID is a phantom (content not yet received).
func IsPhantom(d *db.DB, rid libfossil.FslID) (bool, error) {
	var count int
	err := d.QueryRow("SELECT count(*) FROM phantom WHERE rid=?", rid).Scan(&count)
	if err != nil {
		return false, err
	}
	return count > 0, nil
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./content/...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/content/content.go go-libfossil/content/content_test.go
fossil commit -m "Add content package: delta chain expansion, verification, phantom check"
```

---

### Task 13: repo Package — Create, Open, Close, Verify

**Files:**
- Create: `go-libfossil/repo/repo.go`
- Create: `go-libfossil/repo/repo_test.go`

- [ ] **Step 1: Write the failing test**

Create `go-libfossil/repo/repo_test.go`:

```go
package repo

import (
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/db"
)

func TestCreate(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := Create(path, "testuser")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer r.Close()

	if r.Path() != path {
		t.Fatalf("Path = %q, want %q", r.Path(), path)
	}
}

func TestCreate_FossilValidation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping fossil validation")
	}
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := Create(path, "testuser")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	r.Close()

	// fossil rebuild --verify must pass
	cmd := exec.Command("fossil", "rebuild", "--verify", path)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild --verify failed: %v\n%s", err, out)
	}
}

func TestOpenFossilCreated(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping fossil validation")
	}
	// Create a repo with fossil, open with Go
	path := filepath.Join(t.TempDir(), "fossil-created.fossil")
	cmd := exec.Command("fossil", "new", path)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil new: %v\n%s", err, out)
	}

	r, err := Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer r.Close()

	if r.Path() != path {
		t.Fatalf("Path = %q, want %q", r.Path(), path)
	}
}

func TestOpen_NotARepo(t *testing.T) {
	// Create a plain SQLite database (no application_id)
	path := filepath.Join(t.TempDir(), "not-a-repo.db")
	cmd := exec.Command("sqlite3", path, "CREATE TABLE test(x);")
	cmd.Run() // ignore errors, sqlite3 might not be installed

	_, err := Open(path)
	if err == nil {
		t.Fatal("expected error opening non-repo database")
	}
}

func TestOpen_NonExistent(t *testing.T) {
	_, err := Open("/tmp/nonexistent-repo-12345.fossil")
	if err == nil {
		t.Fatal("expected error opening nonexistent file")
	}
}

func TestWithTx(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.fossil")
	r, err := Create(path, "testuser")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer r.Close()

	err = r.WithTx(func(tx *db.Tx) error {
		_, err := tx.Exec("INSERT INTO config(name,value,mtime) VALUES('test-key','test-val',0)")
		return err
	})
	if err != nil {
		t.Fatalf("WithTx: %v", err)
	}

	var val string
	r.DB().QueryRow("SELECT value FROM config WHERE name='test-key'").Scan(&val)
	if val != "test-val" {
		t.Fatalf("val = %q, want %q", val, "test-val")
	}
}

func TestRoundTrip_FossilValidation(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping fossil validation")
	}

	// 1. fossil creates repo
	fossilPath := filepath.Join(t.TempDir(), "fossil-created.fossil")
	exec.Command("fossil", "new", fossilPath).CombinedOutput()

	// 2. Go opens it
	r1, err := Open(fossilPath)
	if err != nil {
		t.Fatalf("Open fossil-created: %v", err)
	}
	r1.Close()

	// 3. Go creates repo
	goPath := filepath.Join(t.TempDir(), "go-created.fossil")
	r2, err := Create(goPath, "testuser")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	r2.Close()

	// 4. fossil validates Go-created repo
	cmd := exec.Command("fossil", "rebuild", "--verify", goPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild go-created failed: %v\n%s", err, out)
	}

	// 5. fossil queries Go-created repo
	cmd = exec.Command("fossil", "sql", "-R", goPath, "SELECT count(*) FROM blob;")
	out, err = cmd.Output()
	if err != nil {
		t.Fatalf("fossil sql go-created: %v", err)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./repo/...
```

Expected: FAIL — `Create`, `Open`, `Repo` undefined

- [ ] **Step 3: Write minimal implementation**

Create `go-libfossil/repo/repo.go`:

```go
// Package repo provides Fossil repository lifecycle management:
// create, open, close, and verify .fossil repository files.
package repo

import (
	"fmt"
	"os"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
)

// Repo represents an open Fossil repository.
type Repo struct {
	db   *db.DB
	path string
}

// Create creates a new .fossil repository file at the given path
// with the complete Fossil schema, seed data, and application_id.
func Create(path string, user string) (*Repo, error) {
	if _, err := os.Stat(path); err == nil {
		return nil, fmt.Errorf("repo.Create: file already exists: %s", path)
	}

	d, err := db.Open(path)
	if err != nil {
		return nil, fmt.Errorf("repo.Create open: %w", err)
	}

	if err := db.CreateRepoSchema(d); err != nil {
		d.Close()
		os.Remove(path)
		return nil, fmt.Errorf("repo.Create schema: %w", err)
	}

	if err := db.SeedUser(d, user); err != nil {
		d.Close()
		os.Remove(path)
		return nil, fmt.Errorf("repo.Create seed user: %w", err)
	}

	if err := db.SeedConfig(d); err != nil {
		d.Close()
		os.Remove(path)
		return nil, fmt.Errorf("repo.Create seed config: %w", err)
	}

	return &Repo{db: d, path: path}, nil
}

// Open opens an existing .fossil repository file.
// Verifies the application_id matches Fossil's expected value.
func Open(path string) (*Repo, error) {
	if _, err := os.Stat(path); err != nil {
		return nil, fmt.Errorf("repo.Open: %w", err)
	}

	d, err := db.Open(path)
	if err != nil {
		return nil, fmt.Errorf("repo.Open: %w", err)
	}

	id, err := d.ApplicationID()
	if err != nil {
		d.Close()
		return nil, fmt.Errorf("repo.Open application_id: %w", err)
	}
	if id != libfossil.FossilApplicationID {
		d.Close()
		return nil, fmt.Errorf("repo.Open: not a fossil repo (application_id=%d, want %d)",
			id, libfossil.FossilApplicationID)
	}

	return &Repo{db: d, path: path}, nil
}

// Close closes the repository database connection.
func (r *Repo) Close() error {
	return r.db.Close()
}

// Path returns the filesystem path to the .fossil file.
func (r *Repo) Path() string {
	return r.path
}

// DB returns the underlying database handle for direct operations.
func (r *Repo) DB() *db.DB {
	return r.db
}

// WithTx executes fn inside a database transaction.
func (r *Repo) WithTx(fn func(tx *db.Tx) error) error {
	return r.db.WithTx(fn)
}

// Verify checks the internal consistency of the repository:
// re-hashes all non-phantom blobs and confirms they match their stored UUID.
func (r *Repo) Verify() error {
	rows, err := r.db.Query("SELECT rid FROM blob WHERE size >= 0")
	if err != nil {
		return fmt.Errorf("repo.Verify query: %w", err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid int64
		if err := rows.Scan(&rid); err != nil {
			return fmt.Errorf("repo.Verify scan: %w", err)
		}
		if err := content.Verify(r.db, libfossil.FslID(rid)); err != nil {
			return err
		}
	}
	return rows.Err()
}
```

Note: `repo.go` imports will need `content` package: `"github.com/dmestas/edgesync/go-libfossil/content"`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test ./repo/...
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/repo/repo.go go-libfossil/repo/repo_test.go
fossil commit -m "Add repo package: Create, Open, Close with fossil CLI validation"
```

---

### Task 14: Phase A Integration Test

**Files:**
- Create: `go-libfossil/integration_test.go`

This is the end-to-end test that validates the Phase A exit criteria:
create a repo, store blobs, store deltas, expand content, and have `fossil rebuild --verify` pass.

- [ ] **Step 1: Write the integration test**

Create `go-libfossil/integration_test.go`:

```go
package libfossil_test

import (
	"bytes"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

func TestPhaseA_EndToEnd(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test")
	}

	// 1. Create a repo with Go
	path := filepath.Join(t.TempDir(), "phase-a.fossil")
	r, err := repo.Create(path, "testuser")
	if err != nil {
		t.Fatalf("Create: %v", err)
	}
	defer r.Close()

	// 2. Store full-text blobs
	content1 := []byte("first blob with enough content to make deltas meaningful in testing")
	rid1, uuid1, err := blob.Store(r.DB(), content1)
	if err != nil {
		t.Fatalf("Store blob 1: %v", err)
	}

	content2 := []byte("second blob with entirely different content for variety")
	rid2, _, err := blob.Store(r.DB(), content2)
	if err != nil {
		t.Fatalf("Store blob 2: %v", err)
	}

	// 3. Store delta blob (against blob 1)
	content3 := []byte("first blob with MODIFIED content to make deltas meaningful in testing")
	rid3, _, err := blob.StoreDelta(r.DB(), content3, rid1)
	if err != nil {
		t.Fatalf("StoreDelta: %v", err)
	}

	// 4. Store phantom
	_, err = blob.StorePhantom(r.DB(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
	if err != nil {
		t.Fatalf("StorePhantom: %v", err)
	}

	// 5. Verify content retrieval
	got1, err := content.Expand(r.DB(), rid1)
	if err != nil {
		t.Fatalf("Expand rid1: %v", err)
	}
	if !bytes.Equal(got1, content1) {
		t.Fatalf("Expand rid1 mismatch")
	}

	got2, err := content.Expand(r.DB(), rid2)
	if err != nil {
		t.Fatalf("Expand rid2: %v", err)
	}
	if !bytes.Equal(got2, content2) {
		t.Fatalf("Expand rid2 mismatch")
	}

	// 6. Expand delta chain
	got3, err := content.Expand(r.DB(), rid3)
	if err != nil {
		t.Fatalf("Expand rid3 (delta): %v", err)
	}
	if !bytes.Equal(got3, content3) {
		t.Fatalf("Expand rid3 mismatch")
	}

	// 7. Verify blob integrity
	if err := content.Verify(r.DB(), rid1); err != nil {
		t.Fatalf("Verify rid1: %v", err)
	}
	if err := content.Verify(r.DB(), rid3); err != nil {
		t.Fatalf("Verify rid3: %v", err)
	}

	// 8. Retrieve blob by UUID via fossil CLI
	r.Close() // close before fossil accesses it

	cmd := exec.Command("fossil", "artifact", uuid1, "-R", path)
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("fossil artifact: %v", err)
	}
	if !bytes.Equal(out, content1) {
		t.Fatalf("fossil artifact mismatch: got %d bytes, want %d", len(out), len(content1))
	}

	// 9. fossil rebuild --verify must pass
	cmd = exec.Command("fossil", "rebuild", "--verify", path)
	out, err = cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("fossil rebuild --verify failed: %v\n%s", err, out)
	}
}

func TestPhaseA_FossilCreatedRepo(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test")
	}

	// fossil creates a repo, Go opens and reads it
	path := filepath.Join(t.TempDir(), "fossil-created.fossil")
	exec.Command("fossil", "new", path).CombinedOutput()

	r, err := repo.Open(path)
	if err != nil {
		t.Fatalf("Open fossil-created: %v", err)
	}
	defer r.Close()

	// Should be able to query tables
	var blobCount int
	r.DB().QueryRow("SELECT count(*) FROM blob").Scan(&blobCount)
	// A fresh fossil repo has 1 blob (the initial manifest)
	t.Logf("fossil-created repo has %d blobs", blobCount)
}
```

- [ ] **Step 2: Run the integration test**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test -v -run TestPhaseA ./...
```

Expected: PASS — all assertions pass, `fossil rebuild --verify` succeeds

- [ ] **Step 3: Commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/integration_test.go
fossil commit -m "Add Phase A end-to-end integration test with fossil CLI validation"
```

---

### Task 15: Phase A Full Validation

- [ ] **Step 1: Run complete validation suite**

```bash
cd ~/projects/EdgeSync/go-libfossil && make validate
```

Expected: `go vet`, `go test`, `go test -race` all pass

- [ ] **Step 2: Run benchmarks and record baselines**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test -bench=. -benchmem ./... > bench/baseline-$(date +%Y%m%d).txt 2>&1
```

(Create `bench/` directory first if needed: `mkdir -p bench`)

- [ ] **Step 3: Run coverage report**

```bash
cd ~/projects/EdgeSync/go-libfossil && go test -cover ./...
```

Record coverage percentages per package.

- [ ] **Step 4: Final commit**

```bash
cd ~/projects/EdgeSync
fossil add go-libfossil/bench/
fossil commit -m "Phase A complete: repo fundamentals with full fossil CLI validation"
```

---

## Deferred Items (address during or after Task 15)

- **testutil.OpenWithGo**: Add `func (r *TestRepo) OpenWithGo(t *testing.T) *repo.Repo` to testutil after Task 13 completes (testutil depends on repo package).
- **C benchmark harness**: `bench/cbench/` and `bench/Makefile` for comparing Go vs C performance. This requires compiling the libfossil amalgamation. Build as a follow-up task after Phase A core is green.
- **Per-package `_FossilValidation` tests for blob and content**: The end-to-end integration test (Task 14) covers these scenarios. Add dedicated per-package fossil validation tests as a polish pass.

## Phase A Exit Criteria Checklist

- [ ] Programmatically create a `.fossil` repo with complete schema (23+ tables)
- [ ] `fossil` recognizes it (application_id, all tables present)
- [ ] Store content as blobs (full-text and delta-compressed)
- [ ] Retrieve and verify content (expand delta chains)
- [ ] Phantom blobs tracked correctly
- [ ] `fossil rebuild --verify` passes on Go-created repos
- [ ] `fossil artifact <uuid>` retrieves blobs stored by Go
- [ ] Go can open repos created by `fossil new`
- [ ] All benchmarks recorded (hash, delta, blob store/load)
- [ ] All tests green including race detector
- [ ] Coverage reported per package
