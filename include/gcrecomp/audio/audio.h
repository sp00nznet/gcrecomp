#pragma once
// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// Audio System Header
//
// The GameCube audio subsystem uses a dedicated Macronix DSP running at
// ~81 MHz to decode and mix audio. Games typically use Nintendo's
// MusyX/JAudio middleware, which streams DSP ADPCM-encoded samples at
// 32 kHz through a 64-voice mixer.
//
// This module provides:
//   - DSP ADPCM decoding (4-bit compressed to 16-bit PCM)
//   - Multi-voice mixing with volume, pan, and pitch control
//   - Platform audio output (XAudio2 on Windows)
//
// The implementation is game-agnostic and can be used with any
// statically recompiled GameCube title.
//
// References:
//   - libogc (devkitPro) — DSP ADPCM coefficient structure and AI/DSP API
//   - Pureikyubu — DSP hardware documentation and ADPCM format
//   - YAGCD — Audio Interface (AI) and DSP register documentation
// =============================================================================

#include <cstdint>
#include <vector>
#include <string>

namespace gcrecomp::audio {

/// GameCube DSP ADPCM codec information block.
/// Contains the 16 prediction coefficients (8 pairs) and initial/loop
/// state needed to decode a DSP ADPCM stream.
///
/// The DSP ADPCM format encodes audio as 4-bit nibbles with adaptive
/// prediction. Each block of 14 samples is preceded by a predictor/scale
/// byte that selects one of 8 coefficient pairs and a scale factor.
///
/// Reference: libogc DSPADPCMInfo structure; Pureikyubu DSP ADPCM docs.
struct DSPADPCMInfo {
    int16_t coef[16];      ///< 8 pairs of ADPCM prediction coefficients
    uint16_t gain;         ///< Gain (typically 0, reserved)
    uint16_t pred_scale;   ///< Initial predictor/scale byte
    int16_t yn1;           ///< Initial history sample yn[-1]
    int16_t yn2;           ///< Initial history sample yn[-2]
    uint16_t loop_pred_scale; ///< Predictor/scale at loop point
    int16_t loop_yn1;      ///< History yn[-1] at loop point
    int16_t loop_yn2;      ///< History yn[-2] at loop point
};

/// Decode a DSP ADPCM stream to signed 16-bit PCM samples.
/// @param data Pointer to the encoded ADPCM data
/// @param num_samples Number of samples to decode
/// @param info ADPCM codec info (coefficients and initial state)
/// @param loop If true, use loop state when restarting
/// @return Vector of decoded PCM16 samples
std::vector<int16_t> decode_dsp_adpcm(const uint8_t* data, uint32_t num_samples,
                                       const DSPADPCMInfo& info, bool loop = false);

/// A single audio voice (a playing sound instance).
/// The mixer processes all active voices each audio frame, combining
/// them into the stereo output buffer.
struct AudioVoice {
    int16_t* samples;      ///< Pointer to PCM16 sample data
    uint32_t num_samples;  ///< Total number of samples
    uint32_t position;     ///< Current playback position (sample index)
    float    volume;       ///< Volume multiplier (0.0 to 1.0+)
    float    pan;          ///< Stereo pan (-1.0 left, 0.0 center, 1.0 right)
    float    pitch;        ///< Playback rate multiplier (1.0 = normal speed)
    bool     looping;      ///< Whether to loop when reaching the end
    bool     playing;      ///< Whether this voice is currently active
    uint32_t loop_start;   ///< Loop start point (sample index)
    uint32_t loop_end;     ///< Loop end point (sample index)
};

/// Initialize the audio output system.
/// On Windows this creates an XAudio2 device. The GameCube's native
/// audio sample rate is 32000 Hz (some games use 48000 Hz).
/// @param sample_rate Output sample rate in Hz (default: 32000)
/// @param channels Number of output channels (default: 2 for stereo)
bool audio_init(uint32_t sample_rate = 32000, uint32_t channels = 2);

/// Shut down the audio system and release all resources.
void audio_shutdown();

/// Update the audio system (called once per frame to feed the output buffer).
void audio_update();

/// Start playing a sound on a free voice.
/// @param samples Pointer to PCM16 sample data (must remain valid while playing)
/// @param count Number of samples
/// @param volume Initial volume (default: 1.0)
/// @param pan Initial pan (default: 0.0 center)
/// @return Voice ID (0-63) or -1 if no free voices
int  audio_play(const int16_t* samples, uint32_t count, float volume = 1.0f, float pan = 0.0f);

/// Stop a playing voice immediately.
void audio_stop(int voice_id);

/// Set the volume of a playing voice.
void audio_set_volume(int voice_id, float volume);

/// Set the stereo pan of a playing voice.
void audio_set_pan(int voice_id, float pan);

/// Set the playback pitch/speed of a playing voice.
void audio_set_pitch(int voice_id, float pitch);

/// Mix all active voices into separate L/R float buffers (called by the audio system).
/// @param output_l Left channel output buffer (frame_count floats, zeroed then summed into)
/// @param output_r Right channel output buffer (frame_count floats, zeroed then summed into)
/// @param frame_count Number of sample frames to mix
void mix_voices(float* output_l, float* output_r, uint32_t frame_count);

} // namespace gcrecomp::audio
