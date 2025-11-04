#!/usr/bin/env bash
set -euo pipefail

# 1) Minden korábbi tmux session bezárása (ha van)
tmux kill-server 2>/dev/null || true

SESSION="moq"

# 2) Új Konsole ablak indítása tmux-szal
konsole -e bash -lc '

  tmux new-session -d -s "'"$SESSION"'" -n Relay      "bash -lc '\''./_rel.sh; exec bash'\''"
  tmux new-window     -t "'"$SESSION"'" -n Subscriber "bash -lc '\''./_sub.sh; exec bash'\''"
  tmux new-window     -t "'"$SESSION"'" -n Publisher  "bash -lc '\''./_pub.sh; exec bash'\''"

  tmux set -g mouse on
  tmux attach -t "'"$SESSION"'"
'
