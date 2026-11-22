// SPDX-License-Identifier: MIT
//
// SDL3 audio sink: one playback stream on the default device. SDL converts and resamples from
// the AI's 32 or 48 kHz to whatever the device runs at.
#include "gcrecomp/audio/audio_sink.h"

#ifdef GCRECOMP_HAVE_SDL3
#include <SDL3/SDL.h>

#include <cstdio>

namespace gcrecomp::audio {

namespace {

class SdlSink final : public AudioSink {
public:
    ~SdlSink() override { close(); }

    bool open(uint32_t sample_rate) override {
        close();
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
            return false;
        }
        inited_ = true;
        const SDL_AudioSpec spec = make_spec(sample_rate);
        stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream_) {
            std::fprintf(stderr, "[audio] cannot open the playback device: %s\n", SDL_GetError());
            close();
            return false;
        }
        rate_ = sample_rate;
        SDL_ResumeAudioStreamDevice(stream_);
        return true;
    }

    void close() override {
        if (stream_) SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
        if (inited_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        inited_ = false;
    }

    bool set_sample_rate(uint32_t sample_rate) override {
        if (!stream_) return false;
        if (sample_rate == rate_) return true;
        const SDL_AudioSpec src = make_spec(sample_rate);
        if (!SDL_SetAudioStreamFormat(stream_, &src, nullptr)) return false;
        rate_ = sample_rate;
        return true;
    }

    void submit(const int16_t* interleaved, size_t frames) override {
        if (stream_) SDL_PutAudioStreamData(stream_, interleaved, int(frames * 4));
    }

    size_t queued_frames() override {
        if (!stream_) return 0;
        const int bytes = SDL_GetAudioStreamQueued(stream_);
        return bytes > 0 ? size_t(bytes) / 4 : 0;
    }

private:
    static SDL_AudioSpec make_spec(uint32_t rate) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_S16;  // host-endian
        spec.channels = 2;
        spec.freq = int(rate);
        return spec;
    }

    SDL_AudioStream* stream_ = nullptr;
    bool inited_ = false;
    uint32_t rate_ = 0;
};

}  // namespace

std::unique_ptr<AudioSink> make_sdl_sink() { return std::make_unique<SdlSink>(); }

}  // namespace gcrecomp::audio

#else

namespace gcrecomp::audio {
std::unique_ptr<AudioSink> make_sdl_sink() { return nullptr; }
}  // namespace gcrecomp::audio

#endif
