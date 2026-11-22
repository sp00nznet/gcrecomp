#!/usr/bin/env bash
# usage: gdb-stacks.sh PID [DEPTH]
# Host backtraces of the guest threads (those running sched::thread_main) of a running recompiled game:
# the recompiled functions show as func_XXXXXXXX, so the guest call chain of every waiting thread is visible.
gdb -p "$1" -batch -ex "set pagination off" -ex "thread apply all bt ${2:-30}" 2>&1 \
  | awk '/^Thread /{ if (keep) printf "%s", buf; buf=$0"\n"; keep=0; next } /^#/{ buf=buf substr($0,1,150)"\n"; if ($0 ~ /sched::.*thread_main/) keep=1 } END{ if (keep) printf "%s", buf }'
