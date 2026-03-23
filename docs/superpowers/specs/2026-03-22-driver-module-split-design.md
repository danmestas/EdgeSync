# SQLite Driver Module Split Design

**Date:** 2026-03-22
**Status:** Approved

## Problem

`go-libfossil/go.mod` carries transitive dependencies for all three SQLite drivers (modernc, ncruces, mattn) even though any given build uses exactly one. This bloats go.mod with ~16 indirect dependencies. Consumer modules (leaf, bridge, dst) inherit all three drivers' dependency trees despite only needing one.

ncruces v0.33.0 switched to wasm2go (no more wazero), but stale deps like `tetratelabs/wazero` lingered until a manual `go mod tidy`.

The root cause: build-tagged driver files in `db/` import all three SQLite libraries directly, so `go mod tidy` resolves all of them regardless of which tag is active.

## Design: Driver Sub-Modules with Registration API

Split each SQLite driver into its own Go sub-module under `go-libfossil/db/driver/`. The `db` package becomes driver-agnostic with a registration API. Consumers select their driver by importing the appropriate sub-module.

### Registration API

New file `db/register.go`:

```go
package db

// DriverConfig defines a SQLite driver's name and DSN builder.
type DriverConfig struct {
    Name     string
    BuildDSN func(path string, pragmas map[string]string) string
}

var registered *DriverConfig

// Register registers a SQLite driver for use by Open/OpenWith.
// Must be called exactly once (typically from a driver package's init()).
// Panics if called more than once.
func Register(cfg DriverConfig) {
    if registered != nil {
        panic("db: driver already registered")
    }
    if cfg.Name == "" {
        panic("db: driver name must not be empty")
    }
    if cfg.BuildDSN == nil {
        panic("db: BuildDSN must not be nil")
    }
    registered = &cfg
}
```

`Open()`/`OpenWith()` use `registered.Name` and `registered.BuildDSN` instead of the build-tagged `driverName()`/`buildDSN()` functions. Panics at open time if no driver is registered (fail-fast).

The `EDGESYNC_SQLITE_DRIVER` env var override is removed — driver selection is now explicit via import.

### Driver Sub-Modules

Three new Go modules, each with its own `go.mod`:

**`go-libfossil/db/driver/modernc/`**

```
go.mod: requires modernc.org/sqlite, go-libfossil
```

```go
// modernc.go
package modernc

import (
    "fmt"
    "strings"

    "github.com/dmestas/edgesync/go-libfossil/db"
    _ "modernc.org/sqlite"
)

func init() {
    db.Register(db.DriverConfig{
        Name:     "sqlite",
        BuildDSN: buildDSN,
    })
}

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

**`go-libfossil/db/driver/ncruces/`**

```
go.mod: requires ncruces/go-sqlite3, go-libfossil
```

Two build-tagged files:

```go
// ncruces.go (//go:build !js)
package ncruces

import (
    "fmt"
    "strings"

    "github.com/dmestas/edgesync/go-libfossil/db"
    _ "github.com/ncruces/go-sqlite3/driver"
)

func init() {
    db.Register(db.DriverConfig{
        Name:     "sqlite3",
        BuildDSN: buildDSN,
    })
}

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

```go
// ncruces_js.go (//go:build js)
package ncruces

import (
    "fmt"
    "strings"

    "github.com/dmestas/edgesync/go-libfossil/db"
    _ "github.com/danmestas/go-sqlite3-opfs"
    _ "github.com/ncruces/go-sqlite3/driver"
)

func init() {
    db.Register(db.DriverConfig{
        Name:     "sqlite3",
        BuildDSN: buildDSN,
    })
}

func buildDSN(path string, pragmas map[string]string) string {
    var parts []string
    parts = append(parts, "vfs=opfs")
    for k, v := range pragmas {
        parts = append(parts, fmt.Sprintf("_pragma=%s(%s)", k, v))
    }
    return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
```

**`go-libfossil/db/driver/mattn/`**

```
go.mod: requires mattn/go-sqlite3, go-libfossil
```

```go
// mattn.go
package mattn

import (
    "fmt"
    "strings"

    "github.com/dmestas/edgesync/go-libfossil/db"
    _ "github.com/mattn/go-sqlite3"
)

func init() {
    db.Register(db.DriverConfig{
        Name:     "sqlite3",
        BuildDSN: buildDSN,
    })
}

func buildDSN(path string, pragmas map[string]string) string {
    if len(pragmas) == 0 {
        return path
    }
    var parts []string
    for k, v := range pragmas {
        parts = append(parts, fmt.Sprintf("_%s=%s", k, v))
    }
    return fmt.Sprintf("file:%s?%s", path, strings.Join(parts, "&"))
}
```

### Test Driver Selection

A new `internal/testdriver/` package under go-libfossil uses build tags to select which driver sub-module tests run against:

```go
// internal/testdriver/modernc.go
//go:build !test_ncruces && !test_mattn

package testdriver

import _ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

```go
// internal/testdriver/ncruces.go
//go:build test_ncruces

package testdriver

