#!/bin/bash
set -euo pipefail

cd ..

HOST="${HOST:-localhost}"
PORT="${PORT:-1234}"
ADDR="${ADDR:-$HOST:$PORT}"
URL="moq://$ADDR"


exec > >(tee ./dev/logs/trans_out.txt) 2>&1

echo "Starting Transcoder Service..."

# A cpp fájl alapján a --sub_namespace paramétert használja root namespace-ként
./build/cmd/examples/qc_transcode \
  -d \
  -r "$URL" \
  -e transcoder_service \
  --sub_namespace bbb
