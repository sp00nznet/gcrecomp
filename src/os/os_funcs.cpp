// =============================================================================
// OS Function Replacements (HLE - High Level Emulation)
//
// The GameCube's Dolphin OS provides system-level services (memory allocation,
// threading, DVD access, timing, etc.) that every game's SDK code invokes.
// In a static recompilation, these functions cannot be recompiled directly
// because they interact with hardware that doesn't exist on the host. Instead,
// we intercept them at the function level and provide native implementations
// that produce equivalent results in the emulated memory space.
//
// This file is game-agnostic. Game-specific parameters (game ID, arena bounds)
// are supplied via the GameConfig struct passed to init_low_memory().
//
// References:
//   - libogc (devkitPro, zlib license):
//       Open-source Dolphin SDK reimplementation. Function signatures, return
//       values, and side effects are derived from libogc headers and source.
//       Key files: ogc/os.h, ogc/dvd.h, ogc/cache.h, ogc/system.h, ogc/pad.h
//
//   - Pureikyubu / Dolwin (CC0 1.0 Universal):
//       GameCube emulator with OS HLE. Low-memory initialization constants,
//       thread management patterns, and interrupt handling semantics are
//       sourced from its HLE/os.cpp module.
//
//   - GameCubeRecompiled (CC0):
//       Static recompilation project. The overall architecture of intercepting
//       SDK calls via a symbol-name lookup table follows this project's design.
// =============================================================================

#include "gcrecomp/runtime.h"
#include "gcrecomp/hw/gc_hw.h"
#include "gcrecomp/hw/os_defs.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace gcrecomp {

// =============================================================================
// Timing
//
// The GameCube's timebase counter runs at bus_clock/4 = 40.5 MHz. OSGetTime
// returns a 64-bit tick count; OSGetTick returns the lower 32 bits. We map
// these to the host's high-resolution clock.
//
// Reference: libogc ogc/lwp_watchdog.h, Pureikyubu HLE timing
// =============================================================================

static auto g_start_time = std::chrono::high_resolution_clock::now();

// OSGetTime: returns 64-bit timebase ticks (40.5 MHz)
// Return convention: high 32 bits in r3, low 32 bits in r4
static void os_get_time(PPCContext* ctx, Memory* mem) {
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - g_start_time);
    uint64_t ticks = (uint64_t)(elapsed.count() * (hw::TIMEBASE_FREQ_HZ / 1000000.0));
    ctx->r[3] = (uint32_t)(ticks >> 32);
    ctx->r[4] = (uint32_t)ticks;
}

// OSGetTick: returns lower 32-bit of timebase in r3
static void os_get_tick(PPCContext* ctx, Memory* mem) {
    auto now = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - g_start_time);
    ctx->r[3] = (uint32_t)(elapsed.count() * (hw::TIMEBASE_FREQ_HZ / 1000000.0));
}

// OSTicksToMilliseconds helper (the game may inline this, but provided as fallback)
static void os_ticks_to_ms(PPCContext* ctx, Memory* mem) {
    uint64_t ticks = ((uint64_t)ctx->r[3] << 32) | ctx->r[4];
    uint64_t ms = ticks / (hw::TIMEBASE_FREQ_HZ / 1000);
    ctx->r[3] = (uint32_t)(ms >> 32);
    ctx->r[4] = (uint32_t)ms;
}

// =============================================================================
// Interrupts (no-ops in recompilation -- no preemption occurs)
//
// The original OS uses these to protect critical sections. Since recompiled
// code runs single-threaded, we simply track the enabled state and return
// the previous value as the real OS would.
//
// Reference: libogc ogc/irq.h
// =============================================================================

static bool g_interrupts_enabled = true;

// OSDisableInterrupts: returns previous state in r3
static void os_disable_interrupts(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = g_interrupts_enabled ? 1 : 0;
    g_interrupts_enabled = false;
}

// OSEnableInterrupts: returns previous state in r3
static void os_enable_interrupts(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = g_interrupts_enabled ? 1 : 0;
    g_interrupts_enabled = true;
}

// OSRestoreInterrupts: restore from saved state (r3 = saved state)
static void os_restore_interrupts(PPCContext* ctx, Memory* mem) {
    bool prev = g_interrupts_enabled;
    g_interrupts_enabled = (ctx->r[3] != 0);
    ctx->r[3] = prev ? 1 : 0;
}

// =============================================================================
// Memory Allocation (OS Arena)
//
// The GameCube OS uses an arena allocator for the main heap. Arena bounds
// are stored in low memory at OS_ARENA_LO and OS_ARENA_HI. Games typically
// call OSGetArenaLo/Hi during initialization, then create one or more heaps
// within the arena using OSCreateHeap.
//
// Reference: libogc ogc/system.h (OSGetArenaLo, OSSetArenaLo, etc.)
// =============================================================================

static void os_get_arena_lo(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = mem->read32(hw::OS_ARENA_LO);
}

static void os_get_arena_hi(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = mem->read32(hw::OS_ARENA_HI);
}

static void os_set_arena_lo(PPCContext* ctx, Memory* mem) {
    mem->write32(hw::OS_ARENA_LO, ctx->r[3]);
}

static void os_set_arena_hi(PPCContext* ctx, Memory* mem) {
    mem->write32(hw::OS_ARENA_HI, ctx->r[3]);
}

// =============================================================================
// Heap Management (OSAlloc / OSFree)
//
// The SDK provides a simple heap allocator that operates on regions carved
// from the arena. Each heap maintains a sorted free list of blocks. We
// implement a first-fit allocator with coalescing that operates entirely
// within emulated memory, matching the OS's expected behavior.
//
// Block alignment: 32 bytes (GameCube standard, required for DMA).
// Cell header: 16 bytes (prev, next, size) stored at the start of each block.
//
// Reference: libogc ogc/system.h (OSCreateHeap, OSAlloc, OSFree)
// =============================================================================

struct HeapInfo {
    uint32_t start;     // Start address in emulated memory
    uint32_t size;      // Total size of the heap region
    uint32_t free_head; // Address of first free cell (0 = empty list)
    bool     active;    // Whether this heap slot is in use
};

static constexpr int MAX_HEAPS = 16;
static HeapInfo g_heaps[MAX_HEAPS];
static int g_current_heap = -1;

// OSCreateHeap(void* start, void* end) -> heap_id
// Creates a new heap covering [start, end). Returns the heap ID or -1 on failure.
static void os_create_heap(PPCContext* ctx, Memory* mem) {
    uint32_t start = ctx->r[3];
    uint32_t end   = ctx->r[4];
    uint32_t size  = end - start;

    // Find free heap slot
    int heap_id = -1;
    for (int i = 0; i < MAX_HEAPS; i++) {
        if (!g_heaps[i].active) {
            heap_id = i;
            break;
        }
    }

    if (heap_id < 0) {
        printf("[OS] OSCreateHeap: no free heap slots!\n");
        ctx->r[3] = (uint32_t)-1;
        return;
    }

    // Initialize heap: one big free cell covering the entire region
    g_heaps[heap_id].start     = start;
    g_heaps[heap_id].size      = size;
    g_heaps[heap_id].free_head = start;
    g_heaps[heap_id].active    = true;

    // Write free cell header at start of region
    mem->write32(start + 0, 0);      // prev = NULL
    mem->write32(start + 4, 0);      // next = NULL
    mem->write32(start + 8, size);   // size = entire region

    printf("[OS] OSCreateHeap: id=%d start=0x%08X size=%u\n", heap_id, start, size);
    ctx->r[3] = (uint32_t)heap_id;
}

