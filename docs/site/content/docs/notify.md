---
title: Notify
weight: 40
---

EdgeSync's notify subsystem is a separate channel for project-scoped, bidirectional messaging. It is designed for human-in-the-loop AI workflows: an agent pings a human, the human replies from a phone, the agent acts on the reply.

## Why a separate channel?

Repo sync moves blobs between agents to converge `.fossil` state. Notify moves *messages* between agents and humans to coordinate work outside the repo. They share infrastructure (the same NATS mesh) but have different lifecycles, retention, and storage.

| | Repo sync | Notify |
| --- | --- | --- |
| Subjects | sync request/reply | `notify.<project>.<thread-short>` (pub-sub) |
| Storage | `*.fossil` (your code repos) | `notify.fossil` (dedicated) |
| Durability | full repo history | log + thread index |
| Retention | forever | configurable per-project |

## Data model

Each message has:

- **project** — namespace string (e.g. `edgesync`, `client-x-deploy`)
- **thread** — opaque short ID; replies share the thread
- **body** — markdown text
- **priority** — `low` / `normal` / `high`
- **action** — optional structured action (button label + payload)
- **author** — who sent it (agent name or human handle)

A **thread** is the conversational unit. Sending a fresh message creates a new thread; replying targets an existing one.

## CLI

Initialize storage (creates `notify.fossil` if missing):

```sh
edgesync notify init --project my-project
```

Send a one-shot notification:

```sh
edgesync notify send \
  --project my-project \
  --priority high \
  --body "Deploy failed: see logs at /var/log/deploy.log"
```

Ask a question (waits for a reply):

```sh
edgesync notify ask \
  --project my-project \
  --body "Approve the prod migration?" \
  --action approve --action reject
```

Watch incoming messages on a project:

```sh
edgesync notify watch --project my-project
```

List threads:

```sh
edgesync notify threads --project my-project
```

Read a thread:

```sh
edgesync notify log --project my-project --thread <id>
```

Status (subscriber liveness, last activity):

```sh
edgesync notify status --project my-project
```

## NATS subjects

Notify uses pub-sub on subjects of the form:

```
notify.<project>.<thread-short>
```

`<thread-short>` is the first eight characters of the thread ID, so a wildcard subscribe `notify.my-project.>` catches all threads in a project. Subscribers dedupe on the full message ID.

## Pairing mobile clients

The pairing flow lets a phone enroll with a leaf agent without typing IP:port. On the agent:

```sh
edgesync pair --project my-project
```

This prints a QR code (and a fallback URL). The mobile app scans the QR, exchanges a one-time token, and stores a long-lived device record in the agent's device registry. From then on, the device receives notifications for that project over its NATS leafnode connection.

To revoke:

```sh
edgesync devices --project my-project    # list paired devices
edgesync unpair --project my-project --device <id>
```

## Storage

Notify storage is a regular libfossil-managed `.fossil` file at `notify.fossil` (or wherever `--notify-repo` points). It uses Fossil's unversioned-content (UV) tables for the message log and a regular versioned tree for the thread index. This means the existing sync machinery handles notify replication for free — a notify repo can be replicated to other leaves the same way a code repo is.
