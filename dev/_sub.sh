#!/bin/bash
set -euo pipefail

cd ..

# Connect to localhost by default.
HOST="${HOST:-localhost}"
PORT="${PORT:-1234}"
ADDR="${ADDR:-$HOST:$PORT}"

# Use the broadcast name "bbb" by default
NAME="${NAME:-bbb}"

# Combine the host and name into a URL.
URL="${URL:-"moq://$ADDR"}"

exec > >(tee ./dev/logs/sub_out.txt) 2>&1

# stderr menjen egyszerre terminálra ÉS fájlba
exec 2> ./dev/logs/sub_error.txt

GST_DEBUG_DUMP_DOT_DIR=logs GST_VIDEOSINK=waylandsink ./build/cmd/examples/qc_video -r moq://localhost:1234 -t -e SUBSCRIBER \
  --sub_announces bbb --sub_namespace bbb --sub_name catalog

  #app*:6,qtdemux:6,decodebin3:6,parsebin:6,pad*:6
#GST_DEBUG=qtdemux:5,input-selector:5,GST_PADS:4,basesink:4,queue:4,vdec:3,element:3,*WARN*:1,*ERROR*:1
