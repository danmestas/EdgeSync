# go-libfossil Repository Split — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract `go-libfossil/` from the EdgeSync monorepo into a standalone repo (`github.com/dmestas/go-libfossil`) and rewire EdgeSync to consume it as an external dependency.

**Architecture:** Clone EdgeSync, use `git filter-repo` to extract the `go-libfossil/` subtree with history, rewrite module paths, publish to GitHub, then clean up EdgeSync to depend on the published module. Local dev uses `go.work` replace directives.

**Tech Stack:** git filter-repo, Go modules, GitHub CLI (`gh`)

---

## Task 1: Create the go-libfossil GitHub repo

**Files:** None (GitHub operation only)

- [ ] **Step 1: Create a private repo on GitHub**

```bash
gh repo create dmestas/go-libfossil --private --description "Pure Go library for reading, writing, and syncing Fossil SCM repositories"
```

- [ ] **Step 2: Verify the repo exists**

```bash
gh repo view dmestas/go-libfossil
```

Expected: Shows the repo metadata with "Private" visibility.

---

## Task 2: Extract go-libfossil with git filter-repo

**Prerequisites:** `git-filter-repo` must be installed (`brew install git-filter-repo` or `pip install git-filter-repo`).

This task operates on a throwaway clone — it does NOT modify the EdgeSync repo.

- [ ] **Step 1: Install git-filter-repo if not present**

```bash
which git-filter-repo || brew install git-filter-repo
```

- [ ] **Step 2: Clone EdgeSync to a temporary directory**

```bash
git clone /Users/dmestas/projects/EdgeSync /tmp/go-libfossil-extract
cd /tmp/go-libfossil-extract
```

- [ ] **Step 3: Run git filter-repo**

This extracts only the `go-libfossil/` directory (making it the repo root) and strips all `Co-Authored-By` lines mentioning Claude from commit messages:

```bash
git filter-repo \
  --subdirectory-filter go-libfossil \
  --message-callback '
import re
message = re.sub(rb"\n\s*Co-Authored-By:.*Claude.*\n?", b"\n", message, flags=re.IGNORECASE)
message = message.rstrip() + b"\n"
return message
'
```

- [ ] **Step 4: Verify the extraction**

```bash
# Should show go-libfossil packages at root (no go-libfossil/ prefix)
ls -la
# Should show: go.mod, README.md, blob/, content/, db/, delta/, hash/, etc.

# Verify history is preserved
git log --oneline | head -20

# Verify co-author lines are stripped
git log --all --format="%B" | grep -i "co-authored" | head -5
# Expected: no output (all stripped)
```

- [ ] **Step 5: Commit checkpoint — do NOT commit yet, more changes needed in Tasks 3-6**

Stay in `/tmp/go-libfossil-extract` for the next tasks.

---

## Task 3: Rewrite module paths

**Files (in `/tmp/go-libfossil-extract`):**
- Modify: All `*.go` files and `go.mod` files (~145 files)

The old module path `github.com/dmestas/edgesync/go-libfossil` must become `github.com/dmestas/go-libfossil` everywhere.

- [ ] **Step 1: Replace in all Go and go.mod files**

```bash
cd /tmp/go-libfossil-extract

# Find and replace in all .go and go.mod files
find . -type f \( -name '*.go' -o -name 'go.mod' \) -exec \
  sed -i '' 's|github.com/dmestas/edgesync/go-libfossil|github.com/dmestas/go-libfossil|g' {} +
```

- [ ] **Step 2: Verify the rewrite**

```bash
# Should find zero occurrences of old path
grep -r 'edgesync/go-libfossil' --include='*.go' --include='go.mod' .
# Expected: no output

# Should find many occurrences of new path
grep -r 'dmestas/go-libfossil' --include='*.go' --include='go.mod' . | head -5
# Expected: lines showing the new module path
```

- [ ] **Step 3: Verify go.mod is correct**

```bash
cat go.mod
```

Expected first line: `module github.com/dmestas/go-libfossil`

```bash
cat db/driver/modernc/go.mod
```