// OSSetCurrentHeap(int heap_id) -> old_heap_id
static void os_set_current_heap(PPCContext* ctx, Memory* mem) {
    int old = g_current_heap;
    g_current_heap = (int)ctx->r[3];
    ctx->r[3] = (uint32_t)old;
}

// OSAlloc(uint32_t size) -> void*
// Allocates from the current heap using first-fit. Returns NULL on failure.
static void os_alloc(PPCContext* ctx, Memory* mem) {
    uint32_t req_size = ctx->r[3];

    // Align to 32 bytes (GameCube standard alignment for DMA compatibility)
    req_size = (req_size + 31) & ~31;
    // Add cell header size
    uint32_t total = req_size + os::OSHeapCell::SIZE;

    if (g_current_heap < 0 || g_current_heap >= MAX_HEAPS || !g_heaps[g_current_heap].active) {
        printf("[OS] OSAlloc: no active heap! (heap=%d, size=%u)\n", g_current_heap, req_size);
        ctx->r[3] = 0;
        return;
    }

    HeapInfo& heap = g_heaps[g_current_heap];

    // First-fit search through free list
    uint32_t cell = heap.free_head;
    while (cell) {
        uint32_t cell_size = mem->read32(cell + 8);
        if (cell_size >= total) {
            // Found a fit
            uint32_t prev = mem->read32(cell + 0);
            uint32_t next = mem->read32(cell + 4);
            uint32_t remaining = cell_size - total;

            if (remaining >= os::OSHeapCell::SIZE + 32) {
                // Split: create a new free cell after the allocated block
                uint32_t new_cell = cell + total;
                mem->write32(new_cell + 0, prev);
                mem->write32(new_cell + 4, next);
                mem->write32(new_cell + 8, remaining);

                // Update links
                if (prev) mem->write32(prev + 4, new_cell);
                else      heap.free_head = new_cell;
                if (next) mem->write32(next + 0, new_cell);

                // Mark allocated cell with its total size
                mem->write32(cell + 8, total);
            } else {
                // Use entire cell (don't split tiny remainder)
                total = cell_size;
                if (prev) mem->write32(prev + 4, next);
                else      heap.free_head = next;
                if (next) mem->write32(next + 0, prev);
            }

            // Return pointer past the header (usable memory starts after cell header)
            ctx->r[3] = cell + os::OSHeapCell::SIZE;
            return;
        }
        cell = mem->read32(cell + 4); // next
    }

    printf("[OS] OSAlloc: out of memory! (heap=%d, requested=%u)\n", g_current_heap, req_size);
    ctx->r[3] = 0;
}

// OSFree(void* ptr)
// Returns the block to the current heap's free list with address-ordered
// insertion and coalescing of adjacent free blocks.
static void os_free(PPCContext* ctx, Memory* mem) {
    uint32_t ptr = ctx->r[3];
    if (!ptr) return;

    if (g_current_heap < 0 || g_current_heap >= MAX_HEAPS || !g_heaps[g_current_heap].active) {
        printf("[OS] OSFree: no active heap!\n");
        return;
    }

    HeapInfo& heap = g_heaps[g_current_heap];

    // The cell header is right before the pointer
    uint32_t cell = ptr - os::OSHeapCell::SIZE;
    uint32_t cell_size = mem->read32(cell + 8);

    // Insert into free list (sorted by address for coalescing)
    uint32_t prev = 0;
    uint32_t cur = heap.free_head;
    while (cur && cur < cell) {
        prev = cur;
        cur = mem->read32(cur + 4);
    }

    mem->write32(cell + 0, prev);
    mem->write32(cell + 4, cur);

    if (prev) mem->write32(prev + 4, cell);
    else      heap.free_head = cell;
    if (cur)  mem->write32(cur + 0, cell);

    // Coalesce with next block
    if (cur && cell + cell_size == cur) {
        uint32_t next_size = mem->read32(cur + 8);
        uint32_t next_next = mem->read32(cur + 4);
        mem->write32(cell + 4, next_next);
        mem->write32(cell + 8, cell_size + next_size);
        if (next_next) mem->write32(next_next + 0, cell);
        cell_size += next_size;
    }

    // Coalesce with previous block
    if (prev) {
        uint32_t prev_size = mem->read32(prev + 8);
        if (prev + prev_size == cell) {
            mem->write32(prev + 4, mem->read32(cell + 4));
            mem->write32(prev + 8, prev_size + cell_size);
            uint32_t cell_next = mem->read32(cell + 4);
            if (cell_next) mem->write32(cell_next + 0, prev);
        }
    }
}

// =============================================================================
// OSCache -- Cache operations (no-ops in recompilation)
//
// The GameCube has separate L1 data and instruction caches. Games flush or
// invalidate cache lines before DMA transfers or after modifying code in
// memory. Since we don't emulate the cache hierarchy, these are no-ops.
//
// Reference: libogc ogc/cache.h
// =============================================================================

static void os_dcache_flush(PPCContext* ctx, Memory* mem) {
    // DCFlushRange(void* addr, uint32_t size) -- no-op in recompilation
}

static void os_dcache_invalidate(PPCContext* ctx, Memory* mem) {
    // DCInvalidateRange(void* addr, uint32_t size) -- no-op
}

static void os_dcache_store(PPCContext* ctx, Memory* mem) {
    // DCStoreRange(void* addr, uint32_t size) -- no-op
}

static void os_icache_invalidate(PPCContext* ctx, Memory* mem) {
    // ICInvalidateRange(void* addr, uint32_t size) -- no-op
}

// =============================================================================
// Console / Debug Output
//
// The SDK provides OSReport for debug printf, OSPanic for assertions, and
// OSFatal for unrecoverable errors. We forward these to the host's stdout/
// stderr so developers can see diagnostic output from the recompiled game.
//
// Reference: libogc ogc/system.h
// =============================================================================

static void os_report(PPCContext* ctx, Memory* mem) {
    // OSReport(const char* fmt, ...)
    // Note: variadic formatting is not fully supported; we print the format
    // string as-is. Full printf emulation would require a vararg ABI shim.
    uint32_t fmt_addr = ctx->r[3];
    char buf[512];
    int i = 0;
    while (i < 511) {
        char c = (char)mem->read8(fmt_addr + i);
        if (c == 0) break;
        buf[i++] = c;
    }
    buf[i] = 0;
    printf("[GameCube] %s", buf);
}

static void os_panic(PPCContext* ctx, Memory* mem) {
    // OSPanic(const char* file, int line, const char* msg, ...)
    uint32_t file_addr = ctx->r[3];
    uint32_t line = ctx->r[4];
    uint32_t msg_addr = ctx->r[5];

    auto read_str = [&](uint32_t addr) -> std::string {
        std::string s;
        for (int j = 0; j < 256; j++) {
            char c = (char)mem->read8(addr + j);
            if (c == 0) break;
            s += c;
        }
        return s;
    };

    std::string file = read_str(file_addr);
    std::string msg = read_str(msg_addr);
    fprintf(stderr, "[GameCube PANIC] %s:%u: %s\n", file.c_str(), line, msg.c_str());
}

