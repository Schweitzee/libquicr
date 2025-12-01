#!/bin/bash
set -euo pipefail

cd ..

HOST="${HOST:-localhost}"
PORT="${PORT:-1234}"
ADDR="${ADDR:-$HOST:$PORT}"
URL="moq://$ADDR"

# GST Video sink beállítása (Linuxon autovideosink vagy waylandsink/xvimagesink)
export GST_VIDEOSINK="${GST_VIDEOSINK:-autovideosink}"

exec > >(tee ./dev/logs/req1_out.txt) 2>&1

echo "Starting Request Client1..."
echo "Video Sink: $GST_VIDEOSINK"

# Interaktív mód miatt az stdout-ot hagyjuk a terminálon
# A logokat a spdlog kezeli (console sink)

./build/cmd/examples/qc_request \
  -r "$URL" \
  -e requester_client1 \
  -n bbb
