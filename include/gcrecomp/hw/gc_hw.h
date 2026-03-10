#pragma once
// =============================================================================
// gcrecomp — GameCube Hardware Constants & Register Definitions
// =============================================================================
//
// This header defines the GameCube's hardware-level constants: clock speeds,
// memory map, register addresses, and the Dolphin OS low-memory layout.
//
// These values come from:
//   - Public GameCube hardware documentation (YAGCD, patents)
//   - Pureikyubu / Dolwin emulator (CC0 / Public Domain)
//     https://github.com/emu-russia/pureikyubu
//   - libogc homebrew SDK (zlib license)
//     https://github.com/devkitPro/libogc
//   - GameCubeRecompiled (CC0 / Public Domain)
//     https://github.com/KaiserGranatapfel/GameCubeRecompiled
//
// The GameCube's hardware (codename "Dolphin") consists of:
//   - Gekko: IBM PowerPC 750CXe derivative @ 486 MHz
//   - Flipper: Custom ASIC containing GPU (GX), DSP, memory controller,
//     audio interface, video interface, and I/O controllers
//   - 24 MB main RAM (MEM1) + 16 MB audio RAM (ARAM)
//
// =============================================================================

#include <cstdint>

namespace gcrecomp::hw {

// =============================================================================
// Clock Speeds
// =============================================================================
//
// The Gekko CPU runs at 486 MHz. The system bus runs at 162 MHz (CPU/3).
// The timebase counter (used for OS timing) ticks at bus/4 = 40.5 MHz.
// The DSP runs at half the bus clock = 81 MHz.
//
// Reference: libogc gc/ogc/machine/processor.h
//   TB_BUS_CLOCK  = 162000000
//   TB_CORE_CLOCK = 486000000
//   TB_TIMER_CLOCK = TB_BUS_CLOCK / 4  (= 40500000)

constexpr uint32_t CPU_CLOCK_HZ       = 486000000;   // 486 MHz (Gekko)
constexpr uint32_t BUS_CLOCK_HZ       = 162000000;   // 162 MHz (system bus)
constexpr uint32_t TIMEBASE_FREQ_HZ   = 40500000;    // 40.5 MHz (bus/4, timebase counter)
constexpr uint32_t DSP_CLOCK_HZ       = 81000000;    // 81 MHz (DSP)

// =============================================================================
// Memory Map
// =============================================================================
//
// The GameCube uses a 32-bit virtual address space with these key regions:
//
//   0x00000000 - 0x017FFFFF  Physical RAM (24 MB)
//   0x80000000 - 0x817FFFFF  Cached virtual mirror of RAM
//   0xC0000000 - 0xC17FFFFF  Uncached virtual mirror of RAM
//   0xCC000000 - 0xCC00FFFF  Hardware registers (Flipper ASIC)
//   0xE0000000 - 0xE0003FFF  L2 cache (locked, as scratchpad)
//
// Games always access RAM through the 0x80000000 (cached) or 0xC0000000
// (uncached) mirrors. The physical address is obtained by masking off the
// top bits: phys = virt & 0x01FFFFFF.
//
// Reference: Pureikyubu src/mem.h (RAMSIZE, RAMMASK)
//            libogc gc/ogc/system.h (SYS_BASE_CACHED, SYS_BASE_UNCACHED)

// Main RAM (MEM1): 24 MB
constexpr uint32_t MEM1_BASE          = 0x80000000;  // Cached
constexpr uint32_t MEM1_SIZE          = 0x01800000;  // 24 MB
constexpr uint32_t MEM1_END           = MEM1_BASE + MEM1_SIZE;
constexpr uint32_t MEM1_UNCACHED      = 0xC0000000;  // Uncached mirror
constexpr uint32_t MEM1_PHYSICAL_MASK = 0x01FFFFFF;  // Physical address mask

// Hardware registers (Flipper ASIC, directly mapped)
constexpr uint32_t HW_REG_BASE        = 0xCC000000;

// Sub-regions within hardware registers
// Reference: Pureikyubu src/*.h, libogc libogc/system.c
constexpr uint32_t CP_REG_BASE        = 0xCC000000;  // Command Processor
constexpr uint32_t PE_REG_BASE        = 0xCC001000;  // Pixel Engine
constexpr uint32_t VI_REG_BASE        = 0xCC002000;  // Video Interface
constexpr uint32_t PI_REG_BASE        = 0xCC003000;  // Processor Interface
constexpr uint32_t MI_REG_BASE        = 0xCC004000;  // Memory Interface
constexpr uint32_t DSP_REG_BASE       = 0xCC005000;  // DSP Interface
constexpr uint32_t DI_REG_BASE        = 0xCC006000;  // DVD Interface
constexpr uint32_t SI_REG_BASE        = 0xCC006400;  // Serial Interface (controllers)
constexpr uint32_t EXI_REG_BASE       = 0xCC006800;  // External Interface (memcard, etc.)
constexpr uint32_t AI_REG_BASE        = 0xCC006C00;  // Audio Interface
constexpr uint32_t GX_FIFO_BASE       = 0xCC008000;  // GX command FIFO (write pipe)

// =============================================================================
// Dolphin OS Low-Memory Globals
// =============================================================================
//
// The GameCube's boot ROM (BS2/IPL) and the Dolphin OS SDK store critical
// values at fixed addresses in the first few KB of RAM. Every game reads
// these during initialization.
//
// In a static recompilation, we must populate these before the game's
// OSInit() or main() runs. See init_low_memory() in os_funcs.cpp.
//
// Reference: libogc libogc/system.c (__lowmem_init)
//            GameCubeRecompiled gcrecomp-core/src/runtime/sdk/os.rs (os_init)
//            Pureikyubu src/os.h (OS low memory vars)

// Boot info
constexpr uint32_t OS_BOOT_MAGIC      = 0x80000020;  // Magic word: 0x0D15EA5E
constexpr uint32_t OS_BOOT_VERSION    = 0x80000024;  // Boot version (1)

// Disc header (first 0x20 bytes mirror the DVD header)
constexpr uint32_t OS_DVD_GAME_ID     = 0x80000000;  // 4 bytes: e.g. "GZLE" for Wind Waker US
constexpr uint32_t OS_DVD_COMPANY     = 0x80000004;  // 2 bytes: e.g. "01" (Nintendo)
constexpr uint32_t OS_DVD_DISC_NUM    = 0x80000006;  // 1 byte: disc number
constexpr uint32_t OS_DVD_VERSION     = 0x80000007;  // 1 byte: game version

// Console type and memory
constexpr uint32_t OS_PHYSICAL_MEM_SIZE = 0x80000028;  // Physical mem size (0x01800000 = 24MB)
constexpr uint32_t OS_CONSOLE_TYPE    = 0x8000002C;  // Console type (see below)
constexpr uint32_t OS_ARENA_LO        = 0x80000030;  // Arena low bound
constexpr uint32_t OS_ARENA_HI        = 0x80000034;  // Arena high bound

// DVD filesystem info
constexpr uint32_t OS_FST_ADDR        = 0x80000038;  // FST (file system table) address
constexpr uint32_t OS_FST_MAX_LEN     = 0x8000003C;  // FST max length

// Thread management
constexpr uint32_t OS_THREAD_QUEUE    = 0x800000DC;  // Active thread queue
constexpr uint32_t OS_CURRENT_THREAD  = 0x800000E4;  // Pointer to current OSThread
constexpr uint32_t OS_DEBUG_MONITOR   = 0x800000E0;  // Debug monitor size
constexpr uint32_t OS_DEBUG_FLAG      = 0x800000E8;  // Debug flag
constexpr uint32_t OS_RAM_END         = 0x800000EC;  // RAM end address

// Memory size
constexpr uint32_t OS_SIMULATED_MEM_SIZE = 0x800000F0; // Simulated mem size

// DVD BI2 info
constexpr uint32_t OS_BI2_ADDR        = 0x800000F4;  // bi2.bin address in RAM

// Clock speeds (games read these for timing calculations)
constexpr uint32_t OS_BUS_CLOCK       = 0x800000F8;  // Bus clock (162 MHz)
constexpr uint32_t OS_CPU_CLOCK       = 0x800000FC;  // CPU clock (486 MHz)

// Misc boot info
constexpr uint32_t OS_BOOT_TIME       = 0x800030D8;  // Boot time (u64)
constexpr uint32_t OS_PRODUCTION_PADS = 0x800030E0;  // Production pads (u16, = 6)
constexpr uint32_t OS_DEVKIT_BOOT     = 0x800030E4;  // 0xC0008000

// =============================================================================
// Console Type IDs
// =============================================================================
// Reference: libogc libogc/system.c (__lowmem_init)
//   console_type = 1 + ((*(u32*)0xCC00302c)>>28)
// Common values:

constexpr uint32_t OS_CONSOLE_RETAIL       = 0x00000001;  // Standard retail GameCube
constexpr uint32_t OS_CONSOLE_DEVKIT       = 0x10000002;  // Development kit (NPDP/GDEV)
constexpr uint32_t OS_CONSOLE_TDEV         = 0x10000003;  // TDEV unit
constexpr uint32_t OS_CONSOLE_RETAIL_HW2   = 0x00000003;  // Later retail revision

// =============================================================================
// BP (Blitting Processor) Register Addresses
// =============================================================================
//
// The BP is not a separate hardware unit — it's the register bus that the
// SU (Setup Unit) uses to distribute configuration to the TEV, Pixel Engine,
// Texture Unit, and Rasterizer.
//
// BP registers are written via the GX FIFO with the 0x61 command byte,
// followed by a 32-bit value where bits 31-24 are the register address
// and bits 23-0 are the data.
//
// Reference: Pureikyubu src/su.h (BPRegister enum — the most complete listing)
//            Dolphin Source/Core/VideoCommon/BPMemory.h (GPL, read-only reference)

namespace bp {
    // General mode
    constexpr uint8_t GEN_MODE           = 0x00;  // Num TEV stages, num tex gens, etc.

