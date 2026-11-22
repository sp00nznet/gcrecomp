// SPDX-License-Identifier: MIT
//
// AI DMA model: see audio_interface.h.
#include "gcrecomp/audio/audio_interface.h"

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
    while (enabled_ && length_ != 0 && started < kMaxBlocksPerPoll) {
        const size_t lead_frames = size_t(ai_sample_rate_hz(rate_id_)) * lead_ms_ / 1000;
        if (sink_.queued_frames() >= lead_frames) break;
        const uint32_t length = length_;
        const uint8_t* data = read_memory_(address_, length);
        if (!data) break;
        const size_t frames = length / 4;
        scratch_.resize(frames * 2);
        convert_ai_frames(data, scratch_.data(), frames);
        sink_.submit(scratch_.data(), frames);
        ++blocks_;
        ++started;
        // The block has started: the handler may program the one after it.
        raise_interrupt_();
    }
    in_poll_ = false;
    return started;
}

}  // namespace gcrecomp::audio
