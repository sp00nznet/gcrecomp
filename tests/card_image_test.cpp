// SPDX-License-Identifier: MIT
//
// Memory card image tests. The expected structure is written out by hand from the public format
// documentation (YAGCD, "Memory Card Format"), independently of the implementation's helpers.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include "card_image.h"

using namespace gcrecomp::card;

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

static uint16_t be16(const std::vector<uint8_t>& v, size_t off) { return (uint16_t)((v[off] << 8) | v[off + 1]); }
static uint32_t be32(const std::vector<uint8_t>& v, size_t off) {
    return ((uint32_t)v[off] << 24) | ((uint32_t)v[off + 1] << 16) | ((uint32_t)v[off + 2] << 8) | v[off + 3];
}

// Checksum as the documentation defines it: sum of big-endian words and sum of their complements,
// 0xFFFF stored as 0.
static void doc_checksum(const std::vector<uint8_t>& v, size_t off, size_t len, uint16_t* sum, uint16_t* inv) {
    uint32_t a = 0, b = 0;
    for (size_t i = 0; i < len; i += 2) {
        uint16_t w = be16(v, off + i);
        a = (a + w) & 0xFFFF;
        b = (b + (uint16_t)~w) & 0xFFFF;
    }
    *sum = a == 0xFFFF ? 0 : (uint16_t)a;
    *inv = b == 0xFFFF ? 0 : (uint16_t)b;
}

static bool header_ok(const std::vector<uint8_t>& v) {
    uint16_t a, b;
    doc_checksum(v, 0, 0x1FC, &a, &b);
    return be16(v, 0x1FC) == a && be16(v, 0x1FE) == b;
}
static bool dir_ok(const std::vector<uint8_t>& v, int blk) {
    size_t o = (size_t)blk * 0x2000;
    uint16_t a, b;
    doc_checksum(v, o, 0x1FFC, &a, &b);
    return be16(v, o + 0x1FFC) == a && be16(v, o + 0x1FFE) == b;
}
static bool fat_ok(const std::vector<uint8_t>& v, int blk) {
    size_t o = (size_t)blk * 0x2000;
    uint16_t a, b;
    doc_checksum(v, o + 4, 0x1FFC, &a, &b);
    return be16(v, o) == a && be16(v, o + 2) == b;
}
// Current copy: higher update counter.
static int cur_dir(const std::vector<uint8_t>& v) {
    return (int16_t)(be16(v, 2 * 0x2000 + 0x1FFA) - be16(v, 1 * 0x2000 + 0x1FFA)) > 0 ? 2 : 1;
}
static int cur_fat(const std::vector<uint8_t>& v) {
    return (int16_t)(be16(v, 4 * 0x2000 + 4) - be16(v, 3 * 0x2000 + 4)) > 0 ? 4 : 3;
}

static const uint8_t kId[6] = {'G', 'Q', 'S', 'F', '8', 'P'};
static const uint8_t kOtherId[6] = {'G', 'Z', 'L', 'E', '0', '1'};

static CardImage fresh(size_t size) {
    CardImage c;
    CHECK(c.adopt(std::vector<uint8_t>(size, 0xFF)));
    FormatParams p;
    p.format_time = 0x0123456789ABCDEFull;
    p.sram_bias = 0x11223344;
    p.sram_language = 2;
    CHECK_EQ(c.format(p), kReady);
    return c;
}

static void test_format_structure() {
    const struct { size_t bytes; uint16_t mbit; uint32_t usable; } sizes[] = {
        {0x80000, 4, 59}, {0x100000, 8, 123}, {0x200000, 16, 251}};
    for (auto& s : sizes) {
        CardImage c = fresh(s.bytes);
        const auto& v = c.bytes();
        CHECK_EQ(v.size(), s.bytes);
        // header
        CHECK_EQ(be32(v, 0x0C), 0x01234567);
        CHECK_EQ(be32(v, 0x10), 0x89ABCDEF);
        CHECK_EQ(be32(v, 0x14), 0x11223344);
        CHECK_EQ(be32(v, 0x18), 2);
        CHECK_EQ(be16(v, 0x20), 0);
        CHECK_EQ(be16(v, 0x22), s.mbit);
        CHECK_EQ(be16(v, 0x24), 0);
        CHECK(header_ok(v));
        CHECK_EQ(v[0x300], 0xFF);  // padding
        // directory: both copies valid, no entries, 0xFF filled
        CHECK(dir_ok(v, 1) && dir_ok(v, 2));
        for (int blk : {1, 2})
            for (size_t i = 0; i < 127 * 0x40; ++i)
                if (v[(size_t)blk * 0x2000 + i] != 0xFF) { CHECK(false); break; }
        // block map: all free, last allocated = 4
        CHECK(fat_ok(v, 3) && fat_ok(v, 4));
        for (int blk : {3, 4}) {
            size_t o = (size_t)blk * 0x2000;
            CHECK_EQ(be16(v, o + 6), s.usable);
            CHECK_EQ(be16(v, o + 8), 4);
            for (size_t i = 0; i < s.usable; ++i) CHECK_EQ(be16(v, o + 0xA + 2 * i), 0);
        }
        // data area untouched (0xFF)
        CHECK_EQ(v[5 * 0x2000], 0xFF);
        CHECK_EQ(c.free_blocks(), s.usable);
        CHECK_EQ(c.free_entries(), 127);
    }
    CardImage bad;
    CHECK(!bad.adopt(std::vector<uint8_t>(0x40000, 0)));
}

