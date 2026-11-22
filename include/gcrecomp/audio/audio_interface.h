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

#include <chrono>
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

    /// Counters for judging whether the game feeds the DMA in time (GCRECOMP_AUDIO_TRACE prints them).
    struct Stats {
        uint64_t polls = 0;           ///< calls to poll() that ran (DMA enabled)
        uint64_t blocks = 0;          ///< blocks started
        uint64_t silent_blocks = 0;   ///< blocks that were all zero
        uint64_t underruns = 0;       ///< polls that found the sink empty after having played something
        uint64_t frames = 0;          ///< frames submitted
        double max_poll_gap_ms = 0;   ///< longest time between two polls
        uint64_t stalls = 0;          ///< times the sink stopped consuming and the DMA fell back to the host clock
        uint64_t stalled_frames = 0;  ///< frames consumed on the host clock instead of by the sink
    };
    const Stats& stats() const { return stats_; }
    /// The sink has stopped consuming (a stuck or suspended device): the DMA keeps its real-time pace on the
    /// host clock, the game keeps running, and the audio is discarded until the sink drains again.
    bool stalled() const { return stalled_; }
    /// How long a full sink may go without draining before it counts as stalled. Default 250 ms.
    void set_stall_ms(uint32_t ms) { stall_ms_ = ms; }
    /// Time source for the stall detection and the host-clock fallback (default: steady_clock); tests inject a simulated one.
    using ClockFn = std::function<std::chrono::steady_clock::time_point()>;
    void set_clock(ClockFn clock) { clock_ = std::move(clock); }
    /// Resets the maximum-gap counter (the others only grow).
    void reset_max_gap() { stats_.max_poll_gap_ms = 0; }

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
    Stats stats_;
    ClockFn clock_ = [] { return std::chrono::steady_clock::now(); };
    uint32_t stall_ms_ = 250;
    bool stalled_ = false;
    size_t last_queued_ = 0;
    double virtual_queue_ = 0;  // frames "queued" while stalled (host-clock model of the sink)
    std::chrono::steady_clock::time_point progress_since_{};
    std::chrono::steady_clock::time_point last_poll_{};
    std::vector<int16_t> scratch_;
};

}  // namespace gcrecomp::audio
