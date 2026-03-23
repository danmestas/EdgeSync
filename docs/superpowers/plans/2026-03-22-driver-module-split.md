# SQLite Driver Module Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split three SQLite drivers from build-tagged files in `db/` into separate Go sub-modules with a registration API, so consumers only pull in the one driver they need.

**Architecture:** Add `db.Register()` API. Move each driver to `go-libfossil/db/driver/{modernc,ncruces,mattn}/` as independent Go modules. Tests use `internal/testdriver/` with build tags for the driver matrix.

**Tech Stack:** Go modules, `go.work` workspace, `database/sql` driver registration pattern

**Spec:** `docs/superpowers/specs/2026-03-22-driver-module-split-design.md`

---

### Task 1: Create branch and add Registration API

**Files:**
- Create: `go-libfossil/db/register.go`
- Test: `go-libfossil/db/db_test.go` (modify existing)

- [ ] **Step 1: Create feature branch**

```bash
git checkout -b refactor/driver-module-split
```

- [ ] **Step 2: Write `db/register.go`**

Create `go-libfossil/db/register.go`:

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

// resetRegistration is for testing only.
func resetRegistration() {
	registered = nil
}
```

- [ ] **Step 3: Write test for registration API**

Add to `go-libfossil/db/db_test.go`, replacing `TestDriverConfig` (lines 227-242):

```go
func TestRegisterDriver(t *testing.T) {
	// Driver should already be registered by testdriver import.
	if registered == nil {
		t.Fatal("no driver registered")
	}
	if registered.Name == "" {
		t.Fatal("registered driver name is empty")
	}
	t.Logf("active driver: %s", registered.Name)

	dsn := registered.BuildDSN("/tmp/test.db", defaultPragmas())
	t.Logf("DSN: %s", dsn)
	if !strings.Contains(dsn, "journal_mode") {
		t.Fatal("DSN missing journal_mode pragma")
	}
	if !strings.Contains(dsn, "busy_timeout") {
		t.Fatal("DSN missing busy_timeout pragma")
	}
}
```

Note: Double-registration is guarded by a panic, not a subtle bug — no test needed.

- [ ] **Step 4: Run tests — they should still pass**

The old `driverName()`/`buildDSN()` functions still exist (driver files not deleted yet). The new `register.go` is additive.

```bash
go test -buildvcs=false ./go-libfossil/db/ -run TestRegisterDriver -v
```

Expected: FAIL — `registered` is nil because no driver calls `Register()` yet. That's correct — we'll wire it up in Task 3.

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/db/register.go go-libfossil/db/db_test.go
git commit -m "feat(db): add driver registration API"
```

---

### Task 2: Create driver sub-modules

**Files:**
- Create: `go-libfossil/db/driver/modernc/go.mod`, `go-libfossil/db/driver/modernc/modernc.go`
- Create: `go-libfossil/db/driver/ncruces/go.mod`, `go-libfossil/db/driver/ncruces/ncruces.go`, `go-libfossil/db/driver/ncruces/ncruces_js.go`
- Create: `go-libfossil/db/driver/mattn/go.mod`, `go-libfossil/db/driver/mattn/mattn.go`
- Modify: `go.work`

- [ ] **Step 1: Create modernc driver module**

Create `go-libfossil/db/driver/modernc/go.mod`:

```
module github.com/dmestas/edgesync/go-libfossil/db/driver/modernc

go 1.26.0

require (
	github.com/dmestas/edgesync/go-libfossil v0.0.0
	modernc.org/sqlite v1.46.1
)

replace github.com/dmestas/edgesync/go-libfossil => ../../../
```

Create `go-libfossil/db/driver/modernc/modernc.go`:

```go
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

- [ ] **Step 2: Create ncruces driver module**

Create `go-libfossil/db/driver/ncruces/go.mod`:

```
module github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces

go 1.26.0

