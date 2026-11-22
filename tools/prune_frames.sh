#!/usr/bin/env bash
# Keep only the newest frames of a growing frame-dump directory.
#
# Dolphin's "dump frames" option writes one PNG per frame (about 270 KB each, 60 per second) and a movie
# of a few minutes fills a RAM-backed /tmp. Run this next to the emulator to keep the last N frames:
#
#   tools/prune_frames.sh --dir <Dump/Frames> [--keep 3000] [--interval 5] [--while <process-name>]
#
# --dir       directory with files named <prefix>_<number>.<ext> (the number orders them)
# --keep      number of newest files to keep (default 3000)
# --interval  seconds between passes (default 5)
# --while     keep running while a process with this exact name exists (default: one pass
#             only). Example: --while dolphin-emu
# -h/--help   this text
set -euo pipefail
dir=""; keep=3000; interval=5; pattern=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) dir="$2"; shift 2 ;;
    --keep) keep="$2"; shift 2 ;;
    --interval) interval="$2"; shift 2 ;;
    --while) pattern="$2"; shift 2 ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option $1 (see --help)" >&2; exit 2 ;;
  esac
done
[ -n "$dir" ] || { echo "--dir is required (see --help)" >&2; exit 2; }

prune() {
  local n
  n=$(find "$dir" -maxdepth 1 -type f | wc -l)
  # leave some slack so that the sort is not run on every pass
  if [ "$n" -gt $((keep + keep / 6)) ]; then
    find "$dir" -maxdepth 1 -type f | sort -t_ -k2 -n | head -n $((n - keep)) | xargs -r rm -f
  fi
}

prune
if [ -n "$pattern" ]; then
  while pgrep -x "$pattern" > /dev/null; do
    sleep "$interval"
    prune
  done
fi
