.PHONY: build test clean leaf bridge edgesync iroh-sidecar wasm-wasi wasm-browser wasm dst dst-full dst-hostile dst-drivers sim sim-full setup-hooks setup test-interop test-iroh update-libfossil

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
# Unit tests run in parallel across modules; sim/dst run sequentially after.

test:
	@pids=""; fail=0; \
	(cd leaf && go test ./... -short -count=1) & pids="$$pids $$!"; \
	(cd bridge && go test ./... -short -count=1) & pids="$$pids $$!"; \
	for pid in $$pids; do wait $$pid || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAIL: unit tests"; exit 1; fi
	(cd sim && go test . -run 'TestFaultProxy|TestGenerateSchedule|TestBuggify' -count=1)
	(cd sim && go test . -run 'TestServeHTTP|TestLeafToLeaf|TestAgentServe' -count=1 -timeout=120s)
	(cd sim && go test . -run 'TestInterop' -count=1 -short -timeout=60s)
	(cd sim && go test . -run 'TestSimulation' -sim.seed=1 -count=1 -timeout=120s)

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

# --- Sim (Integration Simulation) — run locally, requires fossil ---
# Note: DST tests now live in libfossil (github.com/danmestas/libfossil/dst)

# Quick: 1 seed, normal severity
sim:
	cd sim && go test . -run TestSimulation -sim.seed=1 -v -timeout=120s

# Full: 16 seeds × 3 severities
sim-full:
	@echo "=== Sim full (16 seeds × 3 severities) ==="
	@for severity in normal adversarial hostile; do \
		for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
			echo "  seed=$$seed severity=$$severity ..."; \
			(cd sim && go test . -run TestSimulation -sim.seed=$$seed -sim.severity=$$severity -timeout=120s) || true; \
		done; \
	done
	@echo "=== Sim full done ==="

# Fossil interop: Tier 1 + Tier 2 (requires fossil binary, Tier 2 samples 5K+2K blobs)
test-interop:
	cd sim && go test -buildvcs=false . -run TestInterop -timeout=10m -v

# Iroh P2P: build sidecar then run iroh unit + integration tests
test-iroh: iroh-sidecar
	cd leaf && go test ./agent/ -run TestIroh -count=1 -v
	cd sim && go test . -run TestIroh -count=1 -timeout=120s -v

# --- Dependency update ---

update-libfossil:
	go get github.com/danmestas/libfossil@latest
	go get github.com/danmestas/libfossil/db/driver/modernc@latest
	go get github.com/danmestas/libfossil/db/driver/ncruces@latest
	go mod tidy
