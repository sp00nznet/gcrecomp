// SPDX-License-Identifier: MIT
//
// AI DMA model: see audio_interface.h.
#include "gcrecomp/audio/audio_interface.h"

#include <algorithm>

namespace gcrecomp::audio {

namespace {
constexpr uint32_t kAddressMask = 0x03FFFFE0;  // 26 bits of physical address, 32-byte aligned
constexpr uint32_t kMaxBlocksPerPoll = 64;
}  // namespace

void convert_ai_frames(const uint8_t* big_endian, int16_t* out, size_t frames) {
    // The AI plays the first 16-bit word of each frame as the right channel, the second as the left.
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* f = big_endian + 4 * i;
        out[2 * i] = int16_t(uint16_t(f[2] << 8) | f[3]);
        out[2 * i + 1] = int16_t(uint16_t(f[0] << 8) | f[1]);
    }
}

uint32_t ai_sample_rate_hz(uint32_t rate_id) { return rate_id == 0 ? 32000 : 48000; }

AudioInterface::AudioInterface(AudioSink& sink, ReadMemoryFn read_memory, InterruptFn raise_interrupt)
    : sink_(sink), read_memory_(std::move(read_memory)), raise_interrupt_(std::move(raise_interrupt)) {}

void AudioInterface::init_dma(uint32_t address, uint32_t length) {
    address_ = address & kAddressMask;
    length_ = ((length >> 5) & 0x7FFF) << 5;
}

void AudioInterface::set_sample_rate_id(uint32_t rate_id) {
    rate_id = rate_id == 0 ? 0 : 1;
    if (rate_id == rate_id_) return;
    rate_id_ = rate_id;
    sink_.set_sample_rate(ai_sample_rate_hz(rate_id_));
}

unsigned AudioInterface::poll() {
    if (in_poll_) return 0;
    in_poll_ = true;
    unsigned started = 0;
    const auto now = clock_();
    if (enabled_) {
        if (last_poll_ != std::chrono::steady_clock::time_point{}) {
            const double gap = std::chrono::duration<double, std::milli>(now - last_poll_).count();
            if (gap > stats_.max_poll_gap_ms) stats_.max_poll_gap_ms = gap;
            virtual_queue_ = std::max(0.0, virtual_queue_ - gap / 1000.0 * ai_sample_rate_hz(rate_id_));
        }
        last_poll_ = now;
        ++stats_.polls;
        const size_t queued = sink_.queued_frames();
        if (stats_.frames != 0 && queued == 0) ++stats_.underruns;
        // A sink that holds a full lead and does not drain is stuck (device lost, suspended, paused):
        // without this the game would wait forever for the DMA interrupt that never comes.
        const size_t lead = size_t(ai_sample_rate_hz(rate_id_)) * lead_ms_ / 1000;
        if (queued < last_queued_ || queued < lead || progress_since_ == std::chrono::steady_clock::time_point{}) {
            progress_since_ = now;
            stalled_ = false;
        } else if (!stalled_ && now - progress_since_ > std::chrono::milliseconds(stall_ms_)) {
            stalled_ = true;
            virtual_queue_ = 0;
            ++stats_.stalls;
        }
        last_queued_ = queued;
    }
    while (enabled_ && length_ != 0 && started < kMaxBlocksPerPoll) {
        const size_t lead_frames = size_t(ai_sample_rate_hz(rate_id_)) * lead_ms_ / 1000;
        if ((stalled_ ? size_t(virtual_queue_) : sink_.queued_frames()) >= lead_frames) break;
        const uint32_t length = length_;
        const uint8_t* data = read_memory_(address_, length);
        if (!data) break;
        const size_t frames = length / 4;
        scratch_.resize(frames * 2);
        convert_ai_frames(data, scratch_.data(), frames);
        if (stalled_) {
            virtual_queue_ += double(frames);
            stats_.stalled_frames += frames;
        } else {
            sink_.submit(scratch_.data(), frames);
        }
        ++blocks_;
        ++stats_.blocks;
        stats_.frames += frames;
        bool silent = true;
        for (size_t i = 0; i < frames * 2 && silent; ++i) silent = scratch_[i] == 0;
        if (silent) ++stats_.silent_blocks;
        ++started;
        // The block has started: the handler may program the one after it.
        raise_interrupt_();
    }
    in_poll_ = false;
    return started;
}

}  // namespace gcrecomp::audio
