#!/bin/bash
set -euo pipefail

cd ..

# Connect to localhost by default.
HOST="${HOST:-localhost}"
PORT="${PORT:-1234}"
ADDR="${ADDR:-$HOST:$PORT}"
SCHEME="${SCHEME:-moq}"

# URL összeállítása
URL="${URL:-"$SCHEME://$ADDR"}"

INPUT="${INPUT:-dev/test_source.mp4}"


exec > >(tee ./dev/logs/pub_out.txt) 2>&1

echo "Starting Publisher on $URL with namespace IB027..."

# FFmpeg parancs és pipe a qc_video-ba
# Fontos: --pub_namespace IB027 beállítva
ffmpeg -hide_banner -v quiet \
  -stream_loop -1 -re \
  -i "$INPUT" \
  -map 0 -c copy -f mp4 \
  -movflags +empty_moov+cmaf+separate_moof+skip_trailer+faststart+frag_every_frame \
  -max_interleave_delta 0 -muxpreload 0 -muxdelay 0 \
  -flush_packets 1 \
  -avioflags direct \
  -fflags flush_packets+nobuffer \
  - | ./build/cmd/examples/qc_video -t -r "$URL" -e PUBLISHER \
      --use_announce \
      --pub_namespace bbb \
      --pub_name catalog \
      --video
