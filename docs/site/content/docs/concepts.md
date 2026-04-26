---
title: Concepts
weight: 15
---

EdgeSync is a small set of orthogonal pieces. Read this once and the rest of the docs will make sense.

## Leaf agent

The **leaf agent** is the only required process. It is a long-running Go daemon that:

1. **Owns a `.fossil` repo** — opens the SQLite database directly via [libfossil](https://github.com/danmestas/libfossil), no `fossil` binary required.
2. **Embeds a NATS server** — every agent runs its own NATS server in-process, configured per role (peer, hub, leaf). Two agents on the same host can mesh without a separate broker.
3. **Optionally serves HTTP** — exposes Fossil's `/xfer` protocol on a port so unmodified Fossil clients can clone and pull as if it were a regular Fossil server.
4. **Optionally serves NATS sync** — listens on a NATS subject for sync requests from other leaves, bypassing HTTP entirely.

The agent runs as a single binary with flags. Configuration lives in CLI flags and environment variables; there is no config file.

## Bridge

The **bridge** is an optional process that translates between NATS messages and HTTP `/xfer` cards. You only need it when one side of a sync is an unmodified Fossil server (the upstream `fossil` binary) that does not speak NATS.

```
NATS leaf agent ─NATS─► bridge ─HTTP /xfer─► fossil server
```

Bridge mode is useful for migrations: you can swap one side of a sync to EdgeSync without changing the other.

## NATS mesh roles

When you start a leaf agent you pick a role with `--nats-role`:

- **`peer`** — full NATS server, equal to other peers. Use this for symmetric meshes (laptop ↔ laptop, server ↔ server).
- **`hub`** — accepts inbound leaf connections. Use this for a central VPS that many edge devices connect to.
- **`leaf`** — connects outward to a hub via NATS leafnode protocol. Use this for an edge device behind NAT.

You can also point at an external NATS server with `--nats-upstream nats://host:4222` if you already run NATS infrastructure.

## Iroh tunneling

For peers that cannot reach each other over plain TCP (NAT-bound laptops, home networks, mobile devices), EdgeSync can run NATS leafnode connections over [iroh](https://iroh.computer/) QUIC streams.

```sh
edgesync sync start --iroh --iroh-key node.key --iroh-peer <ticket>
```

Iroh handles relay-fallback hole-punching automatically. The first time you start with `--iroh`, the agent generates a key and prints a connection ticket; share that ticket with peers.

## Notify

The **notify** subsystem is a separate concern from repo sync. It provides project-scoped messaging primitives for human-in-the-loop AI workflows:

- Messages have a project, thread, optional priority, and optional action button.
- Storage lives in a dedicated `notify.fossil` repo — independent of your code repos.
- Wire protocol uses NATS pub-sub on subjects like `notify.<project>.<thread-short>`.
- A `pair`/`unpair` flow with QR codes lets mobile clients enroll without typing IP:port.

See [Notify](./notify) for the full data model and CLI reference.

## What goes where

| Concern | Lives in |
| --- | --- |
| Repo storage and sync logic | [libfossil](https://github.com/danmestas/libfossil) (external dependency) |
| NATS server + sync glue | `leaf/agent/` |
| HTTP `/xfer` serving | `leaf/agent/serve_http.go` |
| NATS↔HTTP translation | `bridge/` |
| Bidirectional messaging | `leaf/agent/notify/` |
| Deterministic simulation | `dst/` (sibling module) |
| CLI binary | `cmd/edgesync/` |
