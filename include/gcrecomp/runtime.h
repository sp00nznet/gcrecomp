#pragma once
// =============================================================================
// gcrecomp - GameCube Static Recompilation Runtime
//
// This header defines the core runtime types that recompiled GameCube code
// operates against. Every recompiled function receives a PPCContext* (the
// emulated CPU state) and a Memory* (the emulated address space), and
// reads/writes these structures to produce the same effects as the original
// PowerPC code.
//
// References:
//   - libogc (devkitPro, zlib license): Dolphin SDK function semantics
//   - Pureikyubu / Dolwin (CC0): OS HLE patterns and low-memory layout
//   - GameCubeRecompiled (CC0): Static recompilation architecture
//
// This is the game-agnostic toolkit layer. Game-specific projects include
// this header and link against the gcrecomp runtime library.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>

namespace gcrecomp {

// =============================================================================
// GameConfig - Configuration for game-specific parameters
//
// Passed to init_low_memory() so that the runtime can initialize the emulated
// boot state with the correct game ID, company code, and memory layout for
// the target game. This replaces hardcoded game-specific values.
// =============================================================================
struct GameConfig {
    // 4-character game ID (e.g. "GZLE" for Wind Waker US, "GM8E" for Metroid Prime US)
    char game_id[5] = {0};

    // 2-character company/maker code (e.g. "01" for Nintendo)
    char company_code[3] = {0};

    // Disc number (0 for single-disc games, 0/1 for multi-disc)
    uint8_t disc_number = 0;

    // Disc version
    uint8_t disc_version = 0;

    // Initial arena bounds. The game's OSInit will adjust these after DOL loading.
    // These defaults cover a typical layout; override per-game if needed.
    uint32_t arena_lo = 0x80400000;
    uint32_t arena_hi = 0x81700000;
};

// =============================================================================
// PPCContext - PowerPC (Gekko) CPU Context
//
// Mirrors the state of the GameCube's Gekko processor. Recompiled code reads
// and writes fields of this struct instead of using actual PowerPC registers.
// The struct is aligned to 16 bytes for SIMD compatibility on the host.
//
// Register conventions (Metrowerks CodeWarrior / Dolphin SDK ABI):
//   r0       : volatile scratch
//   r1       : stack pointer (grows downward)
//   r2       : small data area (SDA2) base (read-only after init)
//   r3-r4    : return value (r3:r4 for 64-bit returns)
//   r3-r10   : integer arguments
//   r11-r12  : volatile scratch
//   r13      : small data area (SDA) base (read-only after init)
//   r14-r31  : non-volatile (callee-saved)
//   f0       : volatile scratch
//   f1       : float/double return value; first FP argument
//   f2-f8    : FP arguments
//   f9-f13   : volatile scratch
//   f14-f31  : non-volatile (callee-saved)
//
// Reference: PowerPC Microprocessor Family Programming Environments Manual,
//            Metrowerks CodeWarrior for Dolphin ABI specification
// =============================================================================
struct alignas(16) PPCContext {
    // General Purpose Registers (r0-r31) -- 32-bit integer registers
    uint32_t r[32];

    // Floating Point Registers (f0-f31) -- 64-bit double-precision
    // The Gekko FPU operates in double precision; single-precision results
    // are rounded and stored in the upper half.
    double f[32];

    // Paired Singles registers (ps0/ps1 for each FPR)
    // The Gekko extends each FPR with a second single-precision slot.
    // ps[i][0] = ps0 (same value as the single-precision view of f[i])
    // ps[i][1] = ps1 (paired single extension, used by ps_* instructions)
    float ps[32][2];

    // Condition Register (8 x 4-bit fields = 32 bits total)
    // CR0-CR7, each containing LT/GT/EQ/SO bits.
    // CR0 is set by integer compare/Rc=1 instructions.
    // CR1 is set by floating-point compare/Rc=1 instructions.
    uint32_t cr;

    // Link Register -- holds the return address after a bl (branch-and-link)
    uint32_t lr;

    // Count Register -- used as a loop counter (bdnz) or indirect branch target (bctr)
    uint32_t ctr;

    // Fixed-Point Exception Register
    // Bit 31: SO (Summary Overflow, sticky)
    // Bit 30: OV (Overflow, non-sticky)
    // Bit 29: CA (Carry, set by addc/subfc/etc.)
    // Bits 0-6: byte count (for lswx/stswx)
    uint32_t xer;

