#!/bin/bash
# Convert the rover's raw point logs (RTCM3) to RINEX .obs/.nav for PPK.
#
#   tools/rtcm2rinex.sh P0001_20261006T090507.rtcm3 [more.rtcm3 ...]
#
# Needs RTKLIB's convbin (Debian/Ubuntu: the rtklib package; Windows: the
# RTKCONV GUI from the same project does the same, with the format set to
# RTCM3 and the approximate time ticked).
#
# RTCM3 carries the time of week but not the week itself, so the converter is
# told roughly when the data was taken. The firmware puts that in the file
# name; a log named without it (no time from the receiver at the start) needs
# the date given by hand: APPROX="2026/10/06 09:00:00" tools/rtcm2rinex.sh ...
set -euo pipefail

command -v convbin >/dev/null || { echo "convbin not found - install RTKLIB" >&2; exit 1; }

for f in "$@"; do
  base=$(basename "$f" .rtcm3)
  approx=${APPROX:-}
  if [[ -z $approx && $base =~ _([0-9]{4})([0-9]{2})([0-9]{2})T([0-9]{2})([0-9]{2})([0-9]{2})$ ]]; then
    m=("${BASH_REMATCH[@]}")
    approx="${m[1]}/${m[2]}/${m[3]} ${m[4]}:${m[5]}:${m[6]}"
  fi
  [[ -n $approx ]] || { echo "$f: no date in the name, set APPROX" >&2; exit 1; }

  out=$(dirname "$f")/$base
  # RINEX 3.04 with Doppler and SNR; -oi/-ot add the ionosphere and time
# corrections to the header.
  convbin -r rtcm3 -tr "$approx" -v 3.04 -od -os -oi -ot \
    -o "$out.obs" -n "$out.nav" "$f"
  echo "$f -> $out.obs, $out.nav"
done
