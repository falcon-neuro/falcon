#!/bin/sh
# Builds the websites (see doc_gen.sh) and serves them to this machine only, until Ctrl+C. Saving a
# page rebuilds it.
# Usage: sh scripts/doc_serve.sh [port]
set -e
cd "$(dirname "$0")/.."

PORT="${1:-8765}"

sh scripts/doc_gen.sh
exec build/website_env/bin/zensical serve -f docs/zensical.toml -a "127.0.0.1:${PORT}"