    // Floating Point Status and Control Register
    // Contains exception flags, rounding mode, and FP condition bits.
    uint32_t fpscr;

    // Graphics Quantization Registers (GQR0-GQR7)
    // Control the scale and type for paired-single load/store quantized
    // instructions (psq_l, psq_st). Each GQR specifies the data type
    // (u8/s8/u16/s16/float) and a scale factor for pack/unpack.
    uint32_t gqr[8];

    // Program counter -- tracks the current emulated address for debugging
    // and indirect branch resolution. Not a real PPC SPR; maintained by
    // the recompilation runtime.
    uint32_t pc;

    // ---- Helpers ----------------------------------------------------------

    // Get a 4-bit CR field (field 0 = bits 28-31, field 7 = bits 0-3)
    uint32_t get_cr_field(int field) const {
        return (cr >> (28 - field * 4)) & 0xF;
    }
    // Set a 4-bit CR field
    void set_cr_field(int field, uint32_t val) {
        uint32_t shift = 28 - field * 4;
        cr = (cr & ~(0xF << shift)) | ((val & 0xF) << shift);
    }

    // Get a single CR bit (bit 0 = MSB, bit 31 = LSB)
    bool get_cr_bit(int bit) const {
        return (cr >> (31 - bit)) & 1;
    }
    // Set a single CR bit
    void set_cr_bit(int bit, bool val) {
        if (val) cr |= (1 << (31 - bit));
        else     cr &= ~(1 << (31 - bit));
    }

    // XER flag accessors
    bool xer_so() const { return (xer >> 31) & 1; }  // Summary Overflow (sticky)
    bool xer_ov() const { return (xer >> 30) & 1; }  // Overflow
    bool xer_ca() const { return (xer >> 29) & 1; }  // Carry
    void set_xer_ca(bool v) { if (v) xer |= (1 << 29); else xer &= ~(1 << 29); }
    void set_xer_ov(bool v) {
        if (v) { xer |= (1 << 30) | (1 << 31); } // OV also sets SO (sticky)
        else   { xer &= ~(1 << 30); }
    }

    // Update CR0 after an integer operation with Rc=1
    // Sets LT/GT/EQ based on the signed result, copies SO from XER.
    void update_cr0(int32_t result) {
        uint32_t val = 0;
        if (result < 0)       val = 0x8; // LT
        else if (result > 0)  val = 0x4; // GT
        else                  val = 0x2; // EQ
        if (xer_so())         val |= 0x1; // SO
        set_cr_field(0, val);
    }

    // Initialize to power-on state (all registers zeroed).
    // The stack pointer (r1), SDA bases (r2, r13), and other registers
    // will be set by the DOL loader before the game's entry point is called.
    void reset() {
        memset(this, 0, sizeof(*this));
    }
};

// =============================================================================
// Memory - Big-endian emulated GameCube RAM
//
// The GameCube has 24 MB of main RAM (MEM1) accessible via two virtual
// address ranges:
//   0x80000000 - 0x817FFFFF  (cached, used by most code)
//   0xC0000000 - 0xC17FFFFF  (uncached, same physical memory)
//
// Both ranges map to the same 24 MB backing buffer. All reads and writes
// use big-endian byte order to match the PowerPC's native endianness.
//
// Reference: "Dolphin (GameCube) Programming" (official SDK docs, NDA),
//            libogc memory map, Pureikyubu memory subsystem
// =============================================================================
struct Memory {
    static constexpr uint32_t MAIN_RAM_SIZE  = 24 * 1024 * 1024;  // 24 MB
    static constexpr uint32_t MAIN_RAM_BASE  = 0x80000000;        // Cached base
    static constexpr uint32_t MAIN_RAM_END   = MAIN_RAM_BASE + MAIN_RAM_SIZE;
    static constexpr uint32_t UNCACHED_BASE  = 0xC0000000;        // Uncached mirror
    static constexpr uint32_t HW_REG_BASE   = 0xCC000000;        // Hardware registers
    static constexpr uint32_t HW_REG_SIZE   = 0x00010000;        // 64 KB of HW regs

    // Host-side backing buffer for the 24 MB emulated RAM
    uint8_t* ram = nullptr;
    // Hardware register space (absorbs reads/writes to 0xCC000000-0xCC00FFFF)
    uint8_t hw_regs[HW_REG_SIZE] = {};

