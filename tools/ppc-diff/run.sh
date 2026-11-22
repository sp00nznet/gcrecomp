#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Differential test of the recompiler's emitted code against a reference PowerPC CPU (Unicorn), end to end:
# virtual environment -> test vectors -> build of the generator and runner -> comparison.
#
# usage: tools/ppc-diff/run.sh [--build-dir DIR] [--venv DIR] [--vectors FILE] [--scale S] [--seed N] [-- RUNNER-ARGS...]
#   --build-dir DIR  CMake build directory (default: build-ppc-diff next to the repository)
#   --venv DIR       virtual environment with Unicorn (default: <build-dir>/venv; created and filled on first use)
#   --vectors FILE   vectors file to write (default: <build-dir>/ppc_diff_vectors.txt); an existing file is reused
#                    when it is given explicitly with --vectors and exists (nothing is generated then)
#   --scale S, --seed N   passed to gen_vectors.py (more or fewer random cases per instruction)
#   RUNNER-ARGS      passed to ppc_diff_run (--only, --ignore, --max-report, --verbose)
# Environment: PPC_DIFF_CMAKE_ARGS adds arguments to the cmake configure (for example -DGCRECOMP_FETCH_SDL3=ON on a
# machine without SDL3). Exit status is that of ppc_diff_run (0: every case matches).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="$repo/../build-ppc-diff"
venv=""
vectors=""
gen_args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) build="$2"; shift 2 ;;
        --venv) venv="$2"; shift 2 ;;
        --vectors) vectors="$2"; shift 2 ;;
        --scale) gen_args+=(--scale "$2"); shift 2 ;;
        --seed) gen_args+=(--seed "$2"); shift 2 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        --) shift; break ;;
        *) echo "unknown argument $1 (see --help)" >&2; exit 2 ;;
    esac
done
mkdir -p "$build"
build="$(cd "$build" && pwd)"
venv="${venv:-$build/venv}"

if [ -z "$vectors" ] || [ ! -f "$vectors" ]; then
    vectors="${vectors:-$build/ppc_diff_vectors.txt}"
    if ! "$venv/bin/python" -c 'import unicorn' 2>/dev/null; then
        echo "[ppc-diff] creating the virtual environment $venv"
        python3 -m venv "$venv"
        "$venv/bin/pip" install -q -r "$here/requirements.txt"
    fi
    echo "[ppc-diff] generating vectors -> $vectors"
    "$venv/bin/python" "$here/gen_vectors.py" "${gen_args[@]}" "$vectors"
fi

echo "[ppc-diff] building"
# shellcheck disable=SC2086
cmake -S "$repo" -B "$build" -G Ninja -DGCRECOMP_BUILD_TESTS=ON -DGCRECOMP_PPC_DIFF_VECTORS="$vectors" ${PPC_DIFF_CMAKE_ARGS:-} >/dev/null
ninja -C "$build" ppc_diff_run
echo "[ppc-diff] running"
ulimit -s unlimited 2>/dev/null || true
"$build/ppc_diff_run" "$@"
