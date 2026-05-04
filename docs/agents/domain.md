# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

## Layout

Single-context repo. ADRs (architectural decision records) live in `docs/architecture/` rather than `docs/adr/`:

```
/
├── CLAUDE.md                      ← project instructions
├── MEMORY.md                      ← cross-session project memory
├── docs/
│   ├── architecture/              ← condensed ADRs (read these first)
│   │   ├── core-library.md
│   │   ├── sync-protocol.md
│   │   ├── agent-deployment.md
│   │   ├── checkout-merge.md
│   │   ├── repo-operations.md
│   │   ├── testing-strategy.md
│   │   ├── notify-messaging.md
│   │   └── wasm-targets.md
│   └── superpowers/specs/         ← design specs for in-flight features
└── src/
```

There is no `CONTEXT.md` glossary yet. The closest equivalents are `CLAUDE.md` (architecture overview, conventions) and `MEMORY.md` (decisions, in-flight projects, user preferences). When a skill asks for `CONTEXT.md`, fall back to those.

## Before exploring, read these

- **`CLAUDE.md`** at the repo root — architecture, conventions, build/test commands.
- **`MEMORY.md`** at the repo root — recent decisions and project state.
- **`docs/architecture/*.md`** — read the ADR(s) covering the area you're about to work in.
- **`docs/superpowers/specs/`** — design specs for in-flight features.

If a relevant ADR doesn't exist, **proceed silently**. Don't flag its absence; don't suggest creating one upfront.

## Use the project's vocabulary

When naming domain concepts (in issue titles, refactor proposals, hypotheses, test names), use the terms defined in `CLAUDE.md` and the ADRs. Don't drift to synonyms.

Examples of canonical terms:
- "leaf agent" not "node" or "client daemon"
- "bridge" not "gateway" or "proxy" (specifically: NATS↔HTTP/xfer translator)
- "xfer cards" not "sync messages"
- "libfossil" — the external Go module (`github.com/danmestas/libfossil`)
- "notify" — the bidirectional messaging subsystem, distinct from "sync"
- "DST" — deterministic simulation testing (in `dst/`)
- "BUGGIFY" — the FoundationDB-style fault-injection pattern

If the concept you need isn't established yet, that's a signal — either you're inventing language the project doesn't use (reconsider) or there's a real gap (flag it).

## Flag ADR conflicts

If your output contradicts an existing ADR in `docs/architecture/`, surface it explicitly rather than silently overriding:

> _Contradicts `docs/architecture/sync-protocol.md` — but worth reopening because…_
