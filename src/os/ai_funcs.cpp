// SPDX-License-Identifier: MIT
//
// AI (audio interface) HLE.
//
// The SDK's AI code programs three DMA registers and waits for an interrupt that never comes on
// the host. These replacements hand the DMA blocks to gcrecomp::audio::AudioInterface, which plays
// them through an audio sink (SDL3, or a null sink) and calls the game's DMA callback, as the AI
// interrupt handler would, each time it starts a block.
//
// Interrupts do not preempt recompiled code here, so the callback runs at the points where the
// game could have been interrupted: when it re-enables interrupts, waits for a retrace or reads
// the clock (see ai_poll). GCRECOMP_AUDIO=null selects the null sink,
// GCRECOMP_DUMP_AUDIO=file.wav dumps the output to a WAV file. GCRECOMP_AUDIO_TRACE=1 prints, once a
// second, how the game feeds the DMA: blocks started (and how many were all zero), underruns (the sink
// ran dry), the longest gap between two polls and the DMA block size and rate.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "gcrecomp/audio/audio_interface.h"
#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"

namespace gcrecomp {

namespace {

constexpr uint32_t kRegDmaAddrHigh = 0xCC005030;
constexpr uint32_t kRegDmaAddrLow = 0xCC005032;
constexpr uint32_t kRegDmaLength = 0xCC005036;
constexpr uint16_t kDmaEnableBit = 0x8000;

struct AiState {
    std::unique_ptr<audio::AudioSink> owned_sink;  // the sink ensure_ai creates, unless one was given
    audio::AudioSink* sink = nullptr;
    std::unique_ptr<audio::AudioInterface> ai;
    uint32_t callback = 0;
    uint32_t stream_vol_left = 0;
    uint32_t stream_vol_right = 0;
    uint32_t stream_play_state = 0;
    PPCContext* ctx = nullptr;  // context of the poll in progress, for the callback
    Memory* mem = nullptr;
    std::chrono::steady_clock::time_point last_poll{};
    bool in_poll = false;  // the DMA callback is running
    uint64_t reported_stalls = 0;
};

AiState g_ai;

void run_callback() {
    if (!g_ai.callback || !g_ai.ctx) return;
    PPCContext saved = *g_ai.ctx;
    const bool was_enabled = sched::interrupts_exchange(false);  // as in an interrupt handler
    g_func_table.call(g_ai.callback, g_ai.ctx, g_ai.mem);
    *g_ai.ctx = saved;
    sched::interrupts_exchange(was_enabled);
}

const uint8_t* read_guest(Memory* mem, uint32_t address, uint32_t bytes) {
    if (address > mem->ram_size || bytes > mem->ram_size - address) return nullptr;
    return mem->translate(Memory::MAIN_RAM_BASE | address);
}

void trace_stats(std::chrono::steady_clock::time_point now) {
    static const bool enabled = [] {
        const char* v = std::getenv("GCRECOMP_AUDIO_TRACE");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    static auto last = now;
    if (!enabled || now - last < std::chrono::seconds(1)) return;
    last = now;
    const auto& st = g_ai.ai->stats();
    std::fprintf(stderr,
                 "[AI] blocks %llu (silent %llu) frames %llu underruns %llu stalls %llu polls %llu max poll gap %.1f ms "
                 "block %u bytes %u Hz queued %zu\n",
                 (unsigned long long)st.blocks, (unsigned long long)st.silent_blocks, (unsigned long long)st.frames,
                 (unsigned long long)st.underruns, (unsigned long long)st.stalls, (unsigned long long)st.polls, st.max_poll_gap_ms,
                 g_ai.ai->dma_length(), audio::ai_sample_rate_hz(g_ai.ai->sample_rate_id()), g_ai.sink->queued_frames());
    g_ai.ai->reset_max_gap();
}

void ensure_ai(Memory* mem) {
    if (g_ai.ai) return;
    if (!g_ai.sink) {
        const char* choice = std::getenv("GCRECOMP_AUDIO");
        const bool want_null = choice && std::strcmp(choice, "null") == 0;
        if (!want_null) {
            g_ai.owned_sink = audio::make_sdl_sink();
            if (g_ai.owned_sink && !g_ai.owned_sink->open(32000)) g_ai.owned_sink.reset();
        }
        if (!g_ai.owned_sink) {
            std::printf("[AI] %s: audio is discarded\n", want_null ? "null sink selected" : "no audio device");
            g_ai.owned_sink = std::make_unique<audio::NullSink>(true);
            g_ai.owned_sink->open(32000);
        }
        // GCRECOMP_DUMP_AUDIO=file.wav also writes everything played to a WAV file.
        const char* dump = std::getenv("GCRECOMP_DUMP_AUDIO");
        if (dump && *dump) {
            g_ai.owned_sink = audio::make_wav_dump_sink(std::move(g_ai.owned_sink), dump);
            std::printf("[AI] dumping audio to %s\n", dump);
        }
        g_ai.sink = g_ai.owned_sink.get();
    }
    g_ai.ai = std::make_unique<audio::AudioInterface>(
        *g_ai.sink, [mem](uint32_t address, uint32_t bytes) { return read_guest(mem, address, bytes); }, run_callback);
}

// AIInit(u8* stack)
void ai_init(PPCContext*, Memory* mem) {
    std::printf("[AI] AIInit\n");
    ensure_ai(mem);
}

// AIRegisterDMACallback(AIDCallback callback) -> previous callback
void ai_register_dma_callback(PPCContext* ctx, Memory*) {
    const uint32_t old = g_ai.callback;
    g_ai.callback = ctx->r[3];
    ctx->r[3] = old;
}

void mirror_registers(Memory* mem, bool enable) {
    const uint32_t address = g_ai.ai->dma_address();
    mem->write16(kRegDmaAddrHigh, uint16_t((address >> 16) & 0x3FF));
    mem->write16(kRegDmaAddrLow, uint16_t(address & 0xFFE0));
    mem->write16(kRegDmaLength, uint16_t((g_ai.ai->dma_length() >> 5) | (enable ? kDmaEnableBit : 0)));
}

// AIInitDMA(u32 address, u32 length)
void ai_init_dma(PPCContext* ctx, Memory* mem) {
    ensure_ai(mem);
    g_ai.ai->init_dma(ctx->r[3], ctx->r[4]);
    mirror_registers(mem, g_ai.ai->dma_enabled());
}

// AIStartDMA(void): the first block starts at once.
void ai_start_dma(PPCContext* ctx, Memory* mem) {
    ensure_ai(mem);
    g_ai.ai->start_dma();
    mirror_registers(mem, true);
    g_ai.last_poll = {};
    ai_poll(ctx, mem);
}

void ai_stop_dma(PPCContext*, Memory* mem) {
    ensure_ai(mem);
    g_ai.ai->stop_dma();
    mirror_registers(mem, false);
}

void ai_get_dma_start_addr(PPCContext* ctx, Memory* mem) {
    ensure_ai(mem);
    ctx->r[3] = g_ai.ai->dma_address();
}

void ai_get_dma_length(PPCContext* ctx, Memory* mem) {
    ensure_ai(mem);
    ctx->r[3] = g_ai.ai->dma_length();
}

// AISetDSPSampleRate(u32 rate): 0 = 32 kHz, 1 = 48 kHz
void ai_set_dsp_sample_rate(PPCContext* ctx, Memory* mem) {
    ensure_ai(mem);
    g_ai.ai->set_sample_rate_id(ctx->r[3]);
}

void ai_get_dsp_sample_rate(PPCContext* ctx, Memory* mem) {
    ensure_ai(mem);
    ctx->r[3] = g_ai.ai->sample_rate_id();
}

// The streaming audio of the DVD drive is not emulated: the volume and play state are only kept.
void ai_set_stream_vol_left(PPCContext* ctx, Memory*) { g_ai.stream_vol_left = ctx->r[3] & 0xFF; }
void ai_set_stream_vol_right(PPCContext* ctx, Memory*) { g_ai.stream_vol_right = ctx->r[3] & 0xFF; }
void ai_get_stream_vol_left(PPCContext* ctx, Memory*) { ctx->r[3] = g_ai.stream_vol_left; }
void ai_get_stream_vol_right(PPCContext* ctx, Memory*) { ctx->r[3] = g_ai.stream_vol_right; }
void ai_set_stream_play_state(PPCContext* ctx, Memory*) { g_ai.stream_play_state = ctx->r[3] & 1; }
void ai_get_stream_play_state(PPCContext* ctx, Memory*) { ctx->r[3] = g_ai.stream_play_state; }

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"AIInit", ai_init},
    {"AIRegisterDMACallback", ai_register_dma_callback},
    {"AIInitDMA", ai_init_dma},
    {"AIStartDMA", ai_start_dma},
    {"AIStopDMA", ai_stop_dma},
    {"AIGetDMAStartAddr", ai_get_dma_start_addr},
    {"AIGetDMALength", ai_get_dma_length},
    {"AISetDSPSampleRate", ai_set_dsp_sample_rate},
    {"AIGetDSPSampleRate", ai_get_dsp_sample_rate},
    {"AISetStreamVolLeft", ai_set_stream_vol_left},
    {"AISetStreamVolRight", ai_set_stream_vol_right},
    {"AIGetStreamVolLeft", ai_get_stream_vol_left},
    {"AIGetStreamVolRight", ai_get_stream_vol_right},
    {"AISetStreamPlayState", ai_set_stream_play_state},
    {"AIGetStreamPlayState", ai_get_stream_play_state},
    {nullptr, nullptr},
};

}  // namespace

