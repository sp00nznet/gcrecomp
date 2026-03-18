// =============================================================================
// gcrecomp - Yaz0 Decompression
// =============================================================================

#include "gcrecomp/yaz0.h"
#include <cstdio>
#include <cstring>

namespace gcrecomp {

// Read big-endian u32
static inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

bool yaz0_is_compressed(const uint8_t* data, size_t size) {
    return size >= 16 && data[0] == 'Y' && data[1] == 'a' &&
           data[2] == 'z' && data[3] == '0';
}

uint32_t yaz0_decompressed_size(const uint8_t* data, size_t size) {
    if (!yaz0_is_compressed(data, size)) return 0;
    return read_be32(data + 4);
}

size_t yaz0_decompress(const uint8_t* src, size_t src_size,
                       uint8_t* dst, size_t dst_size) {
    if (!yaz0_is_compressed(src, src_size)) {
        fprintf(stderr, "[Yaz0] Invalid header\n");
        return 0;
    }

    uint32_t decomp_size = read_be32(src + 4);
    if (dst_size < decomp_size) {
        fprintf(stderr, "[Yaz0] Output buffer too small (%zu < %u)\n",
                dst_size, decomp_size);
        return 0;
    }

    size_t src_pos = 16;  // Skip header
    size_t dst_pos = 0;
    uint8_t flags = 0;
    int bits_left = 0;

    while (dst_pos < decomp_size && src_pos < src_size) {
        if (bits_left == 0) {
            flags = src[src_pos++];
            bits_left = 8;
        }

        if (flags & 0x80) {
            // Literal byte
            if (src_pos >= src_size) break;
            dst[dst_pos++] = src[src_pos++];
        } else {
            // Back-reference
            if (src_pos + 1 >= src_size) break;
            uint8_t b1 = src[src_pos++];
            uint8_t b2 = src[src_pos++];

            uint32_t dist = ((b1 & 0x0F) << 8) | b2;
            uint32_t length;

            if ((b1 >> 4) == 0) {
                // 3-byte back-reference: length in next byte + 0x12
                if (src_pos >= src_size) break;
                length = src[src_pos++] + 0x12;
            } else {
                // 2-byte back-reference: length = upper nibble + 2
                length = (b1 >> 4) + 2;
            }

            // Copy from already-decompressed data
            size_t copy_src = dst_pos - (dist + 1);
            if (copy_src >= dst_pos) {
                fprintf(stderr, "[Yaz0] Invalid back-reference at src offset %zu\n",
                        src_pos);
                return 0;
            }

            for (uint32_t i = 0; i < length && dst_pos < decomp_size; i++) {
                dst[dst_pos++] = dst[copy_src + i];
            }
        }

        flags <<= 1;
        bits_left--;
    }

    if (dst_pos != decomp_size) {
        fprintf(stderr, "[Yaz0] Decompressed %zu bytes, expected %u\n",
                dst_pos, decomp_size);
    }

    return dst_pos;
}

std::vector<uint8_t> yaz0_decompress(const uint8_t* src, size_t src_size) {
    uint32_t decomp_size = yaz0_decompressed_size(src, src_size);
    if (decomp_size == 0) return {};

    std::vector<uint8_t> out(decomp_size);
    size_t written = yaz0_decompress(src, src_size, out.data(), out.size());
    if (written != decomp_size) {
        return {};
    }
    return out;
}

} // namespace gcrecomp
