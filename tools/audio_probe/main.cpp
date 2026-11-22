// SPDX-License-Identifier: MIT
//
// gcrecomp_audio_probe: plays a sine tone through the same sink the game uses (the default SDL sink
// unless --null) in the same way the AI DMA does (blocks of --block frames, topped up while the sink
// holds less than --lead ms), and prints how the sink behaves: whether it opens, how the queue
// drains. With tools/audio-capture.sh and tools/wav-compare.py it proves that sound reaches the host:
//
//   tools/audio-capture.sh --out /tmp/cap.wav -- gcrecomp_audio_probe --seconds 5
//   tools/wav-compare.py /tmp/cap.wav --require-nonzero
//
// Usage: gcrecomp_audio_probe [--seconds N] [--rate HZ] [--freq HZ] [--block FRAMES] [--lead MS]
//                             [--amp 0..32767] [--null] [--dump file.wav]
// Exit status: 0 if the sink opened and all frames were played, 1 otherwise.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "gcrecomp/audio/audio_sink.h"

using namespace gcrecomp::audio;

int main(int argc, char** argv) {
    double seconds = 5, freq = 440;
    uint32_t rate = 32000, block = 1280, lead_ms = 60;
    int amp = 8000;
    bool null_sink = false;
    const char* dump = nullptr;
    for (int i = 1; i < argc; ++i) {
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
        else if (!std::strcmp(argv[i], "--rate")) rate = uint32_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--freq")) freq = std::atof(next());
        else if (!std::strcmp(argv[i], "--block")) block = uint32_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--lead")) lead_ms = uint32_t(std::atoi(next()));
        else if (!std::strcmp(argv[i], "--amp")) amp = std::atoi(next());
        else if (!std::strcmp(argv[i], "--null")) null_sink = true;
        else if (!std::strcmp(argv[i], "--dump")) dump = next();
        else {
            std::printf("usage: %s [--seconds N] [--rate HZ] [--freq HZ] [--block FRAMES] [--lead MS] [--amp N] [--null] [--dump f.wav]\n",
                        argv[0]);
            return std::strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    std::unique_ptr<AudioSink> sink = null_sink ? std::make_unique<NullSink>(true) : make_sdl_sink();
    if (!sink || !sink->open(rate)) {
        std::printf("sink: cannot open (%s)\n", null_sink ? "null" : "sdl");
        return 1;
    }
    if (dump) sink = make_wav_dump_sink(std::move(sink), dump);
    std::printf("sink opened at %u Hz\n", rate);
    const size_t total = size_t(seconds * rate), lead = size_t(rate) * lead_ms / 1000;
    size_t sent = 0;
    std::vector<int16_t> buf(block * 2);
    const auto t0 = std::chrono::steady_clock::now();
    size_t max_queue = 0;
    double next_report = 1;
    while (sent < total) {
        const size_t queued = sink->queued_frames();
        if (queued > max_queue) max_queue = queued;
        if (queued < lead) {
            for (uint32_t i = 0; i < block; ++i) {
                const int16_t v = int16_t(amp * std::sin(6.283185307179586 * freq * double(sent + i) / rate));
                buf[2 * i] = buf[2 * i + 1] = v;
            }
            sink->submit(buf.data(), block);
            sent += block;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t >= next_report) {
            std::printf("t=%.1fs sent=%zu queued=%zu\n", t, sent, queued);
            next_report += 1;
        }
    }
    // Let the queue drain, but not forever: report what the sink never plays.
    for (int i = 0; i < 400 && sink->queued_frames() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (const size_t left = sink->queued_frames()) std::printf("queue stuck at %zu frames after 2 s\n", left);
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("played %zu frames in %.2fs (expected %.2fs), max queue %zu frames\n", sent, t, double(sent) / rate, max_queue);
    return 0;
}
