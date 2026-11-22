// SPDX-License-Identifier: MIT
//
// NullSink: the sink for tests and for machines without an audio device.
#include "gcrecomp/audio/audio_sink.h"

#include <algorithm>

namespace gcrecomp::audio {

bool NullSink::open(uint32_t sample_rate) {
    open_ = true;
    rate_ = sample_rate;
    queued_ = 0.0;
    last_ = std::chrono::steady_clock::now();
    return true;
}

bool NullSink::set_sample_rate(uint32_t sample_rate) {
    drain();
    rate_ = sample_rate;
    return true;
}

void NullSink::submit(const int16_t* interleaved, size_t frames) {
    drain();
    recorded_.insert(recorded_.end(), interleaved, interleaved + frames * 2);
    submit_rates_.push_back(rate_);
    queued_ += double(frames);
}

size_t NullSink::queued_frames() {
    drain();
    return size_t(queued_);
}

void NullSink::consume(size_t frames) { queued_ = std::max(0.0, queued_ - double(frames)); }

void NullSink::drain() {
    if (!realtime_) return;
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    queued_ = std::max(0.0, queued_ - seconds * rate_);
}

}  // namespace gcrecomp::audio
