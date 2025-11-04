#!/usr/bin/env bash
set -euo pipefail



if [ ! -f dev/bbb.mp4 ]; then
    echo "Downloading ya boye Big Buck Bunny..."
    wget http://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4 -O dev/bbb.mp4
fi


INPUT="${INPUT:-bbb.mp4}"
output="output_$(date +%Y%m%d_%H%M%S).mp4"

ffmpeg -hide_banner -loglevel warning -stats -y \
  -fflags +genpts \
  -i "$INPUT" \
  \
  -filter_complex "[0:v] \
    split=2[v_base][v_down]; \
    [v_base]scale=1280:720,setsar=1,format=yuv420p[v720]; \
    [v_down]scale=854:480,setsar=1,format=yuv420p[v480]" \
  \
  -map "[v720]" -map "[v480]" -map 0:a:0? \
  \
  -c:v:0 libx264 -b:v:0 2000k -preset fast -profile:v:0 high -g 48 -sc_threshold 0 \
  -c:v:1 libx264 -b:v:1 800k -preset fast -profile:v:1 main -g 48 -sc_threshold 0 \
  \
  -c:a aac -ar 48000 -b:a 128k -af aresample=async=1:first_pts=0 \
  -movflags +faststart+frag_every_frame+cmaf \
  -avoid_negative_ts make_zero -muxpreload 0 -muxdelay 0 \
  "$output"

echo "Kész: $output"
