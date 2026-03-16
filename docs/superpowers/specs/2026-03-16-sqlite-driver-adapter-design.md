# Configurable SQLite Driver Adapter

**Date:** 2026-03-16
**Status:** Draft
**Scope:** `go-libfossil/db/` package

## Problem

The `db` package hardcodes `modernc.org/sqlite`. Different deployment targets need different drivers:
- **Default (modernc):** Pure Go, compiles everywhere, slowest (5.3s/1M inserts)
- **WASM target (ncruces):** Pure Go via WASM runtime, only driver that compiles to WASM (3.0s/1M inserts)
- **Performance (mattn):** CGo, de-facto standard, best `database/sql` performance (1.5s/1M inserts)

All three implement `database/sql` and support FTS5, WAL mode, and the full SQL surface area `go-libfossil` uses.

## Non-Goals

- Supporting non-`database/sql` drivers (crawshaw, zombie). Their custom APIs would require rewriting the `Querier` interface and all consumers.
- Runtime hot-swapping of drivers. The driver is selected at build time via tags, with optional env var override.
- sqlite-vec or vector search extensions (none of the drivers ship these natively).

## Design

### Build Tag Selection

Three driver files in `go-libfossil/db/`, one active per build:

```
db/driver_modernc.go   //go:build !ncruces && !mattn
db/driver_ncruces.go   //go:build ncruces
db/driver_mattn.go     //go:build mattn
```

Build commands:
```bash
go build ./...                    # modernc (default, pure Go, no CGo)
go build -tags ncruces ./...      # ncruces (WASM-capable, pure Go)
go build -tags mattn ./...        # mattn (CGo, best database/sql perf)
```

Each driver file provides:

```go
func driverName() string          // "sqlite" or "sqlite3"
func buildDSN(path string, pragmas map[string]string) string
```

### DSN Normalization

Each driver has different PRAGMA-in-DSN syntax:

| Driver | Syntax | Example |
|--------|--------|---------|
| modernc | `_pragma=name(value)` | `?_pragma=journal_mode(WAL)` |
| ncruces | `_pragma=name(value)` | `?_pragma=journal_mode(WAL)` |
| mattn | `_name=value` | `?_journal_mode=WAL` |

The `buildDSN()` function in each driver file handles its syntax. Default pragmas applied to every connection:

```go
func defaultPragmas() map[string]string {
    return map[string]string{
        "journal_mode":  "WAL",
        "busy_timeout":  "5000",
        "foreign_keys":  "ON",
    }
}
```

This normalizes behavior across drivers. Notable: ncruces defaults to 60s busy timeout if unset — our explicit 5s prevents silent behavior differences.

### Open / OpenWith API

```go
// OpenConfig allows callers to customize driver selection and pragmas.
type OpenConfig struct {
    Driver  string            // override driver name (empty = build-tag default)
    Pragmas map[string]string // override/add pragmas (merged with defaults)
}

// Open opens a SQLite database with the build-tag-selected driver and default pragmas.
// Signature unchanged — zero impact on existing callers.
func Open(path string) (*DB, error) {
    return OpenWith(path, OpenConfig{})
}

// OpenWith opens a SQLite database with explicit configuration.
func OpenWith(path string, cfg OpenConfig) (*DB, error) {
    driver := cfg.Driver
    if driver == "" {
        driver = driverFromEnv() // EDGESYNC_SQLITE_DRIVER env var, else build-tag default
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

func driverFromEnv() string {
    if d := os.Getenv("EDGESYNC_SQLITE_DRIVER"); d != "" {
        return d
    }
    return driverName()
}
```

The `DB` struct gains a `driver` field:
```go
type DB struct {
    conn   *sql.DB
    path   string
    driver string
}

func (d *DB) Driver() string { return d.driver }
```

### What Does NOT Change

- `Querier` interface — unchanged
- `Tx` struct — unchanged
- All consumers (`blob`, `content`, `sync`, `repo`, `manifest`, `deck`) — unchanged
- `WithTx()` — unchanged
- `Open(path)` signature — unchanged (existing callers unaffected)

### Driver-Specific Notes

**modernc (default):**
- Registers as `"sqlite"`
- DSN: `file:path?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=foreign_keys(ON)`
- FTS5 compiled in by default
- No CGo required

**ncruces:**
- Registers as `"sqlite3"`
- Requires TWO blank imports: `_ "github.com/ncruces/go-sqlite3/driver"` and `_ "github.com/ncruces/go-sqlite3/embed"`
- DSN: same syntax as modernc (`_pragma=name(value)`)
- FTS5 compiled in by default
- No CGo required. Compiles to WASM.

**mattn:**
- Registers as `"sqlite3"` (conflicts with ncruces — cannot coexist, enforced by build tags)
- Requires CGo (`CGO_ENABLED=1`)
- DSN: `file:path?_journal_mode=WAL&_busy_timeout=5000&_foreign_keys=ON`
- FTS5 requires build tag: `-tags "mattn sqlite_fts5"`

## Testing

### Unit Tests

Existing test suite (`go test ./go-libfossil/...`) validates all SQL paths. Run once per driver in CI:

```yaml
strategy:
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
  - run: CGO_ENABLED=${{ matrix.cgo }} go test ${{ matrix.tags }} ./go-libfossil/... -count=1
```

### Driver Config Test

One test in `db/` verifies the active driver's config:

```go
func TestDriverConfig(t *testing.T) {
    t.Logf("driver: %s", driverName())
    dsn := buildDSN("/tmp/test.db", defaultPragmas())
    t.Logf("dsn: %s", dsn)
    if !strings.Contains(dsn, "journal_mode") {
        t.Fatal("DSN missing journal_mode pragma")
    }
}
```

### DST and Sim

Run with default driver only. They test protocol logic, not driver behavior.

## Files

### New Files

| File | Contents |
|------|----------|
| `go-libfossil/db/driver_modernc.go` | Build tag `!ncruces && !mattn`, blank import, `driverName()`, `buildDSN()` |
| `go-libfossil/db/driver_ncruces.go` | Build tag `ncruces`, two blank imports, `driverName()`, `buildDSN()` |
| `go-libfossil/db/driver_mattn.go` | Build tag `mattn`, blank import, `driverName()`, `buildDSN()` |
| `go-libfossil/db/config.go` | `OpenConfig`, `OpenWith()`, `defaultPragmas()`, `driverFromEnv()` |

### Modified Files

| File | Change |
|------|--------|
| `go-libfossil/db/db.go` | Remove `_ "modernc.org/sqlite"` import, remove inline `PRAGMA journal_mode=WAL` exec, add `driver` field to `DB` struct, `Open()` delegates to `OpenWith()` |

### Dependencies

| Driver | go.mod addition |
|--------|----------------|
| modernc | Already present |
| ncruces | `github.com/ncruces/go-sqlite3` (conditional via build tag) |
| mattn | `github.com/mattn/go-sqlite3` (conditional via build tag) |

Note: `go.mod` will list all three dependencies even though only one is active per build. The Go compiler only links the one selected by build tags.

## Benchmark Reference (from go-sqlite-bench)

| Benchmark | modernc | ncruces | mattn |
|-----------|---------|---------|-------|
| Simple 1M insert (ms) | 5288 | 3046 | 1531 |
| Simple 1M query (ms) | 760 | 910 | 1018 |
| Concurrent 8-goroutine (ms) | 2139 | 2516 | 2830 |
| Large 200K row query (ms) | 1116 | 648 | 449 |
| Dependencies | 12 | 12 | 0 (CGo) |
| CGo required | No | No | Yes |
| WASM | No | Yes | No |
