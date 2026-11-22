// SPDX-License-Identifier: MIT
//
// Memory card (CARD) HLE on top of a card image (see card_image.h).
//
// The recompiled SDK's CARD driver talks to the card over EXI, which does not exist here. These
// replacements implement the SDK's API on a CardImage that the host program points at a file with
// set_card_image(). Only slot A (channel 0) can hold a card; the card is reported only while
// g_card_inserted is true and an image path is set. Asynchronous calls complete at once and run
// their callback shortly after, from card_run_pending_callbacks (the SDK runs it from an interrupt,
// after the call returned; running it inside the call would break games that set up their wait state
// after calling). Signatures follow the Dolphin SDK (libogc's card.h for names).
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>

#include "card_image.h"
#include "gcrecomp/runtime.h"

namespace gcrecomp {

// Whether a memory card is reported in slot A. Off by default; with no image path set the slot is
// empty regardless.
bool g_card_inserted = false;

namespace {

using namespace card;

constexpr int kSlots = 2;
constexpr uint32_t kDiskIdAddr = 0x80000000;  // __CARDDiskID: the game's id lives at the start of RAM
constexpr uint32_t kCardStatSize = 0x6C;

std::mutex g_mu;
std::string g_path;
size_t g_size = 0x200000;

struct Slot {
    CardImage img;
    bool image_ready = false;
    bool image_bad = false;  // the file exists but is not a memory card image
    bool attached = false;   // CARDMount found a card (even a broken one: it can be formatted)
    bool mounted = false;    // the card verified
    int32_t result = kReady;
};
Slot g_slot[kSlots];

uint32_t card_now() {
    std::time_t t = std::time(nullptr) - 946684800;  // seconds since 2000-01-01
    return t < 0 ? 0 : (uint32_t)t;
}

// GCRECOMP_CARD_IMAGE=path[:size] inserts a card image in slot A for a host program that did not call
// set_card_image() itself (a missing file starts as a blank card that the game formats). Read once.
void card_from_env() {
    static const bool once = [] {
        const char* env = std::getenv("GCRECOMP_CARD_IMAGE");
        if (!env || !*env || g_card_inserted || !g_path.empty()) return true;
        std::string path = env;
        size_t size = g_size;
        const size_t colon = path.rfind(':');
        if (colon != std::string::npos && colon + 1 < path.size() && std::isdigit((unsigned char)path[colon + 1])) {
            size = size_t(std::strtoull(path.c_str() + colon + 1, nullptr, 0));
            path.resize(colon);
        }
        g_path = path;
        g_size = size;
        g_card_inserted = true;
        return true;
    }();
    (void)once;
}

bool present(int32_t chan) {
    card_from_env();
    return chan == 0 && g_card_inserted && !g_path.empty();
}

// Open (or create in memory) the image behind slot A. Call with g_mu held.
bool ensure_image(Slot& s) {
    if (s.image_ready) return true;
    if (s.image_bad) return false;
    std::FILE* f = std::fopen(g_path.c_str(), "rb");
    if (f) {
        std::fclose(f);
        if (!s.img.load(g_path)) {
            s.image_bad = true;
            return false;
        }
    } else if (!s.img.create_blank(g_path, g_size)) {
        s.image_bad = true;
        return false;
    }
    s.image_ready = true;
    return true;
}

void game_id(Memory* mem, uint8_t out[kGameIdLen]) {
    for (uint32_t i = 0; i < kGameIdLen; ++i) out[i] = mem->read8(kDiskIdAddr + i);
}

std::string read_name(Memory* mem, uint32_t addr) {
    std::string s;
    for (int i = 0; i < 64; ++i) {
        char c = (char)mem->read8(addr + i);
        if (!c) break;
        s += c;
    }
    return s;
}

// A host pointer for a guest buffer, or null if it is not entirely inside RAM.
uint8_t* guest_buf(Memory* mem, uint32_t addr, uint32_t len) {
    uint32_t base = addr >= Memory::UNCACHED_BASE ? Memory::UNCACHED_BASE : Memory::MAIN_RAM_BASE;
    if (addr < base || (uint64_t)(addr - base) + len > mem->ram_size) return nullptr;
    return mem->ram + (addr - base);
}

// CARD_TRACE=1 logs every completed call.
bool trace() {
    static const bool on = std::getenv("CARD_TRACE") != nullptr;
    return on;
}

struct Pending {
    uint32_t cb;
    int32_t chan, result;
};
std::deque<Pending> g_pending;  // completed asynchronous operations whose callback has not run

void run_callback(PPCContext* ctx, Memory* mem, uint32_t cb, int32_t chan, int32_t result) {
    if (trace()) std::printf("[CARD] callback 0x%08X(chan=%d, result=%d)\n", cb, chan, result);
    PPCContext saved = *ctx;
    ctx->r[3] = (uint32_t)chan;
    ctx->r[4] = (uint32_t)result;
    g_func_table.call(cb, ctx, mem);
    *ctx = saved;
}

// The slot for `chan`, or an error: the state every file operation needs (mounted card).
int32_t file_slot(int32_t chan, Slot** out) {
    if (chan < 0 || chan >= kSlots) return kFatalError;
    if (!present(chan)) return kNoCard;
    Slot& s = g_slot[chan];
    if (!s.attached) return kNoCard;
    if (!s.mounted || !s.img.mounted()) return kBroken;
    *out = &s;
    return kReady;
}

// Finish an operation: record the result, flush the image, return it in r3.
int32_t sync_done(PPCContext* ctx, int32_t chan, int32_t r) {
    if (trace()) std::printf("[CARD] sync call at lr=0x%08X -> %d\n", ctx->lr, r);
    if (chan >= 0 && chan < kSlots) g_slot[chan].result = r;
    ctx->r[3] = (uint32_t)r;
    return r;
}

// Asynchronous completion. Errors the SDK finds before starting the transfer are returned and the
// callback is not run; with `via_callback` (mount, check, format) every result goes to the callback.
void async_done(PPCContext* ctx, int32_t chan, int32_t r, uint32_t cb, bool via_callback) {
    if (trace()) std::printf("[CARD] async call at lr=0x%08X args=%X %X %X %X %X cb=0x%08X -> %d\n", ctx->lr, ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], ctx->r[7], cb, r);
    if (chan >= 0 && chan < kSlots) g_slot[chan].result = r;
    if (r < 0 && !via_callback) {
        ctx->r[3] = (uint32_t)r;
        return;
    }
    if (cb) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending.push_back({cb, chan, r});
    }
    ctx->r[3] = (uint32_t)kReady;
}

