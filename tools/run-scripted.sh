#!/usr/bin/env bash
# Run a harness with a scripted-input file (docs/INPUT_SCRIPT.md), a fresh blank memory card image,
# screenshots and a log.
#
#   tools/run-scripted.sh --bin tos_boot --dol main.dol --files DISC_FILES_DIR --script s.txt \
#       [--seconds 120] [--out DIR] [--shot-every N] [--keep-card] [--script more.txt ...] [--card-in FILE]
#
# --script may be given several times: the files run one after the other (segments: new-game, then
# classroom, ...). --card-in starts from a copy of that card image instead of a blank one (a card
# that already holds a save, to skip the segments before it).
# Writes DIR/*.ppm (named `shot` commands and every Nth frame), DIR/run.log, DIR/script.txt (the
# scripts joined) and DIR.card (the card image, recreated unless --keep-card). Prints the [script] lines of the log. Set TMPDIR to a disk if
# /tmp is a small tmpfs (the compiler and the screenshots can fill it).
set -euo pipefail
bin=""; dol=""; files=""; scripts=(); seconds=120; out=""; every=600; keep=0; card_in=""
while [ $# -gt 0 ]; do
  case "$1" in
    --bin) bin="$2"; shift 2 ;; --dol) dol="$2"; shift 2 ;; --files) files="$2"; shift 2 ;;
    --script) scripts+=("$2"); shift 2 ;; --card-in) card_in="$2"; shift 2 ;; --seconds) seconds="$2"; shift 2 ;; --out) out="$2"; shift 2 ;;
    --shot-every) every="$2"; shift 2 ;; --keep-card) keep=1; shift ;;
    -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/, ""); print; next} NR>1 {exit}' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$bin" ] && [ -n "$dol" ] && [ -n "$files" ] && [ "${#scripts[@]}" -gt 0 ] || { echo "need --bin --dol --files --script" >&2; exit 2; }
out="${out:-${TMPDIR:-/tmp}/scripted-run}"
rm -rf "$out"; mkdir -p "$out"; [ "$keep" = 1 ] || rm -f "$out.card"
[ -z "$card_in" ] || cp "$card_in" "$out.card"
for f in "${scripts[@]}"; do cat "$f"; echo; done > "$out/script.txt"
ulimit -s unlimited
GCRECOMP_CARD_IMAGE="$out.card" TOS_SCRIPT="@$out/script.txt" TOS_SCREENSHOT_DIR="$out" GP_SHOT_EVERY="$every" GCRECOMP_AUDIO=null \
  timeout $((seconds + 40)) "$bin" "$dol" "$files" "$seconds" > "$out/run.log" 2>&1 || true
grep '^\[script\]' "$out/run.log" || true