    // Indirect texture matrices (3 matrices, each 3 rows)
    constexpr uint8_t IND_MTXA0          = 0x06;  // Indirect matrix A, row 0
    constexpr uint8_t IND_MTXB0          = 0x07;
    constexpr uint8_t IND_MTXC0          = 0x08;
    constexpr uint8_t IND_CMD0           = 0x10;  // Indirect TEV stage 0 command

    // Scissor
    constexpr uint8_t SCISSOR_TL         = 0x20;  // Top-left corner
    constexpr uint8_t SCISSOR_BR         = 0x21;  // Bottom-right corner

    // Setup unit
    constexpr uint8_t SU_LPSIZE          = 0x22;  // Line/point size
    constexpr uint8_t SU_SCIS0           = 0x23;  // Scissor offset X
    constexpr uint8_t SU_SCIS1           = 0x24;  // Scissor offset Y

    // Rasterizer texture references (maps tex coord gen -> texture map)
    // RAS1_TREF0-7 at 0x28-0x2F

    // Pixel Engine registers (PE)
    constexpr uint8_t PE_ZMODE           = 0x40;  // Z-buffer mode
    constexpr uint8_t PE_CMODE0          = 0x41;  // Blend mode
    constexpr uint8_t PE_CMODE1          = 0x42;  // Alpha / YUV control
    constexpr uint8_t PE_CONTROL         = 0x43;  // Pixel format, Z format
    constexpr uint8_t COPY_CLEAR_AR      = 0x4F;  // EFB copy clear color (AR)
    constexpr uint8_t COPY_CLEAR_GB      = 0x50;  // EFB copy clear color (GB)
    constexpr uint8_t COPY_CLEAR_Z       = 0x51;  // EFB copy clear Z
    constexpr uint8_t TRIGGER_EFB_COPY   = 0x52;  // Trigger EFB -> XFB/texture copy