void flush_locked(Slot& s) {
    if (!s.img.flush()) std::printf("[CARD] writing %s failed\n", s.img.path().c_str());
}

// --- operations (g_mu is taken here; they return the SDK result) --------------------------------

int32_t do_mount(int32_t chan) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (chan < 0 || chan >= kSlots) return kFatalError;
    if (!present(chan)) return kNoCard;
    Slot& s = g_slot[chan];
    if (!ensure_image(s)) return kWrongDevice;
    s.attached = true;
    int32_t r = s.img.mount();
    s.mounted = r == kReady;
    flush_locked(s);  // a repaired copy is written back
    std::printf("[CARD] mount slot %d -> %d\n", chan, r);
    return r;
}

int32_t do_unmount(int32_t chan) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (chan < 0 || chan >= kSlots) return kFatalError;
    g_slot[chan].attached = false;
    g_slot[chan].mounted = false;
    return kReady;
}

int32_t do_check(int32_t chan, uint32_t xfer_ptr, Memory* mem) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (chan < 0 || chan >= kSlots) return kFatalError;
    if (!present(chan) || !g_slot[chan].attached) return kNoCard;
    Slot& s = g_slot[chan];
    int32_t r = s.img.mount();
    flush_locked(s);
    if (xfer_ptr) mem->write32(xfer_ptr, 0);
    return r;
}

int32_t do_format(int32_t chan) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (chan < 0 || chan >= kSlots) return kFatalError;
    if (!present(chan) || !g_slot[chan].attached) return kNoCard;
    Slot& s = g_slot[chan];
    FormatParams p;
    p.format_time = (uint64_t)card_now() * 40500000ull;  // OS_TIMER_CLOCK ticks since 2000
    p.serial_seed = (uint32_t)std::time(nullptr);
    int32_t r = s.img.format(p);
    s.mounted = r == kReady;
    flush_locked(s);
    std::printf("[CARD] format slot %d -> %d\n", chan, r);
    return r;
}

void write_file_info(Memory* mem, uint32_t fi, int32_t chan, int file_no, const DirEntry& e) {
    mem->write32(fi + 0, (uint32_t)chan);
    mem->write32(fi + 4, (uint32_t)file_no);
    mem->write32(fi + 8, 0);            // offset
    mem->write32(fi + 0xC, 0xFFFFFFFF);  // length
    mem->write16(fi + 0x10, e.first_block);
}

