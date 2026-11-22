// SPDX-License-Identifier: MIT
//
// Memory card image. See card_image.h for the format and its references.
#include "card_image.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace gcrecomp::card {

namespace {

constexpr uint32_t kHeaderBlock = 0;
constexpr uint32_t kDirBlock0 = 1;
constexpr uint32_t kDirBlock1 = 2;
constexpr uint32_t kFatBlock0 = 3;
constexpr uint32_t kFatBlock1 = 4;

constexpr uint32_t kHeaderChecksumOff = 0x1FC;
constexpr uint32_t kDirCounterOff = 0x1FFA;
constexpr uint32_t kDirChecksumOff = 0x1FFC;
constexpr uint32_t kFatCounterOff = 4;
constexpr uint32_t kFatFreeOff = 6;
constexpr uint32_t kFatLastAllocOff = 8;
constexpr uint32_t kFatMapOff = 0xA;

uint16_t get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

bool id_is_zero(const uint8_t* id) {
    for (uint32_t i = 0; i < kGameIdLen; ++i)
        if (id[i]) return false;
    return true;
}

// strncmp(name, entry_name, 32) == 0 for a name given with an explicit length.
bool name_equal(const char* entry_name, const char* name, size_t len) {
    char padded[kFileNameLen] = {};
    std::memcpy(padded, name, std::min<size_t>(len, kFileNameLen));
    return std::strncmp(padded, entry_name, kFileNameLen) == 0;
}

}  // namespace

void checksum(const uint8_t* p, size_t bytes, uint16_t* sum, uint16_t* inv) {
    uint16_t a = 0, b = 0;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint16_t w = get16(p + i);
        a = (uint16_t)(a + w);
        b = (uint16_t)(b + (uint16_t)~w);
    }
    if (a == 0xFFFF) a = 0;
    if (b == 0xFFFF) b = 0;
    *sum = a;
    *inv = b;
}

bool valid_image_size(size_t bytes) { return bytes == 0x80000 || bytes == 0x100000 || bytes == 0x200000; }

void DirEntry::parse(const uint8_t* p) {
    std::memcpy(game_code, p, 4);
    std::memcpy(maker_code, p + 4, 2);
    banner_format = p[7];
    std::memcpy(name, p + 8, kFileNameLen);
    time = get32(p + 0x28);
    icon_addr = get32(p + 0x2C);
    icon_format = get16(p + 0x30);
    icon_speed = get16(p + 0x32);
    permission = p[0x34];
    copy_counter = p[0x35];
    first_block = get16(p + 0x36);
    block_count = get16(p + 0x38);
    comment_addr = get32(p + 0x3C);
}

void DirEntry::store(uint8_t* p) const {
    std::memcpy(p, game_code, 4);
    std::memcpy(p + 4, maker_code, 2);
    p[6] = 0xFF;
    p[7] = banner_format;
    std::memcpy(p + 8, name, kFileNameLen);
    put32(p + 0x28, time);
    put32(p + 0x2C, icon_addr);
    put16(p + 0x30, icon_format);
    put16(p + 0x32, icon_speed);
    p[0x34] = permission;
    p[0x35] = copy_counter;
    put16(p + 0x36, first_block);
    put16(p + 0x38, block_count);
    put16(p + 0x3A, 0xFFFF);
    put32(p + 0x3C, comment_addr);
}

// --- image file ------------------------------------------------------------------------------

bool CardImage::adopt(std::vector<uint8_t> bytes) {
    if (!valid_image_size(bytes.size())) return false;
    data_ = std::move(bytes);
    dirty_.assign(total_blocks(), 0);
    loaded_ = false;
    return true;
}

bool CardImage::create_blank(const std::string& path, size_t size_bytes) {
    if (!valid_image_size(size_bytes)) return false;
    adopt(std::vector<uint8_t>(size_bytes, 0xFF));
    path_ = path;
    file_exists_ = false;
    return true;
}

