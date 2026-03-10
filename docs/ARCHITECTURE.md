# gcrecomp Architecture

This document explains how gcrecomp works — the design decisions, the component
boundaries, and how everything fits together.

## The Big Picture

Static recompilation converts a game's machine code into portable C/C++ source
**ahead of time**. At runtime, the game code runs natively — no interpreter, no JIT.
But the game still needs to talk to hardware (GPU, audio, controllers, disc drive),
so we provide native replacements for all of those.

```
┌─────────────────────────────────────────────┐
│         Recompiled Game Code                 │
│  void func_XXXX(PPCContext* ctx, Memory* mem)│
├─────────────────────────────────────────────┤
│              SDK Intercept Layer             │
│  GX*, OS*, DVD*, PAD* function hooks        │
├──────────┬──────────┬───────────┬───────────┤
│  GX/TEV  │  OS/HLE  │ DSP/Audio │ DVD/File  │
│  → D3D11 │  Heap,   │ ADPCM,   │ Host FS   │
│  shader  │  Thread, │ AX mixer │ read      │
│  gen     │  Time    │          │           │
├──────────┴──────────┴───────────┴───────────┤
│        Memory (24MB BE, address translate)   │
├─────────────────────────────────────────────┤
│           PPCContext (Gekko registers)        │
└─────────────────────────────────────────────┘
```

## The Function Signature

Every recompiled function has the same signature:

```c
void func(PPCContext* ctx, Memory* mem);
```

- **`PPCContext`** carries the entire CPU state: 32 GPRs, 32 FPRs, paired singles,
  condition register, link register, counter register, XER, GQRs, FPSCR.
- **`Memory`** is the 24MB emulated RAM with big-endian byte-swapped access.

This is the same pattern used by:
- [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) (`void func(PPCContext& ctx, uint8_t* base)`)
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (`void func(uint8_t* rdram, recomp_context* ctx)`)
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (`void func(PPCContext& ctx, uint8_t* base)`)

## How OS Functions Are Replaced

GameCube games link against the Dolphin OS SDK. Functions like `OSAlloc`, `DVDRead`,
`GXSetTevColorIn` are part of the game's binary. When the recompiler encounters a call
to a known SDK function (identified by address from the symbol map), it emits a call
to our native replacement instead.

```c
// The recompiler sees: bl 0x800ABC10  (which the symbol map says is "OSAlloc")
// It emits:
gcrecomp::os_alloc(ctx, mem);  // Our native heap allocator
```

The `lookup_os_func(name)` function maps SDK function names to native implementations.
During initialization, the symbol map loader calls this for each known symbol and
registers the mapping in the `FuncTable`.

## GX Graphics Translation

The GameCube's GX GPU uses a programmable pixel pipeline called the **TEV** (Texture
Environment). It has up to 16 stages, each combining texture samples, vertex colors,
and constants using a fixed formula:

```
result = D op ((1-C)*A + C*B + bias) * scale
```

We intercept GX API calls at the SDK level and capture the TEV configuration. When
a draw call happens, we:

1. Hash the current TEV state
2. Check the shader cache for a matching HLSL shader
3. If not cached, generate a new HLSL pixel shader from the TEV config
4. Compile it with D3DCompile
5. Bind it and draw

This is the same approach used by [Dolphin Emulator](https://github.com/dolphin-emu/dolphin)
(though our implementation is clean-room, not derived from Dolphin's GPL code).

## Memory Model

The GameCube uses big-endian byte ordering and a 32-bit virtual address space.
Our `Memory` struct allocates 24MB of host RAM and translates addresses:

- `0x80000000 - 0x817FFFFF` → Cached RAM (most game accesses)
- `0xC0000000 - 0xC17FFFFF` → Uncached mirror (same physical RAM)

All reads/writes byte-swap automatically since x86 is little-endian.

## Licensing Strategy

We deliberately avoid GPL code. Our references:

| Source | License | How Used |
|--------|---------|----------|
| Pureikyubu | CC0 (public domain) | HW register defs, OS HLE patterns |
| libogc | zlib (permissive) | SDK function semantics, API constants |
| GameCubeRecompiled | CC0 (public domain) | TEV shader gen reference, OS init constants |
| Dolphin | GPL-2.0+ | **Read-only** correctness reference — no code copied |

This keeps gcrecomp under the MIT license, so you can use it for anything.

## References

- [Yet Another GameCube Documentation](http://hitmen.c02.at/files/yagcd/)
- [Dolphin Emulator Source](https://github.com/dolphin-emu/dolphin) (GPL, read-only reference)
- [libogc Source](https://github.com/devkitPro/libogc) (zlib)
- [Pureikyubu Source](https://github.com/emu-russia/pureikyubu) (CC0)
- [GameCubeRecompiled Source](https://github.com/KaiserGranatapfel/GameCubeRecompiled) (CC0)
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (MIT)
- [RexGlueSDK](https://github.com/rexglue/rexglue-sdk) (BSD 3-Clause)
