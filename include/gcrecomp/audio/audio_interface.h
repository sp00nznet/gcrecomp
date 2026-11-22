#pragma once
// SPDX-License-Identifier: MIT
//
// The GameCube Audio Interface (AI) DMA: the CPU gives it the address and length of a block of
// big-endian 16-bit stereo PCM; it plays the block and, when it starts the next one, raises an
// interrupt so the game can program the block after that.
//
// This class is the machine-independent part (no guest memory, no PPC context): it reads blocks
// through a callback, converts them, feeds an AudioSink and raises the interrupt through another
// callback. The binding to the recompiled SDK is src/os/ai_funcs.cpp.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "gcrecomp/audio/audio_sink.h"

namespace gcrecomp::audio {

/// Converts `frames` stereo frames of big-endian int16 (as the AI reads them from RAM: right word
/// first, left word second) to host-endian interleaved L, R.
void convert_ai_frames(const uint8_t* big_endian, int16_t* out, size_t frames);

/// Sample rate of the AI DMA as the SDK's AISetDSPSampleRate numbers it: 0 = 32 kHz, 1 = 48 kHz.
uint32_t ai_sample_rate_hz(uint32_t rate_id);

class AudioInterface {
public:
    /// Returns a pointer to `bytes` readable bytes of guest memory at `address`, or nullptr.
    using ReadMemoryFn = std::function<const uint8_t*(uint32_t address, uint32_t bytes)>;
    /// Runs the guest's DMA callback (the AI interrupt handler).
    using InterruptFn = std::function<void()>;

    AudioInterface(AudioSink& sink, ReadMemoryFn read_memory, InterruptFn raise_interrupt);

    /// AIInitDMA: programs the next block. The hardware keeps the address 32-byte aligned and
    /// within 64 MB, and the length in 32-byte units (15 bits).
    void init_dma(uint32_t address, uint32_t length);
    uint32_t dma_address() const { return address_; }
    uint32_t dma_length() const { return length_; }

    /// AIStartDMA / AIStopDMA.
    void start_dma() { enabled_ = true; }
    void stop_dma() { enabled_ = false; }
    bool dma_enabled() const { return enabled_; }

    /// AISetDSPSampleRate / AIGetDSPSampleRate (see ai_sample_rate_hz).
    void set_sample_rate_id(uint32_t rate_id);
    uint32_t sample_rate_id() const { return rate_id_; }

    /// How far ahead of playback the AI runs, in milliseconds of audio. Default 60.
    void set_lead_ms(uint32_t ms) { lead_ms_ = ms; }

    /// Advances the DMA: while it is enabled and the sink holds less than the lead, starts the
    /// next block (the registers' current contents), submits it and raises the interrupt, whose
    /// handler may program another block. Not re-entrant: called from the interrupt handler it
    /// does nothing. Returns the number of blocks started.
    unsigned poll();

    uint64_t blocks_started() const { return blocks_; }

private:
    AudioSink& sink_;
    ReadMemoryFn read_memory_;
    InterruptFn raise_interrupt_;
    uint32_t address_ = 0;
    uint32_t length_ = 0;
    uint32_t rate_id_ = 0;
    uint32_t lead_ms_ = 60;
    bool enabled_ = false;
    bool in_poll_ = false;
    uint64_t blocks_ = 0;
    std::vector<int16_t> scratch_;
};

}  // namespace gcrecomp::audio