bool CardImage::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0 || !valid_image_size((size_t)n)) return false;
    std::vector<uint8_t> bytes((size_t)n);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), n)) return false;
    adopt(std::move(bytes));
    path_ = path;
    file_exists_ = true;
    return true;
}

void CardImage::touch(uint32_t n) { dirty_[n] = 1; }

bool CardImage::flush() {
    if (path_.empty()) return true;
    if (std::find(dirty_.begin(), dirty_.end(), 1) == dirty_.end()) return true;
    if (!file_exists_) {
        std::ofstream f(path_, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data_.data()), (std::streamsize)data_.size());
        if (!f) return false;
        file_exists_ = true;
        std::fill(dirty_.begin(), dirty_.end(), 0);
        return true;
    }
    std::fstream f(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return false;
    for (uint32_t n = 0; n < total_blocks(); ++n) {
        if (!dirty_[n]) continue;
        f.seekp((std::streamoff)n * kBlockSize);
        f.write(reinterpret_cast<const char*>(block(n)), kBlockSize);
        dirty_[n] = 0;
    }
    f.flush();
    return (bool)f;
}

// --- format and mount -------------------------------------------------------------------------

int CardImage::format(const FormatParams& p) {
    if (data_.empty()) return kNoCard;
    std::fill(data_.begin(), data_.end(), 0xFF);

    uint8_t* h = block(kHeaderBlock);
    // The real serial is derived from the card's flash ID; nothing checks it but the host's own
    // consistency, so a deterministic pseudo-random value keyed on the format time stands in.
    uint32_t s = p.serial_seed ^ (uint32_t)p.format_time ^ (uint32_t)(p.format_time >> 32) ^ 0x9E3779B9u;
    for (int i = 0; i < 12; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        h[i] = (uint8_t)s;
    }
    put64(h + 0x0C, p.format_time);
    put32(h + 0x14, p.sram_bias);
    put32(h + 0x18, p.sram_language);
    put32(h + 0x1C, 0);  // DTV status flag
    put16(h + 0x20, 0);  // device id
    put16(h + 0x22, (uint16_t)size_mbit());
    put16(h + 0x24, p.encoding);
    uint16_t a, b;
    checksum(h, kHeaderChecksumOff, &a, &b);
    put16(h + kHeaderChecksumOff, a);
    put16(h + kHeaderChecksumOff + 2, b);

    for (uint32_t d : {kDirBlock0, kDirBlock1}) {
        uint8_t* dir = block(d);
        put16(dir + kDirCounterOff, 0);
        checksum(dir, kDirChecksumOff, &a, &b);
        put16(dir + kDirChecksumOff, a);
        put16(dir + kDirChecksumOff + 2, b);
    }

    for (uint32_t f : {kFatBlock0, kFatBlock1}) {
        uint8_t* fat = block(f);
        std::memset(fat, 0, kBlockSize);
        put16(fat + kFatCounterOff, 0);
        put16(fat + kFatFreeOff, (uint16_t)(total_blocks() - kSystemBlocks));
        put16(fat + kFatLastAllocOff, (uint16_t)(kSystemBlocks - 1));
        checksum(fat + 4, kBlockSize - 4, &a, &b);
        put16(fat, a);
        put16(fat + 2, b);
    }
    std::fill(dirty_.begin(), dirty_.end(), 1);
    return mount();
}

bool CardImage::header_valid() const {
    const uint8_t* h = block(kHeaderBlock);
    uint16_t a, b;
    checksum(h, kHeaderChecksumOff, &a, &b);
    return a == get16(h + kHeaderChecksumOff) && b == get16(h + kHeaderChecksumOff + 2) &&
           get16(h + 0x22) == size_mbit();
}

bool CardImage::dir_valid(uint32_t n) const {
    const uint8_t* d = block(n);
    uint16_t a, b;
    checksum(d, kDirChecksumOff, &a, &b);
    return a == get16(d + kDirChecksumOff) && b == get16(d + kDirChecksumOff + 2);
}

