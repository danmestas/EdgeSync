# go-libfossil Repository Split

Split the EdgeSync monorepo into two repositories: `go-libfossil` (standalone library) and `EdgeSync` (application layer depending on the published library).

## Motivation

go-libfossil is a general-purpose Go library for reading, writing, and syncing Fossil SCM repositories. It has no reverse dependencies on EdgeSync and deserves its own identity as an open-source project. Separating it enables independent versioning, a clean public API surface, and allows other projects to depend on it without pulling in EdgeSync's application concerns.

## Repositories

| Repo | Module Path | Visibility | License |
|------|-------------|------------|---------|
| `go-libfossil` | `github.com/dmestas/go-libfossil` | Private initially, public later | MIT |
| `EdgeSync` | `github.com/dmestas/edgesync` (unchanged) | Private | Unchanged |

## 1. Repository Extraction

Use `git filter-repo` on a clone of EdgeSync to create the new go-libfossil repo.

### 1.1 filter-repo Command

```bash
# Clone EdgeSync to a throwaway copy
git clone /path/to/EdgeSync /tmp/go-libfossil-extract
cd /tmp/go-libfossil-extract

# Extract go-libfossil/ as root, strip co-author trailers
git filter-repo \
  --subdirectory-filter go-libfossil \
  --message-callback '
import re
message = re.sub(rb"\n\s*Co-Authored-By:.*Claude.*\n?", b"\n", message, flags=re.IGNORECASE)
message = message.rstrip() + b"\n"
return message
'
```

This preserves full commit history for all files that lived under `go-libfossil/`, with that directory becoming the repo root. All commit hashes will change (unavoidable when rewriting history).

### 1.2 Module Path Rewrite

After extraction, globally replace the module path across all `.go` and `go.mod` files:

```
github.com/dmestas/edgesync/go-libfossil → github.com/dmestas/go-libfossil
```

This affects ~145 files (469 import path occurrences).

### 1.3 Drop mattn Driver

Remove the mattn SQLite driver sub-module:
- Delete `db/driver/mattn/` directory and its `go.mod`
- Delete `internal/testdriver/mattn.go`

Rationale: modernc (pure Go, default) and ncruces (WASM) cover all current use cases. The `db.OpenWith()` API lets any consumer wire up their own driver (including mattn) without us maintaining a sub-module for it.

### 1.4 Resulting Module Structure

```
github.com/dmestas/go-libfossil                     (root go.mod)
github.com/dmestas/go-libfossil/db/driver/modernc    (sub-module)
github.com/dmestas/go-libfossil/db/driver/ncruces    (sub-module)
```

## 2. EdgeSync Repo Cleanup

After go-libfossil is published (tagged, pushed to GitHub):

### 2.1 Remove go-libfossil Directory

Delete the entire `go-libfossil/` directory from EdgeSync.

### 2.2 Update go.work

Remove the four go-libfossil workspace entries:

```diff
 use (
     .
     ./bridge
     ./dst
     ./leaf
-    ./go-libfossil
-    ./go-libfossil/db/driver/mattn
-    ./go-libfossil/db/driver/modernc
-    ./go-libfossil/db/driver/ncruces
 )
```

### 2.3 Update go.mod Files

Each module (root, leaf, bridge, dst) updates its `go.mod`:

```go
require (
    github.com/dmestas/go-libfossil v0.1.0
    github.com/dmestas/go-libfossil/db/driver/modernc v0.1.0  // or ncruces where needed
)
```

Drop any `require` for the mattn driver.

### 2.4 Rewrite Import Paths

Find-and-replace across ~80 Go files outside go-libfossil:

```
github.com/dmestas/edgesync/go-libfossil → github.com/dmestas/go-libfossil
```

Affected directories: `cmd/edgesync/`, `leaf/`, `bridge/`, `dst/`, `sim/`.

### 2.5 GOPRIVATE

Set `GOPRIVATE=github.com/dmestas/go-libfossil` in:
- Developer machines: `go env -w GOPRIVATE=github.com/dmestas/go-libfossil` or shell profile
- CI: env var in `.github/workflows/test.yml`

Remove when go-libfossil goes public (no other changes needed for that transition).

## 3. Local Development Workflow

Developers clone both repos side by side:

```
~/projects/
  go-libfossil/     ← new repo
  EdgeSync/         ← this repo
```

EdgeSync's `go.work` (gitignored) uses replace directives for local development:

```go
go 1.26.0

use (
    .
    ./bridge
    ./dst
    ./leaf
)

replace (
    github.com/dmestas/go-libfossil => ../go-libfossil
    github.com/dmestas/go-libfossil/db/driver/modernc => ../go-libfossil/db/driver/modernc
    github.com/dmestas/go-libfossil/db/driver/ncruces => ../go-libfossil/db/driver/ncruces
)
```

This gives the same edit-and-test experience as today's monorepo. CI uses the published module version (no `go.work`).

**Important:** After the split, EdgeSync's `go.work` is gitignored. The committed `go.mod` files are the source of truth for dependency versions. A `go.work.example` file (committed) documents the recommended local setup so new developers know how to configure their workspace.

## 4. CI & Build

### 4.1 go-libfossil CI (new)

`.github/workflows/test.yml`:
- `go test ./... -count=1` (root module)
- `go test ./db/driver/modernc/... -count=1`
- `go test ./db/driver/ncruces/... -count=1`
- `go vet ./...`
- Install `fossil` binary (needed by `testutil/` integration tests)
- Matrix: Go 1.26, ubuntu-latest
- No NATS, Doppler, or OTel dependencies

Makefile targets:
- `make test` — `go test ./... -count=1`
- `make test-drivers` — test modernc + ncruces variants
- `make vet` — `go vet ./...`

### 4.2 EdgeSync CI (updated)

- Remove `go test ./go-libfossil/...` from unit test step
- Remove `make drivers` target
- Add `GOPRIVATE=github.com/dmestas/go-libfossil` to workflow env
- All other steps unchanged (leaf, bridge, dst, sim)

### 4.3 EdgeSync Makefile (updated)

- Remove `go test ./go-libfossil/...` from `make test`
- Remove `make drivers`
- Add `make update-libfossil` convenience target:
  ```bash
  go get github.com/dmestas/go-libfossil@latest
  go get github.com/dmestas/go-libfossil/db/driver/modernc@latest
  go get github.com/dmestas/go-libfossil/db/driver/ncruces@latest
  go mod tidy
  ```

## 5. Versioning & Releases

### 5.1 Lockstep v0.x

All modules in go-libfossil share the same version. Three tags per release:

```bash
git tag v0.1.0
git tag db/driver/modernc/v0.1.0
git tag db/driver/ncruces/v0.1.0
git push origin --tags
```

Starting at v0.x signals API stability is not yet guaranteed, though no breaking changes are expected. Promote to v1.0 after external consumers have validated the API.

### 5.2 Release Cadence

Manual releases. Cut a new version when go-libfossil changes that EdgeSync needs are ready. Update EdgeSync's `go.mod` files and verify tests pass.

### 5.3 Going Public

1. Flip GitHub repo from private to public
2. Remove `GOPRIVATE` from CI and dev machines
3. Go module proxy picks it up automatically
4. No code changes or version bump needed

## 6. Open Source Prep

### 6.1 License

MIT license added to go-libfossil repo root.

### 6.2 README

Full rewrite of `README.md` as a standalone library introduction:
- What it is (pure Go library for Fossil SCM repos)
- Package overview table
- Quick start code snippets (open repo, read artifacts, sync)
- SQLite driver selection (modernc default, ncruces for WASM)
- Installation (`go get`)
- License

### 6.3 Source Cleanup

Before publishing:
- Strip comments referencing Linear ticket numbers (CDG-xxx)
- Verify `bench/baseline-20260314.txt` contains no sensitive data
- Remove any EdgeSync-specific references from code comments

Do NOT:
- Rewrite Fossil protocol documentation in comments (valuable to consumers)
- Rename packages or restructure the API
- Add badges or social metadata (defer until public)

## 7. testutil

`testutil/` stays in go-libfossil as a public package. Both go-libfossil's own tests and EdgeSync's integration tests (leaf, bridge, sim) import it as `github.com/dmestas/go-libfossil/testutil`. Single source of truth for Fossil CLI test helpers.

## 8. Execution Order

1. Create `github.com/dmestas/go-libfossil` repo on GitHub (private)
2. Run `git filter-repo` extraction
3. Rewrite module paths in extracted repo
4. Drop mattn driver
5. Add MIT license, write README
6. Clean up source (Linear refs, EdgeSync refs in comments)
7. Push to GitHub, tag `v0.1.0` (all three modules)
8. In EdgeSync: delete `go-libfossil/`, rewrite imports, update `go.mod` files
9. Update EdgeSync CI and Makefile
10. Set up `GOPRIVATE`, verify CI passes
11. Create gitignored `go.work` template for local dev
12. Update EdgeSync's CLAUDE.md to reflect new structure
