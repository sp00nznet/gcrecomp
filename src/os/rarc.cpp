// =============================================================================
// gcrecomp - RARC Archive Parser
// =============================================================================

#include "gcrecomp/rarc.h"
#include <cstdio>
#include <cstring>

namespace gcrecomp {

// Big-endian readers
static inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// Read a null-terminated string from the string table
static std::string read_string(const uint8_t* str_table, size_t str_table_size,
                               uint32_t offset) {
    if (offset >= str_table_size) return "";
    const char* s = reinterpret_cast<const char*>(str_table + offset);
    size_t max_len = str_table_size - offset;
    size_t len = strnlen(s, max_len);
    return std::string(s, len);
}

bool rarc_is_archive(const uint8_t* data, size_t size) {
    return size >= 0x20 && data[0] == 'R' && data[1] == 'A' &&
           data[2] == 'R' && data[3] == 'C';
}

// RARC Header layout (all offsets from start of file):
//   0x00: "RARC" magic
//   0x04: File size
//   0x08: Data header offset (always 0x20)
//   0x0C: File data offset (relative to 0x20)
//   0x10: File data length
//   0x14: File data length 2 (mram)
//   0x18: File data length 3 (aram)
//   0x1C: Padding (0)
//
// Info block (at data_header_offset + 0x20 = 0x40... actually at 0x20):
//   Wait - let me re-check. The header at 0x00-0x1F has the RARC header.
//   Then at 0x20 is the "data header" / info block:
//   0x20: Node count
//   0x24: Node table offset (relative to 0x20)
//   0x28: File entry count
//   0x2C: File entry offset (relative to 0x20)
//   0x30: String table size
//   0x34: String table offset (relative to 0x20)
//   0x38: Next file ID
//   0x3A: Sync flag
//   0x3C: Padding

bool rarc_parse(const uint8_t* data, size_t size, RARCArchive& out) {
    out.files.clear();

    if (!rarc_is_archive(data, size)) {
        fprintf(stderr, "[RARC] Invalid header\n");
        return false;
    }

    uint32_t archive_size = read_be32(data + 0x04);
    uint32_t data_header_off = read_be32(data + 0x08); // Usually 0x20
    uint32_t file_data_off = read_be32(data + 0x0C) + data_header_off;

    // Info block starts at 0x20
    const uint8_t* info = data + 0x20;
    if (0x20 + 0x20 > size) {
        fprintf(stderr, "[RARC] File too small for info block\n");
        return false;
    }

    uint32_t node_count     = read_be32(info + 0x00);
    uint32_t node_off       = read_be32(info + 0x04) + 0x20;
    uint32_t file_count     = read_be32(info + 0x08);
    uint32_t file_entry_off = read_be32(info + 0x0C) + 0x20;
    uint32_t str_table_size = read_be32(info + 0x10);
    uint32_t str_table_off  = read_be32(info + 0x14) + 0x20;

    fprintf(stderr, "[RARC] %u nodes, %u file entries, string table %u bytes\n",
            node_count, file_count, str_table_size);

    // Validate offsets
    if (node_off + node_count * 0x10 > size ||
        file_entry_off + file_count * 0x14 > size ||
        str_table_off + str_table_size > size) {
        fprintf(stderr, "[RARC] Offsets exceed archive size\n");
        return false;
    }

    const uint8_t* str_table = data + str_table_off;

    // Parse nodes to build directory paths
    // Each node is 0x10 bytes:
    //   0x00: 4-byte type identifier (e.g. "ROOT", "SCNE", etc.)
    //   0x04: Name offset (into string table)
    //   0x08: Name hash (u16) + entry count (u16)
    //   0x0C: First file entry index (u32)
    struct Node {
        std::string name;
        uint32_t first_entry;
        uint16_t entry_count;
    };
    std::vector<Node> nodes(node_count);

    for (uint32_t i = 0; i < node_count; i++) {
        const uint8_t* n = data + node_off + i * 0x10;
        uint32_t name_off = read_be32(n + 0x04);
        nodes[i].name = read_string(str_table, str_table_size, name_off);
        nodes[i].entry_count = read_be16(n + 0x0A);
        nodes[i].first_entry = read_be32(n + 0x0C);
    }

    // Recursive directory traversal using a stack
    struct DirFrame {
        uint32_t node_idx;
        std::string path;
    };
    std::vector<DirFrame> stack;
    if (node_count > 0) {
        stack.push_back({0, ""});
    }

    while (!stack.empty()) {
        DirFrame frame = stack.back();
        stack.pop_back();

        if (frame.node_idx >= node_count) continue;
        const Node& node = nodes[frame.node_idx];

        for (uint16_t i = 0; i < node.entry_count; i++) {
            uint32_t entry_idx = node.first_entry + i;
            if (entry_idx >= file_count) break;

            const uint8_t* e = data + file_entry_off + entry_idx * 0x14;
            uint16_t file_id   = read_be16(e + 0x00);
            uint16_t name_hash = read_be16(e + 0x02);
            uint16_t type      = read_be16(e + 0x04);
            uint16_t name_off  = read_be16(e + 0x06);
            uint32_t data_off  = read_be32(e + 0x08);
            uint32_t data_sz   = read_be32(e + 0x0C);

            std::string name = read_string(str_table, str_table_size, name_off);

            bool is_dir = (type & 0x0200) != 0;

            // Skip . and .. entries
            if (is_dir && (name == "." || name == "..")) continue;

            std::string full_path = frame.path.empty()
                ? name
                : frame.path + "/" + name;

            if (is_dir) {
                // data_off field for directories = index of the child node
                uint32_t child_node = data_off;
                stack.push_back({child_node, full_path});
            } else {
                RARCFile file;
                file.name = name;
                file.path = full_path;
                file.file_id = file_id;
                file.data_offset = data_off;
                file.data_size = data_sz;
                file.is_dir = false;
                out.files.push_back(std::move(file));
            }
        }
    }

    fprintf(stderr, "[RARC] Parsed %zu files\n", out.files.size());
    return true;
}

const RARCFile* RARCArchive::find(const char* name) const {
    for (const auto& f : files) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

const RARCFile* RARCArchive::find_path(const char* path) const {
    for (const auto& f : files) {
        if (f.path == path) return &f;
    }
    return nullptr;
}

const uint8_t* RARCArchive::file_data(const RARCFile& file,
                                       const uint8_t* archive_data,
                                       size_t archive_size) const {
    // File data section starts after the RARC header
    // data_header_off (0x08) + file_data_off (0x0C)
    uint32_t data_header_off = read_be32(archive_data + 0x08);
    uint32_t file_data_start = read_be32(archive_data + 0x0C) + data_header_off;

    uint32_t abs_offset = file_data_start + file.data_offset;
    if (abs_offset + file.data_size > archive_size) {
        fprintf(stderr, "[RARC] File '%s' data exceeds archive bounds\n",
                file.name.c_str());
        return nullptr;
    }
    return archive_data + abs_offset;
}

} // namespace gcrecomp