int32_t do_open(Memory* mem, int32_t chan, uint32_t name_addr, uint32_t fi) {
    std::string name = read_name(mem, name_addr);
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    int file_no;
    r = s->img.find(name.c_str(), name.size(), id, &file_no);
    if (r != kReady) return r;
    DirEntry e;
    s->img.get_entry(file_no, &e);
    write_file_info(mem, fi, chan, file_no, e);
    return kReady;
}

int32_t do_fast_open(Memory* mem, int32_t chan, int32_t file_no, uint32_t fi) {
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    r = s->img.check_access(file_no, id);
    if (r != kReady) return r;
    DirEntry e;
    s->img.get_entry(file_no, &e);
    write_file_info(mem, fi, chan, file_no, e);
    return kReady;
}

int32_t do_create(Memory* mem, int32_t chan, uint32_t name_addr, uint32_t size, uint32_t fi) {
    std::string name = read_name(mem, name_addr);
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    int file_no;
    r = s->img.create(name.c_str(), name.size(), id, size, card_now(), &file_no);
    if (r != kReady) return r;
    flush_locked(*s);
    DirEntry e;
    s->img.get_entry(file_no, &e);
    write_file_info(mem, fi, chan, file_no, e);
    std::printf("[CARD] create \"%s\" size=0x%X -> file %d\n", name.c_str(), size, file_no);
    return kReady;
}

// Resolve a CARDFileInfo to (slot, file number); checks that it still names the same file.
int32_t file_of_info(Memory* mem, uint32_t fi, Slot** s, int32_t* chan, int* file_no) {
    *chan = (int32_t)mem->read32(fi + 0);
    *file_no = (int)mem->read32(fi + 4);
    int32_t r = file_slot(*chan, s);
    if (r != kReady) return r;
    DirEntry e;
    r = (*s)->img.get_entry(*file_no, &e);
    if (r != kReady) return r;
    if (e.first_block != mem->read16(fi + 0x10)) return kFatalError;
    return kReady;
}

int32_t do_read(Memory* mem, uint32_t fi, uint32_t buf, uint32_t length, uint32_t offset, int32_t* chan_out) {
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int file_no;
    int32_t r = file_of_info(mem, fi, &s, chan_out, &file_no);
    if (r != kReady) return r;
    uint8_t* dst = guest_buf(mem, buf, length);
    if (!dst) return kFatalError;
    return s->img.read(file_no, dst, length, offset);
}

int32_t do_write(Memory* mem, uint32_t fi, uint32_t buf, uint32_t length, uint32_t offset, int32_t* chan_out) {
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int file_no;
    int32_t r = file_of_info(mem, fi, &s, chan_out, &file_no);
    if (r != kReady) return r;
    r = s->img.check_access(file_no, id);
    if (r != kReady) return r;
    const uint8_t* src = guest_buf(mem, buf, length);
    if (!src) return kFatalError;
    r = s->img.write(file_no, src, length, offset, card_now());
    if (r == kReady) flush_locked(*s);
    return r;
}

int32_t do_delete_no(int32_t chan, int32_t file_no, const uint8_t* id) {
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    r = s->img.remove(file_no, id);
    if (r == kReady) flush_locked(*s);
    return r;
}

int32_t do_delete(Memory* mem, int32_t chan, uint32_t name_addr) {
    std::string name = read_name(mem, name_addr);
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    int file_no;
    r = s->img.find(name.c_str(), name.size(), id, &file_no);
    if (r != kReady) return r;
    return do_delete_no(chan, file_no, id);
}

int32_t do_fast_delete(Memory* mem, int32_t chan, int32_t file_no) {
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    return do_delete_no(chan, file_no, id);
}

int32_t do_rename(Memory* mem, int32_t chan, uint32_t old_addr, uint32_t new_addr) {
    std::string old_name = read_name(mem, old_addr), new_name = read_name(mem, new_addr);
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    int file_no;
    r = s->img.find(old_name.c_str(), old_name.size(), id, &file_no);
    if (r != kReady) return r;
    r = s->img.rename(file_no, new_name.c_str(), new_name.size(), id);
    if (r == kReady) flush_locked(*s);
    return r;
}

