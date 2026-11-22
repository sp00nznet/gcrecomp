// SPDX-License-Identifier: MIT
// Unit tests for the audio path: sample conversion, DSP ADPCM decoding and the AI DMA model.
// No display, no disc, no audio device.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "gcrecomp/audio/audio.h"
#include "gcrecomp/audio/audio_interface.h"

using namespace gcrecomp::audio;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// --- sample conversion ---------------------------------------------------------------------------

static void test_convert_ai_frames() {
    // R0 L0 R1 L1 as the AI reads them: big-endian, right word first.
    const uint8_t in[] = {0x01, 0x02, 0xFF, 0xFE, 0x7F, 0xFF, 0x80, 0x00};
    int16_t out[4] = {};
    convert_ai_frames(in, out, 2);
    CHECK(out[0] == -2);        // L0
    CHECK(out[1] == 0x0102);    // R0
    CHECK(out[2] == -32768);    // L1
    CHECK(out[3] == 32767);     // R1
}

static void test_sample_rate_ids() {
    // AISetDSPSampleRate(0) is 32 kHz, (1) is 48 kHz.
    CHECK(ai_sample_rate_hz(0) == 32000);
    CHECK(ai_sample_rate_hz(1) == 48000);
}

// --- DSP ADPCM decoder, against vectors worked out by hand ---------------------------------------

static DSPADPCMInfo make_info() {
    DSPADPCMInfo info{};
    info.coef[0] = 0;     // predictor 0: no prediction
    info.coef[1] = 0;
    info.coef[2] = 2048;  // predictor 1: yn1 * 1.0
    info.coef[3] = 0;
    info.coef[4] = 1024;  // predictor 2: yn1 * 0.5
    info.coef[5] = 0;
    info.coef[6] = 0;     // predictor 3: yn2 * 1.0
    info.coef[7] = 2048;
    return info;
}

static void test_adpcm_no_prediction() {
    // Header 0x01: predictor 0, scale 2^1. Nibbles 1 2 3 4 5 6 7 8 9 A B C D E, as two's complement.
    const uint8_t data[] = {0x01, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};
    const int16_t expect[14] = {2, 4, 6, 8, 10, 12, 14, -16, -14, -12, -10, -8, -6, -4};
    const auto pcm = decode_dsp_adpcm(data, 14, make_info());
    CHECK(pcm.size() == 14);
    for (size_t i = 0; i < pcm.size(); ++i) CHECK(pcm[i] == expect[i]);
}

static void test_adpcm_accumulating_predictor() {
    // Predictor 1 adds the previous sample: a running sum of nibble * 2.
    const uint8_t data[] = {0x11, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE};
    const int16_t expect[14] = {2, 6, 12, 20, 30, 42, 56, 40, 26, 14, 4, -4, -10, -14};
    const auto pcm = decode_dsp_adpcm(data, 14, make_info());
    for (size_t i = 0; i < 14; ++i) CHECK(pcm[i] == expect[i]);
}

