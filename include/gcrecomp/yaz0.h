#pragma once
// =============================================================================
// gcrecomp - Yaz0 Decompression
//
// Nintendo's Yaz0 is an LZ-based compression format used across GameCube,
// Wii, and other Nintendo platforms. Files with .szs extension or "Yaz0"
// magic header use this encoding. Common in:
//   - Stage archives (Stage.arc, Room*.arc)
//   - Model data, textures, UI layouts
//   - REL modules (*.rel.szs)
//
// Format:
//   Bytes 0-3:   "Yaz0" magic
//   Bytes 4-7:   Decompressed size (big-endian u32)
//   Bytes 8-15:  Reserved (alignment/padding)
//   Bytes 16+:   Compressed data stream
//
// The data stream uses a flag-byte scheme: each flag byte controls 8
// operations. Bit=1 means copy one literal byte. Bit=0 means copy from
// a back-reference (distance + length encoded in 2-3 bytes).
//
// Reference: yaz0.h from various open-source Dolphin/GC tools
// =============================================================================

#include <cstdint>
#include <cstddef>
#include <vector>

namespace gcrecomp {

// Check if a buffer starts with the Yaz0 magic header.
bool yaz0_is_compressed(const uint8_t* data, size_t size);

// Read the decompressed size from a Yaz0 header.
// Returns 0 if the data is not valid Yaz0.
uint32_t yaz0_decompressed_size(const uint8_t* data, size_t size);

// Decompress Yaz0 data into the provided output buffer.
// `dst` must be at least yaz0_decompressed_size() bytes.
// Returns the number of bytes written, or 0 on failure.
size_t yaz0_decompress(const uint8_t* src, size_t src_size,
                       uint8_t* dst, size_t dst_size);

// Convenience: decompress into a newly allocated vector.
// Returns an empty vector on failure.
std::vector<uint8_t> yaz0_decompress(const uint8_t* src, size_t src_size);

} // namespace gcrecomp
