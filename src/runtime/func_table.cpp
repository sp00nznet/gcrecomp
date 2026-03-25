// =============================================================================
// Function Table - Maps GameCube addresses to native recompiled functions
//
// This is the dispatch backbone of the static recompilation runtime. When the
// original game code performs an indirect call (function pointer, C++ virtual
// method, or switch-table jump), the recompiled code looks up the target
// GameCube address in this table to find the corresponding native function.
//
// The table is populated at startup in two phases:
//   1. The DOL loader registers every recompiled game function.
//   2. register_os_functions() and init_low_memory() set up OS HLE stubs
//      so that SDK calls are handled natively instead of being recompiled.
//
// Unresolved calls are logged with the target address and current register
// state to aid debugging.
//
// References:
//   - GameCubeRecompiled (CC0): Static recompilation dispatch architecture
//   - libogc: SDK function names used for OS HLE binding
// =============================================================================

#include "gcrecomp/runtime.h"
#include <cstdio>

namespace gcrecomp {

FuncTable g_func_table;

void FuncTable::register_func(uint32_t gc_addr, RecompiledFunc func) {
    table[gc_addr] = func;
}

RecompiledFunc FuncTable::lookup(uint32_t gc_addr) const {
    auto it = table.find(gc_addr);
    if (it != table.end()) return it->second;
    return nullptr;
}

void FuncTable::call(uint32_t gc_addr, PPCContext* ctx, Memory* mem) const {
    if (gc_addr == 0) return; // Null function pointer — skip silently

    // Basic sanity check: must be in GameCube RAM range and 4-byte aligned.
    // Valid code addresses are 0x80000000 - 0x82FFFFFF (up to 48 MB for Triforce).
    if (gc_addr < 0x80000000 || gc_addr >= 0x83000000 || (gc_addr & 3) != 0) {
        return; // Not a valid code address — skip silently
    }

    RecompiledFunc func = lookup(gc_addr);
    if (func) {
        // Trace indirect calls to understand execution flow
        static int indirect_count = 0;
        indirect_count++;
        if (indirect_count <= 20 || (indirect_count % 10000 == 0)) {
            printf("[Trace] Indirect call #%d -> 0x%08X (LR=0x%08X)\n",
                   indirect_count, gc_addr, ctx->lr);
            fflush(stdout);
        }
        func(ctx, mem);
    } else {
        // Rate-limit unresolved call warnings
        static int unresolved_count = 0;
        static std::unordered_map<uint32_t, int> seen;
        unresolved_count++;
        int& addr_count = seen[gc_addr];
        addr_count++;
        if (addr_count <= 3) {
            printf("[FuncTable] Unresolved call to 0x%08X (LR=0x%08X)\n",
                    gc_addr, ctx->lr);
            fflush(stdout);
        }
        if (unresolved_count == 50 || unresolved_count == 200 ||
            unresolved_count == 1000 || (unresolved_count % 5000 == 0)) {
            printf("[FuncTable] %d total unresolved calls (%zu unique addresses)\n",
                    unresolved_count, seen.size());
            fflush(stdout);
        }
    }
}

bool runtime_init() {
    printf("[Runtime] Initializing...\n");

    // Initialize memory
    if (!g_mem.init()) return false;

    // Initialize CPU context
    g_ctx.reset();

    // Set initial stack pointer (top of main RAM minus space for OS data)
    g_ctx.r[1] = Memory::MAIN_RAM_BASE + g_mem.ram_size - 0x100;
    // r2 = small data area (SDA2) base -- will be set by DOL loader
    // r13 = small data area (SDA) base -- will be set by DOL loader

    // Note: init_low_memory() and register_os_functions() are NOT called here
    // automatically because init_low_memory() requires a GameConfig. The
    // game-specific project must call them explicitly after constructing
    // an appropriate GameConfig:
    //
    //   gcrecomp::GameConfig config;
    //   // ... fill in config ...
    //   gcrecomp::init_low_memory(&gcrecomp::g_mem, config);
    //   gcrecomp::register_os_functions();

    printf("[Runtime] Ready. Stack at 0x%08X\n", g_ctx.r[1]);
    return true;
}

void runtime_shutdown() {
    g_mem.shutdown();
    printf("[Runtime] Shutdown complete.\n");
}

} // namespace gcrecomp
