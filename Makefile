.PHONY: build test clean leaf bridge edgesync wasm-wasi wasm-browser wasm dst dst-full dst-hostile dst-drivers sim sim-full setup-hooks setup drivers test-interop

# --- Build ---

build: edgesync leaf bridge

VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

edgesync:
	go build -buildvcs=false -o bin/edgesync ./cmd/edgesync

leaf:
	cd leaf && go build -buildvcs=false -ldflags "-X main.version=$(VERSION)" -o ../bin/leaf ./cmd/leaf

bridge:
	cd bridge && go build -buildvcs=false -o ../bin/bridge ./cmd/bridge

wasm-wasi:
	GOOS=wasip1 GOARCH=wasm go build -buildvcs=false -o bin/leaf.wasm ./leaf/cmd/leaf/

wasm-browser:
	GOOS=js GOARCH=wasm go build -buildvcs=false -o bin/leaf-browser.wasm ./leaf/cmd/wasm/
	cp "$$(go env GOROOT)/lib/wasm/wasm_exec.js" bin/

wasm: wasm-wasi wasm-browser

clean:
	rm -rf bin/

# --- Test (what CI runs) ---
# Unit tests run in parallel across modules; sim/dst run sequentially after.

test:
	@pids=""; fail=0; \
	go test ./go-libfossil/... -short -count=1 & pids="$$pids $$!"; \
	go test ./leaf/... -short -count=1 & pids="$$pids $$!"; \
	go test ./bridge/... -short -count=1 & pids="$$pids $$!"; \
	for pid in $$pids; do wait $$pid || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "FAIL: unit tests"; exit 1; fi
	go test ./dst/ -run 'TestScenario|TestE2E|TestMockFossil|TestSimulator|TestCheck' -count=1
	go test ./sim/ -run 'TestFaultProxy|TestGenerateSchedule|TestBuggify' -count=1
	go test ./sim/ -run 'TestServeHTTP|TestLeafToLeaf|TestAgentServe' -count=1 -timeout=120s
	go test ./sim/ -run 'TestInterop' -count=1 -short -timeout=60s

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

# --- DST (Deterministic Simulation Testing) — run locally ---

# Quick: 8 seeds × normal, ~2s
dst:
	@echo "=== DST quick (8 seeds, normal) ==="
	@for seed in 1 2 3 4 5 6 7 8; do \
		echo "  seed=$$seed ..."; \
		go test ./dst/ -run TestDST -seed=$$seed -level=normal -steps=5000 -timeout 30s; \
	done
	@echo "=== DST quick passed ==="

# Full: 16 seeds × 3 levels, ~40s
dst-full:
	@echo "=== DST full (16 seeds × 3 levels) ==="
	@fail=0; \
	for level in normal adversarial hostile; do \
		for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
			echo "  level=$$level seed=$$seed ..."; \
			go test ./dst/ -run TestDST -seed=$$seed -level=$$level -steps=10000 -timeout 180s || fail=1; \
		done; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== DST FAILED ==="; exit 1; fi
	@echo "=== DST full passed ==="

# Hostile only: 16 seeds
dst-hostile:
	@echo "=== DST hostile (16 seeds) ==="
	@for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
		echo "  seed=$$seed level=hostile ..."; \
		go test ./dst/ -run TestDST -seed=$$seed -level=hostile -steps=10000 -timeout 180s; \
	done
	@echo "=== DST hostile passed ==="

# DST across all 3 SQLite drivers
dst-drivers:
	@echo "=== DST driver sweep (4 seeds x hostile x 3 drivers) ==="
	@fail=0; \
	for driver in "default:" "ncruces:-tags=test_ncruces" "mattn:-tags=test_mattn"; do \
		name=$${driver%%:*}; \
		tags=$${driver#*:}; \
		cgo=0; \
		if [ "$$name" = "mattn" ]; then cgo=1; fi; \
		for seed in 1 2 3 4; do \
			echo "  driver=$$name seed=$$seed ..."; \
			(cd dst && CGO_ENABLED=$$cgo go test $$tags -run TestDST -seed=$$seed -level=hostile -steps=10000 -timeout 180s) || fail=1; \
		done; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== DST drivers FAILED ==="; exit 1; fi
	@echo "=== DST driver sweep passed ==="

# --- Sim (Integration Simulation) — run locally, requires fossil ---

# Quick: 1 seed, normal severity
sim:
	go test ./sim/ -run TestSimulation -sim.seed=1 -v -timeout=120s

# Full: 16 seeds × 3 severities
sim-full:
	@echo "=== Sim full (16 seeds × 3 severities) ==="
	@for severity in normal adversarial hostile; do \
		for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
			echo "  seed=$$seed severity=$$severity ..."; \
			go test ./sim/ -run TestSimulation -sim.seed=$$seed -sim.severity=$$severity -timeout=120s || true; \
		done; \
	done
	@echo "=== Sim full done ==="

# Fossil interop: Tier 1 + Tier 2 (requires fossil binary, Tier 2 samples 5K+2K blobs)
test-interop:
	go test -buildvcs=false ./sim/ -run TestInterop -timeout=10m -v

# --- Driver matrix — run locally ---

drivers:
	@echo "=== modernc (default) ==="
	go test -buildvcs=false ./go-libfossil/... -count=1
	@echo "=== ncruces ==="
	go test -buildvcs=false -tags test_ncruces ./go-libfossil/... -count=1
	@echo "=== mattn ==="
	CGO_ENABLED=1 go test -buildvcs=false -tags test_mattn ./go-libfossil/... -count=1
	@echo "=== all drivers passed ==="