static void os_fatal(PPCContext* ctx, Memory* mem) {
    // OSFatal(const char* msg) -- display fatal error and halt
    uint32_t msg_addr = ctx->r[3];
    char buf[256];
    int i = 0;
    while (i < 255) {
        char c = (char)mem->read8(msg_addr + i);
        if (c == 0) break;
        buf[i++] = c;
    }
    buf[i] = 0;
    fprintf(stderr, "[GameCube FATAL] %s\n", buf);
}

// =============================================================================
// String / Memory Utilities
//
// The Dolphin SDK links against Metrowerks' CRT, which provides standard C
// memory and string functions. We intercept these and forward to the host CRT,
// translating emulated addresses to host pointers.
//
// Reference: ISO C standard library (memset, memcpy, strlen, etc.)
// =============================================================================

static void os_memset(PPCContext* ctx, Memory* mem) {
    uint32_t dst = ctx->r[3];
    uint8_t val = (uint8_t)ctx->r[4];
    uint32_t n = ctx->r[5];
    uint8_t* ptr = mem->translate(dst);
    if (ptr) memset(ptr, val, n);
    ctx->r[3] = dst; // return dst
}

static void os_memcpy(PPCContext* ctx, Memory* mem) {
    uint32_t dst = ctx->r[3];
    uint32_t src = ctx->r[4];
    uint32_t n = ctx->r[5];
    uint8_t* d = mem->translate(dst);
    const uint8_t* s = mem->translate(src);
    if (d && s) memcpy(d, s, n);
    ctx->r[3] = dst;
}

static void os_memmove(PPCContext* ctx, Memory* mem) {
    uint32_t dst = ctx->r[3];
    uint32_t src = ctx->r[4];
    uint32_t n = ctx->r[5];
    uint8_t* d = mem->translate(dst);
    const uint8_t* s = mem->translate(src);
    if (d && s) memmove(d, s, n);
    ctx->r[3] = dst;
}

static void os_memcmp(PPCContext* ctx, Memory* mem) {
    uint32_t a = ctx->r[3];
    uint32_t b = ctx->r[4];
    uint32_t n = ctx->r[5];
    const uint8_t* pa = mem->translate(a);
    const uint8_t* pb = mem->translate(b);
    ctx->r[3] = (uint32_t)memcmp(pa, pb, n);
}

static void os_strlen(PPCContext* ctx, Memory* mem) {
    uint32_t s = ctx->r[3];
    uint32_t len = 0;
    while (mem->read8(s + len) != 0 && len < 0x100000) len++;
    ctx->r[3] = len;
}

static void os_strcmp(PPCContext* ctx, Memory* mem) {
    uint32_t a = ctx->r[3];
    uint32_t b = ctx->r[4];
    const char* pa = (const char*)mem->translate(a);
    const char* pb = (const char*)mem->translate(b);
    ctx->r[3] = (uint32_t)strcmp(pa, pb);
}

static void os_strcpy(PPCContext* ctx, Memory* mem) {
    uint32_t dst = ctx->r[3];
    uint32_t src = ctx->r[4];
    char* d = (char*)mem->translate(dst);
    const char* s = (const char*)mem->translate(src);
    if (d && s) strcpy(d, s);
    ctx->r[3] = dst;
}

static void os_strncpy(PPCContext* ctx, Memory* mem) {
    uint32_t dst = ctx->r[3];
    uint32_t src = ctx->r[4];
    uint32_t n = ctx->r[5];
    char* d = (char*)mem->translate(dst);
    const char* s = (const char*)mem->translate(src);
    if (d && s) strncpy(d, s, n);
    ctx->r[3] = dst;
}

// =============================================================================
// Math Functions (CRT passthrough)
//
// The SDK's math library (from Metrowerks CRT) provides standard math
// functions. We forward to the host's libm. Arguments and return values
// use the FPR convention: first FP arg in f1, second in f2, result in f1.
//
// Reference: Metrowerks CodeWarrior floating-point ABI
// =============================================================================

static void os_sinf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = sin(ctx->f[1]);
}

static void os_cosf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = cos(ctx->f[1]);
}

static void os_tanf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = tan(ctx->f[1]);
}

static void os_atanf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = atan(ctx->f[1]);
}

static void os_atan2f(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = atan2(ctx->f[1], ctx->f[2]);
}

static void os_sqrtf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = sqrt(ctx->f[1]);
}

static void os_floorf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = floor(ctx->f[1]);
}

static void os_ceilf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = ceil(ctx->f[1]);
}

static void os_fabsf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = fabs(ctx->f[1]);
}

static void os_fmodf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = fmod(ctx->f[1], ctx->f[2]);
}

static void os_powf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = pow(ctx->f[1], ctx->f[2]);
}

static void os_logf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = log(ctx->f[1]);
}

static void os_expf(PPCContext* ctx, Memory* mem) {
    ctx->f[1] = exp(ctx->f[1]);
}

// =============================================================================
// DVD File System
//
// Intercepts Dolphin SDK disc-read functions and serves data from extracted
// game files on the host filesystem. DVDOpen translates the game path to a
// host path relative to the configured game root directory, then uses
// standard C file I/O.
//
// DVDReadAsync is implemented synchronously (reads immediately, then calls
// the callback). This is correct for single-threaded recompilation since
// no other code runs between the read request and the callback.
//
// Reference: libogc ogc/dvd.h (DVDOpen, DVDClose, DVDReadPrio)
// =============================================================================

struct DVDFileEntry {
    std::string host_path;
    uint32_t    offset;     // Offset on the virtual disc
    uint32_t    size;
};

static std::string g_game_root;  // Root directory of extracted game files
static std::unordered_map<uint32_t, FILE*> g_open_dvd_files;  // fileinfo addr -> FILE*

// DVDOpen(const char* path, DVDFileInfo* info) -> BOOL
static void dvd_open(PPCContext* ctx, Memory* mem) {
    uint32_t path_addr = ctx->r[3];
    uint32_t info_addr = ctx->r[4];

    // Read path from emulated memory
    char path[256];
    int i = 0;
    while (i < 255) {
        char c = (char)mem->read8(path_addr + i);
        if (c == 0) break;
        path[i++] = c;
    }
    path[i] = 0;

    // Convert game path to host path
    std::string host_path = g_game_root + "/" + path;
    for (char& c : host_path) {
        if (c == '\\') c = '/';
    }

    FILE* fp = fopen(host_path.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        uint32_t file_size = (uint32_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);

        // Write file info to emulated memory
        mem->write32(info_addr + os::DVDFileInfo::OFF_START, 0);
        mem->write32(info_addr + os::DVDFileInfo::OFF_LENGTH, file_size);

        g_open_dvd_files[info_addr] = fp;

        printf("[DVD] Open: %s (%u bytes)\n", path, file_size);
        ctx->r[3] = 1; // TRUE
    } else {
        printf("[DVD] Open failed: %s (host: %s)\n", path, host_path.c_str());
        ctx->r[3] = 0; // FALSE
    }
}

// DVDClose(DVDFileInfo* info)
static void dvd_close(PPCContext* ctx, Memory* mem) {
    uint32_t info_addr = ctx->r[3];
    auto it = g_open_dvd_files.find(info_addr);
    if (it != g_open_dvd_files.end()) {
        fclose(it->second);
        g_open_dvd_files.erase(it);
    }
    ctx->r[3] = 1;
}