bool CardImage::fat_valid(uint32_t n) const {
    const uint8_t* f = block(n);
    uint16_t a, b;
    checksum(f + 4, kBlockSize - 4, &a, &b);
    return a == get16(f) && b == get16(f + 2);
}

uint16_t CardImage::encoding() const { return data_.empty() ? 0 : get16(block(kHeaderBlock) + 0x24); }

int CardImage::mount() {
    loaded_ = false;
    if (data_.empty()) return kNoCard;
    if (!header_valid()) return kBroken;

    bool d0 = dir_valid(kDirBlock0), d1 = dir_valid(kDirBlock1);
    bool f0 = fat_valid(kFatBlock0), f1 = fat_valid(kFatBlock1);
    if ((!d0 && !d1) || (!f0 && !f1)) return kBroken;

    // The copy with the newer counter is current (signed distance, so the counter may wrap).
    auto pick = [&](bool v0, bool v1, uint32_t b0, uint32_t b1, uint32_t counter_off) {
        if (v0 && v1) {
            int16_t diff = (int16_t)(get16(block(b1) + counter_off) - get16(block(b0) + counter_off));
            return diff > 0 ? b1 : b0;
        }
        return v0 ? b0 : b1;
    };
    dir_block_ = pick(d0, d1, kDirBlock0, kDirBlock1, kDirCounterOff);
    fat_block_ = pick(f0, f1, kFatBlock0, kFatBlock1, kFatCounterOff);

    // Repair a damaged copy from the good one.
    if (d0 != d1) {
        uint32_t bad = dir_block_ == kDirBlock0 ? kDirBlock1 : kDirBlock0;
        std::memcpy(block(bad), block(dir_block_), kBlockSize);
        touch(bad);
    }
    if (f0 != f1) {
        uint32_t bad = fat_block_ == kFatBlock0 ? kFatBlock1 : kFatBlock0;
        std::memcpy(block(bad), block(fat_block_), kBlockSize);
        touch(bad);
    }

    dir_.assign(block(dir_block_), block(dir_block_) + kBlockSize);
    fat_.assign(block(fat_block_), block(fat_block_) + kBlockSize);
    if (!chains_valid()) return kBroken;
    loaded_ = true;
    return kReady;
}

bool CardImage::chains_valid() const {
    const uint32_t total = total_blocks();
    uint32_t zeros = 0;
    for (uint32_t b = kSystemBlocks; b < total; ++b)
        if (fat_get(b) == 0) ++zeros;
    if (zeros != fat_free_count()) return false;

    std::vector<uint8_t> seen(total, 0);
    for (uint32_t i = 0; i < kDirEntries; ++i) {
        DirEntry e;
        e.parse(entry_ptr((int)i));
        if (!e.in_use()) continue;
        if (e.block_count == 0 || e.first_block < kSystemBlocks || e.first_block >= total) return false;
        uint32_t b = e.first_block;
        for (uint32_t n = 0; n < e.block_count; ++n) {
            if (b < kSystemBlocks || b >= total || seen[b]) return false;
            seen[b] = 1;
            uint16_t next = fat_get(b);
            bool last = n + 1 == e.block_count;
            if (last != (next == kChainEnd)) return false;
            b = next;
        }
    }
    return true;
}

// --- block map ---------------------------------------------------------------------------------

uint16_t CardImage::fat_get(uint32_t blk) const { return get16(fat_.data() + kFatMapOff + 2 * (blk - kSystemBlocks)); }
void CardImage::fat_set(uint32_t blk, uint16_t v) { put16(fat_.data() + kFatMapOff + 2 * (blk - kSystemBlocks), v); }
uint16_t CardImage::fat_free_count() const { return get16(fat_.data() + kFatFreeOff); }
void CardImage::fat_set_free_count(uint16_t v) { put16(fat_.data() + kFatFreeOff, v); }

