// SPDX-License-Identifier: MIT
// The AI replacements as the recompiled SDK code reaches them: guest registers in, guest memory
// read, the game's DMA callback run through the function table. No display, no disc, no device.
#include <chrono>
#include <cstdio>
#include <thread>

#include "gcrecomp/audio/audio_sink.h"
#include "gcrecomp/runtime.h"

using namespace gcrecomp;

namespace gcrecomp {
RecompiledFunc lookup_ai_func(const char* name);
}

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static void call(const char* name, uint32_t r3 = 0, uint32_t r4 = 0) {
    RecompiledFunc f = lookup_ai_func(name);
    CHECK(f != nullptr);
    if (!f) return;
    g_ctx.r[3] = r3;
    g_ctx.r[4] = r4;
    f(&g_ctx, &g_mem);
}

constexpr uint32_t kCallback = 0x80003000;
constexpr uint32_t kBufferA = 0x80100000;
constexpr uint32_t kBufferB = 0x80100400;  // 1 KB further
static unsigned g_callback_runs = 0;

// What a game's AI callback does: program the other buffer, and use registers freely.
static void guest_callback(PPCContext* ctx, Memory*) {
    ++g_callback_runs;
    const uint32_t next = (g_callback_runs % 2) ? kBufferB : kBufferA;
    ctx->r[3] = next;
    ctx->r[4] = 640;
    lookup_ai_func("AIInitDMA")(ctx, &g_mem);
    ctx->r[3] = 0xDEAD;  // must not leak into the interrupted code
    ctx->r[31] = 0xBEEF;
}

static void fill(uint32_t address, int16_t left, int16_t right) {
    for (uint32_t i = 0; i < 160; ++i) {
        g_mem.write16(address + i * 4, uint16_t(right));  // the AI reads the right word first
        g_mem.write16(address + i * 4 + 2, uint16_t(left));
    }
}

int main() {
    if (!runtime_init()) {
        std::printf("runtime_init failed\n");
        return 1;
    }
    g_func_table.register_func(kCallback, guest_callback);
    fill(kBufferA, 1000, -1000);
    fill(kBufferB, 2000, -2000);

    audio::NullSink sink;
    sink.open(32000);
    ai_hle_reset(&sink);

    call("AIInit");
    call("AIRegisterDMACallback", kCallback);
    CHECK(g_ctx.r[3] == 0);  // no previous callback
    call("AIRegisterDMACallback", kCallback);
    CHECK(g_ctx.r[3] == kCallback);  // returns the previous one
    call("AISetDSPSampleRate", 0);
    call("AIInitDMA", kBufferA, 640);  // as the game passes it: a cached virtual address

    // The registers the SDK's own readers use follow.
    CHECK(g_mem.read16(0xCC005030) == 0x0010);  // 0x00100000 >> 16
    CHECK(g_mem.read16(0xCC005032) == 0x0000);
    CHECK(g_mem.read16(0xCC005036) == 20);      // 640 / 32, DMA not enabled yet
    call("AIGetDMALength");
    CHECK(g_ctx.r[3] == 640);

    // Starting the DMA plays the first block at once and the callback queues the next.
    g_ctx.r[31] = 0x1234;
    call("AIStartDMA");
    CHECK(g_mem.read16(0xCC005036) == (0x8000 | 20));
    CHECK(g_ctx.r[31] == 0x1234);  // callback's register changes are undone
    CHECK(g_callback_runs >= 2);
    CHECK(sink.recorded().size() >= 4 * 160 * 2);
    // Big-endian guest samples arrive as host int16, block A first, then B, then A ...
    CHECK(sink.recorded()[0] == 1000);
    CHECK(sink.recorded()[1] == -1000);
    CHECK(sink.recorded()[160 * 2] == 2000);
    CHECK(sink.recorded()[160 * 2 + 1] == -2000);
    CHECK(sink.recorded()[2 * 160 * 2] == 1000);

    // The game runs on; the DMA continues when the sink has played something and the game next
    // re-enables interrupts, reads the clock, ...
    const unsigned runs = g_callback_runs;
    sink.consume(sink.queued_frames());
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    ai_poll(&g_ctx, &g_mem);
    CHECK(g_callback_runs > runs);

    // Stopping ends it.
    call("AIStopDMA");
    CHECK(g_mem.read16(0xCC005036) == 20);
    const unsigned after_stop = g_callback_runs;
    sink.consume(sink.queued_frames());
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    ai_poll(&g_ctx, &g_mem);
    CHECK(g_callback_runs == after_stop);

    // 48 kHz is rate id 1.
    call("AISetDSPSampleRate", 1);
    call("AIGetDSPSampleRate");
    CHECK(g_ctx.r[3] == 1);
    CHECK(sink.sample_rate() == 48000);

    ai_hle_reset(nullptr);
    runtime_shutdown();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all AI HLE tests passed\n");
    return 0;
}
