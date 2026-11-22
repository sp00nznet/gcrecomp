# Debugging a recompiled game

Switches and tools for finding out why a frame looks wrong. Nothing here needs a particular game.

## Environment variables (GP, the graphics processor)

| Variable | Effect |
| --- | --- |
| `GP_TRACE=1` | Prints the first draws, the large XF loads and the viewport/projection of a few draws. |
| `GP_TRACE_FRAME=N` | Prints one line per draw of presented frame `N` (primitive, vertex count, VCD, TEV stage 0, Z mode, blend, alpha compare, texture 0, projection, viewport), then for the first 60 draws the first vertex in clip space and normalised device coordinates, its lighting inputs and result, and for the first 3 draws four vertices with the position matrix rows they were transformed with. Frame numbers count EFB copies to the XFB (`[gp] N frames` at exit). |
| `GP_TRACE_DRAWS=first-last` | With `GP_TRACE_FRAME`: only the draws numbered first..last (as printed) are rendered in that frame, to see what a group of draws contributes (compare screenshots of two ranges). |
| `GP_TRACE_STATE=n[,m...]` | With `GP_TRACE_FRAME`: dumps the full TEV, texgen, texture-map and raster state at those draws. |
| `GP_CULL=none` | Disables face culling (a quick test for geometry that vanishes). |
| `GP_DUMP_TEX=dir` | Writes the textures the game uses as PPM files (`tex_NN_fmtF_WxH_ADDR.ppm`, alpha shown over grey). Each (address, format, size) is written once. |
| `GP_DUMP_TEX_MAX=n` | How many textures `GP_DUMP_TEX` writes (default 24). |
| `GP_DUMP_TEX_AFTER=n` | Skip textures used before frame `n` (a movie, an earlier scene). |
| `GP_SHOT_EVERY=n` | Screenshot (PPM, window size) every `n` presented frames, into the screenshot directory of the host program. |
| `GP_XHIST=1` | Histogram of XF register writes at exit. |

## Tools

* `tools/frame_compare.py OURS REFERENCE [--out sheet.png]`: compares one of our screenshots (PPM) with a
  reference frame (for example from Dolphin) and prints the error and a luminance grid of both, so a black
  render next to a bright reference is obvious. `--max-rmse` turns it into a check.
* `tools/prune_frames.sh --dir DIR --keep N [--while dolphin-emu]`: Dolphin's frame dump writes 60 PNGs per
  second; run this next to it to keep only the newest N frames.
* `tests/ps_semantics_*`: the floating-point and paired-single semantics of the code the recompiler emits
  (see below). Add an instruction by listing its word in `tests/ps_semantics_cases.h` and its expectation in
  `tests/ps_semantics_test.cpp`.

## Reference frames from Dolphin

Dolphin writes the video with `[Movie] DumpFrames = True` in its `Dolphin.ini` (frames in
`<user>/Dump/Frames/framedump_N.png`). Press the buttons of the scene you want through a pipe controller
or `dolphin-run.sh`, run `prune_frames.sh` beside it, then compare with `frame_compare.py`.

## Wrong matrices, wrong geometry

When the geometry of a scene is garbage but the GP parser is right (compare the XF position matrix in the
`GP_TRACE_FRAME` output: rows with a shear or a non-uniform scale are suspicious), the matrices come from guest
code running on the recompiled CPU: suspect the floating-point instructions. The paired-single registers have
one representation: `f[i]` is ps0 and `ps1[i]` is ps1; every scalar FP instruction, `ps_*` instruction and
`psq_l` / `psq_st` goes through those two, and `tests/ps_semantics_test.cpp` runs the emitted code of a set of
instructions against hand-computed results (including a destination register that is also a source, and the
quantised load/store with saturation and signed scales).