// CARDStat (SDK): name[32], length, time, gameName[4], company[2], bannerFormat, pad, iconAddr,
// iconFormat, iconSpeed, commentAddr, offsetBanner, offsetBannerTlut, offsetIcon[8], offsetIconTlut,
// offsetData.
int32_t do_get_status(Memory* mem, int32_t chan, int32_t file_no, uint32_t stat) {
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    r = s->img.check_access(file_no, id);
    if (r != kReady) return r;
    DirEntry e;
    s->img.get_entry(file_no, &e);
    for (uint32_t i = 0; i < kCardStatSize; ++i) mem->write8(stat + i, 0);
    for (uint32_t i = 0; i < kFileNameLen; ++i) mem->write8(stat + i, (uint8_t)e.name[i]);
    mem->write32(stat + 0x20, (uint32_t)e.block_count * kBlockSize);
    mem->write32(stat + 0x24, e.time);
    for (uint32_t i = 0; i < 4; ++i) mem->write8(stat + 0x28 + i, e.game_code[i]);
    for (uint32_t i = 0; i < 2; ++i) mem->write8(stat + 0x2C + i, e.maker_code[i]);
    uint32_t offset = e.icon_addr;
    uint8_t banner = e.banner_format;
    uint16_t icon_fmt = e.icon_format;
    if (e.icon_addr == 0xFFFFFFFF) {
        banner = 0;
        icon_fmt = 0;
        offset = 0;
    }
    mem->write8(stat + 0x2E, e.banner_format);
    mem->write32(stat + 0x30, e.icon_addr);
    mem->write16(stat + 0x34, e.icon_format);
    mem->write16(stat + 0x36, e.icon_speed);
    mem->write32(stat + 0x38, e.comment_addr);
    // Offsets of the banner, icons and palettes within the file (the SDK's UpdateIconOffsets).
    bool icon_tlut = false;
    uint32_t banner_off = 0xFFFFFFFF, banner_tlut = 0xFFFFFFFF;
    switch (banner & 3) {
        case 1:  // C8
            banner_off = offset;
            offset += 96 * 32;
            banner_tlut = offset;
            offset += 2 * 256;
            break;
        case 2:  // RGB5A3
            banner_off = offset;
            offset += 2 * 96 * 32;
            break;
        default: break;
    }
    mem->write32(stat + 0x3C, banner_off);
    mem->write32(stat + 0x40, banner_tlut);
    for (int i = 0; i < 8; ++i) {
        uint32_t o = 0xFFFFFFFF;
        switch ((icon_fmt >> (2 * i)) & 3) {
            case 1:
                o = offset;
                offset += 32 * 32;
                icon_tlut = true;
                break;
            case 2:
                o = offset;
                offset += 2 * 32 * 32;
                break;
            default: break;
        }
        mem->write32(stat + 0x44 + 4 * i, o);
    }
    if (icon_tlut) {
        mem->write32(stat + 0x64, offset);
        offset += 2 * 256;
    } else {
        mem->write32(stat + 0x64, 0xFFFFFFFF);
    }
    mem->write32(stat + 0x68, offset);
    return kReady;
}

int32_t do_set_status(Memory* mem, int32_t chan, int32_t file_no, uint32_t stat) {
    uint8_t id[kGameIdLen];
    game_id(mem, id);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r != kReady) return r;
    r = s->img.set_status(file_no, id, mem->read8(stat + 0x2E), mem->read32(stat + 0x30),
                          mem->read16(stat + 0x34), mem->read16(stat + 0x36), mem->read32(stat + 0x38),
                          card_now());
    if (r == kReady) flush_locked(*s);
    return r;
}

// --- SDK entry points ---------------------------------------------------------------------------

#define ARG_S32(n) ((int32_t)ctx->r[n])

void card_init(PPCContext* ctx, Memory*) {
    std::printf("[CARD] CARDInit\n");
    ctx->r[3] = (uint32_t)kReady;
}

// CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize): memSize in Mbit
void card_probe_ex(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    card_run_pending_callbacks(ctx, mem);  // games poll this while waiting for an operation
    std::lock_guard<std::mutex> lk(g_mu);
    if (chan < 0 || chan >= kSlots) {
        ctx->r[3] = (uint32_t)kFatalError;
        return;
    }
    if (!present(chan)) {
        ctx->r[3] = (uint32_t)kNoCard;
        return;
    }
    Slot& s = g_slot[chan];
    if (!ensure_image(s)) {
        ctx->r[3] = (uint32_t)kWrongDevice;
        return;
    }
    if (ctx->r[4]) mem->write32(ctx->r[4], s.img.size_mbit());
    if (ctx->r[5]) mem->write32(ctx->r[5], kBlockSize);
    ctx->r[3] = (uint32_t)kReady;
}