// DVDReadPrio(DVDFileInfo* info, void* buf, int32_t length, int32_t offset, int32_t prio)
// Synchronous read from disc file. Returns number of bytes read in r3.
static void dvd_read_prio(PPCContext* ctx, Memory* mem) {
    uint32_t info_addr = ctx->r[3];
    uint32_t buf_addr  = ctx->r[4];
    int32_t  length    = (int32_t)ctx->r[5];
    int32_t  offset    = (int32_t)ctx->r[6];

    auto it = g_open_dvd_files.find(info_addr);
    if (it != g_open_dvd_files.end()) {
        FILE* fp = it->second;
        fseek(fp, offset, SEEK_SET);

        uint8_t* dst = mem->translate(buf_addr);
        if (dst) {
            size_t read = fread(dst, 1, length, fp);
            ctx->r[3] = (uint32_t)read;
        } else {
            ctx->r[3] = 0;
        }
    } else {
        printf("[DVD] ReadPrio: invalid file info 0x%08X\n", info_addr);
        ctx->r[3] = 0;
    }
}

// DVDReadAsync(DVDFileInfo* info, void* buf, int32_t len, int32_t off, DVDCallback cb)
// Implemented as synchronous read + immediate callback invocation.
static void dvd_read_async(PPCContext* ctx, Memory* mem) {
    uint32_t info_addr = ctx->r[3];
    uint32_t buf_addr  = ctx->r[4];
    int32_t  length    = (int32_t)ctx->r[5];
    int32_t  offset    = (int32_t)ctx->r[6];
    uint32_t callback  = ctx->r[7];

    auto it = g_open_dvd_files.find(info_addr);
    if (it != g_open_dvd_files.end()) {
        FILE* fp = it->second;
        fseek(fp, offset, SEEK_SET);
        uint8_t* dst = mem->translate(buf_addr);
        if (dst) fread(dst, 1, length, fp);
    }

    // If there's a callback, invoke it synchronously
    if (callback) {
        uint32_t save_r3 = ctx->r[3];
        ctx->r[3] = info_addr;
        ctx->r[4] = 0; // result = success
        g_func_table.call(callback, ctx, mem);
        ctx->r[3] = save_r3;
    }

    ctx->r[3] = 1; // Success
}

// =============================================================================
// Disc Image (ISO) Support
//
// Mounts a GameCube disc image and loads the FST into emulated memory.
// Once mounted, disc_read() can serve raw data from any offset.
// =============================================================================

static FILE* g_disc_image = nullptr;
static uint32_t g_fst_ram_addr = 0;

bool mount_disc_image(const char* iso_path, Memory* mem) {
    if (g_disc_image) {
        fclose(g_disc_image);
        g_disc_image = nullptr;
    }

    FILE* fp = fopen(iso_path, "rb");
    if (!fp) {
        fprintf(stderr, "[DVD] Failed to open disc image: %s\n", iso_path);
        return false;
    }

    // Read disc header (first 0x440 bytes)
    uint8_t header[0x0440];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "[DVD] Failed to read disc header\n");
        fclose(fp);
        return false;
    }

    // Read big-endian 32-bit values from header
    auto be32 = [](const uint8_t* p) -> uint32_t {
        return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    };

    // Game ID (first 6 bytes)
    char game_id[7] = {};
    memcpy(game_id, header, 6);

    // FST location
    uint32_t fst_offset = be32(header + 0x0424);
    uint32_t fst_size   = be32(header + 0x0428);

    printf("[DVD] Disc image: %s\n", iso_path);
    printf("[DVD]   Game ID: %s\n", game_id);
    printf("[DVD]   FST offset: 0x%08X, size: %u bytes\n", fst_offset, fst_size);

    if (fst_size == 0 || fst_size > 0x200000) {  // Sanity: < 2MB
        fprintf(stderr, "[DVD] Invalid FST size\n");
        fclose(fp);
        return false;
    }

    // Read FST
    std::vector<uint8_t> fst_data(fst_size);
    fseek(fp, fst_offset, SEEK_SET);
    if (fread(fst_data.data(), 1, fst_size, fp) != fst_size) {
        fprintf(stderr, "[DVD] Failed to read FST\n");
        fclose(fp);
        return false;
    }

    // Place FST near top of emulated RAM (below arena end)
    g_fst_ram_addr = 0x81600000;
    uint8_t* fst_dst = mem->translate(g_fst_ram_addr);
    memcpy(fst_dst, fst_data.data(), fst_size);

    // Set OS low-memory FST pointers
    mem->write32(hw::OS_FST_ADDR, g_fst_ram_addr);
    mem->write32(hw::OS_FST_MAX_LEN, fst_size);

    uint32_t num_entries = be32(fst_data.data() + 8);
    printf("[DVD]   FST loaded at 0x%08X (%u entries)\n", g_fst_ram_addr, num_entries);

    g_disc_image = fp;
    return true;
}

size_t disc_read(uint32_t disc_offset, void* dst, size_t length) {
    if (!g_disc_image || !dst || length == 0) return 0;
    fseek(g_disc_image, disc_offset, SEEK_SET);
    return fread(dst, 1, length, g_disc_image);
}

bool is_disc_mounted() {
    return g_disc_image != nullptr;
}

// =============================================================================
// Thread Management (simplified stubs)
//
// Most GameCube games are primarily single-threaded for gameplay logic, with
// auxiliary threads for audio, DVD loading, etc. In a static recompilation
// environment, we run everything on a single host thread and provide minimal
// stubs that satisfy SDK initialization (OSCreateThread, OSGetCurrentThread).
//
// Reference: libogc ogc/lwp_threads.h, Pureikyubu HLE/os.cpp
// =============================================================================

static uint32_t g_main_thread_addr = 0;

// OSCreateThread(thread, func, arg, stack_top, stack_size, priority, attrs)
// Writes a valid thread structure but does not actually spawn a host thread.
static void os_create_thread(PPCContext* ctx, Memory* mem) {
    uint32_t thread_addr = ctx->r[3];
    uint32_t func_addr   = ctx->r[4];
    uint32_t arg         = ctx->r[5];
    uint32_t stack_top   = ctx->r[6];
    uint32_t stack_size  = ctx->r[7];
    // priority is in r8

    // Zero the thread structure
    uint8_t* t = mem->translate(thread_addr);
    if (t) memset(t, 0, os::OSThread::SIZE);

    // Set initial state
    mem->write16(thread_addr + os::thread_off::STATE, os::OSThread::STATE_READY);
    mem->write32(thread_addr + os::thread_off::PRIORITY, ctx->r[8]);
    mem->write32(thread_addr + os::thread_off::BASE_PRIORITY, ctx->r[8]);
    mem->write32(thread_addr + os::thread_off::STACK_BASE, stack_top);
    mem->write32(thread_addr + os::thread_off::STACK_END, stack_top - stack_size);

    ctx->r[3] = 1; // TRUE (success)
}

// OSResumeThread -- mark thread as running
static void os_resume_thread(PPCContext* ctx, Memory* mem) {
    uint32_t thread_addr = ctx->r[3];
    mem->write16(thread_addr + os::thread_off::STATE, os::OSThread::STATE_RUNNING);
    ctx->r[3] = 0;
}

