#!/usr/bin/env bash
# Build the EdgeSync docs site. Used by both wrangler.jsonc (Cloudflare's
# Linux build container) and local `wrangler deploy` from macOS.
#
# Strategy:
# - If `hugo` is on PATH, use it (developer machines, CI with hugo preinstalled).
# - Otherwise download the Linux extended build into the repo root and use that
#   (Cloudflare Workers build container, which has no Hugo by default).
set -euo pipefail

HUGO_VERSION="0.160.0"

if command -v hugo >/dev/null 2>&1; then
    HUGO=hugo
else
    if [ ! -x ./hugo ]; then
        echo "Downloading Hugo extended ${HUGO_VERSION} (linux-amd64)..."
        curl -sSL "https://github.com/gohugoio/hugo/releases/download/v${HUGO_VERSION}/hugo_extended_${HUGO_VERSION}_linux-amd64.tar.gz" | tar xz hugo
    fi
    HUGO=./hugo
fi

bash scripts/gen-llms-txt.sh

"${HUGO}" -s docs/site --minify
