// SPDX-License-Identifier: MIT
//
// WAV dump: a sink decorator that also writes what is played to a file, to check the audio
// without listening (GCRECOMP_DUMP_AUDIO=file.wav).
#include <cstdio>

#include "gcrecomp/audio/audio_sink.h"

namespace gcrecomp::audio {

namespace {

constexpr uint32_t kHeaderBytes = 44;

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}
void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

class WavDumpSink final : public AudioSink {
public:
    WavDumpSink(std::unique_ptr<AudioSink> inner, std::FILE* file) : inner_(std::move(inner)), file_(file) {}
    ~WavDumpSink() override { finish(); }

    bool open(uint32_t sample_rate) override {
        rate_ = sample_rate;
        return inner_->open(sample_rate);
    }
    void close() override {
        finish();
        inner_->close();
    }
    bool set_sample_rate(uint32_t sample_rate) override {
        if (!started_) rate_ = sample_rate;
        return inner_->set_sample_rate(sample_rate);
    }
    void submit(const int16_t* interleaved, size_t frames) override {
        inner_->submit(interleaved, frames);
        if (!file_) return;
        if (!started_) {
            started_ = true;
            write_header();
        }
        uint8_t buf[4096];
        size_t n = frames * 2;
        const int16_t* s = interleaved;
        while (n) {
            const size_t chunk = n < sizeof buf / 2 ? n : sizeof buf / 2;
            for (size_t i = 0; i < chunk; ++i) put16(buf + 2 * i, uint16_t(s[i]));
            std::fseek(file_, 0, SEEK_END);
            std::fwrite(buf, 2, chunk, file_);
            s += chunk;
            n -= chunk;
        }
        data_bytes_ += uint32_t(frames * 4);
        write_header();
    }
    size_t queued_frames() override { return inner_->queued_frames(); }

private:
    void write_header() {
        uint8_t h[kHeaderBytes] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
        put32(h + 4, 36 + data_bytes_);
        put32(h + 16, 16);
        put16(h + 20, 1);  // PCM
        put16(h + 22, 2);  // stereo
        put32(h + 24, rate_);
        put32(h + 28, rate_ * 4);
        put16(h + 32, 4);
        put16(h + 34, 16);
        h[36] = 'd';
        h[37] = 'a';
        h[38] = 't';
        h[39] = 'a';
        put32(h + 40, data_bytes_);
        std::fseek(file_, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof h, file_);
        std::fflush(file_);
    }
    void finish() {
        if (!file_) return;
        if (!started_) write_header();
        std::fclose(file_);
        file_ = nullptr;
    }

    std::unique_ptr<AudioSink> inner_;
    std::FILE* file_;
    uint32_t rate_ = 32000;
    uint32_t data_bytes_ = 0;
    bool started_ = false;
};

}  // namespace

std::unique_ptr<AudioSink> make_wav_dump_sink(std::unique_ptr<AudioSink> inner, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[AI] cannot create %s: audio is not dumped\n", path.c_str());
        return inner;
    }
    return std::make_unique<WavDumpSink>(std::move(inner), f);
}

}  // namespace gcrecomp::audio