// OSGetCurrentThread -- returns pointer to current thread's TCB
static void os_get_current_thread(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = mem->read32(hw::OS_CURRENT_THREAD);
}

// OSSleepThread -- stub (no-op in single-threaded recompilation)
static void os_sleep_thread(PPCContext* ctx, Memory* mem) {
}

// OSWakeupThread -- stub (no-op)
static void os_wakeup_thread(PPCContext* ctx, Memory* mem) {
}

// =============================================================================
// Mutex (simplified stubs)
//
// No-ops in single-threaded recompilation. OSInitMutex zeroes the structure
// for memory consistency.
//
// Reference: libogc ogc/mutex.h
// =============================================================================

static void os_init_mutex(PPCContext* ctx, Memory* mem) {
    uint32_t mutex = ctx->r[3];
    uint8_t* p = mem->translate(mutex);
    if (p) memset(p, 0, os::OSMutex::SIZE);
}

static void os_lock_mutex(PPCContext* ctx, Memory* mem) {
    // No-op in single-threaded recompilation
}

static void os_unlock_mutex(PPCContext* ctx, Memory* mem) {
    // No-op
}

// =============================================================================
// Message Queue (simplified stubs)
//
// Reference: libogc ogc/message.h
// =============================================================================

static void os_init_message_queue(PPCContext* ctx, Memory* mem) {
    uint32_t mq = ctx->r[3];
    uint8_t* p = mem->translate(mq);
    if (p) memset(p, 0, os::OSMessageQueue::SIZE);
}

static void os_send_message(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = 1; // TRUE
}

static void os_receive_message(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = 0; // FALSE (no messages)
}

// =============================================================================
// Low-Memory Initialization
//
// Sets up the values that the Dolphin OS bootloader (BS2/IPL) normally writes
// to low memory during the boot sequence. This must be called before the
// game's OSInit or main entry point, as SDK initialization code reads these
// addresses to determine hardware configuration, memory layout, and game
// identity.
//
// The GameConfig struct provides game-specific values (game ID, company code,
// arena bounds) so this function remains game-agnostic.
//
// Reference:
//   - libogc __lowmem_init (ogc/system.c): boot magic, clock speeds, pads
//   - Pureikyubu HLE/os.cpp: low-memory layout documentation
//   - GameCubeRecompiled: arena and thread initialization pattern
// =============================================================================

void init_low_memory(Memory* mem, const GameConfig& config) {
    printf("[OS] Initializing low memory (Dolphin OS boot state)...\n");

    // Game ID (4 bytes) and company/maker code (2 bytes) from the disc header
    for (int i = 0; i < 4 && config.game_id[i]; i++) {
        mem->write8(hw::OS_DVD_GAME_ID + i, (uint8_t)config.game_id[i]);
    }
    for (int i = 0; i < 2 && config.company_code[i]; i++) {
        mem->write8(hw::OS_DVD_COMPANY + i, (uint8_t)config.company_code[i]);
    }
    mem->write8(hw::OS_DVD_DISC_NUM, config.disc_number);
    mem->write8(hw::OS_DVD_VERSION, config.disc_version);

    // Console type (configurable: retail GC, devkit, Triforce, etc.)
    mem->write32(hw::OS_CONSOLE_TYPE, config.console_type);

    // Physical memory size (configurable: 24 MB for GC, 48 MB for Triforce, etc.)
    mem->write32(hw::OS_PHYSICAL_MEM_SIZE, config.ram_size);
    mem->write32(hw::OS_SIMULATED_MEM_SIZE, config.ram_size);

    // Clock speeds (the game reads these for timing calculations)
    mem->write32(hw::OS_BUS_CLOCK, hw::BUS_CLOCK_HZ);
    mem->write32(hw::OS_CPU_CLOCK, hw::CPU_CLOCK_HZ);

    // Arena bounds (initial -- the game's OSInit will adjust these)
    mem->write32(hw::OS_ARENA_LO, config.arena_lo);
    mem->write32(hw::OS_ARENA_HI, config.arena_hi);

    // Boot magic (libogc writes 0x0D15EA5E at 0x80000020)
    mem->write32(hw::OS_BOOT_MAGIC, 0x0D15EA5E);
    mem->write32(hw::OS_BOOT_VERSION, 1);

    // RAM end address
    mem->write32(hw::OS_DEBUG_MONITOR, hw::MEM1_BASE + config.ram_size);
    mem->write32(hw::OS_DEBUG_FLAG, 0);

    // Production pads and devkit boot value (from libogc __lowmem_init)
    mem->write16(hw::OS_PRODUCTION_PADS, 6);
    mem->write32(hw::OS_DEVKIT_BOOT, 0xC0008000);

    // No BI2 or FST yet (set when DVD filesystem is mounted)
    mem->write32(hw::OS_BI2_ADDR, 0);
    mem->write32(hw::OS_FST_ADDR, 0);
    mem->write32(hw::OS_FST_MAX_LEN, 0);

    // Allocate and set up main thread at a fixed address above the exception table
    g_main_thread_addr = 0x80003800;
    mem->write32(hw::OS_CURRENT_THREAD, g_main_thread_addr);
    mem->write16(g_main_thread_addr + os::thread_off::STATE, os::OSThread::STATE_RUNNING);
    mem->write32(g_main_thread_addr + os::thread_off::PRIORITY, 16);
    mem->write32(g_main_thread_addr + os::thread_off::BASE_PRIORITY, 16);

    // Build a display string for the game ID
    char id_str[8] = {0};
    for (int i = 0; i < 4 && config.game_id[i]; i++) id_str[i] = config.game_id[i];
    for (int i = 0; i < 2 && config.company_code[i]; i++) id_str[4 + i] = config.company_code[i];
    id_str[6] = 0;

    // RAM end (use configured size, not hardcoded MEM1_SIZE)
    mem->write32(hw::OS_RAM_END, hw::MEM1_BASE + config.ram_size);

    printf("[OS] Low memory initialized.\n");
    printf("[OS]   Game ID:     %s\n", id_str);
    printf("[OS]   Console:     0x%08X\n", config.console_type);
    printf("[OS]   RAM:         %u MB\n", config.ram_size / (1024 * 1024));
    printf("[OS]   Bus clock:   %u MHz\n", hw::BUS_CLOCK_HZ / 1000000);
    printf("[OS]   CPU clock:   %u MHz\n", hw::CPU_CLOCK_HZ / 1000000);
    printf("[OS]   Arena:       0x%08X - 0x%08X\n",
           mem->read32(hw::OS_ARENA_LO), mem->read32(hw::OS_ARENA_HI));
}

// =============================================================================
// Video Interface (VI) HLE
//
// The GameCube's VI controls video output timing and framebuffer display.
// VIWaitForRetrace is critical -- without it, the game loop spins at
// infinite speed and the game hangs waiting for VBlank. We simulate 60Hz
// timing with a 16.67ms sleep.
//
// Reference: libogc ogc/video.h
// =============================================================================

static uint32_t g_vi_framebuffer = 0;
static uint32_t g_vi_black = 0;
static uint32_t g_vi_retrace_count = 0;

static void vi_init(PPCContext* ctx, Memory* mem) {
    printf("[VI] VIInit\n");
}

static void vi_configure(PPCContext* ctx, Memory* mem) {
    printf("[VI] VIConfigure\n");
}