void CardImage::commit_fat() {
    uint32_t target = fat_block_ == kFatBlock0 ? kFatBlock1 : kFatBlock0;
    put16(fat_.data() + kFatCounterOff, (uint16_t)(get16(fat_.data() + kFatCounterOff) + 1));
    uint16_t a, b;
    checksum(fat_.data() + 4, kBlockSize - 4, &a, &b);
    put16(fat_.data(), a);
    put16(fat_.data() + 2, b);
    std::memcpy(block(target), fat_.data(), kBlockSize);
    touch(target);
    fat_block_ = target;
}

void CardImage::commit_dir() {
    uint32_t target = dir_block_ == kDirBlock0 ? kDirBlock1 : kDirBlock0;
    put16(dir_.data() + kDirCounterOff, (uint16_t)(get16(dir_.data() + kDirCounterOff) + 1));
    uint16_t a, b;
    checksum(dir_.data(), kDirChecksumOff, &a, &b);
    put16(dir_.data() + kDirChecksumOff, a);
    put16(dir_.data() + kDirChecksumOff + 2, b);
    std::memcpy(block(target), dir_.data(), kBlockSize);
    touch(target);
    dir_block_ = target;
}

uint16_t CardImage::alloc_chain(uint16_t count) {
    if (count == 0 || fat_free_count() < count) return kChainEnd;
    const uint32_t total = total_blocks();
    uint32_t cur = get16(fat_.data() + kFatLastAllocOff);
    uint16_t first = kChainEnd, prev = kChainEnd;
    for (uint16_t n = 0; n < count; ++n) {
        do {
            if (++cur >= total) cur = kSystemBlocks;
        } while (fat_get(cur) != 0);
        if (prev == kChainEnd)
            first = (uint16_t)cur;
        else
            fat_set(prev, (uint16_t)cur);
        fat_set(cur, kChainEnd);
        prev = (uint16_t)cur;
    }
    put16(fat_.data() + kFatLastAllocOff, (uint16_t)cur);
    fat_set_free_count((uint16_t)(fat_free_count() - count));
    return first;
}

void CardImage::free_chain(uint16_t first) {
    uint32_t b = first;
    uint32_t freed = 0;
    // Bounded by the block count so a corrupt chain cannot loop forever.
    while (b >= kSystemBlocks && b < total_blocks() && freed < total_blocks()) {
        uint16_t next = fat_get(b);
        fat_set(b, 0);
        ++freed;
        if (next == kChainEnd) break;
        b = next;
    }
    fat_set_free_count((uint16_t)(fat_free_count() + freed));
}

uint32_t CardImage::free_blocks() const { return loaded_ ? fat_free_count() : 0; }

uint32_t CardImage::free_entries() const {
    if (!loaded_) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < kDirEntries; ++i)
        if (entry_ptr((int)i)[0] == 0xFF) ++n;
    return n;
}

// --- files -------------------------------------------------------------------------------------

bool CardImage::access_ok(const DirEntry& e, const uint8_t* game_id) const {
    if (!game_id || id_is_zero(game_id)) return true;
    return std::memcmp(e.game_code, game_id, 4) == 0 && std::memcmp(e.maker_code, game_id + 4, 2) == 0;
}

int CardImage::create(const char* name, size_t name_len, const uint8_t* game_id, uint32_t size, uint32_t now,
                      int* file_no) {
    if (!loaded_) return kBroken;
    if (name_len > kFileNameLen) return kNameTooLong;
    if (name_len == 0 || size == 0 || size % kBlockSize != 0) return kFatalError;
    for (uint32_t i = 0; i < kDirEntries; ++i) {
        DirEntry e;
        e.parse(entry_ptr((int)i));
        if (e.in_use() && access_ok(e, game_id) && name_equal(e.name, name, name_len)) return kExist;
    }
    int slot = -1;
    for (uint32_t i = 0; i < kDirEntries; ++i)
        if (entry_ptr((int)i)[0] == 0xFF) {
            slot = (int)i;
            break;
        }
    if (slot < 0) return kNoEnt;
    uint32_t blocks = size / kBlockSize;
    if (blocks > fat_free_count()) return kInsSpace;

    uint16_t first = alloc_chain((uint16_t)blocks);
    DirEntry e = {};
    if (game_id) {
        std::memcpy(e.game_code, game_id, 4);
        std::memcpy(e.maker_code, game_id + 4, 2);
    }
    std::memcpy(e.name, name, name_len);
    e.banner_format = 0;
    e.time = now;
    e.icon_addr = 0xFFFFFFFF;
    e.icon_format = 0;
    e.icon_speed = 0;
    e.permission = kAttrPublic;
    e.copy_counter = 0;
    e.first_block = first;
    e.block_count = (uint16_t)blocks;
    e.comment_addr = 0xFFFFFFFF;
    e.store(entry_ptr(slot));
    commit_fat();
    commit_dir();
    *file_no = slot;
    return kReady;
}

