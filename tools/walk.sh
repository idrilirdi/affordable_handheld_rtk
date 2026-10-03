#!/bin/bash
# Record a walked RTK test. Run it, walk, press Ctrl-C when you are done.
#
#   tools/walk.sh [RTK-Rover-XXXX]
#
# Joins the rover's setup network with NetworkManager when a name is given;
# otherwise the laptop must already be on it. Everything lands in logs/. The
# laptop is kept awake for the duration, since a suspend halfway through loses
# the rest of the run.
cd "$(dirname "$0")/.." || exit 1
OUT="logs/walk-$(date +%Y%m%d-%H%M).csv"
mkdir -p logs

if [ -n "${1:-}" ] && ! nmcli -t -f NAME con show --active | grep -qx "$1"; then
  echo "joining $1..."
  nmcli con up "$1" >/dev/null 2>&1
  sleep 3
fi

# Reassociating after a profile change can take a few seconds, and a single
# timeout is no reason to refuse a walk - dropouts are recorded, not fatal.
ok=0
for i in 1 2 3 4 5; do
  if curl -s --max-time 4 http://192.168.4.1/d.txt >/dev/null; then ok=1; break; fi
  sleep 2
done
[ "$ok" = 1 ] || echo "WARNING: rover not answering yet - recording anyway, gaps are logged"

trap 'echo' INT

echo "recording to $OUT   -   Ctrl-C to stop"
exec systemd-inhibit --what=idle:sleep:handle-lid-switch --why="RTK walk" \
     python3 tools/logpanel.py --out "$OUT" --interval 1 --live