void ai_hle_reset(audio::AudioSink* sink) {
    g_ai = AiState{};
    g_ai.sink = sink;
}

RecompiledFunc lookup_ai_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

void ai_poll(PPCContext* ctx, Memory* mem) {
    if (!g_ai.ai || !g_ai.ai->dma_enabled()) return;
    // The DMA callback can switch guest threads (it wakes one, for instance); the thread that runs next
    // must not deliver the interrupt again, nor replace the context the callback runs on.
    if (g_ai.in_poll) return;
    // Called from hot paths (clock reads, interrupt re-enabling): look at the DMA once a millisecond.
    const auto now = std::chrono::steady_clock::now();
    if (now - g_ai.last_poll < std::chrono::milliseconds(1)) return;
    g_ai.last_poll = now;
    g_ai.ctx = ctx;
    g_ai.mem = mem;
    g_ai.in_poll = true;
    g_ai.ai->poll();
    g_ai.in_poll = false;
    const uint64_t stalls = g_ai.ai->stats().stalls;
    if (stalls != g_ai.reported_stalls) {
        g_ai.reported_stalls = stalls;
        std::fprintf(stderr, "[AI] the audio device stopped consuming samples: the DMA runs on the host clock, audio is discarded until it drains\n");
    }
    trace_stats(now);
    g_ai.ctx = nullptr;
}

}  // namespace gcrecomp
