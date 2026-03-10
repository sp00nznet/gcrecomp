// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// DSP ADPCM Decoder
//
// The GameCube uses a custom DSP ADPCM (Adaptive Differential Pulse Code
// Modulation) format for audio compression. This format was designed by
// Nintendo for the GameCube's dedicated Macronix DSP and provides good
// audio quality at a 4:1 compression ratio over 16-bit PCM.
//
// DSP ADPCM Format Details:
//
//   - Sample width: 4 bits (nibbles), decoded to 16-bit signed PCM
//   - Block size: 8 bytes = 1 header byte + 7 data bytes = 14 samples
//   - Compression ratio: 4:1 vs 16-bit PCM
//   - Prediction: 2nd-order linear prediction with 8 selectable coefficient pairs
//   - Coefficients: Pre-computed per-sound using least-squares optimization
//
// Decoding Algorithm (per sample):
//
//   1. Every 14 samples, read a predictor/scale byte:
//      - High nibble (bits 7-4): Coefficient pair index (0-7)
//      - Low nibble (bits 3-0): Scale shift count (2^n)
//
//   2. For each 4-bit nibble:
//      a. Sign-extend to a full integer (-8 to +7)
//      b. Multiply by the scale factor (2^shift)
//      c. Add the prediction: (coef1 * yn1 + coef2 * yn2 + 1024) >> 11
//      d. Clamp to signed 16-bit range [-32768, 32767]
//      e. Update history: yn2 = yn1, yn1 = output
//
// The coefficient pairs are pre-computed during encoding and stored in the
// DSPADPCMInfo structure (16 int16_t values = 8 pairs of coef1, coef2).
//
// References:
//   - libogc — DSPADPCMInfo structure definition
//   - Pureikyubu — DSP ADPCM hardware decoder documentation
//   - DSPADPCM encoder tool (Nintendo SDK) — Encoding algorithm reference
// =============================================================================

#include "gcrecomp/audio/audio.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace gcrecomp::audio {

std::vector<int16_t> decode_dsp_adpcm(const uint8_t* data, uint32_t num_samples,
                                       const DSPADPCMInfo& info, bool loop) {
    std::vector<int16_t> pcm(num_samples);

    int16_t yn1 = info.yn1;
    int16_t yn2 = info.yn2;
    uint16_t pred_scale = info.pred_scale;

    uint32_t sample_idx = 0;
    uint32_t byte_idx = 0;

    while (sample_idx < num_samples) {
        // Every 14 samples, read a new predictor/scale byte
        if (sample_idx % 14 == 0) {
            pred_scale = data[byte_idx++];
        }

        int pred = (pred_scale >> 4) & 0xF;
        int scale = 1 << (pred_scale & 0xF);
        int16_t coef1 = info.coef[pred * 2];
        int16_t coef2 = info.coef[pred * 2 + 1];

        // Process 14 nibbles (7 bytes)
        for (int n = 0; n < 14 && sample_idx < num_samples; n++) {
            int nibble;
            if (n % 2 == 0) {
                nibble = (data[byte_idx] >> 4) & 0xF;
            } else {
                nibble = data[byte_idx] & 0xF;
                byte_idx++;
            }

            // Sign extend 4-bit nibble
            if (nibble >= 8) nibble -= 16;

            // Predict and scale
            int32_t sample = (scale * nibble) +
                             ((coef1 * (int32_t)yn1 + coef2 * (int32_t)yn2 + 1024) >> 11);

            // Clamp to 16-bit
            sample = std::clamp(sample, -32768, 32767);

            pcm[sample_idx++] = (int16_t)sample;
            yn2 = yn1;
            yn1 = (int16_t)sample;
        }
    }

    return pcm;
}

} // namespace gcrecomp::audio
