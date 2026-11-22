#pragma once
// SPDX-License-Identifier: MIT
//
// Audio output behind an interface: an SDL3 sink for playing, a null sink for tests and for
// machines without an audio device.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gcrecomp::audio {

/// Destination of the interleaved stereo PCM the AI DMA produces.
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /// Starts (or restarts) the output at `sample_rate` Hz, 16-bit stereo. Returns false when the
    /// device cannot be opened.
    virtual bool open(uint32_t sample_rate) = 0;
    virtual void close() = 0;

    /// Changes the rate of the data submitted from now on. Data already queued keeps its rate.
    virtual bool set_sample_rate(uint32_t sample_rate) = 0;

    /// Queues `frames` stereo frames (L, R, L, R ... as host-endian int16) for playback.
    virtual void submit(const int16_t* interleaved, size_t frames) = 0;

    /// Frames submitted and not played yet. The AI emulation paces itself on this value.
    virtual size_t queued_frames() = 0;
};

/// A sink that plays nothing. In manual mode (tests) frames leave the queue only through
/// `consume()` and everything submitted is recorded. In real-time mode the queue drains at the
/// sample rate, so the game's audio callbacks keep their real-time pace without a device.
class NullSink final : public AudioSink {
public:
    explicit NullSink(bool realtime = false) : realtime_(realtime) {}

    bool open(uint32_t sample_rate) override;
    void close() override { open_ = false; }
    bool set_sample_rate(uint32_t sample_rate) override;
    void submit(const int16_t* interleaved, size_t frames) override;
    size_t queued_frames() override;

    /// Manual mode: removes `frames` frames from the queue (as if they had been played).
    void consume(size_t frames);

    bool is_open() const { return open_; }
    uint32_t sample_rate() const { return rate_; }
    /// Every sample submitted so far, interleaved, host-endian.
    const std::vector<int16_t>& recorded() const { return recorded_; }
    /// Sample rate in effect for each submit() call, in order.
    const std::vector<uint32_t>& submit_rates() const { return submit_rates_; }

private:
    void drain();

    bool realtime_;
    bool open_ = false;
    uint32_t rate_ = 32000;
    double queued_ = 0.0;
    std::chrono::steady_clock::time_point last_;
    std::vector<int16_t> recorded_;
    std::vector<uint32_t> submit_rates_;
};

/// Wraps `inner` so that everything submitted is also written to `path` as a 16-bit stereo WAV
/// file (one rate for the whole file: the rate at the first submit). The header is kept valid
/// after every block, so a killed process leaves a playable file. Returns `inner` unchanged when
/// the file cannot be created.
std::unique_ptr<AudioSink> make_wav_dump_sink(std::unique_ptr<AudioSink> inner, const std::string& path);

/// SDL3 sink (default playback device, SDL does the resampling to the device rate). Returns
/// nullptr when the library was built without SDL3.
std::unique_ptr<AudioSink> make_sdl_sink();

}  // namespace gcrecomp::audio