Expected first line: `module github.com/dmestas/go-libfossil/db/driver/modernc`

```bash
cat db/driver/ncruces/go.mod
```

Expected first line: `module github.com/dmestas/go-libfossil/db/driver/ncruces`

---

## Task 4: Drop mattn driver and clean up go.mod

**Files (in `/tmp/go-libfossil-extract`):**
- Delete: `db/driver/mattn/` (entire directory)
- Delete: `internal/testdriver/mattn.go`
- Modify: `go.mod` (remove mattn require + replace)

- [ ] **Step 1: Delete mattn driver directory**

```bash
cd /tmp/go-libfossil-extract
rm -rf db/driver/mattn
```

- [ ] **Step 2: Delete mattn test driver**

```bash
rm internal/testdriver/mattn.go
```

- [ ] **Step 3: Remove mattn from root go.mod**

Edit `go.mod` to remove the mattn require and replace lines. The file should have:

```go
require (
	github.com/dmestas/go-libfossil/db/driver/modernc v0.0.0-00010101000000-000000000000
	github.com/dmestas/go-libfossil/db/driver/ncruces v0.0.0-00010101000000-000000000000
	golang.org/x/crypto v0.49.0
)
```

And the replace block should only have:

```go
replace (
	github.com/dmestas/go-libfossil/db/driver/modernc => ./db/driver/modernc
	github.com/dmestas/go-libfossil/db/driver/ncruces => ./db/driver/ncruces
)
```

Remove the mattn line from both blocks. Also remove `github.com/mattn/go-sqlite3` from the indirect dependencies since no driver imports it anymore.

- [ ] **Step 4: Run go mod tidy**

```bash
go mod tidy
```

- [ ] **Step 5: Verify the module compiles**

```bash
go build ./...
```

Expected: clean build with no errors.

---

## Task 5: Add MIT license and rewrite README

**Files (in `/tmp/go-libfossil-extract`):**
- Create: `LICENSE`
- Modify: `README.md`

- [ ] **Step 1: Create MIT LICENSE file**

```bash
cd /tmp/go-libfossil-extract
```

Write `LICENSE` with the following content (replace year with 2026, name with Dan Mestas):

```
MIT License

Copyright (c) 2026 Dan Mestas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

- [ ] **Step 2: Rewrite README.md**

Replace the entire `README.md` with a standalone library introduction. Key changes from the current version:
- Update all import paths from `github.com/dmestas/edgesync/go-libfossil/...` to `github.com/dmestas/go-libfossil/...`
- Remove mattn from the SQLite drivers table
- Remove the reference to `leaf/telemetry` in the Observability section (say "consumers implement Observer with their own telemetry stack" instead)
- Add an Installation section: `go get github.com/dmestas/go-libfossil`
- Add a License section at the bottom: "MIT — see LICENSE"
- Remove `EDGESYNC_SQLITE_DRIVER` env var reference (EdgeSync-specific)
- Remove `-buildvcs=false` from the build section (that was an EdgeSync dual-VCS workaround)

```markdown
# go-libfossil

