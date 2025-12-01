#!/usr/bin/env bash
set -euo pipefail

cd ..

# 1) Minden korábbi tmux session bezárása (tiszta lap)
tmux kill-server 2>/dev/null || true

# Alap könyvtár beállítása (ahol a scriptek és a libquicr mappa van)

SESSION="moq_demo"

# Log mappa takarítása/létrehozása
mkdir -p "dev/logs"

echo "Indítás..."

# 2) Új Konsole ablak indítása tmux-szal
# Ablakok sorrendje: Relay -> Publisher -> Catalog Maker -> Transcoder -> Request Client (Aktív)
konsole -e bash -lc '
  cd "'"$BASE"'" || exit 1

  # 1. Ablak: Relay (Szerver)
  tmux new-session -d -s "'"$SESSION"'" -n Relay "bash -lc '\''./dev/_rel.sh; exec bash'\''"

  # 2. Ablak: Publisher (Forrás videó)
  tmux new-window -t "'"$SESSION"'" -n Publisher "bash -lc '\''sleep 1; ./dev/_pub.sh; exec bash'\''"

  # 3. Ablak: Catalog Maker (Logika)
  tmux new-window -t "'"$SESSION"'" -n CatMaker "bash -lc '\''sleep 2; ./dev/_cat.sh; exec bash'\''"

  # 4. Ablak: Transcoder (Worker)
  tmux new-window -t "'"$SESSION"'" -n Transcoder "bash -lc '\''sleep 2; ./dev/_trans.sh; exec bash'\''"

  # 5. Ablak: Request Client (Interaktív felhasználó)
  # Ide switch-elünk a végén, hogy a felhasználó rögtön tudjon gépelni
  tmux new-window -t "'"$SESSION"'" -n Request1 "bash -lc '\''sleep 3; ./dev/_req1.sh; exec bash'\''"

  # 6. Ablak: Request Client (Interaktív felhasználó)
  # tmux new-window -t "'"$SESSION"'" -n Request2 "bash -lc '\''sleep 3; ./dev/_req2.sh; exec bash'\''"

  # 7. Ablak: Simple subscribe Client (Interaktív felhasználó)
  tmux new-window -t "'"$SESSION"'" -n SimpleSub "bash -lc '\''sleep 3; ./dev/_sub.sh; exec bash'\''"

  # Egér támogatás bekapcsolása tmux-ban
  tmux set -g mouse on

  # Fókusz a Request kliensre
  tmux select-window -t "'"$SESSION"':Request1"

  # Csatlakozás a sessionhöz
  tmux attach -t "'"$SESSION"'"
'