int CardImage::find(const char* name, size_t name_len, const uint8_t* game_id, int* file_no) const {
    if (!loaded_) return kBroken;
    if (name_len > kFileNameLen) return kNameTooLong;
    int result = kNoFile;
    for (uint32_t i = 0; i < kDirEntries; ++i) {
        DirEntry e;
        e.parse(entry_ptr((int)i));
        if (!e.in_use() || !name_equal(e.name, name, name_len)) continue;
        if (access_ok(e, game_id)) {
            *file_no = (int)i;
            return kReady;
        }
        result = kNoPerm;
    }
    return result;
}

int CardImage::get_entry(int file_no, DirEntry* out) const {
    if (!loaded_) return kBroken;
    if (file_no < 0 || file_no >= (int)kDirEntries) return kFatalError;
    out->parse(entry_ptr(file_no));
    return out->in_use() ? (int)kReady : (int)kNoFile;
}

int CardImage::check_access(int file_no, const uint8_t* game_id) const {
    DirEntry e;
    int r = get_entry(file_no, &e);
    if (r != kReady) return r;
    return access_ok(e, game_id) ? (int)kReady : (int)kNoPerm;
}

int CardImage::remove(int file_no, const uint8_t* game_id) {
    int r = check_access(file_no, game_id);
    if (r != kReady) return r;
    DirEntry e;
    e.parse(entry_ptr(file_no));
    free_chain(e.first_block);
    std::memset(entry_ptr(file_no), 0xFF, kDirEntrySize);
    commit_fat();
    commit_dir();
    return kReady;
}

int CardImage::rename(int file_no, const char* new_name, size_t new_len, const uint8_t* game_id) {
    int r = check_access(file_no, game_id);
    if (r != kReady) return r;
    if (new_len > kFileNameLen) return kNameTooLong;
    if (new_len == 0) return kFatalError;
    DirEntry e;
    e.parse(entry_ptr(file_no));
    for (uint32_t i = 0; i < kDirEntries; ++i) {
        DirEntry o;
        o.parse(entry_ptr((int)i));
        if (o.in_use() && access_ok(o, game_id) && name_equal(o.name, new_name, new_len)) return kExist;
    }
    std::memset(e.name, 0, kFileNameLen);
    std::memcpy(e.name, new_name, new_len);
    e.store(entry_ptr(file_no));
    commit_dir();
    return kReady;
}

int CardImage::set_status(int file_no, const uint8_t* game_id, uint8_t banner_format, uint32_t icon_addr,
                          uint16_t icon_format, uint16_t icon_speed, uint32_t comment_addr, uint32_t now) {
    int r = check_access(file_no, game_id);
    if (r != kReady) return r;
    DirEntry e;
    e.parse(entry_ptr(file_no));
    uint32_t length = (uint32_t)e.block_count * kBlockSize;
    if (!(icon_addr == 0xFFFFFFFF || icon_addr < length)) return kFatalError;
    if (!(comment_addr == 0xFFFFFFFF || comment_addr + 64 <= length)) return kFatalError;
    e.banner_format = banner_format;
    e.icon_addr = icon_addr;
    e.icon_format = icon_format;
    e.icon_speed = icon_speed;
    e.comment_addr = comment_addr;
    e.time = now;
    e.store(entry_ptr(file_no));
    commit_dir();
    return kReady;
}

