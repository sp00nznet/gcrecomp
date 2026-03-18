#pragma once
// =============================================================================
// gcrecomp - RARC Archive Parser
//
// RARC (Resource ARChive) is Nintendo's hierarchical archive format used
// extensively in GameCube and Wii games. It stores files organized in a
// directory tree with the following structure:
//
//   Header (0x20 bytes):
//     "RARC" magic, archive size, data header offset, file data offsets,
//     total file count, sync file count, padding
//
//   Info Block (0x20 bytes at data_header_offset + 0x20):
//     Node count, node offset, file entry count, file entry offset,
//     string table size, string table offset, next file ID, sync flag
//
//   Nodes: Directory entries (0x10 bytes each)
//     4-byte type tag, name offset (into string table), name hash,
//     entry count, first entry index
//
//   File Entries (0x14 bytes each):
//     File ID, name hash, type (0x0200=dir, 0x1100=file), name offset,
//     data offset, data size, padding
//
// All multi-byte values are big-endian.
//
// Games using RARC: Wind Waker, Twilight Princess, Mario Sunshine,
// Luigi's Mansion, Pikmin, etc.
//
// Reference: wwlib, oead, J3DUlern RARC documentation
// =============================================================================

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace gcrecomp {

// A single file entry within an RARC archive
struct RARCFile {
    std::string name;       // File name (from string table)
    std::string path;       // Full path within archive (e.g. "res/Stage.bsv")
    uint16_t    file_id;    // File index
    uint32_t    data_offset;// Offset of file data from start of archive data section
    uint32_t    data_size;  // Size of file data in bytes
    bool        is_dir;     // True if this is a directory entry (. or ..)
};

// Parsed RARC archive - provides access to the file listing and data
struct RARCArchive {
    std::vector<RARCFile> files;

    // Find a file by name (not path). Returns nullptr if not found.
    const RARCFile* find(const char* name) const;

    // Find a file by full path. Returns nullptr if not found.
    const RARCFile* find_path(const char* path) const;

    // Get a pointer to a file's data within the archive buffer.
    // `archive_data` is the full decompressed archive buffer.
    const uint8_t* file_data(const RARCFile& file,
                             const uint8_t* archive_data,
                             size_t archive_size) const;
};

// Check if a buffer starts with the RARC magic header.
bool rarc_is_archive(const uint8_t* data, size_t size);

// Parse an RARC archive from a buffer. The buffer must be the full
// decompressed archive (after Yaz0 decompression if applicable).
// Returns true on success, populating `out` with the file listing.
bool rarc_parse(const uint8_t* data, size_t size, RARCArchive& out);

} // namespace gcrecomp
