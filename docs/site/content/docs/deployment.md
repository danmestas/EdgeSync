---
title: Deployment
weight: 30
---

A working production recipe for EdgeSync, derived from the reference deployment at `sync.craftdesign.group`. The shape: one VPS running NATS + a leaf agent, exposed publicly via Cloudflare Tunnel and privately via Tailscale.

## Topology

```
public internet
      │
      ▼ HTTPS
Cloudflare Tunnel ──► VPS:9000 (leaf HTTP /xfer)
                         │
                         ▼
                  leaf agent ◄────► NATS:4222 (Tailscale-only)
                         │
                         ▼
                   .fossil repo (volume mount)

tailnet ──────────────► VPS:9000 (HTTP)
                  └───► VPS:4222 (NATS)
```

## Files

The reference deployment lives in `deploy/`:

```
deploy/
├── Dockerfile              # multi-stage Go build, GOWORK=off
├── docker-compose.yml      # NATS on Tailscale IP, leaf on :9000
└── data/                   # host volume mount with .fossil files
```

### Dockerfile

The leaf binary is built standalone (libfossil pulled from the public Go module proxy) so no private-module auth is needed. The build uses `GOWORK=off` because Docker copies only `leaf/`, not the whole workspace.

### docker-compose.yml

Two containers:

- **`nats`** — official `nats:latest` image, bound to the host's Tailscale interface only (`100.78.32.45:4222`)
- **`leaf`** — built from `Dockerfile`, exposed on `:9000`

Ports `8080` and `8090` are intentionally avoided because they are occupied by Coolify and Caddy on the reference VPS. Adjust as needed.

### Cloudflare Tunnel

```yaml
# ~/.cloudflared/config.yml on the VPS
tunnel: <tunnel-id>
credentials-file: /home/user/.cloudflared/<tunnel-id>.json
ingress:
  - hostname: sync.craftdesign.group
    service: http://localhost:9000
  - service: http_status:404
```

Run as a systemd service: `cloudflared-fossil`.

## Update flow

```sh
# On the VPS
cd ~/EdgeSync && git pull
cd deploy && sudo docker compose up -d --build
```

This rebuilds the leaf container with the latest code and restarts both services. NATS does not need to restart for a leaf upgrade.

## Endpoints

| Endpoint | URL | Access |
| --- | --- | --- |
| Public HTTPS | `https://sync.craftdesign.group` | Cloudflare Tunnel (anyone with the URL) |
| Tailscale HTTP | `http://100.78.32.45:9000` | Tailnet only |
| Tailscale NATS | `nats://100.78.32.45:4222` | Tailnet only |

## Mobile clients

For Expo / React Native clients (e.g. `edgesync-notify-app`), you'll typically:

1. Run a NATS leafnode on the VPS bound to port `7422` (separate from the regular `4222` so leafnode auth can be scoped)
2. Open the leafnode port to your tailnet (`ufw route allow ...`)
3. In the app, configure the NATS leafnode URL and pair via QR (see [Notify](./notify))

## Observability

Set `OTEL_EXPORTER_OTLP_ENDPOINT` and (if exporting to Honeycomb) the API key. EdgeSync uses [Doppler](https://doppler.com) to manage OTel secrets without committing `.env` files:

```sh
doppler run -- docker compose up -d --build
```

When the env var is unset the agent runs with a no-op observer — telemetry is zero-cost when not configured.

The reference Honeycomb dashboard ("EdgeSync Sim — Operational Overview") tracks sync latency, convergence, file throughput, and errors across the dataset `edgesync-sim` in the `test` environment.