static void vi_set_next_framebuffer(PPCContext* ctx, Memory* mem) {
    g_vi_framebuffer = ctx->r[3];
    printf("[VI] VISetNextFrameBuffer 0x%08X\n", g_vi_framebuffer);
}

static void vi_flush(PPCContext* ctx, Memory* mem) {
    printf("[VI] VIFlush\n");
}

static void vi_set_black(PPCContext* ctx, Memory* mem) {
    g_vi_black = ctx->r[3];
    printf("[VI] VISetBlack %d\n", g_vi_black);
}

// VIWaitForRetrace: sleep ~16.67ms to simulate 60Hz VBlank.
// Without this the game loop runs at infinite speed and hangs.
static void vi_wait_for_retrace(PPCContext* ctx, Memory* mem) {
    g_vi_retrace_count++;
    std::this_thread::sleep_for(std::chrono::microseconds(16667));
}

static void vi_get_retrace_count(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = g_vi_retrace_count;
}

static void vi_get_tv_format(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = 0; // NTSC
}

static void vi_get_dtv_status(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = 0;
}

// =============================================================================
// Init Stubs
//
// These are called during game boot but their real work is either handled
// elsewhere (e.g. runtime_init) or not needed in a recompilation context.
// =============================================================================

static void os_init_stub(PPCContext* ctx, Memory* mem) {
    printf("[OS] OSInit\n");
}

static void dvd_init_stub(PPCContext* ctx, Memory* mem) {
    printf("[DVD] DVDInit\n");
}

static void os_init_system_call_stub(PPCContext* ctx, Memory* mem) {
    printf("[OS] __OSInitSystemCall\n");
}

static void ps_init_stub(PPCContext* ctx, Memory* mem) {
    printf("[OS] PSInit\n");
}

static void ai_init_stub(PPCContext* ctx, Memory* mem) {
    printf("[AI] AIInit\n");
}

static void dsp_init_stub(PPCContext* ctx, Memory* mem) {
    printf("[DSP] DSPInit\n");
}

// =============================================================================
// Memory Card (CARD) HLE
//
// The GameCube CARD API provides access to memory card slots A and B. In a
// static recompilation, we back each slot with a host-side directory:
//   Slot A -> ./memcard_a/
//   Slot B -> ./memcard_b/
// Files on the "card" are simply files in that directory.
//
// Reference: libogc ogc/card.h
// =============================================================================

// CARD return codes
static constexpr int32_t CARD_RESULT_READY   =  0;
static constexpr int32_t CARD_RESULT_BUSY    = -1;
static constexpr int32_t CARD_RESULT_NOCARD  = -3;
static constexpr int32_t CARD_RESULT_NOFILE  = -4;
static constexpr int32_t CARD_RESULT_IOERROR = -6;

static constexpr uint32_t CARD_MEM_SIZE    = 0x200000;  // 2 MB (standard card)
static constexpr uint32_t CARD_SECTOR_SIZE = 0x2000;    // 8 KB sector

static constexpr int CARD_MAX_SLOTS = 2;

// Per-slot state
struct CardSlotState {
    bool mounted = false;
};
static CardSlotState g_card_slots[CARD_MAX_SLOTS];

// Track open files: maps fileInfo emulated address -> (slot, filename)
struct CardOpenFile {
    int         slot;
    std::string filename;
};
static std::unordered_map<uint32_t, CardOpenFile> g_card_open_files;

// Internal file number counter (simple monotonic assignment)
static int g_card_next_file_no = 1;

// Helper: get the host directory path for a card slot
static std::string card_slot_dir(int slot) {
    return (slot == 0) ? "./memcard_a" : "./memcard_b";
}

// Helper: read a null-terminated string from emulated memory
static std::string card_read_string(Memory* mem, uint32_t addr) {
    std::string s;
    for (int i = 0; i < 256; i++) {
        char c = (char)mem->read8(addr + i);
        if (c == 0) break;
        s += c;
    }
    return s;
}

