---
title: Documentation
cascade:
  type: docs
---

EdgeSync is a NATS-native sync engine for [Fossil SCM](https://fossil-scm.org) repositories. Leaf agents exchange blobs and notifications over NATS messaging — with optional bridge mode for talking to unmodified Fossil servers over HTTP.

## What you'll find here

- **[Quickstart](./quickstart)** — install the binary and stand up a two-node mesh in five minutes.
- **[Concepts](./concepts)** — the leaf agent, bridge, NATS mesh roles, iroh tunneling, and notify messaging.
- **[Architecture](./architecture)** — how the leaf agent embeds NATS and libfossil, the role-based topology, and the sync wire protocol.
- **[Deployment](./deployment)** — running EdgeSync on a VPS with Docker Compose, Tailscale, and Cloudflare Tunnel.
- **[Notify](./notify)** — bidirectional messaging for human-in-the-loop AI workflows: project-scoped subjects, threads, and the conversation model.
