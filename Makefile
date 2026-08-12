.PHONY: build test clean leaf bridge edgesync iroh-sidecar wasm-wasi wasm-browser wasm setup-hooks setup test-iroh update-libfossil auto-release auto-release-dry

# --- Build ---

build: edgesync leaf bridge iroh-sidecar

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

edgesync:
	go build -buildvcs=false -o bin/edgesync ./cmd/edgesync

leaf:
	cd leaf && go build -buildvcs=false -ldflags "-X main.version=$(VERSION)" -o ../bin/leaf ./cmd/leaf

bridge:
	cd bridge && go build -buildvcs=false -o ../bin/bridge ./cmd/bridge

iroh-sidecar:
	cd iroh-sidecar && cargo build --release
	cp iroh-sidecar/target/release/iroh-sidecar bin/

wasm-wasi:
	@mkdir -p bin
	cd leaf && GOOS=wasip1 GOARCH=wasm go build -buildvcs=false -o ../bin/leaf.wasm ./cmd/leaf

wasm-browser:
	@mkdir -p bin
	cd leaf && GOOS=js GOARCH=wasm go build -buildvcs=false -o ../bin/leaf-browser.wasm ./cmd/wasm
	cp "$$(go env GOROOT)/lib/wasm/wasm_exec.js" bin/

wasm: wasm-wasi wasm-browser

clean:
	rm -rf bin/

# --- Test (what CI runs) ---
# Unit tests run in parallel across modules; sim integration tests run after.
# Iroh tests need the sidecar binary — run them via test-iroh.

test:
	@pids=""; fail=0; \
	(cd leaf && go test ./... -short -count=1) & pids="$$pids $$!"; \
	(cd bridge && go test ./... -short -count=1) & pids="$$pids $$!"; \
	for pid in $$pids; do wait $$pid || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAIL: unit tests"; exit 1; fi
	(cd sim && go test . -run 'TestHubLeafE2E|TestAgentNew_|TestHubNATSFossilSync_' -count=1 -timeout=120s)

vet:
	go vet ./...
	cd leaf && go vet ./...
	cd bridge && go vet ./...

# --- Setup (first-time onboarding) ---

setup: setup-hooks build test
	@echo ""
	@echo "Setup complete. Binaries in bin/. Try:"
	@echo "  bin/edgesync repo info"
	@echo "  bin/leaf --help"

setup-hooks:
	git config core.hooksPath .githooks
	@echo "Pre-commit hook installed. Runs ~5s of tests before each commit."
	@echo "Skip with: git commit --no-verify"

# --- Sim (Integration Simulation) ---
# DST and the seed-sweep simulation/interop suites live in go-libfossil
# (github.com/danmestas/go-libfossil): the old sim/dst regex targets here
# ran against tests that were removed in the v0.2.0 handle-API migration.

# Iroh P2P: build sidecar then run iroh unit + integration tests
test-iroh: iroh-sidecar
	cd leaf && go test ./agent/ -run TestIroh -count=1 -v
	cd sim && go test . -run TestIroh -count=1 -timeout=120s -v

# --- Dependency update ---

update-libfossil:
	go get github.com/danmestas/libfossil@latest
	go get github.com/danmestas/libfossil/db/driver/modernc@latest

# --- Auto-release ---
# Coordinated multi-module release driver. `make auto-release-dry`
# reports what would happen; `make auto-release` runs it (interactive,
# prompts before each push). The workflow_dispatch entry point is
# .github/workflows/auto-release.yml.
#
# Distinct from the manual `release:` target above, which takes a
# specific VERSION= and tags the root only.

auto-release-dry:
	./scripts/release.sh --dry-run

auto-release:
	./scripts/release.sh
	go get github.com/danmestas/libfossil/db/driver/ncruces@latest
	go mod tidy

# CI mirror — must match .github/workflows/ci.yml verbatim.
.PHONY: ci ci-vet ci-leaf-bridge ci-cmd ci-fast

ci: ci-vet ci-leaf-bridge ci-cmd

ci-vet:
	go vet ./...
	cd leaf && go vet ./...
	cd bridge && go vet ./...

ci-leaf-bridge:
	cd leaf && go test ./... -short -count=1
	cd bridge && go test ./... -short -count=1

ci-cmd:
	go build -buildvcs=false ./cmd/edgesync/
	go test -buildvcs=false ./cmd/edgesync/ -count=1 -timeout=60s

# Fast subset for pre-push hook.
# Mirrors the bones-side of ci.yml in -short mode, including descending into
# the leaf/ and bridge/ sub-modules (separate go.mod) which `go test ./...`
# from root would miss. Skips the cmd/edgesync build+test (the slowest CI lane).
ci-fast: ci-vet
	go build ./...
	go test -short -count=1 -timeout=30s ./...
	cd leaf && go test -short -count=1 -timeout=30s ./...
	cd bridge && go test -short -count=1 -timeout=30s ./...

.PHONY: release
release:
	@test -n "$(VERSION)" || { echo "VERSION=vX.Y.Z required"; exit 1; }
	@echo "$(VERSION)" | grep -qE '^v[0-9]+\.[0-9]+\.[0-9]+(-.+)?$$' || { echo "bad version format: $(VERSION)"; exit 1; }
	@git diff --quiet || { echo "tree dirty; commit or stash first"; exit 1; }
	@git fetch origin --tags
	@if git rev-parse "$(VERSION)" >/dev/null 2>&1; then echo "tag $(VERSION) already exists"; exit 1; fi
	@$(MAKE) ci
	@PREV=$$(git describe --tags --abbrev=0 2>/dev/null || echo ""); \
	  TMPL=.github/RELEASE_TEMPLATE.md; \
	  TMP=$$(mktemp); \
	  { echo "Release $(VERSION)"; echo; \
	    [ -f $$TMPL ] && { cat $$TMPL; echo; }; \
	    echo "## Changes"; \
	    if [ -n "$$PREV" ]; then git log --oneline $$PREV..HEAD; else git log --oneline; fi; } > $$TMP; \
	  $${EDITOR:-vi} $$TMP; \
	  git tag -a "$(VERSION)" -F $$TMP; \
	  rm $$TMP
	@echo ""
	@echo "Tag $(VERSION) created locally. To publish:"
	@echo "  git push origin $(VERSION)"