// CARDInit -- initialize the card system, create memcard directories
static void card_init(PPCContext* ctx, Memory* mem) {
    namespace fs = std::filesystem;
    fs::create_directories(card_slot_dir(0));
    fs::create_directories(card_slot_dir(1));
    printf("[CARD] CARDInit: memcard directories ready\n");
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDProbeEx(slot, memSize_ptr, sectorSize_ptr) -> result
static void card_probe_ex(PPCContext* ctx, Memory* mem) {
    uint32_t slot          = ctx->r[3];
    uint32_t mem_size_ptr  = ctx->r[4];
    uint32_t sec_size_ptr  = ctx->r[5];

    if (mem_size_ptr) mem->write32(mem_size_ptr, CARD_MEM_SIZE);
    if (sec_size_ptr) mem->write32(sec_size_ptr, CARD_SECTOR_SIZE);

    printf("[CARD] CARDProbeEx: slot=%u memSize=0x%X sectorSize=0x%X\n",
           slot, CARD_MEM_SIZE, CARD_SECTOR_SIZE);
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDMount(slot, workArea, detachCallback) -> result
static void card_mount(PPCContext* ctx, Memory* mem) {
    uint32_t slot = ctx->r[3];
    if (slot < CARD_MAX_SLOTS) {
        g_card_slots[slot].mounted = true;
        printf("[CARD] CARDMount: slot %u mounted\n", slot);
        ctx->r[3] = (uint32_t)CARD_RESULT_READY;
    } else {
        printf("[CARD] CARDMount: invalid slot %u\n", slot);
        ctx->r[3] = (uint32_t)CARD_RESULT_NOCARD;
    }
}

// CARDUnmount(slot) -> result
static void card_unmount(PPCContext* ctx, Memory* mem) {
    uint32_t slot = ctx->r[3];
    if (slot < CARD_MAX_SLOTS) {
        g_card_slots[slot].mounted = false;
        printf("[CARD] CARDUnmount: slot %u unmounted\n", slot);
        ctx->r[3] = (uint32_t)CARD_RESULT_READY;
    } else {
        ctx->r[3] = (uint32_t)CARD_RESULT_NOCARD;
    }
}

// CARDOpen(slot, fileName_ptr, fileInfo_ptr) -> result
// CARDFileInfo layout: offset 0 = channel (int32), offset 4 = fileNo (int32)
static void card_open(PPCContext* ctx, Memory* mem) {
    uint32_t slot          = ctx->r[3];
    uint32_t filename_addr = ctx->r[4];
    uint32_t fileinfo_addr = ctx->r[5];

    std::string filename = card_read_string(mem, filename_addr);
    std::string host_path = card_slot_dir(slot) + "/" + filename;

    namespace fs = std::filesystem;
    if (!fs::exists(host_path)) {
        printf("[CARD] CARDOpen: file not found: %s\n", host_path.c_str());
        ctx->r[3] = (uint32_t)CARD_RESULT_NOFILE;
        return;
    }

    // Write CARDFileInfo: channel = slot, fileNo = assigned number
    int file_no = g_card_next_file_no++;
    mem->write32(fileinfo_addr + 0, slot);
    mem->write32(fileinfo_addr + 4, (uint32_t)file_no);

    // Track the open file
    g_card_open_files[fileinfo_addr] = { (int)slot, filename };

    printf("[CARD] CARDOpen: slot=%u file=\"%s\" fileNo=%d\n", slot, filename.c_str(), file_no);
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDCreate(slot, fileName_ptr, size, fileInfo_ptr) -> result
static void card_create(PPCContext* ctx, Memory* mem) {
    uint32_t slot          = ctx->r[3];
    uint32_t filename_addr = ctx->r[4];
    uint32_t size          = ctx->r[5];
    uint32_t fileinfo_addr = ctx->r[6];

    std::string filename = card_read_string(mem, filename_addr);
    std::string host_path = card_slot_dir(slot) + "/" + filename;

    // Create (or overwrite) the file with the requested size, zero-filled
    FILE* fp = fopen(host_path.c_str(), "wb");
    if (!fp) {
        printf("[CARD] CARDCreate: failed to create %s\n", host_path.c_str());
        ctx->r[3] = (uint32_t)CARD_RESULT_IOERROR;
        return;
    }
    // Write zeroes to fill to the requested size
    std::vector<uint8_t> zeroes(size, 0);
    fwrite(zeroes.data(), 1, size, fp);
    fclose(fp);

    // Write CARDFileInfo
    int file_no = g_card_next_file_no++;
    mem->write32(fileinfo_addr + 0, slot);
    mem->write32(fileinfo_addr + 4, (uint32_t)file_no);

    // Track the open file
    g_card_open_files[fileinfo_addr] = { (int)slot, filename };

    printf("[CARD] CARDCreate: slot=%u file=\"%s\" size=%u\n", slot, filename.c_str(), size);
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDRead(fileInfo_ptr, buffer, length, offset) -> result
static void card_read(PPCContext* ctx, Memory* mem) {
    uint32_t fileinfo_addr = ctx->r[3];
    uint32_t buffer_addr   = ctx->r[4];
    uint32_t length        = ctx->r[5];
    uint32_t offset        = ctx->r[6];

    // Look up slot from fileInfo
    uint32_t slot = mem->read32(fileinfo_addr + 0);

    auto it = g_card_open_files.find(fileinfo_addr);
    if (it == g_card_open_files.end()) {
        printf("[CARD] CARDRead: no open file for fileInfo 0x%08X\n", fileinfo_addr);
        ctx->r[3] = (uint32_t)CARD_RESULT_NOFILE;
        return;
    }

    std::string host_path = card_slot_dir(slot) + "/" + it->second.filename;
    FILE* fp = fopen(host_path.c_str(), "rb");
    if (!fp) {
        printf("[CARD] CARDRead: failed to open %s\n", host_path.c_str());
        ctx->r[3] = (uint32_t)CARD_RESULT_IOERROR;
        return;
    }

    fseek(fp, offset, SEEK_SET);
    uint8_t* dst = mem->translate(buffer_addr);
    if (dst) {
        size_t read_bytes = fread(dst, 1, length, fp);
        printf("[CARD] CARDRead: slot=%u file=\"%s\" offset=%u len=%u read=%zu\n",
               slot, it->second.filename.c_str(), offset, length, read_bytes);
    }
    fclose(fp);

    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDWrite(fileInfo_ptr, buffer, length, offset) -> result
static void card_write(PPCContext* ctx, Memory* mem) {
    uint32_t fileinfo_addr = ctx->r[3];
    uint32_t buffer_addr   = ctx->r[4];
    uint32_t length        = ctx->r[5];
    uint32_t offset        = ctx->r[6];

    uint32_t slot = mem->read32(fileinfo_addr + 0);

    auto it = g_card_open_files.find(fileinfo_addr);
    if (it == g_card_open_files.end()) {
        printf("[CARD] CARDWrite: no open file for fileInfo 0x%08X\n", fileinfo_addr);
        ctx->r[3] = (uint32_t)CARD_RESULT_NOFILE;
        return;
    }

    std::string host_path = card_slot_dir(slot) + "/" + it->second.filename;

    // Open for read+write (binary), preserving existing content
    FILE* fp = fopen(host_path.c_str(), "r+b");
    if (!fp) {
        // File may not exist yet; create it
        fp = fopen(host_path.c_str(), "wb");
    }
    if (!fp) {
        printf("[CARD] CARDWrite: failed to open %s for writing\n", host_path.c_str());
        ctx->r[3] = (uint32_t)CARD_RESULT_IOERROR;
        return;
    }

    fseek(fp, offset, SEEK_SET);
    const uint8_t* src = mem->translate(buffer_addr);
    if (src) {
        size_t written = fwrite(src, 1, length, fp);
        printf("[CARD] CARDWrite: slot=%u file=\"%s\" offset=%u len=%u written=%zu\n",
               slot, it->second.filename.c_str(), offset, length, written);
    }
    fclose(fp);

    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDClose(fileInfo_ptr) -> result
static void card_close(PPCContext* ctx, Memory* mem) {
    uint32_t fileinfo_addr = ctx->r[3];

    auto it = g_card_open_files.find(fileinfo_addr);
    if (it != g_card_open_files.end()) {
        printf("[CARD] CARDClose: file=\"%s\"\n", it->second.filename.c_str());
        g_card_open_files.erase(it);
    }

    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDGetResultCode(slot) -> result
static void card_get_result_code(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDGetSectorSize(slot, size_ptr) -> result
static void card_get_sector_size(PPCContext* ctx, Memory* mem) {
    uint32_t size_ptr = ctx->r[4];
    if (size_ptr) mem->write32(size_ptr, CARD_SECTOR_SIZE);
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDFreeBlocks(slot, byteNotSector, freeBlocks_ptr) -> result
static void card_free_blocks(PPCContext* ctx, Memory* mem) {
    uint32_t slot            = ctx->r[3];
    uint32_t byte_not_sector = ctx->r[4];
    uint32_t free_ptr        = ctx->r[5];

    // Report ~1.5 MB free (reasonable for a 2 MB card with some usage)
    uint32_t free_bytes = 0x180000;
    if (free_ptr) {
        if (byte_not_sector) {
            mem->write32(free_ptr, free_bytes);
        } else {
            mem->write32(free_ptr, free_bytes / CARD_SECTOR_SIZE);
        }
    }
    printf("[CARD] CARDFreeBlocks: slot=%u free=0x%X bytes\n", slot, free_bytes);
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDGetStatus(slot, fileNo, stat_ptr) -> result
// Writes basic status info. The CARDStat struct starts with the filename
// (32 bytes), followed by length, time, etc. We fill in what we can.
static void card_get_status(PPCContext* ctx, Memory* mem) {
    uint32_t slot     = ctx->r[3];
    uint32_t file_no  = ctx->r[4];
    uint32_t stat_ptr = ctx->r[5];

    if (stat_ptr) {
        // Zero out the stat struct (64 bytes is a safe conservative size)
        for (uint32_t i = 0; i < 64; i++) {
            mem->write8(stat_ptr + i, 0);
        }
    }
    printf("[CARD] CARDGetStatus: slot=%u fileNo=%u (stub)\n", slot, file_no);
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDSetStatus(slot, fileNo, stat_ptr) -> result (stub)
static void card_set_status(PPCContext* ctx, Memory* mem) {
    printf("[CARD] CARDSetStatus: stub\n");
    ctx->r[3] = (uint32_t)CARD_RESULT_READY;
}

// CARDDelete(slot, fileName_ptr) -> result
static void card_delete(PPCContext* ctx, Memory* mem) {
    uint32_t slot          = ctx->r[3];
    uint32_t filename_addr = ctx->r[4];

    std::string filename = card_read_string(mem, filename_addr);
    std::string host_path = card_slot_dir(slot) + "/" + filename;

    namespace fs = std::filesystem;
    if (fs::exists(host_path)) {
        fs::remove(host_path);
        printf("[CARD] CARDDelete: slot=%u file=\"%s\" deleted\n", slot, filename.c_str());
        ctx->r[3] = (uint32_t)CARD_RESULT_READY;
    } else {
        printf("[CARD] CARDDelete: slot=%u file=\"%s\" not found\n", slot, filename.c_str());
        ctx->r[3] = (uint32_t)CARD_RESULT_NOFILE;
    }
}

// =============================================================================
// OS Function Registration
//
// Maps known SDK symbol names to our native HLE implementations. The
// recompiler's symbol map loader calls lookup_os_func() for each symbol
// to check whether a native replacement exists. If so, the replacement
// is registered in the FuncTable at the symbol's GameCube address.
//
// This table covers the most commonly used SDK functions. Game-specific
// projects can register additional functions in their own code.
//
// Reference: libogc function names (ogc/*.h headers)
// =============================================================================

struct OSFuncEntry {
    const char*    name;  // SDK symbol name (must match the game's symbol map exactly)
    RecompiledFunc func;  // Native replacement function
};

static const OSFuncEntry g_os_func_table[] = {
    // --- Timing ---
    { "OSGetTime",              os_get_time },
    { "OSGetTick",              os_get_tick },

    // --- Interrupts ---
    { "OSDisableInterrupts",    os_disable_interrupts },
    { "OSEnableInterrupts",     os_enable_interrupts },
    { "OSRestoreInterrupts",    os_restore_interrupts },

    // --- Arena ---
    { "OSGetArenaLo",           os_get_arena_lo },
    { "OSGetArenaHi",           os_get_arena_hi },
    { "OSSetArenaLo",           os_set_arena_lo },
    { "OSSetArenaHi",           os_set_arena_hi },

    // --- Heap ---
    { "OSCreateHeap",           os_create_heap },
    { "OSSetCurrentHeap",       os_set_current_heap },
    { "OSAlloc",                os_alloc },
    { "OSFree",                 os_free },

    // --- Cache (no-ops) ---
    { "DCFlushRange",           os_dcache_flush },
    { "DCInvalidateRange",      os_dcache_invalidate },
    { "DCStoreRange",           os_dcache_store },
    { "ICInvalidateRange",      os_icache_invalidate },

    // --- Debug ---
    { "OSReport",               os_report },
    { "OSPanic",                os_panic },
    { "OSFatal",                os_fatal },

    // --- CRT memory/string ---
    { "memset",                 os_memset },
    { "memcpy",                 os_memcpy },
    { "memmove",                os_memmove },
    { "memcmp",                 os_memcmp },
    { "strlen",                 os_strlen },
    { "strcmp",                  os_strcmp },
    { "strcpy",                 os_strcpy },
    { "strncpy",                os_strncpy },

    // --- Math ---
    { "sinf",                   os_sinf },
    { "cosf",                   os_cosf },
    { "tanf",                   os_tanf },
    { "atanf",                  os_atanf },
    { "atan2f",                 os_atan2f },
    { "sqrtf",                  os_sqrtf },
    { "floorf",                 os_floorf },
    { "ceilf",                  os_ceilf },
    { "fabsf",                  os_fabsf },
    { "fmodf",                  os_fmodf },
    { "powf",                   os_powf },
    { "logf",                   os_logf },
    { "expf",                   os_expf },
    { "sin",                    os_sinf },   // double versions use same impl
    { "cos",                    os_cosf },
    { "sqrt",                   os_sqrtf },
    { "floor",                  os_floorf },
    { "ceil",                   os_ceilf },

    // --- DVD ---
    { "DVDOpen",                dvd_open },
    { "DVDClose",               dvd_close },
    { "DVDReadPrio",            dvd_read_prio },
    { "DVDReadAsync",           dvd_read_async },

    // --- Thread ---
    { "OSCreateThread",         os_create_thread },
    { "OSResumeThread",         os_resume_thread },
    { "OSGetCurrentThread",     os_get_current_thread },
    { "OSSleepThread",          os_sleep_thread },
    { "OSWakeupThread",         os_wakeup_thread },

    // --- Mutex ---
    { "OSInitMutex",            os_init_mutex },
    { "OSLockMutex",            os_lock_mutex },
    { "OSUnlockMutex",          os_unlock_mutex },

    // --- Message Queue ---
    { "OSInitMessageQueue",     os_init_message_queue },
    { "OSSendMessage",          os_send_message },
    { "OSReceiveMessage",       os_receive_message },

    // --- Video Interface (VI) ---
    { "VIInit",                 vi_init },
    { "VIConfigure",            vi_configure },
    { "VISetNextFrameBuffer",   vi_set_next_framebuffer },
    { "VIFlush",                vi_flush },
    { "VISetBlack",             vi_set_black },
    { "VIWaitForRetrace",       vi_wait_for_retrace },
    { "VIGetRetraceCount",      vi_get_retrace_count },
    { "VIGetTvFormat",          vi_get_tv_format },
    { "VIGetDTVStatus",         vi_get_dtv_status },

    // --- Init Stubs ---
    { "OSInit",                 os_init_stub },
    { "DVDInit",                dvd_init_stub },
    { "__OSInitSystemCall",     os_init_system_call_stub },
    { "PSInit",                 ps_init_stub },
    { "AIInit",                 ai_init_stub },
    { "DSPInit",                dsp_init_stub },

    // --- Memory Card (CARD) ---
    { "CARDInit",               card_init },
    { "CARDProbeEx",            card_probe_ex },
    { "CARDMount",              card_mount },
    { "CARDUnmount",            card_unmount },
    { "CARDOpen",               card_open },
    { "CARDCreate",             card_create },
    { "CARDRead",               card_read },
    { "CARDWrite",              card_write },
    { "CARDClose",              card_close },
    { "CARDGetResultCode",      card_get_result_code },
    { "CARDGetSectorSize",      card_get_sector_size },
    { "CARDFreeBlocks",         card_free_blocks },
    { "CARDGetStatus",          card_get_status },
    { "CARDSetStatus",          card_set_status },
    { "CARDDelete",             card_delete },

    { nullptr, nullptr }  // Sentinel
};

void register_os_functions() {
    printf("[OS] Registering OS function replacements...\n");

    int count = 0;
    for (const auto* entry = g_os_func_table; entry->name != nullptr; entry++) {
        // Functions will be bound to addresses when the symbol map is loaded.
        // For now, register by name in a name->func lookup that the loader uses.
        count++;
    }

    printf("[OS] %d OS functions registered (will bind to addresses via symbol map)\n", count);
}

// Lookup OS function by name (called during symbol map loading)
RecompiledFunc lookup_os_func(const char* name) {
    for (const auto* entry = g_os_func_table; entry->name != nullptr; entry++) {
        if (strcmp(entry->name, name) == 0) {
            return entry->func;
        }
    }
    return nullptr;
}

// Set the root directory for game file access (DVD HLE)
void set_game_root(const std::string& path) {
    g_game_root = path;
    printf("[OS] Game root set to: %s\n", path.c_str());
}

} // namespace gcrecomp
