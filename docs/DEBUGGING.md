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

## Environment variables (guest memory and calls)

| Variable | Effect |
| --- | --- |
| `GCRECOMP_WATCH=0xADDR:LEN[,..]` | Every store overlapping a range prints `[watch] #N writeW addr = value bt: ...` with the host call chain; `tools/guest-watch.py --bin BINARY LOG [--summary]` names the guest functions. `GCRECOMP_WATCH_MAX=N` caps the lines (default 64). |
| `GCRECOMP_TRACE_CALLS=LR[,LR..]` | Prints the target and r3-r6 of function-table calls made with those link-register values (`GCRECOMP_TRACE_CALLS_MAX`, default 20). |
| `GCRECOMP_TRACE_ARGS=ADDR:SPEC[,SPEC..][;ADDR:..]` | When the function at ADDR is entered through the function table (callbacks, pointer calls), prints guest registers (`r3`) or memory words (`r13[-30980]`). `GCRECOMP_TRACE_ARGS_EVERY=N`, `GCRECOMP_TRACE_ARGS_MAX=N` (default 50). |
| `GCRECOMP_DEBUG_AFTER=S` | The three facilities above stay quiet for the first S seconds of wall-clock time. |

## Tools

* `tools/frame_compare.py OURS REFERENCE [--out sheet.png]`: compares one of our screenshots (PPM) with a
  reference frame (for example from Dolphin) and prints the error and a luminance grid of both, so a black
  render next to a bright reference is obvious. `--max-rmse` turns it into a check.
* `tools/prune_frames.sh --dir DIR --keep N [--while dolphin-emu]`: Dolphin's frame dump writes 60 PNGs per
  second; run this next to it to keep only the newest N frames.
* `tests/ps_semantics_*`: the floating-point and paired-single semantics of the code the recompiler emits
  (see below). Add an instruction by listing its word in `tests/ps_semantics_cases.h` and its expectation in
  `tests/ps_semantics_test.cpp`.
* `tools/ppc-diff/run.sh`: differential test of the recompiler's emitted code against a reference PowerPC CPU
  (Unicorn) on random integer / CR / FP / paired-single / load-store vectors, see `tools/ppc-diff/README.md`. Run it
  after touching `tools/recompiler/ppc_to_c.cpp` or `ppc_disasm.cpp`, and whenever a game does something subtly
  wrong (a silent audio channel, a slightly wrong value) with no visible crash: a recompiler instruction bug is
  the usual cause.
* `tools/wav-channels.py A.wav [B.wav ...]`: per-channel RMS, L-R and the share of identical samples per time bin
  of a stereo WAV (`GCRECOMP_DUMP_AUDIO` or Dolphin's dumps), with a verdict mono / left only / right only / stereo.

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

## Audio

Where the sound goes: the SDK's AI functions are replaced (`src/os/ai_funcs.cpp`); the game's PCM blocks reach an
`AudioSink` (SDL3 by default, `GCRECOMP_AUDIO=null` for none). Some scenes mix on the DSP (the AX microcode, lists
of commands in `src/audio/ax_hle.cpp`), others do not: Tales of Symphonia's intro movie (`mov/op.h4m`) stops the AX
lists and feeds the AI DMA itself, the title music is mixed by game code and handed to AX as aux buffers.

Environment switches:

| Variable | Effect |
| --- | --- |
| `GCRECOMP_DUMP_AUDIO=file.wav` | Everything played, as a 16-bit stereo WAV (valid after every block). |
| `GCRECOMP_AUDIO_TRACE=1` | Once a second: DMA blocks started, all-zero blocks, underruns, sink stalls, longest gap between polls, block size, rate, sink queue. |
| `GCRECOMP_AX_TRACE=1` | The first running AX voices (parameter block, format, addresses, ratio) and, once a second, the command histogram, voice slices and the RMS at stages of the mix (upload L/R/S, surround buffer in, output L/R/S): shows at which stage a channel dies. |
| `DSP_TRACE=1` | DSP mails, control writes, every 500th AX list. |

A sink that holds a full lead and does not drain for 250 ms (device suspended or lost) is detected: the DMA then keeps
its real-time pace on the host clock and discards the audio, so the game does not freeze (`[AI] the audio device
stopped consuming`).

Tools:

* `tools/wav-compare.py A.wav [B.wav] [--align] [--a-from/--a-to/--b-from/--b-to S]`: non-zero ratio, RMS and peak per
  second, energy per band, and for two files envelope correlation, band-share distance and the time offset. Works on our
  dump and on Dolphin's `Dump/Audio/*dspdump.wav`; `--require-nonzero` makes it a check.
* `tools/dolphin-run.sh --iso game.iso --user DIR --dump-audio -- wait:20 press:A ...`: Dolphin reference run with
  scripted input (ported from fepor-recomp).
* `tools/audio-capture.sh --out cap.wav -- program args...`: records what the program really sends to the host audio
  server (its streams are moved to a temporary null sink and the sink's monitor is recorded), to test the default SDL
  sink end to end without listening. `build/gcrecomp_audio_probe --seconds 4` plays a tone through the same sink;
  `tools/audio-capture.sh --out c.wav -- build/gcrecomp_audio_probe` then `tools/wav-compare.py c.wav --require-nonzero`.
