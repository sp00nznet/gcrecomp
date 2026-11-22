// SPDX-License-Identifier: MIT
//
// GameCube memory card image: format, directory, block allocation table, file data.
//
// Independent of the guest runtime: it works on a byte buffer that mirrors the raw card image used
// by emulators (no header). The on-card structures follow the public documentation of the format:
//   - YAGCD (Yet Another GameCube Documentation), "Memory Card Format" chapter;
//   - the libogc and Dolphin SDK-compatible behaviour for the CARD_RESULT_* codes.
//
// Layout (8 KiB blocks):
//   block 0      header      serial, format time, SRAM bias/language, device id, size in Mbit,
//                            encoding; checksum pair at 0x1FC/0x1FE over bytes 0x000..0x1FB
//   block 1, 2   directory   two copies; 127 entries of 0x40 bytes, then padding, update counter
//                            (0x1FFA), checksum pair (0x1FFC/0x1FFE) over bytes 0x0000..0x1FFB
//   block 3, 4   block map   two copies; checksum pair at 0/2 over bytes 4..0x1FFF, update counter
//                            (4), free block count (6), last allocated block (8), then one u16 per
//                            usable block (entry i describes block i + 5): next block, 0xFFFF
//                            for the end of a chain, 0 for a free block
//   block 5..    file data
// All integers are big-endian. The copy of a directory or block map with the newer update counter
// is current; an update is written to the other copy and then becomes current.
//
// Sizes: 0x80000 (59 usable blocks), 0x100000 (123), 0x200000 (251). A single save can be exported
// to and imported from a .gci file: the 0x40-byte directory entry followed by the file's blocks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gcrecomp::card {

constexpr uint32_t kBlockSize = 0x2000;
constexpr uint32_t kSystemBlocks = 5;
constexpr uint32_t kDirEntries = 127;
constexpr uint32_t kDirEntrySize = 0x40;
constexpr uint32_t kFileNameLen = 32;
constexpr uint16_t kChainEnd = 0xFFFF;
constexpr uint32_t kGameIdLen = 6;  // game code (4) + maker code (2)

// SDK result codes (CARD_RESULT_*).
enum Result : int32_t {
    kReady = 0,
    kBusy = -1,
    kWrongDevice = -2,
    kNoCard = -3,
    kNoFile = -4,
    kIoError = -5,
    kBroken = -6,
    kExist = -7,
    kNoEnt = -8,
    kInsSpace = -9,
    kNoPerm = -10,
    kLimit = -11,
    kNameTooLong = -12,
    kEncoding = -13,
    kCanceled = -14,
    kFatalError = -128,
};

// Directory permission bits.
constexpr uint8_t kAttrPublic = 0x04;

// One directory entry (0x40 bytes on the card).
struct DirEntry {
    uint8_t game_code[4];
    uint8_t maker_code[2];
    uint8_t banner_format;  // bits 0..1: 0 none, 1 C8, 2 RGB5A3
    char name[kFileNameLen];
    uint32_t time;  // seconds since 2000-01-01
    uint32_t icon_addr;
    uint16_t icon_format;  // 2 bits per icon
    uint16_t icon_speed;   // 2 bits per icon
    uint8_t permission;
    uint8_t copy_counter;
    uint16_t first_block;
    uint16_t block_count;
    uint32_t comment_addr;

    bool in_use() const { return game_code[0] != 0xFF; }
    void parse(const uint8_t* p);
    void store(uint8_t* p) const;
};

struct FormatParams {
    uint64_t format_time = 0;  // OSTime (40.5 MHz ticks since 2000-01-01) of the format
    uint32_t sram_bias = 0;
    uint32_t sram_language = 0;
    uint16_t encoding = 0;  // 0 ANSI, 1 Shift-JIS
    uint32_t serial_seed = 0;
};

// Checksum pair over 16-bit big-endian words: sum, and sum of the complements. A result of 0xFFFF
// is stored as 0 (as the SDK does).
void checksum(const uint8_t* p, size_t bytes, uint16_t* sum, uint16_t* inv);

// True for the three sizes the format defines.
bool valid_image_size(size_t bytes);