// CARDProbe(s32 chan) -> BOOL
void card_probe(PPCContext* ctx, Memory*) {
    std::lock_guard<std::mutex> lk(g_mu);
    int32_t chan = ARG_S32(3);
    ctx->r[3] = (present(chan) && ensure_image(g_slot[chan])) ? 1 : 0;
}

// CARDMount(chan, workArea, detachCallback)
void card_mount(PPCContext* ctx, Memory*) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_mount(chan));
}
// CARDMountAsync(chan, workArea, detachCallback, attachCallback)
void card_mount_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[6];
    async_done(ctx, chan, do_mount(chan), cb, true);
}

void card_unmount(PPCContext* ctx, Memory*) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_unmount(chan));
}

// CARDCheck(chan), CARDCheckAsync(chan, cb), CARDCheckEx(chan, s32* xferBytes), CARDCheckExAsync(chan, xfer, cb)
void card_check(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_check(chan, 0, mem));
}
void card_check_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[4];
    async_done(ctx, chan, do_check(chan, 0, mem), cb, true);
}
void card_check_ex(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_check(chan, ctx->r[4], mem));
}
void card_check_ex_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[5];
    async_done(ctx, chan, do_check(chan, ctx->r[4], mem), cb, true);
}

// CARDFormat(chan), CARDFormatAsync(chan, cb)
void card_format(PPCContext* ctx, Memory*) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_format(chan));
}
void card_format_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[4];
    async_done(ctx, chan, do_format(chan), cb, true);
}

// CARDOpen(chan, fileName, CARDFileInfo*), CARDFastOpen(chan, fileNo, CARDFileInfo*)
void card_open(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_open(mem, chan, ctx->r[4], ctx->r[5]));
}
void card_fast_open(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_fast_open(mem, chan, ARG_S32(4), ctx->r[5]));
}

// CARDClose(CARDFileInfo*): the file info no longer names a file
void card_close(PPCContext* ctx, Memory* mem) {
    uint32_t fi = ctx->r[3];
    int32_t chan = (int32_t)mem->read32(fi);
    mem->write32(fi, 0xFFFFFFFF);
    sync_done(ctx, chan, kReady);
}

// CARDCreate(chan, fileName, size, CARDFileInfo*), CARDCreateAsync(..., cb)
void card_create(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_create(mem, chan, ctx->r[4], ctx->r[5], ctx->r[6]));
}
void card_create_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[7];
    async_done(ctx, chan, do_create(mem, chan, ctx->r[4], ctx->r[5], ctx->r[6]), cb, false);
}

// CARDRead / CARDWrite (CARDFileInfo*, buf, length, offset), Async variants take cb as the fifth argument
void card_read(PPCContext* ctx, Memory* mem) {
    int32_t chan = -1;
    int32_t r = do_read(mem, ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], &chan);
    sync_done(ctx, chan, r);
}
void card_read_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = -1;
    uint32_t cb = ctx->r[7];
    int32_t r = do_read(mem, ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], &chan);
    async_done(ctx, chan, r, cb, false);
}
void card_write(PPCContext* ctx, Memory* mem) {
    int32_t chan = -1;
    int32_t r = do_write(mem, ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], &chan);
    sync_done(ctx, chan, r);
}
void card_write_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = -1;
    uint32_t cb = ctx->r[7];
    int32_t r = do_write(mem, ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6], &chan);
    async_done(ctx, chan, r, cb, false);
}

// CARDDelete(chan, fileName), CARDFastDelete(chan, fileNo); Async variants take cb as the third argument
void card_delete(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_delete(mem, chan, ctx->r[4]));
}
void card_delete_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[5];
    async_done(ctx, chan, do_delete(mem, chan, ctx->r[4]), cb, false);
}
void card_fast_delete(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_fast_delete(mem, chan, ARG_S32(4)));
}
void card_fast_delete_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[5];
    async_done(ctx, chan, do_fast_delete(mem, chan, ARG_S32(4)), cb, false);
}

// CARDRename(chan, oldName, newName), CARDRenameAsync(..., cb)
void card_rename(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_rename(mem, chan, ctx->r[4], ctx->r[5]));
}
void card_rename_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[6];
    async_done(ctx, chan, do_rename(mem, chan, ctx->r[4], ctx->r[5]), cb, false);
}

