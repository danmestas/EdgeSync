#!/usr/bin/env bash
set -euo pipefail

SITE_DIR="docs/site"
STATIC_DIR="$SITE_DIR/static"

mkdir -p "$STATIC_DIR"

# llms.txt: curated summary
cat > "$STATIC_DIR/llms.txt" << 'HEADER'
# EdgeSync

> NATS-native sync engine for Fossil SCM repositories.
> Leaf agents exchange blobs over NATS messaging instead of HTTP.
> Optional bridge translates NATS to HTTP /xfer for legacy interop.

## Key Concepts
- Leaf Agent — daemon that reads/writes a .fossil repo and syncs over NATS
- Bridge — translates NATS messages to HTTP /xfer cards for unmodified Fossil servers
- NATS Mesh — peer/hub/leaf topology with embedded NATS server in each agent
- Iroh — optional P2P QUIC transport for NAT traversal between leaves
- Notify — bidirectional messaging for human-in-the-loop AI workflows

## Quick Start
1. go install github.com/danmestas/EdgeSync/cmd/edgesync@latest
2. edgesync sync start --repo my.fossil --serve-http :9000
3. Add a peer: --iroh --iroh-peer <ticket>
4. Send a notification: edgesync notify send --project my-proj --body "hello"

## Modules
- cmd/edgesync — unified CLI (sync, bridge, notify, doctor, libfossil commands)
- leaf/ — leaf agent module (sync daemon, NATS mesh, optional iroh)
- bridge/ — NATS-to-HTTP bridge module
- dst/ — deterministic simulation tests
- External: github.com/danmestas/libfossil (pure-Go Fossil library)

## Documentation
Full docs: https://github.com/danmestas/EdgeSync/tree/main/docs/site
HEADER

# llms-full.txt: all content concatenated, frontmatter stripped
echo "# EdgeSync Documentation (Full)" > "$STATIC_DIR/llms-full.txt"
echo "" >> "$STATIC_DIR/llms-full.txt"
echo "Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$STATIC_DIR/llms-full.txt"
echo "" >> "$STATIC_DIR/llms-full.txt"

find "$SITE_DIR/content" -name "*.md" -type f | sort | while read -r file; do
    echo "---" >> "$STATIC_DIR/llms-full.txt"
    echo "# Source: $file" >> "$STATIC_DIR/llms-full.txt"
    echo "" >> "$STATIC_DIR/llms-full.txt"
    awk 'NR==1 && /^---$/ { in_fm=1; next }
     in_fm && /^---$/ { in_fm=0; next }
     !in_fm' "$file" >> "$STATIC_DIR/llms-full.txt"
    echo "" >> "$STATIC_DIR/llms-full.txt"
done

echo "Generated llms.txt and llms-full.txt in $STATIC_DIR/"
