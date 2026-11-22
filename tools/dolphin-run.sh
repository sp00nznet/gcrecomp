#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Ported from the author's own fepor-recomp (tools/dolphin-run.sh), same author.
# Run the game in Dolphin (the reference emulator) in an isolated user directory, driven through Dolphin's
# named-pipe controller, and take screenshots: for comparing saves, memory cards, frames and audio with the recomp.
#
# Usage: tools/dolphin-run.sh --iso <disc> [--card <raw> | --gci <save.gci>] [--user <dir>] -- <step>...
# Steps run in order: wait:<seconds>  press:<A|B|X|Y|Z|START|L|R|D_UP|D_DOWN|D_LEFT|D_RIGHT>  mash:<button>:<seconds>  shot:<file.png>
# --card <raw>: raw image as slot A (Dolphin may write to it: pass a copy). --gci <file>: a GCI-folder card holding that one save.
# --dump-audio: Dolphin writes its audio to <user>/Dump/Audio/*dspdump.wav (compare with GCRECOMP_DUMP_AUDIO using tools/wav-compare.py).
# --dump-frames: Dolphin writes the video to <user>/Dump/Frames/*.avi (extract frames with ffmpeg to see what was on screen).
# Needs dolphin-emu, spectacle (KDE) and ImageMagick for the screenshots. Linux only.
set -euo pipefail
iso=""; card=""; gci=""; user="/tmp/dolphin-run-user"; dump_audio=0; dump_frames=0
while [ $# -gt 0 ]; do
  case "$1" in
    --iso) iso="$2"; shift 2 ;;
    --card) card="$2"; shift 2 ;;
    --gci) gci="$2"; shift 2 ;;
    --user) user="$2"; shift 2 ;;
    --dump-audio) dump_audio=1; shift ;;
    --dump-frames) dump_frames=1; shift ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$iso" ] || { echo "--iso is required" >&2; exit 2; }
if [ -n "$card" ] && [ -n "$gci" ]; then echo "use --card or --gci, not both" >&2; exit 2; fi

rm -rf "$user"; mkdir -p "$user/Config" "$user/Pipes"
slot=255
if [ -n "$card" ]; then slot=1; fi
if [ -n "$gci" ]; then
  # a GCI folder card (Dolphin's default): the saves are loose .gci files
  slot=8
  mkdir -p "$user/GC/EUR/Card A"
  cp "$gci" "$user/GC/EUR/Card A/"
fi
cat > "$user/Config/Dolphin.ini" <<EOF
[Core]
SkipIPL = True
SlotA = $slot
SlotB = 255
$([ -n "$card" ] && echo "MemcardAPath = $card")
[DSP]
DumpAudio = $([ "$dump_audio" = 1 ] && echo True || echo False)
[Interface]
ConfirmStop = False
EOF
# [Movie]: dump the video (lossless AVI) so the scene at each moment can be checked offline
if [ "$dump_frames" = 1 ]; then
  printf '[Movie]\nDumpFrames = True\nDumpFramesSilent = True\n' >> "$user/Config/Dolphin.ini"
fi
cat > "$user/Config/GCPadNew.ini" <<'EOF'
[GCPad1]
Device = Pipe/0/pad
Buttons/A = `Button A`
Buttons/B = `Button B`
Buttons/X = `Button X`
Buttons/Y = `Button Y`
Buttons/Z = `Button Z`
Buttons/Start = `Button START`
Triggers/L = `Button L`
Triggers/R = `Button R`
D-Pad/Up = `Button D_UP`
D-Pad/Down = `Button D_DOWN`
D-Pad/Left = `Button D_LEFT`
D-Pad/Right = `Button D_RIGHT`
Main Stick/Up = `Axis Y +`
Main Stick/Down = `Axis Y -`
Main Stick/Left = `Axis X -`
Main Stick/Right = `Axis X +`
EOF
mkfifo "$user/Pipes/pad"

dolphin-emu -u "$user" -b -e "$iso" > "$user/dolphin.log" 2>&1 &
dolphin=$!
trap 'kill $dolphin 2>/dev/null; pkill -f "dolphin-emu-x11 -u $user" 2>/dev/null || true' EXIT
exec 3<>"$user/Pipes/pad"
for step in "$@"; do
  case "$step" in
    wait:*) sleep "${step#wait:}" ;;
    press:*) echo "PRESS ${step#press:}" >&3; sleep 0.3; echo "RELEASE ${step#press:}" >&3 ;;
    stick:*)
      # tilt the main stick: stick:left|right|up|down (menus often read the stick, not the D-pad)
      case "${step#stick:}" in
        left) xy="0 0.5" ;; right) xy="1 0.5" ;; up) xy="0.5 1" ;; down) xy="0.5 0" ;;
        *) echo "unknown stick direction ${step#stick:}" >&2; exit 2 ;;
      esac
      echo "SET MAIN $xy" >&3; sleep 0.4; echo "SET MAIN 0.5 0.5" >&3 ;;
    mash:*)
      # press a button every 1.5 s for the given time: gets through screens whose timing varies
      IFS=: read -r _ btn secs <<< "$step"
      end=$((SECONDS + secs))
      while [ "$SECONDS" -lt "$end" ]; do
        echo "PRESS $btn" >&3; sleep 0.3; echo "RELEASE $btn" >&3; sleep 1.2
      done ;;
    shot:*) spectacle -b -n -f -o "${step#shot:}" > /dev/null 2>&1 ;;
    *) echo "unknown step $step" >&2; exit 2 ;;
  esac
done
