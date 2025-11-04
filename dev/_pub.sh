#!/bin/bash
set -euo pipefail
cd ..

# Connect to localhost by default.
HOST="${HOST:-localhost}"
PORT="${PORT:-1234}"
ADDR="${ADDR:-$HOST:$PORT}"
SCHEME="${SCHEME:-moq}"

# Use the name "bbb" for the broadcast.
NAME="${NAME:-bbb}"

# Combine the host into a URL.
URL="${URL:-"$SCHEME://$ADDR"}"

# Default to a source video
INPUT="${INPUT:-dev/multi_frag.mp4}"

# Print out the watch URL
echo  "URL: $URL"

# Run ffmpeg and pipe the output to moq-pub

# stdout menjen csak a fájlba
exec 1>./dev/logs/pub_stdout.txt

# stderr menjen egyszerre terminálra ÉS fájlba
exec 2> >(tee ./dev/logs/log_output_pub.txt >&2)

ffmpeg -hide_banner -v quiet \
  -stream_loop -1 -re \
  -i "$INPUT" \
  -map 0 -c copy -f mp4 \
  -movflags +empty_moov+cmaf+separate_moof+skip_trailer+faststart+frag_keyframe \
  -max_interleave_delta 0 -muxpreload 0 -muxdelay 0 \
  -frag_duration 250000 \
  -flush_packets 1 \
  -avioflags direct \
  -fflags flush_packets \
  -fflags nobuffer \
  - | ./build/cmd/examples/qclient2 -r $URL --trace -e PUBLISHER --use_announce --pub_namespace bbb --pub_name catalog --video

#  -max_interleave_delta 0 -muxpreload 0 -muxdelay 0 \
#  -flush_packets 1 \
#  -avioflags direct \
#  -fflags flush_packets \
#  -fflags nobuffer \