require (
	github.com/dmestas/edgesync/go-libfossil v0.0.0
	github.com/danmestas/go-sqlite3-opfs v0.2.0
	github.com/ncruces/go-sqlite3 v0.33.0
)

replace github.com/dmestas/edgesync/go-libfossil => ../../../
```

Create `go-libfossil/db/driver/ncruces/ncruces.go`:

```go
//go:build !js

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

Create `go-libfossil/db/driver/ncruces/ncruces_js.go`:

```go
//go:build js

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

- [ ] **Step 3: Create mattn driver module**

Create `go-libfossil/db/driver/mattn/go.mod`:

```
module github.com/dmestas/edgesync/go-libfossil/db/driver/mattn

go 1.26.0

require (
	github.com/dmestas/edgesync/go-libfossil v0.0.0
	github.com/mattn/go-sqlite3 v1.14.34
)

replace github.com/dmestas/edgesync/go-libfossil => ../../../
```

Create `go-libfossil/db/driver/mattn/mattn.go`:

```go
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

- [ ] **Step 4: Update `go.work`**

Replace contents of `go.work`:

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

- [ ] **Step 5: Run `go mod tidy` in each new module**

```bash
cd go-libfossil/db/driver/modernc && go mod tidy && cd -
cd go-libfossil/db/driver/ncruces && go mod tidy && cd -
cd go-libfossil/db/driver/mattn && go mod tidy && cd -
```

- [ ] **Step 6: Verify each module compiles**

```bash
go build -buildvcs=false ./go-libfossil/db/driver/modernc/
go build -buildvcs=false ./go-libfossil/db/driver/ncruces/
CGO_ENABLED=1 go build -buildvcs=false ./go-libfossil/db/driver/mattn/
```

Expected: all succeed with no errors.

- [ ] **Step 7: Commit**

```bash
git add go-libfossil/db/driver/ go.work
git commit -m "feat(db): add driver sub-modules for modernc, ncruces, mattn"
```

---

### Task 3: Create testdriver package and migrate test imports

**Files:**
- Create: `go-libfossil/internal/testdriver/modernc.go`
- Create: `go-libfossil/internal/testdriver/ncruces.go`
- Create: `go-libfossil/internal/testdriver/mattn.go`
- Modify: `go-libfossil/db/db_test.go` (remove old `TestDriverConfig`, use testdriver)
- Modify: `go-libfossil/bisect/bisect_test.go`
- Modify: `go-libfossil/path/path_test.go`
- Modify: `go-libfossil/stash/stash_test.go`
- Modify: `go-libfossil/undo/undo_test.go`
- Modify: `go-libfossil/integration_cli_test.go`

- [ ] **Step 1: Create testdriver package**

Create `go-libfossil/internal/testdriver/modernc.go`:

```go
//go:build !test_ncruces && !test_mattn

package testdriver

import _ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

Create `go-libfossil/internal/testdriver/ncruces.go`:

```go
//go:build test_ncruces

package testdriver

import _ "github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces"
```

Create `go-libfossil/internal/testdriver/mattn.go`:

```go
//go:build test_mattn

package testdriver

