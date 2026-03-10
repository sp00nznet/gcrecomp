// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// Audio System - XAudio2 Backend (Windows)
//
// Outputs mixed game audio to the speakers via XAudio2. The GameCube's
// native audio output is 32000 Hz stereo, though some games use 48000 Hz.
//
// Uses double-buffered submission: while XAudio2 plays one buffer, we mix
// the next one. The source voice callback is not used; instead audio_update()
// polls BuffersQueued to know when a new buffer can be submitted.
//
// References:
//   - libogc — AI (Audio Interface) initialization and sample rate
//   - Microsoft XAudio2 SDK — Audio output API
// =============================================================================

#include "gcrecomp/audio/audio.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#endif

namespace gcrecomp::audio {

static bool g_audio_initialized = false;
static uint32_t g_sample_rate = 32000;
static uint32_t g_channels = 2;

#ifdef _WIN32

// XAudio2 state
static IXAudio2* g_xaudio2 = nullptr;
static IXAudio2MasteringVoice* g_mastering_voice = nullptr;
static IXAudio2SourceVoice* g_source_voice = nullptr;

// Double-buffered audio output.
// Each buffer holds AUDIO_BUFFER_SAMPLES frames of interleaved stereo int16.
static const uint32_t AUDIO_BUFFER_SAMPLES = 1024;
static int16_t g_audio_buffers[2][AUDIO_BUFFER_SAMPLES * 2]; // stereo interleaved
static int g_current_buffer = 0;

// Temporary float mix buffers (L/R separate, converted to interleaved int16 after mixing)
static float g_mix_l[AUDIO_BUFFER_SAMPLES];
static float g_mix_r[AUDIO_BUFFER_SAMPLES];

bool audio_init(uint32_t sample_rate, uint32_t channels) {
    g_sample_rate = sample_rate;
    g_channels = channels;
    printf("[Audio] Initializing (%u Hz, %u channels)\n", sample_rate, channels);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        printf("[Audio] CoInitializeEx failed: 0x%08lX\n", hr);
        return false;
    }

    hr = XAudio2Create(&g_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        printf("[Audio] XAudio2Create failed: 0x%08lX\n", hr);
        CoUninitialize();
        return false;
    }

    hr = g_xaudio2->CreateMasteringVoice(&g_mastering_voice);
    if (FAILED(hr)) {
        printf("[Audio] CreateMasteringVoice failed: 0x%08lX\n", hr);
        g_xaudio2->Release();
        g_xaudio2 = nullptr;
        CoUninitialize();
        return false;
    }

    // Set up the wave format for PCM16 stereo at the requested sample rate
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channels);
    wfx.nSamplesPerSec = sample_rate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(channels * sizeof(int16_t));
    wfx.nAvgBytesPerSec = sample_rate * wfx.nBlockAlign;

    hr = g_xaudio2->CreateSourceVoice(&g_source_voice, &wfx);
    if (FAILED(hr)) {
        printf("[Audio] CreateSourceVoice failed: 0x%08lX\n", hr);
        g_mastering_voice->DestroyVoice();
        g_mastering_voice = nullptr;
        g_xaudio2->Release();
        g_xaudio2 = nullptr;
        CoUninitialize();
        return false;
    }

    // Clear the double buffers
    memset(g_audio_buffers, 0, sizeof(g_audio_buffers));
    g_current_buffer = 0;

    // Start the source voice (it will play submitted buffers as they arrive)
    g_source_voice->Start(0);

    g_audio_initialized = true;
    printf("[Audio] XAudio2 backend ready.\n");
    return true;
}

void audio_shutdown() {
    if (!g_audio_initialized) return;

    if (g_source_voice) {
        g_source_voice->Stop(0);
        g_source_voice->FlushSourceBuffers();
        g_source_voice->DestroyVoice();
        g_source_voice = nullptr;
    }

    if (g_mastering_voice) {
        g_mastering_voice->DestroyVoice();
        g_mastering_voice = nullptr;
    }

    if (g_xaudio2) {
        g_xaudio2->Release();
        g_xaudio2 = nullptr;
    }

    CoUninitialize();
    g_audio_initialized = false;
    printf("[Audio] Shutdown.\n");
}

void audio_update() {
    if (!g_audio_initialized || !g_source_voice) return;

    // Check how many buffers are currently queued on the source voice.
    // We use double-buffering, so only submit when fewer than 2 are queued.
    XAUDIO2_VOICE_STATE state;
    g_source_voice->GetState(&state);
    if (state.BuffersQueued >= 2) {
        return; // Both buffers are still in the queue; nothing to do this frame
    }

    // Mix all active voices into the float L/R buffers
    mix_voices(g_mix_l, g_mix_r, AUDIO_BUFFER_SAMPLES);

    // Convert from float L/R to interleaved int16 stereo
    int16_t* dest = g_audio_buffers[g_current_buffer];
    for (uint32_t i = 0; i < AUDIO_BUFFER_SAMPLES; i++) {
        // Clamp to [-1.0, 1.0] and convert to int16 range
        float l = std::clamp(g_mix_l[i], -1.0f, 1.0f);
        float r = std::clamp(g_mix_r[i], -1.0f, 1.0f);
        dest[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
        dest[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
    }

    // Submit the buffer to XAudio2
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = AUDIO_BUFFER_SAMPLES * g_channels * sizeof(int16_t);
    buf.pAudioData = reinterpret_cast<const BYTE*>(dest);

    HRESULT hr = g_source_voice->SubmitSourceBuffer(&buf);
    if (FAILED(hr)) {
        printf("[Audio] SubmitSourceBuffer failed: 0x%08lX\n", hr);
    }

    // Swap to the other buffer for next time
    g_current_buffer ^= 1;
}

#else
// Non-Windows stub (no audio output)

bool audio_init(uint32_t sample_rate, uint32_t channels) {
    g_sample_rate = sample_rate;
    g_channels = channels;
    printf("[Audio] No audio backend available on this platform (stub).\n");
    g_audio_initialized = true;
    return true;
}

void audio_shutdown() {
    g_audio_initialized = false;
    printf("[Audio] Shutdown (stub).\n");
}

void audio_update() {
    // No-op on non-Windows platforms
}

#endif // _WIN32

} // namespace gcrecomp::audio
