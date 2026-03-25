# Leaf Agent Docker Deployment — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deploy a leaf agent on Hetzner VPS via Docker Compose, serving Fossil repos over public HTTP and Tailscale-only NATS.

**Architecture:** Two-container Docker Compose stack (NATS + leaf). Leaf serves HTTP on public `:8080` for `fossil clone/sync`. NATS binds to Tailscale IP only for leaf-to-leaf sync. Repo files live in a host-mounted volume.

**Tech Stack:** Go 1.25, Docker, Docker Compose, NATS w/ JetStream, Alpine Linux

**Spec:** `docs/dev/specs/2026-03-20-leaf-docker-deploy-design.md`

---

## File Structure

| File | Responsibility |
|------|---------------|
| `leaf/cmd/leaf/main.go` | Modify: add `--serve-http`, `--serve-nats`, `--uv` flags |
| `go-libfossil/sync/serve_http.go` | Modify: add `/healthz` endpoint |
| `go-libfossil/sync/serve_http_test.go` | Modify: add healthz test |
| `deploy/Dockerfile` | Create: multi-stage Go build |
| `deploy/docker-compose.yml` | Create: leaf + nats services |
| `deploy/nats.conf` | Create: minimal JetStream config |
| `.dockerignore` | Create: exclude .git, fossil/, dst/, sim/, etc. |

---

### Task 1: Add /healthz endpoint to ServeHTTP

**Files:**
- Modify: `go-libfossil/sync/serve_http.go:32-33`
- Modify: `go-libfossil/sync/serve_http_test.go`

- [ ] **Step 1: Write the failing test**

Add to `go-libfossil/sync/serve_http_test.go`. Uses existing helpers `setupSyncTestRepo` (from `sync_test.go`) and `freePort` (from `serve_http_test.go`). Add `"io"` to the import block.

```go
func TestServeHTTPHealthz(t *testing.T) {
	r := setupSyncTestRepo(t)

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	addr := freePort(t)
	go ServeHTTP(ctx, addr, r, HandleSync)
	time.Sleep(100 * time.Millisecond)

	resp, err := http.Get("http://" + addr + "/healthz")
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("healthz: got status %d, want 200", resp.StatusCode)
	}
	body, _ := io.ReadAll(resp.Body)
	if !strings.Contains(string(body), "ok") {
		t.Fatalf("healthz: body %q does not contain 'ok'", body)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd go-libfossil && go test -buildvcs=false -run TestServeHTTPHealthz ./sync/ -v`
Expected: FAIL — `/healthz` hits the xfer handler which returns HTML or error

- [ ] **Step 3: Add /healthz handler**

In `go-libfossil/sync/serve_http.go`, add the healthz route before the catch-all in `ServeHTTP`:

```go
mux := http.NewServeMux()
mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	fmt.Fprint(w, `{"status":"ok"}`)
})
mux.HandleFunc("/", xferHandler(r, h))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd go-libfossil && go test -buildvcs=false -run TestServeHTTPHealthz ./sync/ -v`
Expected: PASS

- [ ] **Step 5: Run full serve_http test suite**

Run: `cd go-libfossil && go test -buildvcs=false -run TestServeHTTP ./sync/ -v`
Expected: All existing tests still pass

- [ ] **Step 6: Commit**

```bash
git add go-libfossil/sync/serve_http.go go-libfossil/sync/serve_http_test.go
git commit -m "sync: add /healthz endpoint to ServeHTTP"
```

---

### Task 2: Add missing CLI flags to leaf

**Files:**
- Modify: `leaf/cmd/leaf/main.go:25-26` (after `pull` flag) and `:56-65` (config struct)

- [ ] **Step 1: Add the three flags**

In `leaf/cmd/leaf/main.go`, after the `pull` flag (line 26), add:

```go
serveHTTP := flag.String("serve-http", envOrDefault("LEAF_SERVE_HTTP", ""), "HTTP listen address (e.g. :8080) to serve fossil clone/sync")
serveNATS := flag.Bool("serve-nats", false, "enable NATS request/reply listener for leaf-to-leaf sync")
uv := flag.Bool("uv", false, "enable unversioned file sync (wiki, forum, attachments)")
```

- [ ] **Step 2: Wire flags into Config struct**

In the `cfg := agent.Config{` block, add the three fields:

```go
cfg := agent.Config{
	RepoPath:         *repoPath,
	NATSUrl:          *natsURL,
	PollInterval:     *poll,
	User:             *user,
	Password:         *password,
	Push:             *push,
	Pull:             *pull,
	UV:               *uv,
	ServeHTTPAddr:    *serveHTTP,
	ServeNATSEnabled: *serveNATS,
	Observer:         obs,
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cd leaf && go build -buildvcs=false ./cmd/leaf/`
Expected: Compiles without errors

- [ ] **Step 4: Verify help output shows new flags**

Run: `cd leaf && go run -buildvcs=false ./cmd/leaf/ --help 2>&1 | grep -E 'serve-http|serve-nats|uv'`
Expected: All three flags appear in help text

- [ ] **Step 5: Commit**

```bash
git add leaf/cmd/leaf/main.go
git commit -m "leaf: add --serve-http, --serve-nats, --uv CLI flags"
```

---

### Task 3: Create .dockerignore

**Files:**
- Create: `.dockerignore`

- [ ] **Step 1: Create .dockerignore**

```
.git
.github
.agents
.claude
fossil
libfossil
dst
sim
bridge
bin
tmp
docs
*.fossil
*.log
*.test
.env
.env.*
.DS_Store
.worktrees
```

- [ ] **Step 2: Commit**