    // TEV stages: each stage has two registers (color env + alpha env)
    // Stage N color at 0xC0 + N*2, alpha at 0xC1 + N*2
    constexpr uint8_t TEV_COLOR_ENV_0    = 0xC0;
    constexpr uint8_t TEV_ALPHA_ENV_0    = 0xC1;
    constexpr uint8_t TEV_COLOR_ENV_STRIDE = 2;

    // TEV color registers (loaded by GXSetTevColor)
    constexpr uint8_t TEV_REGISTERL_0    = 0xE0;  // Low: R(11), A(11)
    constexpr uint8_t TEV_REGISTERH_0    = 0xE1;  // High: B(11), G(11)

    // Fog
    constexpr uint8_t TEV_FOG_PARAM_0    = 0xEE;
    constexpr uint8_t TEV_FOG_PARAM_1    = 0xEF;
    constexpr uint8_t TEV_FOG_PARAM_2    = 0xF0;
    constexpr uint8_t TEV_FOG_PARAM_3    = 0xF1;
    constexpr uint8_t TEV_FOG_COLOR      = 0xF2;

    // Alpha compare
    constexpr uint8_t ALPHA_COMPARE      = 0xF3;

    // Z environment
    constexpr uint8_t TEV_ZENV_0         = 0xF4;
    constexpr uint8_t TEV_ZENV_1         = 0xF5;

