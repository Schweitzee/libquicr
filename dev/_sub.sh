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

# stdout menjen csak a fájlba
exec 1> ./dev/logs/sub_stdout.txt

# stderr menjen egyszerre terminálra ÉS fájlba
exec 2> >(tee ./dev/logs/gst.txt >&2)

GST_DEBUG=qtdemux:5,input-selector:5,GST_PADS:4,basesink:4,queue:4,vdec:3,element:3,*WARN*:1,*ERROR*:1 GST_DEBUG_DUMP_DOT_DIR=dev/logs GST_VIDEOSINK=waylandsink ./build/cmd/examples/qclient2 -r moq://localhost:1234 -e SUBSCRIBER \
  --sub_announces bbb --sub_namespace bbb --sub_name catalog

  #app*:6,qtdemux:6,decodebin3:6,parsebin:6,pad*:6