int CardImage::read(int file_no, uint8_t* dst, uint32_t length, uint32_t offset) const {
    DirEntry e;
    int r = get_entry(file_no, &e);
    if (r != kReady) return r;
    if (length % 512 || offset % 512) return kFatalError;
    uint64_t size = (uint64_t)e.block_count * kBlockSize;
    if ((uint64_t)offset + length > size) return kLimit;
    uint32_t b = e.first_block;
    for (uint32_t skip = offset / kBlockSize; skip; --skip) b = fat_get(b);
    uint32_t in_block = offset % kBlockSize;
    while (length) {
        uint32_t n = std::min(length, kBlockSize - in_block);
        std::memcpy(dst, block(b) + in_block, n);
        dst += n;
        length -= n;
        in_block = 0;
        if (length) b = fat_get(b);
    }
    return kReady;
}

int CardImage::write(int file_no, const uint8_t* src, uint32_t length, uint32_t offset, uint32_t now) {
    DirEntry e;
    int r = get_entry(file_no, &e);
    if (r != kReady) return r;
    if (length % kBlockSize || offset % kBlockSize) return kFatalError;
    uint64_t size = (uint64_t)e.block_count * kBlockSize;
    if ((uint64_t)offset + length > size) return kLimit;
    uint32_t b = e.first_block;
    for (uint32_t skip = offset / kBlockSize; skip; --skip) b = fat_get(b);
    while (length) {
        std::memcpy(block(b), src, kBlockSize);
        touch(b);
        src += kBlockSize;
        length -= kBlockSize;
        if (length) b = fat_get(b);
    }
    e.time = now;
    e.store(entry_ptr(file_no));
    commit_dir();
    return kReady;
}

// --- .gci --------------------------------------------------------------------------------------

int CardImage::export_gci(int file_no, std::vector<uint8_t>* out) const {
    DirEntry e;
    int r = get_entry(file_no, &e);
    if (r != kReady) return r;
    out->assign(entry_ptr(file_no), entry_ptr(file_no) + kDirEntrySize);
    out->resize(kDirEntrySize + (size_t)e.block_count * kBlockSize);
    return read(file_no, out->data() + kDirEntrySize, e.block_count * kBlockSize, 0);
}

int CardImage::import_gci(const uint8_t* gci, size_t size) {
    if (!loaded_) return kBroken;
    if (size < kDirEntrySize) return kBroken;
    DirEntry e;
    e.parse(gci);
    if (!e.in_use() || e.block_count == 0 || size != kDirEntrySize + (size_t)e.block_count * kBlockSize)
        return kBroken;
    for (uint32_t i = 0; i < kDirEntries; ++i) {
        DirEntry o;
        o.parse(entry_ptr((int)i));
        if (o.in_use() && std::memcmp(o.game_code, e.game_code, 4) == 0 &&
            std::memcmp(o.maker_code, e.maker_code, 2) == 0 && name_equal(o.name, e.name, kFileNameLen))
            return kExist;
    }
    int slot = -1;
    for (uint32_t i = 0; i < kDirEntries; ++i)
        if (entry_ptr((int)i)[0] == 0xFF) {
            slot = (int)i;
            break;
        }
    if (slot < 0) return kNoEnt;
    if (e.block_count > fat_free_count()) return kInsSpace;
    e.first_block = alloc_chain(e.block_count);
    uint32_t b = e.first_block;
    const uint8_t* src = gci + kDirEntrySize;
    for (uint16_t n = 0; n < e.block_count; ++n) {
        std::memcpy(block(b), src, kBlockSize);
        touch(b);
        src += kBlockSize;
        if (n + 1 < e.block_count) b = fat_get(b);
    }
    e.store(entry_ptr(slot));
    commit_fat();
    commit_dir();
    return kReady;
}

}  // namespace gcrecomp::card
