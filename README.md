# EdgeSync

Replace Fossil's HTTP sync on leaf repos with NATS messaging. Master stays full Fossil with an HTTP translation bridge.

## Architecture

Two components:

1. **Leaf Agent** — Lightweight daemon on machines with Fossil repos. Reads/writes the SQLite repo DB directly. Publishes artifacts to NATS on commit, subscribes for incoming artifacts. JetStream consumer for offline catch-up.

2. **Bridge** — Speaks NATS on one side, HTTP `/xfer` on the other. To master Fossil it looks like a normal Fossil client doing push/pull. Master doesn't know NATS exists.

## Project Layout

```
cmd/
  leaf/           Leaf agent daemon
  bridge/         NATS <-> HTTP /xfer bridge
pkg/
  delta/          Fossil delta codec (port of delta.c)
  repo/           Fossil repo DB layer (SQLite)
  card/           Xfer card protocol encoder/decoder
  msync/          NATS messaging and sync logic
  hash/           SHA1/SHA3 content hashing
fossil/           Fossil SCM source checkout (reference)
libfossil/        Libfossil source checkout (reference)
```

## Phase Plan

1. **Delta codec** — Port Fossil's delta algorithm to Go
2. **Repo DB layer** — Open/read/write Fossil SQLite databases
3. **Card protocol** — Xfer card encoder/decoder, map to NATS messages
4. **Leaf agent** — Watch repo for changes, publish/subscribe via NATS
5. **Bridge** — NATS <-> HTTP /xfer translation
6. **Checkin support** — Full commit capability on leaves

## NATS Subject Namespace

```
fossil.<project-code>.igot    "I have this artifact" (UUID)
fossil.<project-code>.gimme   "I need this artifact" (UUID)
fossil.<project-code>.file    Artifact payload delivery
fossil.<project-code>.delta   Delta-compressed artifact
fossil.<project-code>.meta    Config, tickets, wiki changes
fossil.<project-code>.events  Commit notifications (CI/webhooks)
```

## Dependencies

- Go stdlib: `crypto/sha1`, `crypto/sha256`, `compress/zlib`, `net/http`
- `modernc.org/sqlite` — Pure Go SQLite driver
- `nats.go` — NATS + JetStream client

## Reference Material

- `fossil/` — Full Fossil SCM source (C, ~172K lines)
- `libfossil/checkout/` — Libfossil source (C99, ~93K lines)
- Key files: `libfossil/checkout/src/delta.c`, `libfossil/checkout/src/content.c`, `libfossil/checkout/src/xfer.c`, `libfossil/checkout/src/deck.c`
- [Fossil sync protocol](https://fossil-scm.org/home/doc/tip/www/sync.wiki)
- [Fossil delta format](https://fossil-scm.org/home/doc/tip/www/delta_format.wiki)
