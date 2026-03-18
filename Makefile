.PHONY: build test clean leaf bridge edgesync dst dst-full dst-hostile dst-drivers sim sim-full setup-hooks

# --- Build ---

build: edgesync leaf bridge

edgesync:
	go build -buildvcs=false -o bin/edgesync ./cmd/edgesync

leaf:
	cd leaf && go build -buildvcs=false -o ../bin/leaf ./cmd/leaf

bridge:
	cd bridge && go build -buildvcs=false -o ../bin/bridge ./cmd/bridge

clean:
	rm -rf bin/

# --- Test (what CI runs) ---

test:
	cd go-libfossil && go test -short ./...
	cd leaf && go test -short ./...
	cd bridge && go test -short ./...
	cd dst && go test ./...

# --- Pre-commit hook setup ---

setup-hooks:
	git config core.hooksPath .githooks
	@echo "Pre-commit hook installed. Runs ~5s of tests before each commit."
	@echo "Skip with: git commit --no-verify"

# --- DST (Deterministic Simulation Testing) — run locally ---

# Quick: 8 seeds x normal, ~2s
dst:
	@echo "=== DST quick (8 seeds, normal) ==="
	@for seed in 1 2 3 4 5 6 7 8; do \
		echo "  seed=$$seed ..."; \
		go test ./dst/ -run TestDST -seed=$$seed -level=normal -steps=5000 -timeout 30s; \
	done
	@echo "=== DST quick passed ==="

# Full: 16 seeds x 3 levels, ~40s
dst-full:
	@echo "=== DST full (16 seeds x 3 levels) ==="
	@fail=0; \
	for level in normal adversarial hostile; do \
		for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
			echo "  level=$$level seed=$$seed ..."; \
			go test ./dst/ -run TestDST -seed=$$seed -level=$$level -steps=10000 -timeout 60s || fail=1; \
		done; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== DST FAILED ==="; exit 1; fi
	@echo "=== DST full passed ==="

# Hostile only: 16 seeds
dst-hostile:
	@echo "=== DST hostile (16 seeds) ==="
	@for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
		echo "  seed=$$seed level=hostile ..."; \
		go test ./dst/ -run TestDST -seed=$$seed -level=hostile -steps=10000 -timeout 60s; \
	done
	@echo "=== DST hostile passed ==="

# DST across all 3 SQLite drivers
dst-drivers:
	@echo "=== DST driver sweep (4 seeds x hostile x 3 drivers) ==="
	@fail=0; \
	for driver in "default:" "ncruces:-tags=ncruces" "mattn:-tags=mattn"; do \
		name=$${driver%%:*}; \
		tags=$${driver#*:}; \
		cgo=0; \
		if [ "$$name" = "mattn" ]; then cgo=1; fi; \
		for seed in 1 2 3 4; do \
			echo "  driver=$$name seed=$$seed ..."; \
			(cd dst && CGO_ENABLED=$$cgo go test $$tags -run TestDST -seed=$$seed -level=hostile -steps=10000 -timeout 60s) || fail=1; \
		done; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== DST drivers FAILED ==="; exit 1; fi
	@echo "=== DST driver sweep passed ==="

# --- Sim (Integration Simulation) — run locally, requires fossil ---

# Quick: 1 seed, normal severity
sim:
	go test ./sim/ -run TestSimulation -sim.seed=1 -v -timeout=120s

# Full: 16 seeds x 3 severities
sim-full:
	@echo "=== Sim full (16 seeds x 3 severities) ==="
	@for severity in normal adversarial hostile; do \
		for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
			echo "  seed=$$seed severity=$$severity ..."; \
			go test ./sim/ -run TestSimulation -sim.seed=$$seed -sim.severity=$$severity -timeout=120s || true; \
		done; \
	done
	@echo "=== Sim full done ==="

# --- Driver matrix — run locally ---

drivers:
	@echo "=== modernc (default) ==="
	cd go-libfossil && go test ./... -count=1
	@echo "=== ncruces ==="
	cd go-libfossil && go test -tags ncruces ./... -count=1
	@echo "=== mattn ==="
	cd go-libfossil && CGO_ENABLED=1 go test -tags mattn ./... -count=1
	@echo "=== all drivers passed ==="
