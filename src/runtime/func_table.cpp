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
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <atomic>

namespace gcrecomp {

FuncTable g_func_table;

// =============================================================================
// Tracing — see runtime.h. The ring is always allocated; the inline
// trace_enter only writes when g_trace_enabled is set, so the cost when
// disabled is one relaxed atomic load per recompiled function entry.
// =============================================================================
TraceRing            g_trace_ring;
std::atomic<bool>    g_trace_enabled{false};

void trace_enter(uint32_t addr) {
    if (g_trace_enabled.load(std::memory_order_relaxed)) {
        uint64_t p = g_trace_ring.pos.fetch_add(1, std::memory_order_relaxed);
        g_trace_ring.entries[p & (TraceRing::SIZE - 1)] = addr;
    }
}

void trace_set_enabled(bool on) {
    g_trace_enabled.store(on, std::memory_order_release);
    printf("[Trace] %s — recompiled function entries %s recorded\n",
           on ? "ENABLED" : "DISABLED",
           on ? "now being" : "no longer");
    fflush(stdout);
}

void trace_dump_recent(size_t n) {
    uint64_t pos = g_trace_ring.pos.load(std::memory_order_acquire);
    if (pos == 0) {
        printf("[Trace] ring empty\n");
        fflush(stdout);
        return;
    }
    size_t count = std::min<size_t>(n, std::min<uint64_t>(pos, TraceRing::SIZE));
    printf("[Trace] last %zu function entries (oldest -> newest, total seen %llu):\n",
           count, (unsigned long long)pos);
    for (size_t i = count; i > 0; --i) {
        uint64_t idx = (pos - i) & (TraceRing::SIZE - 1);
        printf("  %5zu: func_%08X\n", count - i, g_trace_ring.entries[idx]);
    }
    fflush(stdout);
}

void trace_dump_summary() {
    uint64_t pos = g_trace_ring.pos.load(std::memory_order_acquire);
    if (pos == 0) {
        printf("[Trace] ring empty\n");
        fflush(stdout);
        return;
    }
    size_t count = std::min<uint64_t>(pos, TraceRing::SIZE);
    std::unordered_map<uint32_t, uint32_t> hist;
    for (size_t i = 0; i < count; ++i) hist[g_trace_ring.entries[i]]++;

    std::vector<std::pair<uint32_t, uint32_t>> ordered(hist.begin(), hist.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    printf("[Trace] histogram of %zu most-recent entries (%zu unique funcs, total %llu seen):\n",
           count, ordered.size(), (unsigned long long)pos);
    size_t top = std::min<size_t>(ordered.size(), 20);
    for (size_t i = 0; i < top; ++i) {
        printf("  %6u  func_%08X\n", ordered[i].second, ordered[i].first);
    }
    fflush(stdout);
}

void FuncTable::register_func(uint32_t gc_addr, RecompiledFunc func) {
    table[gc_addr] = func;
}

RecompiledFunc FuncTable::lookup(uint32_t gc_addr) const {
    auto it = table.find(gc_addr);
    if (it != table.end()) return it->second;
    return nullptr;
}

// Last-dispatch probe (see last_dispatch() in runtime.h).
static std::atomic<uint64_t> g_dispatch_seq{0};
static std::atomic<uint32_t> g_dispatch_addr{0};
static std::atomic<uint32_t> g_dispatch_lr{0};

uint64_t last_dispatch(uint32_t* addr, uint32_t* lr) {
    if (addr) *addr = g_dispatch_addr.load(std::memory_order_relaxed);
    if (lr)   *lr   = g_dispatch_lr.load(std::memory_order_relaxed);
    return g_dispatch_seq.load(std::memory_order_relaxed);
}

static const auto g_debug_start = std::chrono::steady_clock::now();

bool debug_armed() {
    static const double after = [] {
        const char* v = std::getenv("GCRECOMP_DEBUG_AFTER");
        return v && *v ? std::atof(v) : 0.0;
    }();
    return after <= 0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - g_debug_start).count() >= after;
}

// GCRECOMP_TRACE_CALLS=LR[,LR...] (hex link-register values, e.g. the address after a `bl` thunk that jumps through
// a pointer): prints the target of every indirect call made with one of them and r3-r6 (GCRECOMP_TRACE_CALLS_MAX
// lines, default 20; GCRECOMP_DEBUG_AFTER=S skips the first S seconds). Finds out which method a vtable or callback
// pointer resolves to at run time.
static void trace_call(uint32_t gc_addr, const PPCContext* ctx) {
    static const std::vector<uint32_t> lrs = [] {
        std::vector<uint32_t> v;
        const char* e = std::getenv("GCRECOMP_TRACE_CALLS");
        while (e && *e) {
            char* end = nullptr;
            const unsigned long x = std::strtoul(e, &end, 16);
            if (end == e) break;
            v.push_back(static_cast<uint32_t>(x));
            e = *end == ',' ? end + 1 : end;
            if (*end != ',') break;
        }
        return v;
    }();
    if (lrs.empty() || std::find(lrs.begin(), lrs.end(), ctx->lr) == lrs.end() || !debug_armed()) return;
    static const unsigned long limit = [] {
        const char* v = std::getenv("GCRECOMP_TRACE_CALLS_MAX");
        return v && *v ? std::strtoul(v, nullptr, 0) : 20ul;
    }();
    static unsigned long printed = 0;
    if (printed++ >= limit) return;
    std::fprintf(stderr, "[calls] LR=0x%08X -> 0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n", ctx->lr, gc_addr, ctx->r[3],
                 ctx->r[4], ctx->r[5], ctx->r[6]);
}

// GCRECOMP_TRACE_ARGS=ADDR:SPEC[,SPEC...][;ADDR:SPEC...]: when a function is entered through the function table
// (callbacks, vtable and pointer calls, the AI DMA callback ...), print guest values. ADDR is the hex entry address;
// SPEC is rN (a register) or rN[off] (the 32-bit word at rN + off, decimal or 0x hex, may be negative: an SDA
// global is r13[-30980]). GCRECOMP_TRACE_ARGS_EVERY=N prints every Nth call (default 1), GCRECOMP_TRACE_ARGS_MAX=N
// caps the lines (default 50), GCRECOMP_DEBUG_AFTER=S skips the first S seconds.
static void trace_args(uint32_t gc_addr, const PPCContext* ctx, const Memory* mem) {
    struct Spec { unsigned reg; bool deref; int32_t off; };
    struct Entry { uint32_t addr; std::vector<Spec> specs; };
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> v;
        const char* e = std::getenv("GCRECOMP_TRACE_ARGS");
        while (e && *e) {
            char* end = nullptr;
            Entry en;
            en.addr = static_cast<uint32_t>(std::strtoul(e, &end, 16));
            if (end == e || *end != ':') break;
            e = end + 1;
            while (*e == 'r') {
                Spec s{static_cast<unsigned>(std::strtoul(e + 1, &end, 10)), false, 0};
                e = end;
                if (*e == '[') {
                    s.deref = true;
                    s.off = static_cast<int32_t>(std::strtol(e + 1, &end, 0));
                    e = *end == ']' ? end + 1 : end;
                }
                if (s.reg < 32) en.specs.push_back(s);
                if (*e == ',') ++e;
            }
            v.push_back(en);
            if (*e == ';') ++e; else break;
        }
        return v;
    }();
    if (entries.empty()) return;
    for (const Entry& en : entries) {
        if (en.addr != gc_addr || !debug_armed()) continue;
        static const unsigned long every = [] {
            const char* v = std::getenv("GCRECOMP_TRACE_ARGS_EVERY");
            return v && *v ? std::max(1ul, std::strtoul(v, nullptr, 0)) : 1ul;
        }();
        static const unsigned long limit = [] {
            const char* v = std::getenv("GCRECOMP_TRACE_ARGS_MAX");
            return v && *v ? std::strtoul(v, nullptr, 0) : 50ul;
        }();
        static unsigned long calls = 0, printed = 0;
        if (calls++ % every != 0 || printed >= limit) return;
        ++printed;
        std::fprintf(stderr, "[args] 0x%08X LR=0x%08X", gc_addr, ctx->lr);
        for (const Spec& s : en.specs) {
            if (s.deref) std::fprintf(stderr, " r%u[%d]=0x%08X", s.reg, s.off, mem->read32(ctx->r[s.reg] + static_cast<uint32_t>(s.off)));
            else std::fprintf(stderr, " r%u=0x%08X", s.reg, ctx->r[s.reg]);
        }
        std::fprintf(stderr, "\n");
        return;
    }
}

void FuncTable::call(uint32_t gc_addr, PPCContext* ctx, Memory* mem) const {
    if (gc_addr == 0) return; // Null function pointer — skip silently

    g_dispatch_addr.store(gc_addr, std::memory_order_relaxed);
    g_dispatch_lr.store(ctx->lr, std::memory_order_relaxed);
    g_dispatch_seq.fetch_add(1, std::memory_order_relaxed);
    trace_call(gc_addr, ctx);
    trace_args(gc_addr, ctx, mem);

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