static void test_checksum_rule() {
    // Words 0xFFFF sum to 0xFFFF; the documented rule stores 0. Complements sum to 0.
    uint8_t d[4] = {0xFF, 0xFE, 0x00, 0x01};
    uint16_t a, b;
    checksum(d, 4, &a, &b);
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 0);
}

static void test_create_write_read_delete() {
    CardImage c = fresh(0x200000);
    int f = -1;
    CHECK_EQ(c.create("TOS_SAVE", 8, kId, 3 * 0x2000, 123456, &f), kReady);
    CHECK_EQ(f, 0);
    const auto& v = c.bytes();

    // The new directory copy is current and the previous one still valid.
    CHECK_EQ(cur_dir(v), 2);
    CHECK_EQ(cur_fat(v), 4);
    CHECK(dir_ok(v, 1) && dir_ok(v, 2) && fat_ok(v, 3) && fat_ok(v, 4));
    CHECK_EQ(be16(v, 2 * 0x2000 + 0x1FFA), be16(v, 1 * 0x2000 + 0x1FFA) + 1);

    // Directory entry 0, field by field.
    size_t e = 2 * 0x2000;
    CHECK(std::memcmp(&v[e], "GQSF", 4) == 0);
    CHECK(std::memcmp(&v[e + 4], "8P", 2) == 0);
    CHECK_EQ(v[e + 6], 0xFF);
    CHECK_EQ(v[e + 7], 0);  // banner format
    CHECK(std::memcmp(&v[e + 8], "TOS_SAVE", 8) == 0);
    CHECK_EQ(v[e + 8 + 8], 0);
    CHECK_EQ(be32(v, e + 0x28), 123456);
    CHECK_EQ(be32(v, e + 0x2C), 0xFFFFFFFFu);  // icon address
    CHECK_EQ(be16(v, e + 0x30), 0);
    CHECK_EQ(be16(v, e + 0x32), 0);
    CHECK_EQ(v[e + 0x34], 0x04);  // public
    CHECK_EQ(v[e + 0x35], 0);
    CHECK_EQ(be16(v, e + 0x36), 5);  // first data block
    CHECK_EQ(be16(v, e + 0x38), 3);
    CHECK_EQ(be16(v, e + 0x3A), 0xFFFF);
    CHECK_EQ(be32(v, e + 0x3C), 0xFFFFFFFFu);
    CHECK_EQ(v[e + 0x40], 0xFF);  // entry 1 still free

    // Block map: 5 -> 6 -> 7 -> end; free count and last allocated updated.
    size_t m = 4 * 0x2000;
    CHECK_EQ(be16(v, m + 0xA), 6);
    CHECK_EQ(be16(v, m + 0xC), 7);
    CHECK_EQ(be16(v, m + 0xE), 0xFFFF);
    CHECK_EQ(be16(v, m + 0x10), 0);
    CHECK_EQ(be16(v, m + 6), 251 - 3);
    CHECK_EQ(be16(v, m + 8), 7);

    // Write the middle and last block, read back across blocks, check stray bytes stay untouched.
    std::vector<uint8_t> data(2 * 0x2000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(c.write(f, data.data(), (uint32_t)data.size(), 0x2000, 777), kReady);
    std::vector<uint8_t> back(2 * 0x2000 - 512);
    CHECK_EQ(c.read(f, back.data(), (uint32_t)back.size(), 0x2000 + 512), kReady);
    CHECK(std::memcmp(back.data(), data.data() + 512, back.size()) == 0);
    std::vector<uint8_t> first(0x2000, 0);
    CHECK_EQ(c.read(f, first.data(), 0x2000, 0), kReady);
    for (uint8_t b : first) CHECK_EQ(b, 0xFF);  // never written: still erased flash
    // Block 6 holds the first written block, byte for byte, at the position the map says.
    CHECK(std::memcmp(&v[6 * 0x2000], data.data(), 0x2000) == 0);
    CHECK(std::memcmp(&v[7 * 0x2000], data.data() + 0x2000, 0x2000) == 0);
    DirEntry de;
    CHECK_EQ(c.get_entry(f, &de), kReady);
    CHECK_EQ(de.time, 777);
    CHECK(dir_ok(v, 1) && dir_ok(v, 2));

    // Re-mounting the raw bytes sees the same file.
    CardImage d;
    CHECK(d.adopt(v));
    CHECK_EQ(d.mount(), kReady);
    int g = -1;
    CHECK_EQ(d.find("TOS_SAVE", 8, kId, &g), kReady);
    CHECK_EQ(g, 0);
    std::vector<uint8_t> again(2 * 0x2000);
    CHECK_EQ(d.read(g, again.data(), (uint32_t)again.size(), 0x2000), kReady);
    CHECK(again == data);

    // Delete: entry back to 0xFF, chain freed, free count restored.
    CHECK_EQ(c.remove(f, kId), kReady);
    CHECK_EQ(c.free_blocks(), 251);
    CHECK_EQ(c.free_entries(), 127);
    CHECK(dir_ok(v, 1) && dir_ok(v, 2) && fat_ok(v, 3) && fat_ok(v, 4));
    int cf = cur_fat(v);
    for (int i = 0; i < 3; ++i) CHECK_EQ(be16(v, (size_t)cf * 0x2000 + 0xA + 2 * i), 0);
    CHECK_EQ(be16(v, (size_t)cf * 0x2000 + 6), 251);
    CHECK_EQ(c.find("TOS_SAVE", 8, kId, &g), kNoFile);
}

static void test_alloc_wraps_and_chains() {
    CardImage c = fresh(0x80000);  // 59 usable blocks
    int a, b, d;
    CHECK_EQ(c.create("A", 1, kId, 30 * 0x2000, 1, &a), kReady);
    CHECK_EQ(c.create("B", 1, kId, 20 * 0x2000, 1, &b), kReady);
    CHECK_EQ(c.create("TOO_BIG", 7, kId, 10 * 0x2000, 1, &d), kInsSpace);
    CHECK_EQ(c.free_blocks(), 9);
    CHECK_EQ(c.remove(a, kId), kReady);
    // The next allocation continues after the last allocated block (block 54) and wraps to the freed ones.
    CHECK_EQ(c.create("C", 1, kId, 12 * 0x2000, 1, &d), kReady);
    DirEntry e;
    CHECK_EQ(c.get_entry(d, &e), kReady);
    CHECK_EQ(e.first_block, 55);
    CHECK_EQ(c.free_blocks(), 59 - 20 - 12);
    // Data survives interleaved chains.
    std::vector<uint8_t> wb(20 * 0x2000), rb(20 * 0x2000);
    for (size_t i = 0; i < wb.size(); ++i) wb[i] = (uint8_t)(i ^ (i >> 8));
    CHECK_EQ(c.write(b, wb.data(), (uint32_t)wb.size(), 0, 2), kReady);
    std::vector<uint8_t> wc(12 * 0x2000, 0x5A), rc(12 * 0x2000);
    CHECK_EQ(c.write(d, wc.data(), (uint32_t)wc.size(), 0, 2), kReady);
    CHECK_EQ(c.read(b, rb.data(), (uint32_t)rb.size(), 0), kReady);
    CHECK(rb == wb);
    CHECK_EQ(c.read(d, rc.data(), (uint32_t)rc.size(), 0), kReady);
    CHECK(rc == wc);
    // A freshly mounted copy accepts the structure (chains, free count).
    CardImage m;
    CHECK(m.adopt(c.bytes()));
    CHECK_EQ(m.mount(), kReady);
}

static void test_errors() {
    CardImage c = fresh(0x200000);
    int f;
    CHECK_EQ(c.create("X", 1, kId, 0x2000, 1, &f), kReady);
    CHECK_EQ(c.create("X", 1, kId, 0x2000, 1, &f), kExist);
    // Same name under another game id is a different file.
    int g;
    CHECK_EQ(c.create("X", 1, kOtherId, 0x2000, 1, &g), kReady);
    CHECK(g != f);
    std::string longname(33, 'n');
    CHECK_EQ(c.create(longname.data(), 33, kId, 0x2000, 1, &f), kNameTooLong);
    std::string max32(32, 'm');
    CHECK_EQ(c.create(max32.data(), 32, kId, 0x2000, 1, &f), kReady);
    CHECK_EQ(c.find(max32.data(), 32, kId, &g), kReady);
    CHECK_EQ(c.create("Y", 1, kId, 0x2001, 1, &f), kFatalError);
    CHECK_EQ(c.create("Y", 1, kId, 0, 1, &f), kFatalError);
    CHECK_EQ(c.create("Z", 1, kId, 252 * 0x2000, 1, &f), kInsSpace);

    // Another game cannot open, write or delete a save.
    int x;
    CHECK_EQ(c.find("X", 1, kId, &x), kReady);
    uint8_t other_only[6] = {'A', 'B', 'C', 'D', '9', '9'};
    CHECK_EQ(c.find("X", 1, other_only, &g), kNoPerm);  // exists, but not for this game
    CHECK_EQ(c.check_access(x, other_only), kNoPerm);
    CHECK_EQ(c.remove(x, other_only), kNoPerm);

    // Bounds and alignment.
    std::vector<uint8_t> buf(0x4000);
    CHECK_EQ(c.read(x, buf.data(), 0x2000, 0x2000), kLimit);
    CHECK_EQ(c.read(x, buf.data(), 100, 0), kFatalError);
    CHECK_EQ(c.write(x, buf.data(), 0x2000, 0x2000, 1), kLimit);
    CHECK_EQ(c.write(x, buf.data(), 512, 0, 1), kFatalError);
    CHECK_EQ(c.read(5, buf.data(), 512, 0), kNoFile);
    CHECK_EQ(c.read(127, buf.data(), 512, 0), kFatalError);
    CHECK_EQ(c.remove(100, kId), kNoFile);

    // Icon and comment fields must lie inside the file.
    CHECK_EQ(c.set_status(x, kId, 2, 0, 0xAAAA, 0x5555, 0x2000 - 64, 9), kReady);
    DirEntry e;
    CHECK_EQ(c.get_entry(x, &e), kReady);
    CHECK_EQ(e.banner_format, 2);
    CHECK_EQ(e.icon_addr, 0);
    CHECK_EQ(e.icon_format, 0xAAAA);
    CHECK_EQ(e.icon_speed, 0x5555);
    CHECK_EQ(e.comment_addr, 0x2000 - 64);
    CHECK_EQ(e.time, 9);
    CHECK_EQ(c.set_status(x, kId, 0, 0x2000, 0, 0, 0xFFFFFFFF, 9), kFatalError);
    CHECK_EQ(c.set_status(x, kId, 0, 0xFFFFFFFF, 0, 0, 0x2000 - 63, 9), kFatalError);
    // The fields reach the disk entry unchanged.
    size_t eo = (size_t)cur_dir(c.bytes()) * 0x2000 + (size_t)x * 0x40;
    CHECK_EQ(c.bytes()[eo + 7], 2);
    CHECK_EQ(be16(c.bytes(), eo + 0x30), 0xAAAA);
    CHECK_EQ(be32(c.bytes(), eo + 0x3C), 0x2000 - 64);

    CHECK_EQ(c.rename(x, "Q", 1, kId), kReady);
    CHECK_EQ(c.find("Q", 1, kId, &g), kReady);
    CHECK_EQ(c.find("X", 1, kId, &g), kNoPerm);  // the other game's "X" is still there, not ours

    // 127 files fill the directory.
    CardImage d = fresh(0x200000);
    for (int i = 0; i < 127; ++i) {
        char n[8];
        int len = std::snprintf(n, sizeof n, "f%d", i);
        CHECK_EQ(d.create(n, (size_t)len, kId, 0x2000, 1, &f), kReady);
    }
    CHECK_EQ(d.create("one_more", 8, kId, 0x2000, 1, &f), kNoEnt);
    CHECK_EQ(d.free_entries(), 0);
    CHECK_EQ(d.free_blocks(), 251 - 127);
}

static void test_mount_repair() {
    CardImage c = fresh(0x100000);
    int f;
    CHECK_EQ(c.create("KEEP", 4, kId, 2 * 0x2000, 5, &f), kReady);
    std::vector<uint8_t> payload(0x2000, 0xC3);
    CHECK_EQ(c.write(f, payload.data(), 0x2000, 0, 5), kReady);

    // Damage the current directory copy: the older copy is used and the damaged one repaired.
    std::vector<uint8_t> img = c.bytes();
    size_t cd = (size_t)cur_dir(img) * 0x2000;
    img[cd + 100] ^= 0x55;
    CHECK(!dir_ok(img, cur_dir(img)));
    CardImage m;
    CHECK(m.adopt(img));
    CHECK_EQ(m.mount(), kReady);
    CHECK(dir_ok(m.bytes(), 1) && dir_ok(m.bytes(), 2));
    // Damage the stale block map copy: it is rewritten from the current one.
    img = c.bytes();
    int stale = cur_fat(img) == 3 ? 4 : 3;
    img[(size_t)stale * 0x2000 + 0x20] ^= 0x01;
    CHECK(!fat_ok(img, stale));
    CHECK(m.adopt(img));
    CHECK_EQ(m.mount(), kReady);
    CHECK(fat_ok(m.bytes(), 3) && fat_ok(m.bytes(), 4));
    CHECK(std::memcmp(&m.bytes()[3 * 0x2000], &m.bytes()[4 * 0x2000], 0x2000) == 0);
    // Damage the current block map copy: the older one no longer matches the directory.
    img = c.bytes();
    img[(size_t)cur_fat(img) * 0x2000 + 0x20] ^= 0x01;
    CHECK(m.adopt(img));
    CHECK_EQ(m.mount(), kBroken);

    // Both directory copies damaged, a header byte changed, a free count that lies, a chain cut short.
    img = c.bytes();
    img[1 * 0x2000 + 100] ^= 1;
    img[2 * 0x2000 + 100] ^= 1;
    CHECK(m.adopt(img));
    CHECK_EQ(m.mount(), kBroken);
    img = c.bytes();
    img[0x14] ^= 1;
    CHECK(m.adopt(img));
    CHECK_EQ(m.mount(), kBroken);
    // A valid checksum is not enough: the first block of the chain pointing to a free block.
    img = c.bytes();
    size_t fo = (size_t)cur_fat(img) * 0x2000;
    img[fo + 0xA] = 0;  // block 5 now "free", but the entry still claims it
    img[fo + 0xB] = 0;
    {
        std::vector<uint8_t> v = img;
        uint16_t a, b;
        doc_checksum(v, fo + 4, 0x1FFC, &a, &b);
        img[fo] = (uint8_t)(a >> 8); img[fo + 1] = (uint8_t)a;
        img[fo + 2] = (uint8_t)(b >> 8); img[fo + 3] = (uint8_t)b;
    }
    CHECK(m.adopt(img));
    CHECK_EQ(m.mount(), kBroken);
    // Unformatted card.
    CardImage blank;
    CHECK(blank.adopt(std::vector<uint8_t>(0x80000, 0xFF)));
    CHECK_EQ(blank.mount(), kBroken);
    int g;
    CHECK_EQ(blank.create("A", 1, kId, 0x2000, 1, &g), kBroken);
}

static void test_update_alternation() {
    CardImage c = fresh(0x80000);
    int f;
    CHECK_EQ(c.create("A", 1, kId, 0x2000, 1, &f), kReady);
    int d1 = cur_dir(c.bytes());
    CHECK_EQ(c.create("B", 1, kId, 0x2000, 1, &f), kReady);
    int d2 = cur_dir(c.bytes());
    CHECK(d1 != d2);
    // The older copy lags exactly one update behind: it still lists A only.
    int old = d2 == 1 ? 2 : 1;
    CHECK_EQ(c.bytes()[(size_t)old * 0x2000 + 0x40], 0xFF);
    CHECK_EQ(c.bytes()[(size_t)d2 * 0x2000 + 0x40], 'G');
    CHECK(dir_ok(c.bytes(), 1) && dir_ok(c.bytes(), 2));
}

static void test_gci() {
    CardImage a = fresh(0x200000);
    int f;
    CHECK_EQ(a.create("SYMPHONIA", 9, kId, 3 * 0x2000, 4242, &f), kReady);
    std::vector<uint8_t> data(3 * 0x2000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 31 + 1);
    CHECK_EQ(a.write(f, data.data(), (uint32_t)data.size(), 0, 4242), kReady);
    CHECK_EQ(a.set_status(f, kId, 2, 0x1800, 0x0002, 0x0001, 0x40, 4243), kReady);

    std::vector<uint8_t> gci;
    CHECK_EQ(a.export_gci(f, &gci), kReady);
    CHECK_EQ(gci.size(), 0x40 + 3 * 0x2000);
    CHECK(std::memcmp(gci.data(), "GQSF8P", 6) == 0 || std::memcmp(gci.data(), "GQSF", 4) == 0);
    CHECK(std::memcmp(gci.data() + 8, "SYMPHONIA", 9) == 0);
    CHECK_EQ(be16(gci, 0x38), 3);
    CHECK(std::memcmp(gci.data() + 0x40, data.data(), data.size()) == 0);

    // Import into a card that already has other files, so the first block differs.
    CardImage b = fresh(0x100000);
    int o;
    CHECK_EQ(b.create("OTHER", 5, kOtherId, 5 * 0x2000, 1, &o), kReady);
    CHECK_EQ(b.import_gci(gci.data(), gci.size()), kReady);
    int g;
    CHECK_EQ(b.find("SYMPHONIA", 9, kId, &g), kReady);
    DirEntry e;
    CHECK_EQ(b.get_entry(g, &e), kReady);
    CHECK_EQ(e.first_block, 10);
    CHECK_EQ(e.banner_format, 2);
    CHECK_EQ(e.icon_addr, 0x1800);
    CHECK_EQ(e.time, 4243);
    std::vector<uint8_t> back(data.size());
    CHECK_EQ(b.read(g, back.data(), (uint32_t)back.size(), 0), kReady);
    CHECK(back == data);
    CHECK(dir_ok(b.bytes(), 1) && dir_ok(b.bytes(), 2) && fat_ok(b.bytes(), 3) && fat_ok(b.bytes(), 4));
    CardImage m;
    CHECK(m.adopt(b.bytes()));
    CHECK_EQ(m.mount(), kReady);

    CHECK_EQ(b.import_gci(gci.data(), gci.size()), kExist);
    CHECK_EQ(b.import_gci(gci.data(), gci.size() - 1), kBroken);
    CHECK_EQ(b.import_gci(gci.data(), 10), kBroken);
    // Not enough room.
    CardImage tiny = fresh(0x80000);
    int big;
    CHECK_EQ(tiny.create("BIG", 3, kOtherId, 57 * 0x2000, 1, &big), kReady);
    CHECK_EQ(tiny.import_gci(gci.data(), gci.size()), kInsSpace);
}

static void test_file_backing() {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / "gcrecomp_card_test.raw";
    fs::remove(p);
    CardImage c;
    CHECK(c.create_blank(p.string(), 0x80000));
    CHECK(!fs::exists(p));  // nothing is written until something changes
    FormatParams fp;
    CHECK_EQ(c.format(fp), kReady);
    CHECK(c.flush());
    CHECK_EQ(fs::file_size(p), 0x80000);
    int f;
    CHECK_EQ(c.create("P", 1, kId, 2 * 0x2000, 1, &f), kReady);
    std::vector<uint8_t> data(2 * 0x2000, 0x77);
    CHECK_EQ(c.write(f, data.data(), (uint32_t)data.size(), 0, 1), kReady);
    CHECK(c.flush());

    CardImage d;
    CHECK(d.load(p.string()));
    CHECK(d.bytes() == c.bytes());
    CHECK_EQ(d.mount(), kReady);
    CHECK_EQ(d.find("P", 1, kId, &f), kReady);
    // A change after loading rewrites only what changed, and the file stays consistent.
    CHECK_EQ(d.remove(f, kId), kReady);
    CHECK(d.flush());
    CardImage e;
    CHECK(e.load(p.string()));
    CHECK(e.bytes() == d.bytes());
    CHECK_EQ(fs::file_size(p), 0x80000);
    fs::remove(p);

    // A file of the wrong size is not a card image.
    {
        std::FILE* fh = std::fopen(p.string().c_str(), "wb");
        std::vector<uint8_t> junk(0x12345, 1);
        std::fwrite(junk.data(), 1, junk.size(), fh);
        std::fclose(fh);
    }
    CardImage j;
    CHECK(!j.load(p.string()));
    fs::remove(p);
}

int main() {
    test_format_structure();
    test_checksum_rule();
    test_create_write_read_delete();
    test_alloc_wraps_and_chains();
    test_errors();
    test_mount_repair();
    test_update_alternation();
    test_gci();
    test_file_backing();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all card image tests passed\n");
    return 0;
}
