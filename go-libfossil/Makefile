.PHONY: test test-race vet cover bench validate

test:
	go test ./...

test-race:
	go test -race ./...

vet:
	go vet ./...

cover:
	go test -cover ./...

bench:
	go test -bench=. -benchmem ./...

validate: vet test test-race
