#!/usr/bin/env bash
# Inspect a running recompiled game with gdb through guest-function breakpoints.
#
#   tools/guest-gdb.sh --bin BINARY [--break func_8016EF28 ...] [--args "r3 r4 r5"]
#                      [--mem 0x80359940:8 | --mem r4:16 | --mem r4+0x20:8 ...] [--bt DEPTH] [--ignore N]
#                      [--env NAME=VALUE ...] -- PROGRAM-ARGUMENTS...
#
# The binary has no debug info, so this relies on the generated calling convention: a guest function
# `func_XXXXXXXX(PPCContext* ctx, Memory* mem)` receives ctx in rdi and mem in rsi; guest register rN is a
# 32-bit word at ctx+4*N, and guest RAM is the host pointer stored at mem+8 (guest 0x80xxxxxx maps to
# ram + (addr & 0x1FFFFFF)). Words in RAM are big-endian: gdb's x/wx shows them byte-swapped, the output is
# therefore re-printed here as the guest sees them.
#
# Each breakpoint prints the requested guest registers and memory words (an absolute guest address, or a register
# plus an optional offset: the words at the address the register holds on entry), then a host backtrace (guest call
# chain), and continues. Works for functions called directly (`bl`), which GCRECOMP_TRACE_ARGS (function-table
# entries only) cannot see. --ignore N skips the first N hits of each breakpoint. Needs `gdb`.
#
# Example (print the first 8 words of the buffer r4 points to, 30 hits after the first 500):
#   tools/guest-gdb.sh --bin build/game --break func_8017AD3C --args "r3 r4" --mem r4:8 --ignore 500 -- main.dol files 60
set -euo pipefail

depth=8; regs=""; mems=(); breaks=(); ignore=0; bin=""; envs=(); extra=()

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
  case "$1" in
    --bin) bin="$2"; shift 2 ;;
    --break) breaks+=("$2"); shift 2 ;;
    --bt) depth="$2"; shift 2 ;;
    --args) regs="$2"; shift 2 ;;
    --mem) mems+=("$2"); shift 2 ;;
    --ignore) ignore="$2"; shift 2 ;;
    --env) envs+=("$2"); shift 2 ;;
    -h|--help) usage 0 ;;
    --) shift; extra=("$@"); break ;;
    *) echo "unknown option $1" >&2; usage 2 ;;
  esac
done
[ -n "$bin" ] && [ -x "$bin" ] || { echo "need --bin BINARY (built, with symbols)" >&2; usage 2; }
[ ${#breaks[@]} -gt 0 ] || { echo "need at least one --break func_XXXXXXXX" >&2; usage 2; }

gdbfile="$(mktemp)"; trap 'rm -f "$gdbfile"' EXIT
{
  echo "set pagination off"; echo "set confirm off"
  for e in "${envs[@]:-}"; do [ -n "$e" ] && echo "set environment ${e%%=*} ${e#*=}"; done
  i=0
  for b in "${breaks[@]}"; do
    i=$((i + 1))
    echo "break *$b"
    [ "$ignore" -gt 0 ] && echo "ignore $i $ignore"
    echo "commands $i"; echo "silent"
    echo "printf \"--- $b\\n\""
    for r in $regs; do
      n="${r#r}"
      echo "printf \"  r$n=0x%08x\\n\", *(unsigned int*)(\$rdi+4*$n)"
    done
    for m in "${mems[@]:-}"; do
      [ -n "$m" ] || continue
      addr="${m%%:*}"; words="${m##*:}"; [ "$addr" = "$m" ] && words=1
      case "$addr" in
        r*) reg="${addr%%+*}"; off=0; [ "$reg" != "$addr" ] && off="${addr#*+}"
            expr="(*(unsigned int*)(\$rdi+4*${reg#r}) + ($off)) & 0x1FFFFFF" ;;
        *)  expr="($addr) & 0x1FFFFFF" ;;
      esac
      echo "printf \"  mem $addr:\\n\""
      echo "set \$p = *(unsigned char**)(\$rsi+8) + ($expr)"
      echo "set \$k = 0"
      echo "while \$k < $words"
      echo "  printf \"  %08x\\n\", ((unsigned int)*(unsigned char*)(\$p+4*\$k) << 24) | ((unsigned int)*(unsigned char*)(\$p+4*\$k+1) << 16) | ((unsigned int)*(unsigned char*)(\$p+4*\$k+2) << 8) | (unsigned int)*(unsigned char*)(\$p+4*\$k+3)"
      echo "  set \$k = \$k + 1"
      echo "end"
    done
    echo "bt $depth"; echo "continue"; echo "end"
  done
  echo "run ${extra[*]:-}"
  echo "quit"
} > "$gdbfile"

ulimit -s unlimited
# The game's own output goes to stdout/stderr of the program; keep only gdb's breakpoint reports.
gdb -nx -q -batch -x "$gdbfile" "$bin" 2>&1 | grep -a -E '^(---|  r[0-9]|  mem|  [0-9a-f]{8}$|#|0x[0-9a-f]+:)' | sed 's/(gcrecomp::PPCContext\*, gcrecomp::Memory\*)//'