    // Allocate the backing buffer. Returns false on failure.
    bool init();
    // Free the backing buffer.
    void shutdown();

    // Translate a GameCube virtual address to a host pointer.
    // Handles both cached (0x80xxxxxx) and uncached (0xC0xxxxxx) ranges.
    uint8_t* translate(uint32_t addr);
    const uint8_t* translate(uint32_t addr) const;

    // Big-endian read accessors (match GameCube memory byte order)
    uint8_t  read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;
    uint64_t read64(uint32_t addr) const;
    float    readf32(uint32_t addr) const;
    double   readf64(uint32_t addr) const;

    // Big-endian write accessors
    void write8(uint32_t addr, uint8_t val);
    void write16(uint32_t addr, uint16_t val);
    void write32(uint32_t addr, uint32_t val);
    void write64(uint32_t addr, uint64_t val);
    void writef32(uint32_t addr, float val);
    void writef64(uint32_t addr, double val);
};

// =============================================================================
// FuncTable - Maps GameCube addresses to native recompiled functions
//
// When recompiled code encounters an indirect call (function pointer, virtual
// method dispatch, switch jump table), it looks up the target GameCube address
// in this table to find the corresponding native function. The recompiler
// populates this table at startup with every recompiled function's address.
//
// OS HLE functions are also registered here so that calls to SDK functions
// (e.g. OSAlloc, DVDOpen) are transparently redirected to native replacements.
// =============================================================================
using RecompiledFunc = void(*)(PPCContext* ctx, Memory* mem);

struct FuncTable {
    // Address-to-function mapping
    std::unordered_map<uint32_t, RecompiledFunc> table;

    // Register a recompiled function at the given GameCube address
    void register_func(uint32_t gc_addr, RecompiledFunc func);

    // Look up a function by GameCube address. Returns nullptr if not found.
    RecompiledFunc lookup(uint32_t gc_addr) const;

    // Call a function by its GameCube address. Logs an error if unresolved.
    void call(uint32_t gc_addr, PPCContext* ctx, Memory* mem) const;
};

// =============================================================================
// Global runtime state
//
// These globals are the single instances of the CPU context, memory, and
// function table used throughout the recompilation. They are initialized
// by runtime_init() and torn down by runtime_shutdown().
// =============================================================================
extern PPCContext g_ctx;
extern Memory     g_mem;
extern FuncTable  g_func_table;

// Initialize the runtime: allocate memory, reset CPU state, set up OS
// low-memory globals, and register OS HLE function replacements.
// Returns false on failure (e.g. memory allocation failed).
bool runtime_init();

// Shut down the runtime: free emulated memory.
void runtime_shutdown();

// =============================================================================
// OS HLE (High-Level Emulation) Interface
//
// These functions provide native implementations of Dolphin OS / SDK functions
// so that recompiled game code can call them transparently.
// =============================================================================

// Initialize Dolphin OS low-memory globals (game ID, clock speeds, arena
// bounds, console type, boot magic, main thread). Must be called before
// the game's OSInit or main entry point.
void init_low_memory(Memory* mem, const GameConfig& config);

// Register all OS HLE function replacements into an internal name-to-function
// lookup table. Called during runtime_init().
void register_os_functions();

// Look up an OS HLE function by its SDK symbol name (e.g. "OSAlloc",
// "DVDOpen", "memcpy"). Returns nullptr if the name is not a known OS
// function. Used by the DOL/symbol-map loader to bind addresses.
RecompiledFunc lookup_os_func(const char* name);

// Set the root directory for game file access. DVD file operations
// (DVDOpen, DVDReadPrio, etc.) will resolve game paths relative to this
// directory on the host filesystem.
void set_game_root(const std::string& path);

// Mount a GameCube ISO disc image. Parses the disc header, loads the FST
// (File System Table) into emulated memory, and enables DVD reads directly
// from the ISO file. The FST is placed at the top of emulated RAM and
// OS_FST_ADDR (0x80000038) is set to point to it.
// Returns true on success, false if the ISO can't be opened or parsed.
bool mount_disc_image(const char* iso_path, Memory* mem);

// Read raw data from the mounted disc image at a given byte offset.
// Used internally by DVD HLE functions. Returns bytes read (0 on failure).
size_t disc_read(uint32_t disc_offset, void* dst, size_t length);

// Check if a disc image is currently mounted.
bool is_disc_mounted();

} // namespace gcrecomp