    // Konst color selectors (8 registers, 2 stages each)
    constexpr uint8_t TEV_KSEL_0         = 0xF6;  // Stages 0-1
    // TEV_KSEL_1 = 0xF7 (stages 2-3) ... TEV_KSEL_7 = 0xFD (stages 14-15)

    // BP mask register
    constexpr uint8_t SS_MASK            = 0xFE;
}

// =============================================================================
// CP (Command Processor) Register Addresses
// =============================================================================
//
// The CP receives commands from the GX FIFO and distributes them. It also
// holds vertex format descriptions (VCD/VAT) and array base/stride pointers.
//
// CP registers are written via the 0x08 FIFO command.
//
// Reference: Pureikyubu src/cp.h (CPRegister enum, VCD/VAT bitfields)

namespace cp {
    constexpr uint8_t MATINDEX_A        = 0x30;  // Matrix index A (position/tex0-3)
    constexpr uint8_t MATINDEX_B        = 0x40;  // Matrix index B (tex4-7)
    constexpr uint8_t VCD_LO            = 0x50;  // Vertex descriptor low
    constexpr uint8_t VCD_HI            = 0x60;  // Vertex descriptor high
    constexpr uint8_t VAT_A             = 0x70;  // Vertex attribute table A (groups 0-7)
    constexpr uint8_t VAT_B             = 0x80;  // Vertex attribute table B
    constexpr uint8_t VAT_C             = 0x90;  // Vertex attribute table C
    constexpr uint8_t ARRAY_BASE        = 0xA0;  // Array base addresses (0xA0-0xAF)
    constexpr uint8_t ARRAY_STRIDE      = 0xB0;  // Array strides (0xB0-0xBF)
}

// =============================================================================
// XF (Transform Unit) Register Addresses
// =============================================================================
//
// The XF handles vertex transformation (model-view-projection), lighting,
// and texture coordinate generation. It has a large internal memory for
// matrices and light parameters.
//
// XF registers are written via the 0x10 FIFO command.
//
// Reference: Pureikyubu src/xf.h (XFRegister enum, full address map)

namespace xf {
    // Matrix memory (not registers — internal SRAM)
    // 0x0000-0x00FF: Position/Normal matrices (64 entries x 4 floats)
    // 0x0400-0x045F: Normal matrices (32 entries x 3 floats)
    // 0x0500-0x05FF: Texture matrices (64 entries x 4 floats)
    // 0x0600-0x067F: Light parameters (8 lights x 16 words)

