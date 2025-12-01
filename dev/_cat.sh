#!/bin/bash
set -euo pipefail

cd ..

HOST="${HOST:-localhost}"
PORT="${PORT:-1234}"
ADDR="${ADDR:-$HOST:$PORT}"
URL="moq://$ADDR"


exec > >(tee ./dev/logs/cat_out.txt) 2>&1

echo "Starting Catalog Maker on namespace IB027..."

# -n IB027: Root namespace
# -f false: Delta update-eket küldjön (alapértelmezett)
./build/cmd/examples/qc_catalog_maker \
  -r "$URL" \
  -e catalog_maker \
  -n bbb