import _ "github.com/dmestas/edgesync/go-libfossil/db/driver/mattn"
```

- [ ] **Step 2: Migrate db/db_test.go**

In `go-libfossil/db/db_test.go`:

1. Add import: `_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"`
2. Replace `TestDriverConfig` (lines 227-242) with `TestRegisterDriver` from Task 1
3. Remove `"os/exec"` import if only used by the fossil test (check — it's used by `TestCreateRepoSchema_FossilValidation`, so keep it)

The test file currently has no driver import (it's in `db` package which had `driver_modernc.go`). After removing the build-tagged driver files, the `db` package tests need `testdriver`.

- [ ] **Step 3: Migrate bisect/bisect_test.go**

In `go-libfossil/bisect/bisect_test.go` line 9, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
```

- [ ] **Step 4: Migrate path/path_test.go**

In `go-libfossil/path/path_test.go` line 8, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
```

- [ ] **Step 5: Migrate stash/stash_test.go**

In `go-libfossil/stash/stash_test.go` line 13, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
```

- [ ] **Step 6: Migrate undo/undo_test.go**

In `go-libfossil/undo/undo_test.go` line 11, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
```

- [ ] **Step 7: Migrate integration_cli_test.go**

In `go-libfossil/integration_cli_test.go` line 20, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
```

- [ ] **Step 8: Run tests to verify testdriver wiring**

```bash
go test -buildvcs=false ./go-libfossil/db/ -run TestRegisterDriver -v
```

Expected: PASS — testdriver imports modernc driver module, which calls `Register()`.

```bash
go test -buildvcs=false ./go-libfossil/... -short -count=1
```

Expected: PASS — all packages use testdriver now.

- [ ] **Step 9: Commit**

```bash
git add go-libfossil/internal/testdriver/ go-libfossil/db/db_test.go \
  go-libfossil/bisect/bisect_test.go go-libfossil/path/path_test.go \
  go-libfossil/stash/stash_test.go go-libfossil/undo/undo_test.go \
  go-libfossil/integration_cli_test.go
git commit -m "refactor(db): migrate test imports to internal/testdriver"
```

---

### Task 4: Wire registration into db.Open and remove old driver files

**Files:**
- Modify: `go-libfossil/db/db.go` (lines 26-28, 47)
- Modify: `go-libfossil/db/config.go` (remove `driverFromEnv()`)
- Delete: `go-libfossil/db/driver_modernc.go`
- Delete: `go-libfossil/db/driver_ncruces.go`
- Delete: `go-libfossil/db/driver_ncruces_js.go`
- Delete: `go-libfossil/db/driver_mattn.go`

- [ ] **Step 1: Modify `db/db.go` to use registered driver**

In `go-libfossil/db/db.go`, replace `OpenWith` function (lines 22-73):

Replace lines 26-28:
```go
	driver := cfg.Driver
	if driver == "" {
		driver = driverFromEnv()
	}
```

with:

```go
	if registered == nil {
		panic("db.OpenWith: no driver registered — import a driver package (e.g., _ \"github.com/dmestas/edgesync/go-libfossil/db/driver/modernc\")")
	}
	driver := cfg.Driver
	if driver == "" {
		driver = registered.Name
	}
```

Replace line 47:
```go
		dsn = buildDSN(path, pragmas)
```

with:

```go
		dsn = registered.BuildDSN(path, pragmas)
```

Also update the `Open` doc comment (line 16) from "build-tag-selected driver" to "registered driver":
```go
// Open opens a SQLite database with the registered driver and default pragmas.
```

- [ ] **Step 2: Remove `driverFromEnv()` from config.go**

In `go-libfossil/db/config.go`, delete lines 1-3 (`import "os"`) and lines 23-28 (`driverFromEnv` function). Update the `OpenConfig.Driver` comment (line 7):

Replace:
```go
import "os"
```
with:
```go
// (no imports needed)
```

Actually, just remove the `"os"` import and the `driverFromEnv` function. The file becomes:

```go
package db

// OpenConfig allows callers to customize driver selection and pragmas.
type OpenConfig struct {
	Driver  string            // override driver name (empty = use registered driver)
	Pragmas map[string]string // additional/override pragmas (merged with defaults)
}

func defaultPragmas() map[string]string {
	m := map[string]string{
		"journal_mode": "WAL",
		"busy_timeout": "5000",
		"foreign_keys": "OFF", // ncruces enables FK by default; normalize to OFF for schema compat
	}
	for k, v := range wasmPragmaOverrides() {
		m[k] = v
	}
	return m
}
```

- [ ] **Step 3: Delete old driver files**

```bash
rm go-libfossil/db/driver_modernc.go
rm go-libfossil/db/driver_ncruces.go
rm go-libfossil/db/driver_ncruces_js.go
rm go-libfossil/db/driver_mattn.go
```

- [ ] **Step 4: Run go-libfossil tests**

```bash
go test -buildvcs=false ./go-libfossil/... -short -count=1
```

Expected: PASS — `testdriver` registers the driver, `Open()` uses `registered`.

- [ ] **Step 5: Commit**

```bash
git add go-libfossil/db/db.go go-libfossil/db/config.go
git rm go-libfossil/db/driver_modernc.go go-libfossil/db/driver_ncruces.go \
  go-libfossil/db/driver_ncruces_js.go go-libfossil/db/driver_mattn.go
git commit -m "refactor(db): wire registration API, remove build-tagged driver files"
```

---

### Task 5: Clean up go.mod files

**Files:**
- Modify: `go-libfossil/go.mod`

- [ ] **Step 1: Run go mod tidy on go-libfossil**

```bash
cd go-libfossil && go mod tidy && cd -
```

This should drop the direct `modernc.org/sqlite`, `ncruces/go-sqlite3`, `mattn/go-sqlite3`, and `danmestas/go-sqlite3-opfs` requires. They'll reappear as indirect deps (via the driver sub-modules pulled in by `internal/testdriver`).

- [ ] **Step 2: Verify go.mod is cleaner**

Read `go-libfossil/go.mod`. The direct requires should now only be:
- `golang.org/x/crypto` (for SHA3)
- The three driver sub-modules (via testdriver)

- [ ] **Step 3: Run tests again**

```bash
go test -buildvcs=false ./go-libfossil/... -short -count=1
```

Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add go-libfossil/go.mod go-libfossil/go.sum
git commit -m "chore(go-libfossil): tidy go.mod after driver split"
```

---

### Task 6: Migrate consumer modules

**Files:**
- Modify: `cmd/edgesync/repo_open.go` (line 10)
- Modify: `cmd/edgesync/repo_co_test.go` (line 13)
- Modify: `leaf/cmd/leaf/main.go` (add import)
- Modify: `leaf/cmd/wasm/main.go` (add import)
- Modify: `dst/e2e_test.go` (add import)
- Modify: `sim/equivalence_test.go` (add import)

- [ ] **Step 1: Migrate cmd/edgesync/repo_open.go**

In `cmd/edgesync/repo_open.go` line 10, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

- [ ] **Step 2: Migrate cmd/edgesync/repo_co_test.go**

In `cmd/edgesync/repo_co_test.go` line 13, replace:

```go
_ "modernc.org/sqlite"
```

with:

```go
_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

- [ ] **Step 3: Add driver import to leaf/cmd/leaf/main.go**

Add to the import block in `leaf/cmd/leaf/main.go`:

```go
_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

- [ ] **Step 4: Add driver import to leaf/cmd/wasm/main.go**

Add to the import block in `leaf/cmd/wasm/main.go`:

```go
_ "github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces"
```

- [ ] **Step 5: Add driver import to dst/e2e_test.go**

DST tests call `repo.Create()` which calls `db.Open()`. Add to the import block in `dst/e2e_test.go`:

```go
_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

- [ ] **Step 6: Add driver import to sim/equivalence_test.go**

Sim tests call `repo.Create()` and `repo.Open()`. Add to the import block in `sim/equivalence_test.go`:

```go
_ "github.com/dmestas/edgesync/go-libfossil/db/driver/modernc"
```

Note: sim is a package in the root module, so its deps are covered by the root `go.mod`.

- [ ] **Step 7: Run go mod tidy on all affected modules**

```bash
go mod tidy
cd leaf && go mod tidy && cd -
cd dst && go mod tidy && cd -
cd bridge && go mod tidy && cd -
```

The root module needs the modernc driver sub-module for cmd/edgesync and sim. The leaf module needs modernc (native) and ncruces (wasm). Bridge and dst need tidy to update their indirect dep graphs.

- [ ] **Step 8: Verify builds**

```bash
go build -buildvcs=false ./cmd/edgesync/
cd leaf && go build -buildvcs=false ./cmd/leaf/ && cd -
```

Expected: both succeed.

- [ ] **Step 9: Run full test suite**

```bash
go test -buildvcs=false ./go-libfossil/... -short -count=1
go test -buildvcs=false ./leaf/... -short -count=1
go test -buildvcs=false ./dst/ -run 'TestScenario|TestE2E|TestMockFossil' -count=1
go test -buildvcs=false ./sim/ -run 'TestFaultProxy|TestGenerateSchedule|TestBuggify' -count=1
```

Expected: PASS.

- [ ] **Step 10: Commit**

```bash
git add cmd/edgesync/repo_open.go cmd/edgesync/repo_co_test.go \
  leaf/cmd/leaf/main.go leaf/cmd/wasm/main.go \
  dst/e2e_test.go sim/equivalence_test.go \
  go.mod go.sum leaf/go.mod leaf/go.sum dst/go.mod dst/go.sum bridge/go.mod bridge/go.sum
git commit -m "refactor: migrate consumer modules to driver sub-module imports"
```

---

### Task 7: Update Makefile

**Files:**
- Modify: `Makefile` (lines 17, 20, 82, 114-121)

- [ ] **Step 1: Update wasm targets**

In `Makefile`, line 17, replace:
```makefile
	GOOS=wasip1 GOARCH=wasm go build -buildvcs=false -tags ncruces -o bin/leaf.wasm ./leaf/cmd/leaf/
```
with:
```makefile
	GOOS=wasip1 GOARCH=wasm go build -buildvcs=false -o bin/leaf.wasm ./leaf/cmd/leaf/
```

Line 20, replace:
```makefile
	GOOS=js GOARCH=wasm go build -buildvcs=false -tags ncruces -o bin/leaf-browser.wasm ./leaf/cmd/wasm/
```
with:
```makefile
	GOOS=js GOARCH=wasm go build -buildvcs=false -o bin/leaf-browser.wasm ./leaf/cmd/wasm/
```

- [ ] **Step 2: Update dst-drivers target**

In `Makefile`, line 82, replace:
```makefile
	for driver in "default:" "ncruces:-tags=ncruces" "mattn:-tags=mattn"; do \
```
with:
```makefile
	for driver in "default:" "ncruces:-tags=test_ncruces" "mattn:-tags=test_mattn"; do \
```

- [ ] **Step 3: Update drivers target**

In `Makefile`, replace lines 114-121:
```makefile
drivers:
	@echo "=== modernc (default) ==="
	go test ./go-libfossil/... -count=1
	@echo "=== ncruces ==="
	go test -tags ncruces ./go-libfossil/... -count=1
	@echo "=== mattn ==="
	CGO_ENABLED=1 go test -tags mattn ./go-libfossil/... -count=1
	@echo "=== all drivers passed ==="
```
with:
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

- [ ] **Step 4: Test the driver matrix**

```bash
make drivers
```

Expected: all 3 driver runs pass.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "chore: update Makefile for driver sub-module split"
```

---

### Task 8: Run full CI-equivalent validation

- [ ] **Step 1: Run make test (CI equivalent)**

```bash
make test
```

Expected: PASS.

- [ ] **Step 2: Run DST quick**

```bash
make dst
```

Expected: PASS (8 seeds).

- [ ] **Step 3: Run make build**

```bash
make build
```

Expected: all 3 binaries (edgesync, leaf, bridge) built.

- [ ] **Step 4: Verify go.work.sum is updated**

```bash
git add go.work.sum
```

- [ ] **Step 5: Final commit if any loose files**

```bash
git status
# If anything unstaged:
git add -A && git commit -m "chore: update go.work.sum after driver split"
```