// CARDGetStatus(chan, fileNo, CARDStat*), CARDSetStatus(chan, fileNo, CARDStat*), SetStatusAsync(..., cb)
void card_get_status(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_get_status(mem, chan, ARG_S32(4), ctx->r[5]));
}
void card_set_status(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    sync_done(ctx, chan, do_set_status(mem, chan, ARG_S32(4), ctx->r[5]));
}
void card_set_status_async(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t cb = ctx->r[6];
    async_done(ctx, chan, do_set_status(mem, chan, ARG_S32(4), ctx->r[5]), cb, false);
}

// CARDFreeBlocks(chan, s32* byteNotUsed, s32* filesNotUsed)
void card_free_blocks(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    uint32_t bytes_ptr = ctx->r[4], files_ptr = ctx->r[5];
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r == kReady) {
        if (bytes_ptr) mem->write32(bytes_ptr, s->img.free_blocks() * kBlockSize);
        if (files_ptr) mem->write32(files_ptr, s->img.free_entries());
    }
    sync_done(ctx, chan, r);
}

// CARDGetSectorSize(chan, u32* size), CARDGetMemSize(chan, u16* sizeMbit), CARDGetEncoding(chan, u16*)
void card_get_sector_size(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r == kReady && ctx->r[4]) mem->write32(ctx->r[4], kBlockSize);
    sync_done(ctx, chan, r);
}
void card_get_mem_size(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r == kReady && ctx->r[4]) mem->write16(ctx->r[4], (uint16_t)s->img.size_mbit());
    sync_done(ctx, chan, r);
}
void card_get_encoding(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    std::lock_guard<std::mutex> lk(g_mu);
    Slot* s;
    int32_t r = file_slot(chan, &s);
    if (r == kReady && ctx->r[4]) mem->write16(ctx->r[4], s->img.encoding());
    sync_done(ctx, chan, r);
}

// CARDGetResultCode(chan): the result of the last operation on the channel
void card_get_result_code(PPCContext* ctx, Memory* mem) {
    int32_t chan = ARG_S32(3);
    card_run_pending_callbacks(ctx, mem);
    if (chan < 0 || chan >= kSlots) {
        ctx->r[3] = (uint32_t)kFatalError;
        return;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    ctx->r[3] = (uint32_t)g_slot[chan].result;
}

#undef ARG_S32

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"CARDInit", card_init},
    {"CARDProbe", card_probe},
    {"CARDProbeEx", card_probe_ex},
    {"CARDMount", card_mount},
    {"CARDMountAsync", card_mount_async},
    {"CARDUnmount", card_unmount},
    {"CARDCheck", card_check},
    {"CARDCheckAsync", card_check_async},
    {"CARDCheckEx", card_check_ex},
    {"CARDCheckExAsync", card_check_ex_async},
    {"CARDFormat", card_format},
    {"CARDFormatAsync", card_format_async},
    {"CARDOpen", card_open},
    {"CARDFastOpen", card_fast_open},
    {"CARDClose", card_close},
    {"CARDCreate", card_create},
    {"CARDCreateAsync", card_create_async},
    {"CARDRead", card_read},
    {"CARDReadAsync", card_read_async},
    {"CARDWrite", card_write},
    {"CARDWriteAsync", card_write_async},
    {"CARDDelete", card_delete},
    {"CARDDeleteAsync", card_delete_async},
    {"CARDFastDelete", card_fast_delete},
    {"CARDFastDeleteAsync", card_fast_delete_async},
    {"CARDRename", card_rename},
    {"CARDRenameAsync", card_rename_async},
    {"CARDGetStatus", card_get_status},
    {"CARDSetStatus", card_set_status},
    {"CARDSetStatusAsync", card_set_status_async},
    {"CARDFreeBlocks", card_free_blocks},
    {"CARDGetSectorSize", card_get_sector_size},
    {"CARDGetMemSize", card_get_mem_size},
    {"CARDGetEncoding", card_get_encoding},
    {"CARDGetResultCode", card_get_result_code},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_card_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

void card_run_pending_callbacks(PPCContext* ctx, Memory* mem) {
    static thread_local bool running = false;  // a callback that polls must not re-enter
    if (running) return;
    running = true;
    for (;;) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_pending.empty()) break;
            p = g_pending.front();
            g_pending.pop_front();
        }
        run_callback(ctx, mem, p.cb, p.chan, p.result);
    }
    running = false;
}

void set_card_image(const std::string& path, size_t size_bytes) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pending.clear();
    g_path = path;
    g_size = size_bytes;
    for (Slot& s : g_slot) s = Slot();
}

}  // namespace gcrecomp
