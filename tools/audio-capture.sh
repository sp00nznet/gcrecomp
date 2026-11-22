#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Record what a program really sends to the host audio server (PulseAudio / PipeWire-pulse), to check
# that the default SDL sink, not only the dump sink, delivers sound.
#
# Usage: tools/audio-capture.sh --out capture.wav [--rate 32000] [--match NAME] -- <command> [args...]
#
# Creates a temporary null sink and records its monitor. The command runs normally; every playback
# stream opened by it or its descendants is moved to the null sink as soon as it appears (other
# applications are left alone), so nothing reaches the speakers. A stream is the command's when its
# application.process.id is a descendant of the command, or its application.name / media.name equals
# the command's file name (SDL's pipewire driver sets no process id); --match NAME overrides that name.
# Removes the sink afterwards.
# Analyse the result with tools/wav-compare.py. Needs pactl, parecord and python3.
set -euo pipefail
out=""; rate=32000; match=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out="$2"; shift 2 ;;
    --rate) rate="$2"; shift 2 ;;
    --match) match="$2"; shift 2 ;;
    -h|--help) sed -n 2,14p "$0"; exit 0 ;;
    --) shift; break ;;
    *) echo "unknown option $1 (see --help)" >&2; exit 2 ;;
  esac
done
[ -n "$out" ] && [ $# -gt 0 ] || { echo "usage: $0 --out file.wav [--rate N] [--match NAME] -- command..." >&2; exit 2; }
name="gcrecomp_capture_$$"
module=$(pactl load-module module-null-sink "sink_name=$name" "sink_properties=device.description=$name")
rec=""; mover=""
cleanup() {
  [ -n "$mover" ] && kill "$mover" 2>/dev/null || true
  [ -n "$rec" ] && kill -INT "$rec" 2>/dev/null && wait "$rec" 2>/dev/null || true
  pactl unload-module "$module" 2>/dev/null || true
}
trap cleanup EXIT
parecord --device="$name.monitor" --file-format=wav --channels=2 --rate="$rate" "$out" &
rec=$!
"$@" &
cmd=$!
[ -n "$match" ] || match=$(basename "$1")
python3 - "$cmd" "$name" "$match" <<'PY' &
import json, os, subprocess, sys, time

root, sink, match = int(sys.argv[1]), sys.argv[2], sys.argv[3]

def descendants(pid):
    kids = {root}
    changed = True
    while changed:
        changed = False
        for d in os.listdir("/proc"):
            if d.isdigit():
                try:
                    with open(f"/proc/{d}/stat") as f:
                        ppid = int(f.read().rsplit(")", 1)[1].split()[1])
                except OSError:
                    continue
                if ppid in kids and int(d) not in kids:
                    kids.add(int(d))
                    changed = True
    return kids

while os.path.exists(f"/proc/{root}"):
    kids = descendants(root)
    out = subprocess.run(["pactl", "-f", "json", "list", "sink-inputs"], capture_output=True, text=True).stdout
    try:
        inputs = json.loads(out)
    except ValueError:
        inputs = []
    sinks = subprocess.run(["pactl", "-f", "json", "list", "sinks"], capture_output=True, text=True).stdout
    try:
        target = next(x["index"] for x in json.loads(sinks) if x["name"] == sink)
    except (ValueError, StopIteration):
        continue
    for si in inputs:
        props = si.get("properties", {})
        pid = props.get("application.process.id")
        mine = (pid and int(pid) in kids) or match in (props.get("application.name"), props.get("media.name"))
        if mine and si.get("sink") != target:
            subprocess.run(["pactl", "move-sink-input", str(si["index"]), sink], capture_output=True)
    time.sleep(0.2)
PY
mover=$!
status=0
wait "$cmd" || status=$?
exit $status