    // Control registers
    constexpr uint16_t XF_ERROR          = 0x1000;
    constexpr uint16_t XF_DIAGNOSTICS    = 0x1001;
    constexpr uint16_t XF_STATE0         = 0x1002;
    constexpr uint16_t XF_STATE1         = 0x1003;
    constexpr uint16_t XF_CLOCK          = 0x1004;
    constexpr uint16_t XF_CLIPDISABLE    = 0x1005;
    constexpr uint16_t XF_INVTXSPEC      = 0x1008; // Input vertex spec
    constexpr uint16_t XF_NUMCOLORS      = 0x1009; // Number of output colors
    constexpr uint16_t XF_AMBIENT0       = 0x100A; // Ambient color 0
    constexpr uint16_t XF_AMBIENT1       = 0x100B;
    constexpr uint16_t XF_MATERIAL0      = 0x100C; // Material color 0
    constexpr uint16_t XF_MATERIAL1      = 0x100D;
    constexpr uint16_t XF_COLOR0CNTRL    = 0x100E; // Color channel 0 control
    constexpr uint16_t XF_COLOR1CNTRL    = 0x100F;
    constexpr uint16_t XF_ALPHA0CNTRL    = 0x1010; // Alpha channel 0 control
    constexpr uint16_t XF_ALPHA1CNTRL    = 0x1011;
    constexpr uint16_t XF_DUALTEXTRAN    = 0x1012; // Dual texture transform enable
    constexpr uint16_t XF_MATRIXINDEX_A  = 0x1018;
    constexpr uint16_t XF_MATRIXINDEX_B  = 0x1019;
    constexpr uint16_t XF_PROJECTION     = 0x1020; // Projection params (7 regs)
    constexpr uint16_t XF_NUMTEXGENS     = 0x103F; // Number of texture coordinate generators
    constexpr uint16_t XF_TEXGEN0        = 0x1040; // Tex gen 0-7 params
    constexpr uint16_t XF_DUALTEX0       = 0x1050; // Dual tex 0-7 params
}

// =============================================================================
// Video Interface (VI) Constants
// =============================================================================
//
// The VI controls the display output. The GameCube supports NTSC (480i/480p)
// and PAL (576i/576p) modes.
//
// Reference: Pureikyubu src/vi.h
//            libogc gc/ogc/gx_struct.h (GXRModeObj)

enum VITVMode : uint32_t {
    VI_TVMODE_NTSC_INT   = 0x0000,  // NTSC interlaced (480i) — most common
    VI_TVMODE_NTSC_DS    = 0x0001,  // NTSC double-strike
    VI_TVMODE_NTSC_PROG  = 0x0002,  // NTSC progressive (480p)
    VI_TVMODE_PAL_INT    = 0x0100,
    VI_TVMODE_PAL_DS     = 0x0101,
    VI_TVMODE_PAL_PROG   = 0x0102,
    VI_TVMODE_MPAL_INT   = 0x0200,
    VI_TVMODE_MPAL_DS    = 0x0201,
    VI_TVMODE_MPAL_PROG  = 0x0202,
};

constexpr uint32_t VI_DISPLAY_WIDTH    = 640;
constexpr uint32_t VI_DISPLAY_HEIGHT   = 480;  // NTSC effective
constexpr uint32_t VI_DISPLAY_HEIGHT_I = 528;  // Full interlaced
constexpr uint32_t EFB_WIDTH           = 640;  // Embedded Frame Buffer
constexpr uint32_t EFB_HEIGHT          = 528;

// =============================================================================
// GX FIFO Command Opcodes
// =============================================================================
//
// The CPU writes commands to the GX FIFO at 0xCC008000. Each command starts
// with a single opcode byte. Draw commands have the primitive type encoded
// in the high nibble and the vertex format in bits 2-0.
//
// Reference: Pureikyubu src/cp.h (CPCommand enum)
//            libogc gc/ogc/gx.h (GX_Begin inline)

namespace fifo {
    constexpr uint8_t CMD_NOP            = 0x00;
    constexpr uint8_t CMD_LOAD_CP_REG    = 0x08; // + 1 byte addr + 4 bytes data
    constexpr uint8_t CMD_LOAD_XF_REG    = 0x10; // + 2 bytes (len-1, addr) + N*4 bytes
    constexpr uint8_t CMD_LOAD_INDX_A    = 0x20; // Position matrix
    constexpr uint8_t CMD_LOAD_INDX_B    = 0x28; // Normal matrix
    constexpr uint8_t CMD_LOAD_INDX_C    = 0x30; // Texture coordinate matrix
    constexpr uint8_t CMD_LOAD_INDX_D    = 0x38; // Light
    constexpr uint8_t CMD_CALL_DL        = 0x40; // Call display list
    constexpr uint8_t CMD_INVAL_VC       = 0x48; // Invalidate vertex cache
    constexpr uint8_t CMD_LOAD_BP_REG    = 0x61; // + 4 bytes (addr in bits 31-24, data in 23-0)
    constexpr uint8_t CMD_DRAW_QUADS     = 0x80;
    constexpr uint8_t CMD_DRAW_TRIS      = 0x90;
    constexpr uint8_t CMD_DRAW_TRISTRIP  = 0x98;
    constexpr uint8_t CMD_DRAW_TRIFAN    = 0xA0;
    constexpr uint8_t CMD_DRAW_LINES     = 0xA8;
    constexpr uint8_t CMD_DRAW_LINESTRIP = 0xB0;
    constexpr uint8_t CMD_DRAW_POINTS    = 0xB8;
}

// =============================================================================
// TEV Konst Color Selectors
// =============================================================================
//
// Each TEV stage can select a "konst" (constant) color from these sources.
// The konst inputs are available via GX_CC_KONST (color) and GX_CA_KONST (alpha).
// Which konst value they resolve to is controlled per-stage by GXSetTevKColorSel
// and GXSetTevKAlphaSel.
//
// Reference: libogc gc/ogc/gx.h (GX_TEV_KCSEL_*, GX_TEV_KASEL_*)
//            Pureikyubu src/tev.h (TEV_KSel bitfields)

enum KonstColorSel : uint8_t {
    KCSEL_1     = 0x00,  // Constant 1.0
    KCSEL_7_8   = 0x01,  // 7/8
    KCSEL_3_4   = 0x02,  // 3/4
    KCSEL_5_8   = 0x03,  // 5/8
    KCSEL_1_2   = 0x04,  // 1/2
    KCSEL_3_8   = 0x05,  // 3/8
    KCSEL_1_4   = 0x06,  // 1/4
    KCSEL_1_8   = 0x07,  // 1/8
    KCSEL_K0    = 0x0C,  // Konst color register 0 RGB
    KCSEL_K1    = 0x0D,  // Konst color register 1 RGB
    KCSEL_K2    = 0x0E,
    KCSEL_K3    = 0x0F,
    KCSEL_K0_R  = 0x10,  // Konst 0 red channel broadcast
    KCSEL_K1_R  = 0x11,
    KCSEL_K2_R  = 0x12,
    KCSEL_K3_R  = 0x13,
    KCSEL_K0_G  = 0x14,
    KCSEL_K1_G  = 0x15,
    KCSEL_K2_G  = 0x16,
    KCSEL_K3_G  = 0x17,
    KCSEL_K0_B  = 0x18,
    KCSEL_K1_B  = 0x19,
    KCSEL_K2_B  = 0x1A,
    KCSEL_K3_B  = 0x1B,
    KCSEL_K0_A  = 0x1C,
    KCSEL_K1_A  = 0x1D,
    KCSEL_K2_A  = 0x1E,
    KCSEL_K3_A  = 0x1F,
};

enum KonstAlphaSel : uint8_t {
    KASEL_1     = 0x00,
    KASEL_7_8   = 0x01,
    KASEL_3_4   = 0x02,
    KASEL_5_8   = 0x03,
    KASEL_1_2   = 0x04,
    KASEL_3_8   = 0x05,
    KASEL_1_4   = 0x06,
    KASEL_1_8   = 0x07,
    KASEL_K0_R  = 0x10,
    KASEL_K1_R  = 0x11,
    KASEL_K2_R  = 0x12,
    KASEL_K3_R  = 0x13,
    KASEL_K0_G  = 0x14,
    KASEL_K1_G  = 0x15,
    KASEL_K2_G  = 0x16,
    KASEL_K3_G  = 0x17,
    KASEL_K0_B  = 0x18,
    KASEL_K1_B  = 0x19,
    KASEL_K2_B  = 0x1A,
    KASEL_K3_B  = 0x1B,
    KASEL_K0_A  = 0x1C,
    KASEL_K1_A  = 0x1D,
    KASEL_K2_A  = 0x1E,
    KASEL_K3_A  = 0x1F,
};

// =============================================================================
// Alpha Compare & Fog
// =============================================================================

enum AlphaOp : uint8_t {
    ALPHA_OP_AND  = 0,
    ALPHA_OP_OR   = 1,
    ALPHA_OP_XOR  = 2,
    ALPHA_OP_XNOR = 3,
};

enum FogType : uint8_t {
    FOG_NONE         = 0,
    FOG_PERSP_LIN    = 2,
    FOG_PERSP_EXP    = 4,
    FOG_PERSP_EXP2   = 5,
    FOG_PERSP_REXP   = 6,
    FOG_PERSP_REXP2  = 7,
    FOG_ORTHO_LIN    = 10,
    FOG_ORTHO_EXP    = 12,
    FOG_ORTHO_EXP2   = 13,
    FOG_ORTHO_REXP   = 14,
    FOG_ORTHO_REXP2  = 15,
};

// =============================================================================
// Indirect Texture Definitions
// =============================================================================
//
// Indirect texturing allows one texture to offset the coordinates of another.
// This is used extensively in GameCube games for:
//   - Water/ocean effects (e.g. Wind Waker's Great Sea)
//   - Heat haze / distortion
//   - Animated environment mapping
//
// Up to 4 indirect texture stages, each can feed into any TEV stage.
//
// Reference: libogc gc/ogc/gx.h (GX_ITF_*, GX_ITB_*, GX_ITW_*, GX_ITM_*)

enum IndTexFormat : uint8_t {
    ITF_8  = 0,  // 8-bit offsets
    ITF_5  = 1,  // 5-bit
    ITF_4  = 2,  // 4-bit
    ITF_3  = 3,  // 3-bit
};

enum IndTexBias : uint8_t {
    ITB_NONE = 0,
    ITB_S    = 1,  // Bias S coordinate
    ITB_T    = 2,  // Bias T coordinate
    ITB_ST   = 3,  // Bias both
    ITB_U    = 4,
    ITB_SU   = 5,
    ITB_TU   = 6,
    ITB_STU  = 7,
};

enum IndTexWrap : uint8_t {
    ITW_OFF  = 0,  // No wrapping
    ITW_256  = 1,
    ITW_128  = 2,
    ITW_64   = 3,
    ITW_32   = 4,
    ITW_16   = 5,
    ITW_0    = 6,  // Clamp to 0
};

enum IndTexMtxId : uint8_t {
    ITM_OFF  = 0,
    ITM_0    = 1,
    ITM_1    = 2,
    ITM_2    = 3,
    ITM_S0   = 5,
    ITM_S1   = 6,
    ITM_S2   = 7,
    ITM_T0   = 9,
    ITM_T1   = 10,
    ITM_T2   = 11,
};

} // namespace gcrecomp::hw
