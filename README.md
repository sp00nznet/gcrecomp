# gcrecomp — GameCube Static Recompilation Toolkit

**An open toolkit for statically recompiling GameCube (Gekko/Flipper) games to native x86-64.**

Ever wanted to take a GameCube classic and run it natively on your PC at full speed, with
modern resolution support and zero emulation overhead? That's what static recompilation does —
and this toolkit gives you everything you need to make it happen.

## What Is This?

gcrecomp is a **reusable library and toolset** for building GameCube static recompilation
projects. It provides:

- **A PowerPC 750 (Gekko) recompiler** that translates PPC machine code into portable C
- **A complete runtime library** that replaces the GameCube's hardware and OS at the SDK level
- **GX graphics translation** (GameCube GPU → Direct3D 11) with TEV shader generation
- **Audio decoding** (DSP ADPCM → PCM, voice mixing)
- **Input mapping** (keyboard/gamepad → GameCube controller)
- **Dolphin OS HLE** (heap, DVD, threads, timing — the works)

Think of it like [N64Recomp](https://github.com/N64Recomp/N64Recomp) but for GameCube, or
like [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) but for the purple lunchbox
instead of the Xbox 360.

## How Static Recompilation Works

Unlike an emulator that interprets instructions at runtime, static recompilation translates
the **entire game binary ahead of time** into compilable C/C++ source code:

```c
// Original GameCube PowerPC (Gekko):
//   lwz    r5, 0x10(r3)      // Load word
//   addi   r5, r5, 1         // Increment
//   stw    r5, 0x10(r3)      // Store back
//   bl     updateScore        // Call function

// Recompiled native C:
ctx->r[5] = MEM_READ32(ctx->r[3] + 0x10);
ctx->r[5] = (int32_t)ctx->r[5] + 1;
MEM_WRITE32(ctx->r[3] + 0x10, ctx->r[5]);
updateScore(ctx, mem);
```

The generated code compiles with any modern C compiler (MSVC, Clang, GCC) and runs at
**full native speed** with all compiler optimizations applied. No interpreter loop. No
JIT compilation. Just straight native code.

The trick is that games don't just run on a CPU — they talk to hardware. The GameCube has
a custom GPU (GX/Flipper), DSP audio, DVD drive, controllers, and an operating system
(Dolphin OS). gcrecomp provides **native replacements for all of these**, so the recompiled
game code has everything it needs to run.

## Architecture

```
                    +-----------------+
                    | Your GameCube   |
                    |   DOL / REL     |
                    | (PowerPC 750)   |
                    +--------+--------+
                             |
                    +--------v--------+
                    |  gcrecomp       |
                    |  Recompiler     |
                    |  PPC → C        |
                    +--------+--------+
                             |
              +--------------+--------------+
              |              |              |
     +--------v------+ +----v----+ +-------v------+
     | gcrecomp      | | gcrecomp| | gcrecomp     |
     | GX Runtime    | | Audio   | | Input        |
     | (D3D11)       | | (DSP    | | (XInput /    |
     | TEV → HLSL    | |  ADPCM) | |  Keyboard)   |
     +-------+-------+ +----+----+ +------+-------+
             |               |             |
     +-------v---------------v-------------v-------+
     |              gcrecomp Runtime                |
     |   PPCContext + Memory + OS HLE + FuncTable   |
     +---------------------------------------------+
```

## Getting Started

### Prerequisites

- **Windows 11** with DirectX 11 GPU
- **CMake** 3.20+
- **Visual Studio 2022** (or compatible C++20 compiler)
- A legally obtained GameCube game ISO

### Building

```bash
# Clone
git clone https://github.com/sp00nznet/gcrecomp.git
cd gcrecomp

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build the recompiler tool
cmake --build build --target gcrecomp_recompiler --config Release

# Build the runtime library
cmake --build build --target gcrecomp_runtime --config Release
```

### Using gcrecomp in Your Project

gcrecomp is designed to be used as a library. Your game-specific project links against it:

```cmake
# In your game project's CMakeLists.txt:
add_subdirectory(path/to/gcrecomp)
target_link_libraries(my_game PRIVATE gcrecomp_runtime gcrecomp_gx gcrecomp_audio gcrecomp_input)
```

See the [docs/](docs/) folder for detailed guides on each phase of bringing up a new game.

## Components

### Recompiler Tool (`tools/recompiler/`)

Parses GameCube DOL and REL executables, disassembles all Gekko instructions (including
Paired Singles!), builds a control flow graph, and emits C source files.

| Feature | Status |
|---------|--------|
| DOL parsing (7 text + 11 data sections) | Done |
| REL parsing (relocatable modules) | Done |
| Full Gekko instruction set | Done |
| Paired Singles SIMD | Done |
| Control flow graph construction | Done |
| C code generation | Done |
| Symbol map integration | Done |

### Runtime Library (`src/runtime/`)

The CPU context and memory system. Every recompiled function operates on these:

```c
// Every recompiled function has this signature:
typedef void(*RecompiledFunc)(PPCContext* ctx, Memory* mem);
```

- **PPCContext**: Full Gekko register file — 32 GPRs, 32 FPRs, Paired Singles,
  Condition Register, Link Register, CTR, XER, GQRs, FPSCR
- **Memory**: 24MB big-endian RAM with virtual address translation
  (0x80000000 cached, 0xC0000000 uncached)
- **FuncTable**: Maps GameCube addresses to native function pointers for indirect calls

### OS HLE & Disc Support (`src/os/`)

High-Level Emulation of the Dolphin OS — the system software that every GameCube game
relies on. Instead of emulating the hardware that runs the OS, we **replace the OS
functions directly** with native implementations.

Also includes disc image mounting (GCM/ISO), FST parsing, and Nintendo archive format
support (Yaz0 decompression, RARC parsing) — the building blocks every GameCube game
needs for asset loading.

| Function Group | What It Does | Status |
|----------------|-------------|--------|
| **Timing** | OSGetTime/Tick at 40.5MHz timebase | Done |
| **Heap** | OSCreateHeap/OSAlloc/OSFree (first-fit w/ coalescing) | Done |
| **Arena** | OSGet/SetArenaLo/Hi | Done |
| **DVD** | DVDOpen/Read/Close from extracted files or mounted ISO | Done |
| **Disc Image** | mount_disc_image() — FST parsing, raw disc reads | Done |
| **Yaz0** | Nintendo Yaz0 (SZS) decompression — standard LZ format | Done |
| **RARC** | RARC archive parser — hierarchical file extraction | Done |
| **Threads** | OSCreateThread/Resume (minimal for single-threaded games) | Done |
| **Interrupts** | OSDisable/Enable/RestoreInterrupts (no-ops in recomp) | Done |
| **Mutex/MsgQueue** | Init/Lock/Unlock stubs | Done |
| **Cache** | DCFlush/ICInvalidate (no-ops) | Done |
| **CRT** | memset/memcpy/strcmp/strlen etc. | Done |
| **Math** | sin/cos/sqrt/atan2/pow/floor/ceil etc. | Done |
| **Debug** | OSReport/OSPanic/OSFatal | Done |
| **Low Memory Init** | Boot state: clocks, game ID, arena, console type | Done |

### GX Graphics (`src/gx/`)

The GameCube's GX GPU is translated to Direct3D 11. The heart of this is the **TEV
(Texture Environment) shader generator** — the GameCube's equivalent of a pixel shader.

```
TEV Pipeline (up to 16 stages):
  For each stage:
    color = D + ((1-C)*A + C*B + bias) * scale
    alpha = D + ((1-C)*A + C*B + bias) * scale

  Where A/B/C/D can be: previous result, texture sample, vertex color,
                         constant color, konst register, zero, one, half
```

We generate HLSL shaders on-the-fly from the current TEV configuration and cache them
by a hash of the TEV state. This is the same approach used by Dolphin emulator.

| Feature | Status |
|---------|--------|
| GX state machine (all API functions) | Done |
| TEV color/alpha combiners (16 stages) | Done |
| Konst color selection (32 modes) | Done |
| Alpha compare + discard | Done |
| Fog (linear/exponential) | Done |
| Indirect textures (for water, etc.) | Done |
| Texture decoding (I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/CMPR) | Done |
| Display list parsing (BP/CP/XF commands) | Done |
| Vertex format handling | In Progress |
| Draw call pipeline (state → D3D11) | In Progress |
| TEV shader compilation + caching | In Progress |

### Audio (`src/audio/`)

GameCube audio uses a custom DSP with ADPCM compression:

- **DSP ADPCM Decoder**: 4-bit samples with 16 prediction coefficients.
  Each 8-byte frame decodes to 14 PCM samples.
- **Voice Mixer**: 64 simultaneous voices with volume, pan, pitch, and looping
- **XAudio2 Backend**: Windows audio output (stub, integration in progress)

### Input (`src/input/`)

Maps modern input devices to the GameCube controller:

- Keyboard (WASD + mouse) with configurable bindings
- XInput gamepad with proper analog stick/trigger mapping
- Writes PADStatus structs directly to emulated memory

## Hardware Reference

The `include/gcrecomp/hw/` headers contain complete register definitions for the
GameCube's Flipper ASIC, derived from public documentation and CC0/public domain sources:

- **`gc_hw.h`**: Memory map, clock speeds, low-memory layout, BP/CP/XF register addresses,
  TEV enums, VI modes, FIFO opcodes
- **`os_defs.h`**: OS data structure layouts (OSThread, OSContext, DVD, heap, PAD)

These are invaluable if you're working on any GameCube project — emulator, decompilation,
or recompilation.

### Debug Tools (`tools/`)

- **`dolphin_dump.py`**: Connects to a running Dolphin emulator via `dolphin-memory-engine`
  and captures the game's runtime state — process trees, linked lists, SDA globals, and
  arbitrary memory regions. Outputs JSON for analysis. Useful for understanding how a game's
  framework system works at runtime, without needing a full decompilation.

  ```bash
  pip install dolphin-memory-engine
  # Launch Dolphin with your game, pause emulation, then:
  python tools/dolphin_dump.py > capture.json
  ```

## Want to Recomp Your Favorite GameCube Game?

Here's the rough roadmap for bringing up a new title:

1. **Extract the DOL** from your game's ISO
2. **Find or create a symbol map** (check if there's a decompilation project for your game!)
3. **Run the recompiler** to generate C source files
4. **Link against gcrecomp** runtime libraries
5. **Mount the disc image** with `mount_disc_image()` for asset loading
6. **Test and iterate** — add game-specific OS stubs, use Yaz0/RARC for archive loading
7. **Play your game natively!**

See [docs/BRINGING_UP_A_GAME.md](docs/BRINGING_UP_A_GAME.md) for a detailed walkthrough.

## Standing on the Shoulders of Giants

This project wouldn't exist without the incredible work of these communities and projects:

### Direct References (code derived from or inspired by)

- **[Pureikyubu / Dolwin](https://github.com/emu-russia/pureikyubu)** (CC0 / Public Domain)
  — Hardware register definitions, DSP emulation, OS HLE patterns. The cleanest modular
  GameCube emulator source available.

- **[libogc](https://github.com/devkitPro/libogc)** (zlib license)
  — The open-source GameCube/Wii homebrew SDK. Our authoritative reference for what every
  SDK function actually does at the register level. If you want to understand how
  `GXSetTevColorIn` works, libogc's `gx.c` is the source of truth.

- **[GameCubeRecompiled](https://github.com/KaiserGranatapfel/GameCubeRecompiled)** (CC0)
  — An experimental GameCube recompiler in Rust. Our TEV shader generation and OS init
  constants reference. Pioneering work that proved the concept.

### Architectural Inspiration

- **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** (MIT)
  — Pioneered the static recompilation approach for N64 games. The project that showed
  this was possible and inspired a generation of recomp projects.

- **[RexGlueSDK](https://github.com/rexglue/rexglue-sdk)** (BSD 3-Clause)
  — Xbox 360 static recompilation SDK. Proved the "extract emulator into runtime library"
  architecture by adapting Xenia into a link-time library. Our architecture follows the
  same pattern.

- **[XenonRecomp / UnleashedRecomp](https://github.com/hedge-dev/XenonRecomp)** (MIT)
  — The original Xbox 360 recompiler. Showed that `void func(PPCContext& ctx, uint8_t* base)`
  is the right function signature pattern.

### Knowledge Sources

- **[Dolphin Emulator](https://github.com/dolphin-emu/dolphin)** (GPL-2.0+)
  — The gold standard GameCube/Wii emulator. Used as a **read-only correctness reference**
  for verifying our TEV shader output, texture decoding, and OS behavior. No code was
  copied from Dolphin. (Dolphin's GX implementation represents 20+ years of community
  effort and is the most complete reference for GameCube graphics behavior.)

- **[zeldaret/tww](https://github.com/zeldaret/tww)**
  — Wind Waker decompilation project. Invaluable symbol maps and architectural understanding
  of how a real GameCube game is structured.

- **[decomp-toolkit](https://github.com/encounter/decomp-toolkit)**
  — GameCube binary analysis tools for DOL/REL parsing.

- **[Yet Another GameCube Documentation](http://hitmen.c02.at/files/yagcd/)**
  — Classic hardware documentation covering every register in the Flipper ASIC.

## Licensing

gcrecomp is released under the **MIT License**. Use it for anything — commercial projects,
homebrew, research, education. Just keep the attribution.

Portions derived from:
- Pureikyubu (CC0 — public domain, no restrictions)
- libogc (zlib — use freely with attribution)
- GameCubeRecompiled (CC0 — public domain, no restrictions)

**No code from GPL-licensed projects (Dolphin, gcube) has been incorporated.**
Dolphin is referenced only for behavioral correctness verification.

## Legal

This project does not include any copyrighted game assets, code, or ROMs. You must provide
your own legally obtained GameCube game disc/ISO. gcrecomp is a tool — what you do with
it is your responsibility.

## Contributing

Found a bug? Want to add support for a hardware feature? Have a game you're trying to
bring up? PRs and issues are welcome!

The GameCube library is vast and beautiful. There are hundreds of games waiting to be
recompiled. Every contribution — whether it's a missing OS function, a texture format
decoder, or documentation for a hardware quirk — helps the whole community.

## Part of sp00nznet

Built with love for the games that defined a generation.

---

*"The GameCube may be small, but its games are enormous."*