class CardImage {
public:
    // A blank (unformatted, all 0xFF) image of `size_bytes`; nothing is written to `path` until the
    // first change. Fails if the size is not one of the three defined sizes.
    bool create_blank(const std::string& path, size_t size_bytes);
    // Adopt an in-memory image (no file); used by tests and by `load`.
    bool adopt(std::vector<uint8_t> bytes);
    // Read the image from `path`. Fails if the file cannot be read or has an invalid size.
    bool load(const std::string& path);
    // Write the blocks changed since the last flush (the whole image the first time) to the path.
    bool flush();

    size_t size_bytes() const { return data_.size(); }
    uint32_t total_blocks() const { return (uint32_t)(data_.size() / kBlockSize); }
    uint32_t size_mbit() const { return (uint32_t)(data_.size() / 0x20000); }
    const std::vector<uint8_t>& bytes() const { return data_; }
    const std::string& path() const { return path_; }

    // Erase everything and write a fresh, empty format. Leaves the card mounted.
    int format(const FormatParams& params);
    // Verify header, pick the current directory and block map (repairing a damaged copy from the
    // good one) and check chains against the directory. kBroken if the card is unformatted or
    // damaged beyond that. Required before any file operation.
    int mount();
    bool mounted() const { return loaded_; }
    uint16_t encoding() const;

    // Files. `game_id` is the 6-byte game code + maker code of the running game; an entry whose id
    // differs is not accessible (kNoPerm) unless the id is all zero.
    int create(const char* name, size_t name_len, const uint8_t* game_id, uint32_t size, uint32_t now,
               int* file_no);
    int find(const char* name, size_t name_len, const uint8_t* game_id, int* file_no) const;
    int check_access(int file_no, const uint8_t* game_id) const;
    int remove(int file_no, const uint8_t* game_id);
    int rename(int file_no, const char* new_name, size_t new_len, const uint8_t* game_id);
    int get_entry(int file_no, DirEntry* out) const;
    // Update banner/icon/comment fields; also stamps the modification time.
    int set_status(int file_no, const uint8_t* game_id, uint8_t banner_format, uint32_t icon_addr,
                   uint16_t icon_format, uint16_t icon_speed, uint32_t comment_addr, uint32_t now);
    // Offsets are relative to the file's data. Length multiples of 512 for reads, of 8 KiB for writes.
    int read(int file_no, uint8_t* dst, uint32_t length, uint32_t offset) const;
    int write(int file_no, const uint8_t* src, uint32_t length, uint32_t offset, uint32_t now);

    uint32_t free_blocks() const;
    uint32_t free_entries() const;

    // .gci: directory entry (0x40 bytes) followed by the file's blocks.
    int export_gci(int file_no, std::vector<uint8_t>* out) const;
    int import_gci(const uint8_t* gci, size_t size);

private:
    uint8_t* block(uint32_t n) { return data_.data() + (size_t)n * kBlockSize; }
    const uint8_t* block(uint32_t n) const { return data_.data() + (size_t)n * kBlockSize; }
    void touch(uint32_t n);
    bool header_valid() const;
    bool dir_valid(uint32_t n) const;
    bool fat_valid(uint32_t n) const;
    void commit_dir();
    void commit_fat();
    bool chains_valid() const;
    uint8_t* entry_ptr(int file_no) { return dir_.data() + (size_t)file_no * kDirEntrySize; }
    const uint8_t* entry_ptr(int file_no) const { return dir_.data() + (size_t)file_no * kDirEntrySize; }
    uint16_t fat_get(uint32_t blk) const;
    void fat_set(uint32_t blk, uint16_t v);
    uint16_t fat_free_count() const;
    void fat_set_free_count(uint16_t v);
    bool access_ok(const DirEntry& e, const uint8_t* game_id) const;
    // Allocate `count` blocks as a chain; returns the first block or 0xFFFF if not enough free.
    uint16_t alloc_chain(uint16_t count);
    void free_chain(uint16_t first);

    std::vector<uint8_t> data_;
    std::string path_;
    std::vector<uint8_t> dirty_;  // per block
    bool file_exists_ = false;
    bool loaded_ = false;
    uint32_t dir_block_ = 1;  // block holding the current directory copy
    uint32_t fat_block_ = 3;
    std::vector<uint8_t> dir_;  // working copies of the current directory and block map
    std::vector<uint8_t> fat_;
};

}  // namespace gcrecomp::card