```bash
git add .dockerignore
git commit -m "docker: add .dockerignore to keep build context small"
```

---

### Task 4: Create deploy files (Dockerfile, compose, NATS config)

**Files:**
- Create: `deploy/Dockerfile`
- Create: `deploy/docker-compose.yml`
- Create: `deploy/nats.conf`

- [ ] **Step 1: Create deploy directory**

Run: `mkdir -p deploy`

- [ ] **Step 2: Create Dockerfile**

Create `deploy/Dockerfile`:

```dockerfile
FROM golang:1.25-alpine AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY go.work go.work.sum ./
COPY go-libfossil/ go-libfossil/
COPY leaf/ leaf/
RUN cd leaf && go build -buildvcs=false -o /leaf ./cmd/leaf/

FROM alpine:3.20
RUN apk add --no-cache ca-certificates curl
COPY --from=build /leaf /usr/local/bin/leaf
ENTRYPOINT ["leaf"]
```

- [ ] **Step 3: Create nats.conf**

Create `deploy/nats.conf`:

```
listen: 0.0.0.0:4222

jetstream {
  store_dir: /data
  max_mem: 64MB
  max_file: 1GB
}
```

- [ ] **Step 4: Create docker-compose.yml**

Create `deploy/docker-compose.yml`:

```yaml
services:
  nats:
    image: nats:latest
    command: ["-js", "-c", "/etc/nats/nats.conf"]
    ports:
      - "100.78.32.45:4222:4222"
    volumes:
      - ./nats.conf:/etc/nats/nats.conf:ro
      - nats-data:/data
    restart: unless-stopped

  leaf:
    build:
      context: ..
      dockerfile: deploy/Dockerfile
    command:
      - --repo=/data/repo.fossil
      - --nats=nats://nats:4222
      - --serve-http=:8080
      - --serve-nats
      - --uv
      - --poll=5s
    ports:
      - "0.0.0.0:8080:8080"
    volumes:
      - ./data:/data
    depends_on:
      nats:
        condition: service_started
    restart: unless-stopped
    healthcheck:
      test: ["CMD", "curl", "-sf", "http://localhost:8080/healthz"]
      interval: 30s
      timeout: 5s
      retries: 3

volumes:
  nats-data:
```

- [ ] **Step 5: Commit**

```bash
git add deploy/
git commit -m "deploy: add Dockerfile, docker-compose, NATS config for VPS"
```

---

### Task 5: Local Docker build test

**Files:** None — verification only

- [ ] **Step 1: Build the Docker image locally**

Run: `docker build -f deploy/Dockerfile -t edgesync-leaf .`
Expected: Build succeeds, image created

- [ ] **Step 2: Verify the binary runs**

Run: `docker run --rm edgesync-leaf --help`
Expected: Shows all flags including `--serve-http`, `--serve-nats`, `--uv`

- [ ] **Step 3: Check image size**

Run: `docker images edgesync-leaf --format '{{.Size}}'`
Expected: Under 50MB

---

### Task 6: Run pre-commit tests

**Files:** None — verification only

- [ ] **Step 1: Run full test suite**

Run: `make test`
Expected: All tests pass (unit + sim serve tests)

- [ ] **Step 2: Commit all changes and push**

```bash
git push origin main
```

---

### Task 7: Deploy to VPS

**Files:** None — remote operations

- [ ] **Step 1: SSH into VPS and clone repo**

```bash
ssh dmestas@91.99.202.69
git clone git@github.com:danmestas/EdgeSync.git ~/EdgeSync
```

(Skip if already cloned — just `cd ~/EdgeSync && git pull`)

- [ ] **Step 2: Create data directory and seed a repo**

```bash
cd ~/EdgeSync/deploy
mkdir -p data
```

Then from laptop:
```bash
scp /path/to/test.fossil dmestas@91.99.202.69:~/EdgeSync/deploy/data/repo.fossil
```

Or create on VPS (requires Go installed):
```bash
cd ~/EdgeSync
go run -buildvcs=false ./cmd/edgesync/ repo new ~/EdgeSync/deploy/data/repo.fossil
```

- [ ] **Step 3: Start the stack**

```bash
cd ~/EdgeSync/deploy
docker compose up -d --build
```

- [ ] **Step 4: Verify containers are running**

```bash
docker compose ps
```
Expected: Both `nats` and `leaf` show as "Up" / "healthy"

- [ ] **Step 5: Test healthz**

```bash
curl -sf http://localhost:8080/healthz
```
Expected: `{"status":"ok"}`

---

### Task 8: End-to-end verification

**Files:** None — verification from laptop

- [ ] **Step 1: Test fossil clone over public HTTP**

From laptop:
```bash
fossil clone http://91.99.202.69:8080 /tmp/vps-clone.fossil
```
Expected: Clone succeeds

- [ ] **Step 2: Test fossil sync over public HTTP**

```bash
fossil sync http://91.99.202.69:8080 -R /tmp/vps-clone.fossil
```
Expected: Sync succeeds (0 artifacts if already in sync)

- [ ] **Step 3: Test NATS connectivity over Tailscale**

From laptop:
```bash
nats sub -s nats://100.78.32.45:4222 '>'
```
Expected: Connects successfully (Ctrl+C to exit)

- [ ] **Step 4: Test leaf-to-leaf NATS sync**

From laptop, run a local leaf pointing at VPS NATS:
```bash
cd ~/projects/EdgeSync
go run -buildvcs=false ./leaf/cmd/leaf/ \
  --repo /tmp/vps-clone.fossil \
  --nats nats://100.78.32.45:4222 \
  --poll 10s
```
Expected: Syncs with VPS leaf via NATS over Tailscale
