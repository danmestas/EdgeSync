.PHONY: build test clean leaf bridge dst dst-full dst-hostile dst-drivers

build: leaf bridge

leaf:
	go build -o bin/leaf ./cmd/leaf

bridge:
	go build -o bin/bridge ./cmd/bridge

test:
	cd go-libfossil && go test -short ./...
	cd leaf && go test -short ./...
	cd bridge && go test -short ./...
	cd dst && go test ./...

clean:
	rm -rf bin/

# --- DST (Deterministic Simulation Testing) ---

# Quick DST: 8 seeds × normal level, ~2s total
dst:
	@echo "=== DST quick sweep (8 seeds, normal) ==="
	@for seed in 1 2 3 4 5 6 7 8; do \
		echo "  seed=$$seed ..."; \
		cd dst && go test -run TestDST -seed=$$seed -level=normal -steps=5000 -timeout 30s && cd ..; \
	done
	@echo "=== DST quick sweep passed ==="

# Full DST: 16 seeds × 3 levels, ~30s total
dst-full:
	@echo "=== DST full sweep (16 seeds × 3 levels) ==="
	@fail=0; \
	for level in normal adversarial hostile; do \
		for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
			echo "  level=$$level seed=$$seed ..."; \
			(cd dst && go test -run TestDST -seed=$$seed -level=$$level -steps=10000 -timeout 60s) || fail=1; \
		done; \
	done; \
	if [ $$fail -eq 1 ]; then echo "=== DST FAILED ==="; exit 1; fi
	@echo "=== DST full sweep passed ==="

# DST across all 3 SQLite drivers: 4 seeds × hostile × 3 drivers
dst-drivers:
	@echo "=== DST driver sweep (4 seeds × hostile × 3 drivers) ==="
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

# Hostile-only DST: 16 seeds, hostile level
dst-hostile:
	@echo "=== DST hostile sweep (16 seeds) ==="
	@for seed in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
		echo "  seed=$$seed level=hostile ..."; \
		cd dst && go test -run TestDST -seed=$$seed -level=hostile -steps=10000 -timeout 60s && cd ..; \
	done
	@echo "=== DST hostile sweep passed ==="