import _ "github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces"
```

```go
// internal/testdriver/mattn.go
//go:build test_mattn

package testdriver

import _ "github.com/dmestas/edgesync/go-libfossil/db/driver/mattn"
```

All test files that currently do `_ "modernc.org/sqlite"` change to:

```go
import _ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
```

### Makefile Changes

Update both driver matrix targets:

```makefile
drivers:
    @echo "=== modernc (default) ==="
    go test -buildvcs=false ./go-libfossil/... -count=1
    @echo "=== ncruces ==="
    go test -buildvcs=false -tags test_ncruces ./go-libfossil/... -count=1
    @echo "=== mattn ==="
    CGO_ENABLED=1 go test -buildvcs=false -tags test_mattn ./go-libfossil/... -count=1
    @echo "=== all drivers passed ==="
```

The `dst-drivers` target also updates its tag loop:

```makefile
dst-drivers:
    for driver in "default:" "ncruces:-tags=test_ncruces" "mattn:-tags=test_mattn"; do \
        ...
    done
```

The `wasm-wasi` and `wasm-browser` targets drop `-tags ncruces` since driver selection is now via imports in the entry points.

### go.work Changes

Add the three driver modules:

```
go 1.26.0

use (
    .
    ./bridge
    ./dst
    ./go-libfossil
    ./go-libfossil/db/driver/mattn
    ./go-libfossil/db/driver/modernc
    ./go-libfossil/db/driver/ncruces
    ./leaf
)
```

### Consumer Module Changes

Each consumer imports the driver it needs:

| Module | Current | After |
|--------|---------|-------|
| `cmd/edgesync/repo_open.go` | `_ "modernc.org/sqlite"` | `_ ".../db/driver/modernc"` |
| `leaf/cmd/leaf/main.go` | (via go-libfossil build tag) | `_ ".../db/driver/modernc"` |
| `leaf/cmd/wasm/main.go` | (via `-tags ncruces`) | `_ ".../db/driver/ncruces"` |
| `dst/` | (via go-libfossil) | `_ ".../db/driver/modernc"` |
| `sim/` | (via go-libfossil) | `_ ".../db/driver/modernc"` |

WASM builds no longer need `-tags ncruces`. Each entry point imports the driver it needs directly. The WASM entry point (`leaf/cmd/wasm/`) imports the ncruces driver; the native entry point (`leaf/cmd/leaf/`) imports modernc.

Consumer go.mod files will only pull in the transitive deps of the one driver they import.

### Files Removed

- `go-libfossil/db/driver_modernc.go` — logic moves to `db/driver/modernc/`
- `go-libfossil/db/driver_ncruces.go` — logic moves to `db/driver/ncruces/`
- `go-libfossil/db/driver_ncruces_js.go` — logic moves to `db/driver/ncruces/`
- `go-libfossil/db/driver_mattn.go` — logic moves to `db/driver/mattn/`

### Files Modified

- `go-libfossil/db/db.go` — use `registered.Name`/`registered.BuildDSN` instead of `driverName()`/`buildDSN()`
- `go-libfossil/db/config.go` — remove `driverFromEnv()`, `driverName()` no longer needed
- `go-libfossil/db/db_test.go` — `TestDriverConfig` rewritten: verify `registered` is non-nil and `registered.Name`/`registered.BuildDSN` work correctly (replaces calls to removed `driverName()`/`buildDSN()`)
- `go-libfossil/go.mod` — drops direct SQLite driver deps; gains driver sub-module deps (indirect via testdriver)
- `go.work` — add 3 driver module paths
- `Makefile` — update `drivers` target tags
- All `_test.go` files with `_ "modernc.org/sqlite"` — change to `_ ".../internal/testdriver"`
- `cmd/edgesync/repo_open.go` — change import to `_ ".../db/driver/modernc"`

### Files Created

- `go-libfossil/db/register.go` — `Register()`, `DriverConfig`
- `go-libfossil/db/driver/modernc/go.mod`, `modernc.go`
- `go-libfossil/db/driver/ncruces/go.mod`, `ncruces.go`, `ncruces_js.go`
- `go-libfossil/db/driver/mattn/go.mod`, `mattn.go`
- `go-libfossil/internal/testdriver/modernc.go`, `ncruces.go`, `mattn.go`

## Trade-offs

**Benefits:**
- db/ production code has zero SQLite driver dependencies
- Consumer modules (leaf, bridge, dst) only pull in the one driver they use
- Explicit driver selection — no hidden build-tag magic in production code
- `make drivers` truly isolates each driver
- Follows `database/sql` driver registration pattern — idiomatic Go

**Costs:**
- 3 new modules in go.work (5 -> 8)
- go-libfossil/go.mod still has all 3 driver sub-modules as indirect deps (for tests via internal/testdriver)
- One-time migration of all test file imports

## Out of Scope

- Changing the WASM build-tag strategy (`config_wasm.go`, `config_default.go`) — these handle pragma overrides, not driver selection
- Changing how `leaf/agent/` selects its driver for production — that's a consumer concern
- Removing the `OpenConfig.Driver` field — still useful for explicit override
