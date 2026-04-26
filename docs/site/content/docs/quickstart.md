---
title: Quickstart
weight: 10
---

A five-minute tour of EdgeSync: install the binary, point it at a `.fossil` repo, and stand up a two-node mesh.

## Install

```sh
go install github.com/danmestas/EdgeSync/cmd/edgesync@latest
edgesync --help
```

The binary is pure-Go and CGo-free thanks to the [libfossil](https://github.com/danmestas/libfossil) library underneath — drop it on any Linux/macOS/Windows host with no runtime dependency.

Verify your environment:

```sh
edgesync doctor
```

## Create a repository

EdgeSync embeds libfossil's CLI, so the standard repo commands work directly:

```sh
edgesync new my-project.fossil
edgesync open my-project.fossil checkout/
cd checkout/
echo '# Hello' > README.md
edgesync ci -m "initial commit"
```

## Start the leaf agent

The leaf agent is a long-running daemon that watches a repo and syncs it with peers over NATS:

```sh
edgesync sync start \
  --repo my-project.fossil \
  --serve-http :9000 \
  --poll 5s
```

What this does:

- Embeds a NATS server inside the agent process
- Serves the Fossil `/xfer` endpoint on `:9000` so other Fossil clients can clone over HTTP
- Polls the repo every 5 seconds for local changes to gossip out

## Add a peer over iroh

For NAT-traversing peer-to-peer sync, generate an iroh ticket on one node and pass it to another:

```sh
# Node A
edgesync sync start --repo a.fossil --iroh --iroh-key node-a.key

# Node B (paste the ticket from node A's logs)
edgesync sync start --repo b.fossil --iroh --iroh-peer <ticket-from-a>
```

The two leaves now exchange blobs over a QUIC tunnel — no port forwarding, no central server.

## Send a notification

EdgeSync ships with a separate notify channel for project-scoped messaging (designed for human-in-the-loop AI workflows):

```sh
edgesync notify init --project my-project
edgesync notify send --project my-project --body "deploy succeeded"
edgesync notify watch --project my-project
```

See [Notify](./notify) for thread structure, replies, and the pairing flow for mobile clients.

## Next steps

- [Concepts](./concepts) — the moving pieces and how they fit together
- [Architecture](./architecture) — wire protocol, role topology, and observability
- [Deployment](./deployment) — Docker Compose recipe for production
- [Notify](./notify) — bidirectional messaging deep dive