static void test_adpcm_initial_history_and_rounding() {
    // Predictor 2 halves yn1 with rounding: (1024 * yn1 + 1024) >> 11, an arithmetic shift.
    // yn1 = 3 -> (3072 + 1024) >> 11 = 2; yn1 = -3 -> (-3072 + 1024) >> 11 = -1.
    DSPADPCMInfo info = make_info();
    info.yn1 = 3;
    const uint8_t data[] = {0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto pcm = decode_dsp_adpcm(data, 4, info);
    // 3 -> 2 -> (2048 + 1024) >> 11 = 1 -> (1024 + 1024) >> 11 = 1 -> 1
    CHECK(pcm[0] == 2);
    CHECK(pcm[1] == 1);
    CHECK(pcm[2] == 1);
    CHECK(pcm[3] == 1);

    info.yn1 = -3;
    const auto neg = decode_dsp_adpcm(data, 3, info);
    // -3 -> -1 -> (-1024 + 1024) >> 11 = 0 -> 0
    CHECK(neg[0] == -1);
    CHECK(neg[1] == 0);
    CHECK(neg[2] == 0);
}

static void test_adpcm_second_order_and_frame_boundary() {
    // Predictor 3 repeats yn2, so a +1 impulse (scale 1) comes back every other sample. The history
    // carries over to the second frame, which has its own header (scale 2^2 here).
    DSPADPCMInfo info = make_info();
    const uint8_t data[] = {
        0x30, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // frame 1: nibbles 1,0,0,...
        0x32, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // frame 2: scale 2^2, nibbles 1,0,0,...
    };
    const auto pcm = decode_dsp_adpcm(data, 28, info);
    // Frame 1: s0 = 1 (yn2 = 0), s1 = 0 (yn2 = 0), s2 = s0 = 1, s3 = s1 = 0, ... alternating 1,0.
    CHECK(pcm[0] == 1 && pcm[1] == 0 && pcm[2] == 1 && pcm[3] == 0 && pcm[12] == 1 && pcm[13] == 0);
    // Frame 2 starts with yn1 = pcm[13] = 0, yn2 = pcm[12] = 1: s14 = 4 * 1 + 1 = 5, s15 = 0 + 0 = 0,
    // s16 = 0 + s14 = 5.
    CHECK(pcm[14] == 5);
    CHECK(pcm[15] == 0);
    CHECK(pcm[16] == 5);
}

static void test_adpcm_clamps() {
    // Scale 2^11, predictor 1 (accumulate): 7 * 2048 = 14336 per sample, so the third sample clamps
    // to 32767; with nibble 8 (-8 * 2048 = -16384 per sample) the sum clamps to -32768.
    DSPADPCMInfo info = make_info();
    const uint8_t up[] = {0x1B, 0x77, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto hi = decode_dsp_adpcm(up, 4, info);
    CHECK(hi[0] == 14336);
    CHECK(hi[1] == 28672);
    CHECK(hi[2] == 32767);
    CHECK(hi[3] == 32767);  // nibble 0 and a prediction of exactly yn1: stays at the clamp
    const uint8_t down[] = {0x1B, 0x88, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto lo = decode_dsp_adpcm(down, 3, info);
    CHECK(lo[0] == -16384);
    CHECK(lo[1] == -32768);
    CHECK(lo[2] == -32768);
}

// --- AI DMA model ---------------------------------------------------------------------------------

namespace {

struct Harness {
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000, 0);
    NullSink sink;
    unsigned interrupts = 0;
    std::function<void()> on_interrupt;
    AudioInterface ai;

    Harness()
        : ai(
              sink,
              [this](uint32_t address, uint32_t bytes) -> const uint8_t* {
                  if (address > ram.size() || bytes > ram.size() - address) return nullptr;
                  return ram.data() + address;
              },
              [this] {
                  ++interrupts;
                  if (on_interrupt) on_interrupt();
              }) {
        sink.open(32000);
        ai.set_lead_ms(20);  // 640 frames at 32 kHz = four blocks of 160 frames
    }

    // Fills `frames` stereo frames at `address` with left = tag, right = -tag (big-endian, the AI
    // reads the right word first).
    void fill(uint32_t address, uint32_t frames, int16_t tag) {
        for (uint32_t i = 0; i < frames; ++i) {
            const uint16_t l = uint16_t(tag), r = uint16_t(-tag);
            ram[address + i * 4 + 0] = uint8_t(r >> 8);
            ram[address + i * 4 + 1] = uint8_t(r);
            ram[address + i * 4 + 2] = uint8_t(l >> 8);
            ram[address + i * 4 + 3] = uint8_t(l);
        }
    }
};

}  // namespace

static void test_dma_register_limits() {
    Harness h;
    h.ai.init_dma(0x80123457, 100);
    CHECK(h.ai.dma_address() == 0x00123440);  // 26 bits, 32-byte aligned
    CHECK(h.ai.dma_length() == 96);           // 32-byte units
}

static void test_dma_idle_until_started() {
    Harness h;
    h.fill(0x1000, 160, 7);
    h.ai.init_dma(0x1000, 640);
    CHECK(h.ai.poll() == 0);
    CHECK(h.interrupts == 0);
    h.ai.start_dma();
    CHECK(h.ai.poll() == 4);  // fills the 20 ms lead, one interrupt per block
    CHECK(h.interrupts == 4);
    CHECK(h.sink.queued_frames() == 640);
    CHECK(h.sink.recorded().size() == 640 * 2);
    CHECK(h.sink.recorded()[0] == 7 && h.sink.recorded()[1] == -7);
    // Nothing more until the sink has played something.
    CHECK(h.ai.poll() == 0);
    h.sink.consume(160);
    CHECK(h.ai.poll() == 1);
    h.ai.stop_dma();
    h.sink.consume(640);
    CHECK(h.ai.poll() == 0);
    CHECK(h.interrupts == 5);
}

static void test_handler_programs_next_block() {
    // The handler runs after a block has started and its new registers apply to the block after:
    // blocks A, B, A, B ... even though the registers were only written by the handler.
    Harness h;
    h.fill(0x1000, 160, 1);
    h.fill(0x2000, 160, 2);
    h.ai.init_dma(0x1000, 640);
    bool next_is_b = true;
    h.on_interrupt = [&] {
        h.ai.init_dma(next_is_b ? 0x2000 : 0x1000, 640);
        next_is_b = !next_is_b;
    };
    h.ai.start_dma();
    CHECK(h.ai.poll() == 4);
    const auto& pcm = h.sink.recorded();
    CHECK(pcm.size() == 640 * 2);
    CHECK(pcm[0 * 320] == 1);
    CHECK(pcm[1 * 320] == 2);
    CHECK(pcm[2 * 320] == 1);
    CHECK(pcm[3 * 320] == 2);
}

static void test_unprogrammed_block_repeats() {
    // If the handler does not reprogram the DMA, the hardware plays the same block again.
    Harness h;
    h.fill(0x1000, 160, 5);
    h.ai.init_dma(0x1000, 640);
    h.ai.start_dma();
    CHECK(h.ai.poll() == 4);
    CHECK(h.sink.recorded()[3 * 320] == 5);
}

static void test_poll_is_not_reentrant() {
    Harness h;
    h.fill(0x1000, 160, 1);
    h.ai.init_dma(0x1000, 640);
    unsigned nested = 99;
    h.on_interrupt = [&] { nested = h.ai.poll(); };
    h.ai.start_dma();
    CHECK(h.ai.poll() == 4);
    CHECK(nested == 0);
    CHECK(h.interrupts == 4);
}

static void test_zero_length_and_unreadable_blocks() {
    Harness h;
    h.ai.start_dma();
    CHECK(h.ai.poll() == 0);  // length 0: nothing to play
    h.ai.init_dma(0x20000, 640);  // outside the 64 KB test RAM
    CHECK(h.ai.poll() == 0);
    CHECK(h.interrupts == 0);
    CHECK(h.sink.recorded().empty());
}

static void test_sample_rate_change() {
    Harness h;
    h.fill(0x1000, 160, 3);
    h.ai.init_dma(0x1000, 640);
    h.ai.start_dma();
    h.ai.poll();
    h.sink.consume(h.sink.queued_frames());
    h.ai.set_sample_rate_id(1);
    CHECK(h.sink.sample_rate() == 48000);
    // 20 ms at 48 kHz is 960 frames: six blocks of 160.
    CHECK(h.ai.poll() == 6);
    CHECK(h.sink.submit_rates().front() == 32000);
    CHECK(h.sink.submit_rates().back() == 48000);
    h.ai.set_sample_rate_id(1);  // no change, no reconfiguration
    CHECK(h.sink.sample_rate() == 48000);
}

static void test_stats_count_blocks_silence_and_underruns() {
    Harness h;
    h.fill(0x1000, 160, 7);
    h.ai.init_dma(0x1000, 640);
    h.ai.start_dma();
    CHECK(h.ai.poll() == 4);
    CHECK(h.ai.stats().blocks == 4 && h.ai.stats().frames == 640);
    CHECK(h.ai.stats().silent_blocks == 0);
    CHECK(h.ai.stats().underruns == 0);
    // the game points the DMA at a zero block: counted as silent
    h.ai.init_dma(0x2000, 640);
    h.sink.consume(h.sink.queued_frames());  // the sink ran dry: an underrun at the next poll
    h.ai.poll();
    CHECK(h.ai.stats().underruns == 1);
    CHECK(h.ai.stats().silent_blocks == 4);
    CHECK(h.ai.stats().polls == 2);
}

static void test_stuck_sink_does_not_stop_the_dma() {
    // A device that stops consuming (suspended, lost) must not freeze the game: after the stall time the DMA
    // keeps its real-time pace on the host clock (blocks consumed, interrupts raised, audio discarded).
    Harness h;
    h.fill(0x1000, 160, 7);
    h.ai.init_dma(0x1000, 640);
    auto fake_now = std::chrono::steady_clock::time_point{} + std::chrono::seconds(1000);
    h.ai.set_clock([&] { return fake_now; });
    h.ai.set_stall_ms(30);
    h.ai.start_dma();
    CHECK(h.ai.poll() == 4);  // the lead is full and the sink never consumes
    CHECK(h.ai.poll() == 0);
    CHECK(!h.ai.stalled());
    fake_now += std::chrono::milliseconds(40);
    h.ai.poll();
    CHECK(h.ai.stalled());
    CHECK(h.ai.stats().stalls == 1);
    const uint64_t blocks = h.ai.stats().blocks;
    const size_t recorded = h.sink.recorded().size();
    // 100 ms of host time later the DMA has gone on by about that much (20 ms lead: it refills 20 ms per poll)
    unsigned started = 0;
    for (int i = 0; i < 10; ++i) {
        fake_now += std::chrono::milliseconds(10);
        started += h.ai.poll();
    }
    CHECK(started == 20);  // 100 ms of simulated time = 20 blocks of 5 ms
    CHECK(h.ai.stats().blocks == blocks + started);
    CHECK(h.sink.recorded().size() == recorded);  // nothing went to the stuck sink
    CHECK(h.ai.stats().stalled_frames >= uint64_t(started) * 160);
    // the sink drains again: normal operation resumes
    h.sink.consume(h.sink.queued_frames() / 2);
    h.ai.poll();
    CHECK(!h.ai.stalled());
    h.sink.consume(160);
    h.ai.poll();
    CHECK(h.sink.recorded().size() > recorded);
}

int main() {
    test_convert_ai_frames();
    test_sample_rate_ids();
    test_adpcm_no_prediction();
    test_adpcm_accumulating_predictor();
    test_adpcm_initial_history_and_rounding();
    test_adpcm_second_order_and_frame_boundary();
    test_adpcm_clamps();
    test_dma_register_limits();
    test_dma_idle_until_started();
    test_handler_programs_next_block();
    test_unprogrammed_block_repeats();
    test_poll_is_not_reentrant();
    test_zero_length_and_unreadable_blocks();
    test_sample_rate_change();
    test_stats_count_blocks_silence_and_underruns();
    test_stuck_sink_does_not_stop_the_dma();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all audio tests passed\n");
    return 0;
}
