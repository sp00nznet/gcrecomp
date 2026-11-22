// SPDX-License-Identifier: MIT
// AX microcode HLE: the mail protocol, voice decoding (PCM16 and DSP ADPCM), the volume path and
// the output order, against hand-computed expectations; plus the WAV dump sink.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "gcrecomp/audio/ax.h"
#include "gcrecomp/audio/audio_sink.h"

using namespace gcrecomp::audio;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

namespace {

constexpr uint32_t kCmd = 0x1000;
constexpr uint32_t kPb = 0x2000;
constexpr uint32_t kOut = 0x4000;
constexpr uint32_t kSurr = 0x6000;
constexpr size_t kPbWords = 122;

void put16(std::vector<uint8_t>& ram, uint32_t addr, uint16_t v) {
    ram[addr] = uint8_t(v >> 8);
    ram[addr + 1] = uint8_t(v);
}
uint16_t get16(const std::vector<uint8_t>& ram, uint32_t addr) { return uint16_t(ram[addr] << 8 | ram[addr + 1]); }

// Command list: process the PB list at kPb, output to kOut, end. Returns its size in words.
uint32_t write_cmdlist(std::vector<uint8_t>& ram) {
    const uint16_t words[] = {0x02, 0, uint16_t(kPb), 0x03, 0x0E, 0, uint16_t(kSurr), 0, uint16_t(kOut), 0x0F, 0, 0};
    for (size_t i = 0; i < sizeof words / 2; ++i) put16(ram, kCmd + 2 * uint32_t(i), words[i]);
    return 10;
}

// A running voice with a unity main L/R mix, no ramps.
void write_pb(std::vector<uint8_t>& ram, uint16_t format, uint16_t src_type, uint32_t cur, uint32_t end) {
    uint16_t w[kPbWords] = {};
    w[4] = src_type;
    w[6] = 0x0003;  // main L + R
    w[7] = 1;
    w[9] = 0x7FFF;   // main left volume
    w[11] = 0x7FFF;  // main right volume
    w[50] = 0x7FFF;  // volume envelope
    w[56] = format;
    w[59] = uint16_t(end >> 16);
    w[60] = uint16_t(end);
    w[61] = uint16_t(cur >> 16);
    w[62] = uint16_t(cur);
    w[79] = 0x800;  // gain
    w[83] = 1;      // ratio 1.0 (hi), lo = 0
    for (size_t i = 0; i < kPbWords; ++i) put16(ram, kPb + 2 * uint32_t(i), w[i]);
}

struct Rendered {
    AxUcode ax;
    std::optional<uint32_t> first, second;
};

void render(std::vector<uint8_t>& ram, const std::vector<uint8_t>& aram, Rendered& r) {
    const uint32_t size = write_cmdlist(ram);
    AxMemory mem{ram.data(), ram.size(), aram.data(), aram.size()};
    r.first = r.ax.handle_mail(AxUcode::kMailCmdList | size, mem);
    r.second = r.ax.handle_mail(kCmd, mem);
}

// (right, left) of frame k as the microcode writes them.
void out_frame(const std::vector<uint8_t>& ram, size_t k, int& right, int& left) {
    right = int16_t(get16(ram, kOut + 4 * uint32_t(k)));
    left = int16_t(get16(ram, kOut + 4 * uint32_t(k) + 2));
}

void test_pcm16_voice_reaches_the_output_right_word_first() {
    std::vector<uint8_t> ram(0x8000), aram(0x1000);
    for (int i = 0; i < 300; ++i) {
        const int16_t v = int16_t((i - 150) * 100);
        aram[2 * i] = uint8_t(uint16_t(v) >> 8);
        aram[2 * i + 1] = uint8_t(v);
    }
    write_pb(ram, 0x000A, 2, 0, 0x7FF);
    Rendered r;
    render(ram, aram, r);
    CHECK(!r.first.has_value());
    CHECK(r.second == AxUcode::kDspYield);
    CHECK(r.ax.frames() == 1);
    for (size_t k : {0, 1, 50, 159}) {
        const int expected = (int(k) - 150) * 100;
        int right, left;
        out_frame(ram, k, right, left);
        // three 15-bit gain stages (envelope, mixer) lose at most a couple of LSBs
        CHECK(std::abs(right - expected) <= 3);
        CHECK(left == right);
    }
}

void test_adpcm_frame_decodes_nibbles_scaled_by_the_header() {
    std::vector<uint8_t> ram(0x8000), aram(0x1000);
    // header: predictor 0, scale 1 (x2); then nibbles 1..14
    aram[0] = 0x01;
    for (int i = 0; i < 7; ++i) aram[1 + i] = uint8_t((2 * i + 1) << 4 | ((2 * i + 2) & 0xF));
    // ADPCM addresses are in nibbles: skip the one-byte header (2 nibbles)
    write_pb(ram, 0x0000, 2, 2, 0x1F);
    put16(ram, kPb + 2 * 80, 0x0001);  // coefficients stay 0, the initial pred_scale is scale 1
    Rendered r;
    render(ram, aram, r);
    const int want[] = {2, 4, 6, 8, 10, 12, 14, -16, -14, -12, -10, -8};
    for (size_t k = 0; k < sizeof want / sizeof want[0]; ++k) {
        int right, left;
        out_frame(ram, k, right, left);
        CHECK(std::abs(right - want[k]) <= 2);  // two 15-bit gain stages with a 0x7FFF "unity" floor
    }
}

void test_one_shot_voice_stops_at_its_end() {
    std::vector<uint8_t> ram(0x8000);
    const std::vector<uint8_t> aram(0x1000);
    write_pb(ram, 0x000A, 2, 0, 10);
    Rendered r;
    render(ram, aram, r);
    CHECK(get16(ram, kPb + 14) == 0);  // the running flag cleared
}

void test_mail_protocol() {
    std::vector<uint8_t> ram(0x8000);
    const std::vector<uint8_t> aram(0x1000);
    AxMemory mem{ram.data(), ram.size(), aram.data(), aram.size()};
    AxUcode ax;
    CHECK(!ax.handle_mail(0x12345678, mem));  // not a list size: ignored
    CHECK(!ax.handle_mail(AxUcode::kMailCmdList | 1, mem));
    put16(ram, kCmd, 0x0F);
    CHECK(ax.handle_mail(kCmd, mem) == AxUcode::kDspYield);
    CHECK(!ax.handle_mail(0xCDD10003, mem));  // continue: back to waiting for a list
    CHECK(!ax.handle_mail(AxUcode::kMailCmdList | 1, mem));
    CHECK(ax.handle_mail(kCmd, mem) == AxUcode::kDspYield);
    CHECK(ax.handle_mail(0xCDD10000, mem) == AxUcode::kDspResume);  // resume
    CHECK(!ax.handle_mail(AxUcode::kMailCmdList | 1, mem));
    CHECK(ax.handle_mail(kCmd, mem) == AxUcode::kDspYield);
    CHECK(!ax.reset_requested());
    CHECK(!ax.handle_mail(0xCDD10002, mem));
    CHECK(ax.reset_requested());

    // An unknown command abandons the list and is counted.
    AxUcode bad;
    put16(ram, kCmd, 0x77);
    CHECK(!bad.handle_mail(AxUcode::kMailCmdList | 1, mem));
    CHECK(bad.handle_mail(kCmd, mem) == AxUcode::kDspYield);
    CHECK(bad.unknown_commands() == 1);
}

// The list the Tales of Symphonia mail 0xBABE0038 points to: setup, aux sends, upload, aux return,
// output, end; no voices. A silent frame in, a silent frame out.
void test_idle_game_list_renders_silence() {
    std::vector<uint8_t> ram(0x20000);
    const std::vector<uint8_t> aram(0x1000);
    const uint32_t init = 0x8000, auxa_w = 0x9000, auxa_r = 0xA000, auxb_w = 0xB000, auxb_r = 0xC000;
    const uint32_t up = 0xD000;
    const uint16_t words[] = {0x00, 0, uint16_t(init),
                              0x04, 0, uint16_t(auxa_w), 0, uint16_t(auxa_r),
                              0x05, 0, uint16_t(auxb_w), 0, uint16_t(auxb_r),
                              0x06, 0, uint16_t(up),
                              0x11, 0, uint16_t(up + 0x100),
                              0x09, 0, uint16_t(up),
                              0x0E, 0, uint16_t(kSurr), 0, uint16_t(kOut),
                              0x0F};
    for (size_t i = 0; i < sizeof words / 2; ++i) put16(ram, kCmd + 2 * uint32_t(i), words[i]);
    for (size_t i = 0; i < 0x2000; ++i) ram[kOut + i] = 0x55;
    AxMemory mem{ram.data(), ram.size(), aram.data(), aram.size()};
    AxUcode ax;
    ax.handle_mail(AxUcode::kMailCmdList | uint32_t(sizeof words / 2), mem);
    CHECK(ax.handle_mail(kCmd, mem) == AxUcode::kDspYield);
    CHECK(ax.unknown_commands() == 0);
    for (size_t i = 0; i < 640; ++i) CHECK(ram[kOut + i] == 0);  // the output block was written
}

void test_setup_ramp_and_main_upload() {
    std::vector<uint8_t> ram(0x20000);
    const std::vector<uint8_t> aram(0x1000);
    // buffer 0 (main L): initial value 1000, delta 3 per sample; upload it to 0x8000.
    put16(ram, 0x7000, 0);
    put16(ram, 0x7002, 1000);
    put16(ram, 0x7004, 3);
    const uint16_t words[] = {0x00, 0, 0x7000, 0x06, 0, 0x8000, 0x0F};
    for (size_t i = 0; i < sizeof words / 2; ++i) put16(ram, kCmd + 2 * uint32_t(i), words[i]);
    AxMemory mem{ram.data(), ram.size(), aram.data(), aram.size()};
    AxUcode ax;
    ax.handle_mail(AxUcode::kMailCmdList | 7, mem);
    ax.handle_mail(kCmd, mem);
    for (uint32_t k : {0u, 1u, 100u, 159u}) {
        const uint32_t v = uint32_t(get16(ram, 0x8000 + 4 * k)) << 16 | get16(ram, 0x8000 + 4 * k + 2);
        CHECK(v == 1000 + 3 * k);
    }
}

void test_wav_dump_sink() {
    const std::string path = "ax_tests_dump.wav";
    {
        auto sink = make_wav_dump_sink(std::make_unique<NullSink>(), path);
        CHECK(sink->open(32000));
        const int16_t block[] = {1, -1, 2, -2, 300, -300};
        sink->submit(block, 3);
        sink->submit(block, 3);
        CHECK(sink->queued_frames() == 6);  // forwarded to the inner sink
        // the header is valid before the sink is closed
        std::FILE* f = std::fopen(path.c_str(), "rb");
        CHECK(f != nullptr);
        if (f) {
            uint8_t h[44] = {};
            CHECK(std::fread(h, 1, 44, f) == 44);
            CHECK(std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVEfmt ", 8) == 0);
            CHECK(h[40] == 24);  // data bytes: 6 frames * 4
            std::fclose(f);
        }
    }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        uint8_t all[44 + 24] = {};
        CHECK(std::fread(all, 1, sizeof all, f) == sizeof all);
        CHECK(all[24] == 0x00 && all[25] == 0x7D);  // 32000 Hz little-endian
        CHECK(all[22] == 2 && all[34] == 16);       // stereo, 16 bit
        CHECK(all[44] == 1 && all[45] == 0 && all[46] == 0xFF && all[47] == 0xFF);
        CHECK(all[64] == uint8_t(300 & 0xFF) && all[65] == uint8_t(300 >> 8));
        std::fclose(f);
    }
    std::remove(path.c_str());
}

}  // namespace

int main() {
    test_pcm16_voice_reaches_the_output_right_word_first();
    test_adpcm_frame_decodes_nibbles_scaled_by_the_header();
    test_one_shot_voice_stops_at_its_end();
    test_mail_protocol();
    test_idle_game_list_renders_silence();
    test_setup_ramp_and_main_upload();
    test_wav_dump_sink();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ax tests passed\n");
    return 0;
}