Pure Go library for reading, writing, and syncing [Fossil](https://fossil-scm.org) repositories.

## Installation

```go
go get github.com/dmestas/go-libfossil
```

Import a SQLite driver alongside the packages you need:

```go
import (
    "github.com/dmestas/go-libfossil/repo"
    "github.com/dmestas/go-libfossil/sync"
    "github.com/dmestas/go-libfossil/simio"
    _ "github.com/dmestas/go-libfossil/db/driver/modernc" // pure-Go SQLite
)
```

## Quick Start

```go
// Create a new repository.
r, err := repo.Create("my.fossil", "admin", simio.CryptoRand{})

// Open an existing repository.
r, err := repo.Open("my.fossil")
defer r.Close()

// Sync with a remote Fossil server.
result, err := sync.Sync(ctx, r, &sync.HTTPTransport{URL: "http://host/repo"}, sync.SyncOpts{
    Push: true,
    Pull: true,
})

// Clone a remote repository.
r, result, err := sync.Clone(ctx, "clone.fossil", &sync.HTTPTransport{URL: "http://host/repo"}, sync.CloneOpts{
    User:     "admin",
    Password: "secret",
})
```

## Packages

| Package | Purpose |
|---------|---------|
| `repo/` | Open, create, verify `.fossil` repository files |
| `sync/` | Client sync loop, server handler, `Transport` interface, `Observer` hooks |
| `content/` | Artifact storage, delta-chain expansion |
| `blob/` | Content-addressed blob I/O (4-byte BE size prefix + zlib) |
| `xfer/` | Wire codec for Fossil's xfer card protocol |
| `delta/` | Fossil delta encoder/decoder (port of `delta.c`) |
| `manifest/` | Checkin creation, file listing, timeline, crosslinking |
| `merge/` | Three-way merge with pluggable strategies |
| `checkout/` | Working tree checkout, checkin, update, revert |
| `deck/` | Manifest and control-artifact parser |
| `hash/` | SHA1 and SHA3-256 content addressing |
| `uv/` | Unversioned file sync (wiki, forum, attachments) |
| `tag/` | Tag read/write on artifacts |
| `branch/` | Branch creation and listing |
| `stash/` | Working-tree stash save/pop |
| `undo/` | Undo/redo state tracking |
| `bisect/` | Binary search for regressions |
| `annotate/` | Line-level blame/annotate |
| `verify/` | Repository integrity verification and rebuild |
| `search/` | Full-text search indexing |
| `simio/` | Clock, Rand, Storage interfaces for deterministic testing |
| `db/` | SQLite layer with pluggable drivers |
| `testutil/` | Test helpers (Fossil CLI wrapper for integration tests) |

## SQLite Drivers

Select a driver by blank-importing its package:

| Driver | Import | Notes |
|--------|--------|-------|
| modernc (default) | `db/driver/modernc` | Pure Go, works everywhere |
| ncruces | `db/driver/ncruces` | WASM-based, supports browser and WASI targets |

The `db.OpenWith()` function also accepts any `database/sql` driver directly if you need a different SQLite binding.

## Observability

go-libfossil has **zero OpenTelemetry dependencies**. Observability is injected via the `sync.Observer` interface — lifecycle callbacks for session start/end, round metrics, errors, and server-side handling. Pass `nil` for zero-cost no-ops. Consumers implement `Observer` with their own telemetry stack.

## Deterministic Testing

All I/O flows through `simio.Env` (Clock, Rand, Storage). Pass `simio.SimEnv(seed)` for deterministic time, seeded randomness, and in-memory storage — enabling fully reproducible tests.

## License

MIT — see [LICENSE](LICENSE)
```

---

## Task 6: Source cleanup and verify tests

**Files (in `/tmp/go-libfossil-extract`):**
- Verify: `bench/baseline-20260314.txt` for sensitive content
- Run: full test suite

- [ ] **Step 1: Check bench file for sensitive data**

```bash
cd /tmp/go-libfossil-extract
cat bench/baseline-20260314.txt
```

Scan for: API keys, passwords, internal URLs, IP addresses, employee names. If found, either redact or delete the file.

- [ ] **Step 2: Check for any remaining EdgeSync-specific references in comments**

```bash
grep -ri 'edgesync\|EdgeSync' --include='*.go' . | grep -v '_test.go' | head -20
```

Any hits in non-test code comments should be reworded to be library-generic. Import paths were already rewritten in Task 3, so only free-text references matter here.

- [ ] **Step 3: Run the full test suite**

```bash
go test ./... -count=1
```

Expected: all tests pass. If any fail due to the module path rewrite, fix the failing imports.

- [ ] **Step 4: Run driver variant tests**

```bash
cd db/driver/modernc && go test ./... -count=1 && cd ../../..
cd db/driver/ncruces && go test ./... -count=1 && cd ../../..
```

Expected: both pass.

- [ ] **Step 5: Run go vet**

```bash
go vet ./...
```

Expected: clean.

---

## Task 7: Create Makefile and CI for go-libfossil

**Files (in `/tmp/go-libfossil-extract`):**
- Create: `Makefile`
- Create: `.github/workflows/test.yml`

- [ ] **Step 1: Create the Makefile**

```makefile
.PHONY: test test-drivers vet

test:
	go test ./... -count=1

test-drivers:
	@echo "=== modernc (default) ==="
	go test ./... -count=1
	@echo "=== ncruces ==="
	go test -tags test_ncruces ./... -count=1
	@echo "=== all drivers passed ==="

vet:
	go vet ./...
```

- [ ] **Step 2: Create the CI workflow**

Create `.github/workflows/test.yml`:

```yaml
name: Tests

on:
  push:
    branches: [main]
  pull_request:

jobs:
  test:
    name: Test
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version-file: go.mod
      - name: Install Fossil
        run: sudo apt-get update && sudo apt-get install -y fossil
      - name: Unit tests
        run: go test ./... -count=1
      - name: Driver tests (ncruces)
        run: go test -tags test_ncruces ./... -count=1
      - name: Vet
        run: go vet ./...
```

- [ ] **Step 3: Verify Makefile works**

```bash
make test
make vet
```

Expected: both pass.

---

## Task 8: Commit, push, and tag v0.1.0

**Files (in `/tmp/go-libfossil-extract`):**
- All changes from Tasks 3-7

- [ ] **Step 1: Stage all changes**

```bash
cd /tmp/go-libfossil-extract
git add -A
```

- [ ] **Step 2: Commit the post-extraction changes**

```bash
git commit -m "chore: rewrite module path, drop mattn driver, add MIT license

Rewrite module path from github.com/dmestas/edgesync/go-libfossil
to github.com/dmestas/go-libfossil for standalone library release.

- Drop mattn SQLite driver (modernc + ncruces cover all use cases)
- Add MIT license
- Rewrite README for standalone library identity
- Add Makefile and CI workflow
"
```

- [ ] **Step 3: Add the GitHub remote and push**

```bash
git remote add origin git@github.com:dmestas/go-libfossil.git
git branch -M main
git push -u origin main
```

- [ ] **Step 4: Tag all three modules with v0.1.0**

```bash
git tag v0.1.0
git tag db/driver/modernc/v0.1.0
git tag db/driver/ncruces/v0.1.0
git push origin --tags
```

- [ ] **Step 5: Verify the tags are visible**

```bash
gh repo view dmestas/go-libfossil
git ls-remote --tags origin
```

Expected: three tags visible (`v0.1.0`, `db/driver/modernc/v0.1.0`, `db/driver/ncruces/v0.1.0`).

---

## Task 9: Rewrite EdgeSync imports and go.mod files

**Back in the EdgeSync repo** (`/Users/dmestas/projects/EdgeSync`). This task should be done on a feature branch.

**Files:**
- Modify: ~80 `.go` files in `cmd/edgesync/`, `leaf/`, `bridge/`, `dst/`, `sim/`
- Modify: `go.mod` (root), `leaf/go.mod`, `bridge/go.mod`, `dst/go.mod`
- Delete: `go-libfossil/` (entire directory)

- [ ] **Step 1: Create a feature branch**

```bash
cd /Users/dmestas/projects/EdgeSync
git checkout -b chore/extract-go-libfossil
```

- [ ] **Step 2: Delete the go-libfossil directory**

```bash
rm -rf go-libfossil/
```

- [ ] **Step 3: Rewrite import paths in all Go files**

```bash
find . -type f -name '*.go' -not -path './go-libfossil/*' -exec \
  sed -i '' 's|github.com/dmestas/edgesync/go-libfossil|github.com/dmestas/go-libfossil|g' {} +
```

- [ ] **Step 4: Verify no old import paths remain**

```bash
grep -r 'edgesync/go-libfossil' --include='*.go' .
```

Expected: no output.

- [ ] **Step 5: Update root go.mod**

Edit `/Users/dmestas/projects/EdgeSync/go.mod`:

In the `require` block, change:
```
github.com/dmestas/edgesync/go-libfossil v0.0.0
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc v0.0.0
```
to:
```
github.com/dmestas/go-libfossil v0.1.0
github.com/dmestas/go-libfossil/db/driver/modernc v0.1.0
```

In the `replace` block, remove ALL go-libfossil lines (4 lines):
```
github.com/dmestas/edgesync/go-libfossil => ./go-libfossil
github.com/dmestas/edgesync/go-libfossil/db/driver/mattn => ./go-libfossil/db/driver/mattn
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc => ./go-libfossil/db/driver/modernc
github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces => ./go-libfossil/db/driver/ncruces
```

- [ ] **Step 6: Update leaf/go.mod**

Edit `/Users/dmestas/projects/EdgeSync/leaf/go.mod`:

In the `require` block, change:
```
github.com/dmestas/edgesync/go-libfossil v0.0.0
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc v0.0.0-00010101000000-000000000000
github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces v0.0.0-00010101000000-000000000000
```
to:
```
github.com/dmestas/go-libfossil v0.1.0
github.com/dmestas/go-libfossil/db/driver/modernc v0.1.0
github.com/dmestas/go-libfossil/db/driver/ncruces v0.1.0
```

In the `replace` block, remove ALL go-libfossil lines (4 lines):
```
github.com/dmestas/edgesync/go-libfossil => ../go-libfossil
github.com/dmestas/edgesync/go-libfossil/db/driver/mattn => ../go-libfossil/db/driver/mattn
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc => ../go-libfossil/db/driver/modernc
github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces => ../go-libfossil/db/driver/ncruces
```

- [ ] **Step 7: Update bridge/go.mod**

Edit `/Users/dmestas/projects/EdgeSync/bridge/go.mod`:

In the `require` block, change:
```
github.com/dmestas/edgesync/go-libfossil v0.0.0
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc v0.0.0-00010101000000-000000000000
```
to:
```
github.com/dmestas/go-libfossil v0.1.0
github.com/dmestas/go-libfossil/db/driver/modernc v0.1.0
```

In the `replace` block, remove ALL go-libfossil lines (4 lines):
```
github.com/dmestas/edgesync/go-libfossil => ../go-libfossil
github.com/dmestas/edgesync/go-libfossil/db/driver/mattn => ../go-libfossil/db/driver/mattn
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc => ../go-libfossil/db/driver/modernc
github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces => ../go-libfossil/db/driver/ncruces
```

- [ ] **Step 8: Update dst/go.mod**

Edit `/Users/dmestas/projects/EdgeSync/dst/go.mod`:

In the `require` block, change:
```
github.com/dmestas/edgesync/go-libfossil v0.0.0
```
to:
```
github.com/dmestas/go-libfossil v0.1.0
```

In the `replace` block, remove ALL go-libfossil lines (4 lines):
```
github.com/dmestas/edgesync/go-libfossil => ../go-libfossil
github.com/dmestas/edgesync/go-libfossil/db/driver/mattn => ../go-libfossil/db/driver/mattn
github.com/dmestas/edgesync/go-libfossil/db/driver/modernc => ../go-libfossil/db/driver/modernc
github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces => ../go-libfossil/db/driver/ncruces
```

- [ ] **Step 9: Set GOPRIVATE and run go mod tidy for each module**

```bash
export GOPRIVATE=github.com/dmestas/go-libfossil

cd /Users/dmestas/projects/EdgeSync && go mod tidy
cd /Users/dmestas/projects/EdgeSync/leaf && go mod tidy
cd /Users/dmestas/projects/EdgeSync/bridge && go mod tidy
cd /Users/dmestas/projects/EdgeSync/dst && go mod tidy
```

- [ ] **Step 10: Commit**

```bash
cd /Users/dmestas/projects/EdgeSync
git add -A
git commit -m "chore: consume go-libfossil as external dependency v0.1.0

Replace in-repo go-libfossil/ with published module at
github.com/dmestas/go-libfossil v0.1.0.

- Delete go-libfossil/ directory
- Rewrite all import paths across cmd/edgesync, leaf, bridge, dst, sim
- Update all go.mod files to require published module
- Remove go-libfossil replace directives
"
```

---

## Task 10: Update go.work, Makefile, and CI

**Files:**
- Modify: `go.work`
- Modify: `Makefile`
- Modify: `.github/workflows/test.yml`
- Create: `go.work.example`

- [ ] **Step 1: Remove go.work from git tracking**

The current `go.work` is tracked in git. We need to untrack it before gitignoring:

```bash
cd /Users/dmestas/projects/EdgeSync
git rm --cached go.work
# Also remove go.work.sum if it exists:
git rm --cached go.work.sum 2>/dev/null || true
```

- [ ] **Step 2: Add go.work to .gitignore**

Append to `.gitignore`:

```
go.work
go.work.sum
```

- [ ] **Step 3: Create a local go.work for this dev machine**

This file is now gitignored. Create it locally so the workspace still functions:

```go
go 1.26.0

use (
	.
	./bridge
	./dst
	./leaf
)
```

- [ ] **Step 4: Create go.work.example**

Create `/Users/dmestas/projects/EdgeSync/go.work.example`:

```go
// Copy this file to go.work for local development against a local go-libfossil checkout.
// Assumes go-libfossil is cloned at ../go-libfossil relative to this repo.
// Do NOT commit go.work — it is gitignored.

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

- [ ] **Step 5: Update the Makefile**

Remove the `go test ./go-libfossil/...` line from the `test` target. The parallel unit test block becomes:

```makefile
test:
	@pids=""; fail=0; \
	go test ./leaf/... -short -count=1 & pids="$$pids $$!"; \
	go test ./bridge/... -short -count=1 & pids="$$pids $$!"; \
	for pid in $$pids; do wait $$pid || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAIL: unit tests"; exit 1; fi
	go test ./dst/ -run 'TestScenario|TestE2E|TestMockFossil|TestSimulator|TestCheck' -count=1
	go test ./sim/ -run 'TestFaultProxy|TestGenerateSchedule|TestBuggify' -count=1
	go test ./sim/ -run 'TestServeHTTP|TestLeafToLeaf|TestAgentServe' -count=1 -timeout=120s
	go test ./sim/ -run 'TestInterop' -count=1 -short -timeout=60s
	go test ./sim/ -run 'TestSimulation' -sim.seed=1 -count=1 -timeout=120s
```

Remove the `drivers` target entirely (lines 132-139).

Remove `mattn` from the `dst-drivers` target — update the driver loop:

```makefile
dst-drivers:
	@echo "=== DST driver sweep (4 seeds x hostile x 2 drivers) ==="
	@fail=0; \
	for driver in "default:" "ncruces:-tags=test_ncruces"; do \
		name=$${driver%%:*}; \
		tags=$${driver#*:}; \
		for seed in 1 2 3 4; do \
			echo "  driver=$$name seed=$$seed ..."; \
			(cd dst && go test $$tags -run TestDST -seed=$$seed -level=hostile -steps=10000 -timeout 180s) || fail=1; \
		done; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== DST drivers FAILED ==="; exit 1; fi
	@echo "=== DST driver sweep passed ==="
```

Add the `update-libfossil` target:

```makefile
update-libfossil:
	GOPRIVATE=github.com/dmestas/go-libfossil go get github.com/dmestas/go-libfossil@latest
	GOPRIVATE=github.com/dmestas/go-libfossil go get github.com/dmestas/go-libfossil/db/driver/modernc@latest
	GOPRIVATE=github.com/dmestas/go-libfossil go get github.com/dmestas/go-libfossil/db/driver/ncruces@latest
	go mod tidy
```

Add `update-libfossil` to the `.PHONY` line.

- [ ] **Step 6: Update CI workflow**

Edit `.github/workflows/test.yml`:

In the `unit` job, remove the `go-libfossil` step:
```yaml
      # REMOVE THIS STEP:
      - name: go-libfossil
        run: go test ./go-libfossil/... -short -count=1
```

Add `GOPRIVATE` as a top-level env to the workflow:
```yaml
name: Tests

on:
  push:
    branches: [main]
  pull_request:

env:
  GOPRIVATE: github.com/dmestas/go-libfossil
```

- [ ] **Step 7: Verify the build**

```bash
cd /Users/dmestas/projects/EdgeSync
make build
```

Expected: edgesync, leaf, bridge binaries all build successfully.

- [ ] **Step 8: Commit**

```bash
git add go.work.example .gitignore Makefile .github/workflows/test.yml
git commit -m "chore: update build system for go-libfossil extraction

- Remove go-libfossil from go.work, Makefile test targets, and CI
- Add go.work.example for local dev with replace directives
- Gitignore go.work and go.work.sum
- Add make update-libfossil convenience target
- Drop mattn from dst-drivers sweep
"
```

---

## Task 11: Update CLAUDE.md and set GOPRIVATE

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Set GOPRIVATE on dev machine**

```bash
go env -w GOPRIVATE=github.com/dmestas/go-libfossil
```

- [ ] **Step 2: Update CLAUDE.md**

Key changes to the project instructions:

1. In **Go Modules (go.work)**, update to reflect 4 modules in the workspace (not 5), and note that go-libfossil is an external dependency:
   - `.` (root) — hosts `cmd/edgesync/`, `sim/`, soak runner
   - `leaf/` — leaf agent module
   - `bridge/` — bridge module
   - `dst/` — deterministic simulation tests
   - External: `github.com/dmestas/go-libfossil` v0.x

2. In **Core Library: go-libfossil/**, change the header to note it's an external dependency now:
   - "Core Library: go-libfossil (external: github.com/dmestas/go-libfossil)"

3. In **Build & Test**, remove the `make drivers` reference.

4. In **Key Conventions**, remove the mattn driver from the SQLite drivers list.

5. Add a note about local development workflow:
   - "For local go-libfossil development, copy `go.work.example` to `go.work` and clone go-libfossil at `../go-libfossil`"

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md for go-libfossil extraction"
```

---

## Task 12: Run full test suite and verify

- [ ] **Step 1: Run make test**

```bash
cd /Users/dmestas/projects/EdgeSync
make test
```

Expected: all unit, DST, sim, and interop tests pass.

- [ ] **Step 2: Run make build**

```bash
make build
```

Expected: all three binaries build cleanly.

- [ ] **Step 3: Verify go vet**

```bash
go vet ./...
```

Expected: clean.

- [ ] **Step 4: Clean up the temporary extraction directory**

```bash
rm -rf /tmp/go-libfossil-extract
```

---

## Task 13: Create PR for EdgeSync changes

- [ ] **Step 1: Push the branch**

```bash
cd /Users/dmestas/projects/EdgeSync
git push -u origin chore/extract-go-libfossil
```

- [ ] **Step 2: Create the PR**

```bash
gh pr create --title "Extract go-libfossil into standalone repo" --body "$(cat <<'EOF'
## Summary

- Extracted `go-libfossil/` into standalone repo `github.com/dmestas/go-libfossil` (v0.1.0)
- Rewired all imports from `github.com/dmestas/edgesync/go-libfossil` to `github.com/dmestas/go-libfossil`
- Updated go.mod files to require published module instead of local replace
- Updated CI, Makefile, and go.work for the new structure
- Added `go.work.example` for local dev against a local go-libfossil checkout
- Dropped mattn SQLite driver (modernc + ncruces cover all use cases)

## Test plan

- [ ] `make build` — all binaries compile
- [ ] `make test` — all unit, DST, sim, interop tests pass
- [ ] `go vet ./...` — clean
- [ ] Verify go-libfossil CI passes at github.com/dmestas/go-libfossil
- [ ] Verify local dev workflow: clone go-libfossil at ../go-libfossil, copy go.work.example to go.work, edit + test
EOF
)"
```

- [ ] **Step 3: Return the PR URL to the user**
