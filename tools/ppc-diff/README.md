# ppc-diff: the recompiler's emitted code against a reference CPU

A differential test of the C++ that `gcrecomp_recompiler` emits for each PowerPC instruction. Random inputs
(registers, CR, XER, a 128-byte memory window) go through a reference and through the emitted code; every register,
CR/LR/CTR and the memory window must match. It finds the bugs that make a whole game subtly wrong without any single
obviously broken screen or sound (it found the ones listed in the pull request that added it: swapped `subfze` /
`subfme`, `mcrf` / `ps_cmp*` field decoding, `lhbrx`, `dcbz`, the GQR field layout of `psq_l` / `psq_st`, `stfs`
rounding, non-fused `fmadd`, the missing OE forms).

```
tools/ppc-diff/run.sh [--build-dir DIR] [--venv DIR] [--vectors FILE] [--scale S] [--seed N] [-- RUNNER-ARGS]
```

`run.sh` creates a virtual environment with [Unicorn](https://www.unicorn-engine.org/) (QEMU's PowerPC 750CL), generates
the vectors, builds the generator and the runner with CMake and runs the comparison. Exit status 0 means every case
matches. Runner arguments (after `--`): `--only NAME,..`, `--ignore NAME,..`, `--max-report N`, `--verbose`,
`--inputs` (print the full input state of a mismatch). The instruction names are those of the generator (`rlwinm`,
`psq_l`, ...).

## Pieces

* `gen_vectors.py OUT [--seed N] [--scale S]`: the vector generator. Integer, CR, load/store and standard FP programs
  are executed on Unicorn; the Gekko-only instructions (paired singles, quantized load/store) have no Unicorn model, so
  their expected results come from a small independent model in the script (the conversions of Dolphin's
  `ConvertToSingle` / the GQR formats). Deterministic for a given seed and scale; the vectors are not committed
  (about 2.7 MB of text at scale 1). Ported from `fepor-recomp/tools/gen_vectors.py` (same owner, GPL-2.0-or-later),
  the output format is text. Needs `unicorn` (`requirements.txt`).
* `ppc_diff_gen.cpp`: reads a vectors file and writes one C++ function per case with the recompiler's own emitter
  (`PPCToCEmitter`), plus the table of inputs and expected outputs.
* `ppc_diff_run.cpp`: compiled together with that output; runs every case on `PPCContext` and `g_mem` and compares.
* CMake: configure with `-DGCRECOMP_PPC_DIFF_VECTORS=<vectors file>` (and `GCRECOMP_BUILD_TESTS=ON`) to get the
  `ppc_diff_run` target and the `ppc_diff` ctest. Without the variable nothing is built, so the normal build and CI do
  not need Unicorn.

* `replay.py`: runs one whole guest function of a DOL on Unicorn from a captured state (registers, memory files) and
  compares the memory it writes with a capture of the recompiled build's call, see its `--help`. The capture comes
  from `tools/guest-gdb.sh` / gdb's `dump binary memory`. It showed that Tales of Symphonia's HVQM4 ADPCM decoder was
  recompiled correctly (the mono sound came from the game's inputs, not from the emitted code).

## What it does not cover

* Branches (`b`, `bc`, `bclr`, `bcctr`): the emitter needs a function context for them; the `bc` condition shapes
  are generated but skipped (counted in the summary line).
* The FPSCR instructions (`mtfsfi`, `mtfsf`, `mtfsb0/1`, `mcrfs`, `mffs`) are not decoded by the recompiler; their
  vectors, and the `fctiw` cases that need a rounding mode other than nearest, are skipped and counted. Games that use
  them would show an `UNIMPLEMENTED` line in the generated code.
* The `Rc=1` forms of floating-point instructions (CR1 from FPSCR), `fres` / `frsqrte` (the hardware approximations),
  NaN payloads, `lswi` / `lswx` / `stswi` / `stswx`, `mcrxr`, `mftb`.
* The upper word of `fctiw` / `fctiwz` / `mffs` results (implementation-defined on the Gekko; only the low word is
  compared).

## Adding an instruction

Add its encoder cases to `gen_vectors.py` (`int_cases`, `mem_cases`, `fp_cases`, ...), run `run.sh`, and fix the
emitter (`tools/recompiler/ppc_to_c.cpp`) or the decoder (`ppc_disasm.cpp`) until it matches.
