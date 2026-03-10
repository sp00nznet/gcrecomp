#pragma once
// =============================================================================
// gcrecomp — DOL File Parser
// =============================================================================
//
// The DOL (Dolphin Object Layout) is the GameCube's executable format.
// Every GameCube game has a main.dol containing the game's code and static data.
//
// Format:
//   - 0x100-byte header
//   - Up to 7 text (code) sections
//   - Up to 11 data sections
//   - BSS (uninitialized data) region
//   - Entry point address
//
// All values are big-endian uint32.
//
// Reference: decomp-toolkit (https://github.com/encounter/decomp-toolkit)
//            YAGCD (Yet Another GameCube Documentation)
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gcrecomp {

// DOL header: exactly 0x100 (256) bytes
struct DOLHeader {
    uint32_t text_offsets[7];    // File offsets for text (code) sections
    uint32_t data_offsets[11];   // File offsets for data sections
    uint32_t text_addresses[7]; // Load addresses (virtual) for text sections
    uint32_t data_addresses[11];// Load addresses for data sections
    uint32_t text_sizes[7];     // Sizes of text sections in bytes
    uint32_t data_sizes[11];    // Sizes of data sections in bytes
    uint32_t bss_address;       // BSS start address (zeroed at load time)
    uint32_t bss_size;          // BSS size in bytes
    uint32_t entry_point;       // Entry point address (first instruction executed)
    uint32_t padding[7];        // Unused padding to fill to 0x100 bytes
};
static_assert(sizeof(DOLHeader) == 0x100, "DOL header must be 256 bytes");

struct DOLSection {
    uint32_t file_offset;
    uint32_t address;       // Virtual address in GameCube memory (0x80XXXXXX)
    uint32_t size;
    bool     is_text;       // true = code (executable), false = data
    int      index;         // Section index (0-6 for text, 0-10 for data)
    std::vector<uint8_t> data;
};

struct DOLFile {
    DOLHeader header;
    std::vector<DOLSection> sections;
    uint32_t entry_point;
    uint32_t bss_address;
    uint32_t bss_size;

    // Flat memory image for address lookups
    std::vector<uint8_t> memory;
    uint32_t             memory_base;  // Lowest section address
    uint32_t             memory_end;   // Highest address + size

    bool load(const std::string& path);
    bool load(FILE* fp);

    uint8_t  read8(uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;

    bool is_code(uint32_t addr) const;
    void print_info() const;
};

// Byte-swap helpers (GameCube is big-endian, x86 is little-endian)
inline uint16_t bswap16(uint16_t v) {
    return (v >> 8) | (v << 8);
}
inline uint32_t bswap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |
           ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}

} // namespace gcrecomp
