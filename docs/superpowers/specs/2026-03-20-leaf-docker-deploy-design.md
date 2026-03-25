# Leaf Agent Docker Deployment — Design Spec

**Date:** 2026-03-20
**Status:** Draft

## Goal

Deploy a leaf agent on the Hetzner VPS (`91.99.202.69`) via Docker Compose, serving a Fossil repo over public HTTP and private NATS (Tailscale-only). Stock `fossil clone`/`fossil sync` should work against it.

## Architecture

```
VPS (91.99.202.69 / 100.78.32.45 via Tailscale)
├── docker compose
│   ├── nats (nats:latest + JetStream)
│   │   ├── 100.78.32.45:4222 — Tailscale-only (leaf-to-leaf sync)
│   │   └── nats:4222 — internal container network (for leaf)
│   └── leaf (multi-stage Go build)
│       ├── 0.0.0.0:8080 — public HTTP (fossil clone/sync)
│       ├── nats://nats:4222 — container-internal NATS
│       ├── --serve-http :8080
│       ├── --serve-nats
│       └── volume: ./data/*.fossil
└── ~/EdgeSync/ (git clone — source for builds)
```

## Components

### 1. Dockerfile (`deploy/Dockerfile`)

Multi-stage build:
- **Build stage:** `golang:1.25-alpine` — compiles `leaf/cmd/leaf/` with `-buildvcs=false` and CGO enabled (modernc SQLite needs it)
- **Run stage:** `alpine:3.20` — binary + ca-certificates + curl (for healthcheck)
- Final image ~20-30MB

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

A `.dockerignore` at the repo root keeps the build context small:
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
*.fossil
*.log
```

### 2. docker-compose.yml (`deploy/docker-compose.yml`)

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

### 3. NATS config (`deploy/nats.conf`)

```
listen: 0.0.0.0:4222

jetstream {
  store_dir: /data
  max_mem: 64MB
  max_file: 1GB
}
```

Minimal config. JetStream enabled for durable messaging. No auth for now (Tailscale provides the network boundary).

### 4. Health endpoint

Add `/healthz` to `sync.ServeHTTP` in `go-libfossil/sync/serve_http.go`. Returns 200 OK with `{"status":"ok"}`. Docker uses this for container health checks.

### 5. Missing CLI flags

`leaf/cmd/leaf/main.go` needs three flags that `Config` already supports:
- `--serve-http` → `Config.ServeHTTPAddr`
- `--serve-nats` → `Config.ServeNATSEnabled`
- `--uv` → `Config.UV`

These are additive — no existing behavior changes.

## Code Changes

| File | Change | Lines |
|------|--------|-------|
| `leaf/cmd/leaf/main.go` | Add `--serve-http`, `--serve-nats`, `--uv` flags | ~6 |
| `go-libfossil/sync/serve_http.go` | Add `/healthz` handler | ~8 |
| `deploy/Dockerfile` | New file — multi-stage Go build | ~12 |
| `deploy/docker-compose.yml` | New file — leaf + nats services | ~35 |
| `deploy/nats.conf` | New file — minimal JetStream config | ~7 |
| `.dockerignore` | New file — exclude .git, fossil/, dst/, sim/, etc. | ~12 |

## Prerequisites

- Docker and Docker Compose installed on VPS
- Tailscale running with expected IP: `ip addr show tailscale0 | grep 100.78.32.45`
- SSH key configured for `git clone` from GitHub

## Deploy Workflow

```bash
# On VPS (one-time):
git clone git@github.com:danmestas/EdgeSync.git ~/EdgeSync
cd ~/EdgeSync/deploy
mkdir -p data

# Seed a repo (from laptop):
scp my.fossil dmestas@91.99.202.69:~/EdgeSync/deploy/data/repo.fossil
# Or create on VPS:
ssh dmestas@91.99.202.69 'cd ~/EdgeSync && go run -buildvcs=false ./cmd/edgesync/ repo new ~/EdgeSync/deploy/data/repo.fossil'

# Start:
docker compose up -d --build

# Verify:
fossil clone http://91.99.202.69:8080 /tmp/test.fossil
```

## Update Workflow

```bash
ssh dmestas@91.99.202.69
cd ~/EdgeSync
git pull
cd deploy
docker compose up -d --build
```

## What's Excluded

- **Config file loading** — env vars + flags sufficient for single container
- **Multi-repo** — one leaf per repo; add a second service block if needed
- **TLS** — add a Caddy/nginx reverse proxy later for HTTPS
- **Login verification** — CDG-115, after deployment proves out
- **NATS auth** — Tailscale provides the network boundary for now
- **OTel in container** — leaf supports `--otel-endpoint` already; pass via env vars when ready (Doppler for secrets)
- **Non-root container user** — add later for hardening
- **Backup strategy** — add cron job for `./data/` once in production use

## Success Criteria

1. `fossil clone http://91.99.202.69:8080 /tmp/test.fossil` works from laptop
2. `fossil sync http://91.99.202.69:8080` pushes/pulls artifacts
3. Local leaf agent on laptop syncs with VPS leaf via NATS over Tailscale (`100.78.32.45:4222`)
4. Container restarts automatically on failure
5. `/healthz` returns 200
