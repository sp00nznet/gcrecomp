// SPDX-License-Identifier: MIT
//
// CARD HLE tests: drive the SDK entry points through the same register convention the recompiled
// game uses (arguments in r3.., result in r3) against a card image in a temporary file.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "gcrecomp/runtime.h"

using namespace gcrecomp;

static int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)
#define CHECK_EQ(a, b) CHECK((long long)(a) == (long long)(b))

static int32_t g_cb_chan = 99, g_cb_result = 99;
static int g_cb_calls = 0;
static void test_callback(PPCContext* ctx, Memory*) {
    g_cb_chan = (int32_t)ctx->r[3];
    g_cb_result = (int32_t)ctx->r[4];
    ++g_cb_calls;
    ctx->r[3] = 0xDEAD;  // must not leak into the caller's r3
}
constexpr uint32_t kCbAddr = 0x80700000;

static int32_t call(const char* name, std::initializer_list<uint32_t> args) {
    RecompiledFunc f = lookup_os_func(name);
    if (!f) {
        std::printf("no replacement for %s\n", name);
        ++g_failures;
        return 12345;
    }
    PPCContext ctx = {};
    int n = 3;
    for (uint32_t a : args) ctx.r[n++] = a;
    f(&ctx, &g_mem);
    return (int32_t)ctx.r[3];
}

static void drain() {
    PPCContext ctx = {};
    ctx.r[3] = 0x1234;
    card_run_pending_callbacks(&ctx, &g_mem);
    CHECK_EQ(ctx.r[3], 0x1234);  // the interrupted code's registers are intact
}

static void put_string(uint32_t addr, const char* s) {
    do g_mem.write8(addr++, (uint8_t)*s); while (*s++);
}

int main() {
    CHECK(g_mem.init());
    g_func_table.register_func(kCbAddr, test_callback);
    std::memcpy(g_mem.translate(0x80000000), "GQSF8P", 6);  // the game id the SDK reads from RAM

    namespace fs = std::filesystem;
    fs::path img = fs::temp_directory_path() / "gcrecomp_card_hle_test.raw";
    fs::remove(img);

    const uint32_t kName = 0x80600000, kInfo = 0x80600100, kBuf = 0x80610000, kOut = 0x80620000, kStat = 0x80600200;
    const uint32_t kMem = 0x80600300, kSec = 0x80600304;

    // No card: the switch is off and no path is set.
    CHECK_EQ(call("CARDProbeEx", {0, kMem, kSec}), -3);
    g_card_inserted = true;
    CHECK_EQ(call("CARDProbeEx", {0, kMem, kSec}), -3);  // no image path
    set_card_image(img.string(), 0x100000);
    CHECK_EQ(call("CARDProbeEx", {1, kMem, kSec}), -3);  // slot B is always empty
    CHECK_EQ(call("CARDProbeEx", {0, kMem, kSec}), 0);
    CHECK_EQ(g_mem.read32(kMem), 8);        // megabits, not bytes
    CHECK_EQ(g_mem.read32(kSec), 0x2000);

    // Blank card: mounting reports it broken but formattable; nothing is on disk yet.
    CHECK_EQ(call("CARDMount", {0, 0x80630000, 0}), -6);
    CHECK_EQ(call("CARDGetResultCode", {0}), -6);
    CHECK_EQ(call("CARDOpen", {0, kName, kInfo}), -6);
    CHECK(!fs::exists(img));
    g_cb_calls = 0;
    CHECK_EQ(call("CARDFormatAsync", {0, kCbAddr}), 0);
    CHECK_EQ(g_cb_calls, 0);  // the callback runs after the call returned
    drain();
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_chan, 0);
    CHECK_EQ(g_cb_result, 0);
    CHECK_EQ(fs::file_size(img), 0x100000);
    CHECK_EQ(call("CARDUnmount", {0}), 0);
    CHECK_EQ(call("CARDMount", {0, 0x80630000, 0}), 0);

    CHECK_EQ(call("CARDFreeBlocks", {0, kOut, kOut + 4}), 0);
    CHECK_EQ(g_mem.read32(kOut), 123 * 0x2000);
    CHECK_EQ(g_mem.read32(kOut + 4), 127);

    // Create asynchronously: callback runs, file info is filled in.
    put_string(kName, "SYMPHONIA");
    g_cb_calls = 0;
    CHECK_EQ(call("CARDCreateAsync", {0, kName, 3 * 0x2000, kInfo, kCbAddr}), 0);
    CHECK_EQ(g_cb_calls, 0);
    drain();
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_result, 0);
    CHECK_EQ(g_mem.read32(kInfo + 0), 0);   // chan
    CHECK_EQ(g_mem.read32(kInfo + 4), 0);   // fileNo
    CHECK_EQ(g_mem.read16(kInfo + 0x10), 5);  // iBlock
    CHECK_EQ(call("CARDCreate", {0, kName, 0x2000, kInfo + 0x40}), -7);  // exists
    // An error found before the transfer is returned, the callback is not run.
    g_cb_calls = 0;
    CHECK_EQ(call("CARDCreateAsync", {0, kName, 0x2000, kInfo + 0x40, kCbAddr}), -7);
    drain();
    CHECK_EQ(g_cb_calls, 0);
    CHECK_EQ(call("CARDCreate", {0, kName, 0x2001, kInfo + 0x40}), -128);

    // Write one sector through the async call and read it back with the sync call.
    for (uint32_t i = 0; i < 0x2000; ++i) g_mem.write8(kBuf + i, (uint8_t)(i * 5));
    CHECK_EQ(call("CARDWriteAsync", {kInfo, kBuf, 0x2000, 0x2000, kCbAddr}), 0);
    // Polling the card delivers the callback, as the game's wait loop does.
    CHECK_EQ(call("CARDProbeEx", {0, kMem, kSec}), 0);
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(call("CARDRead", {kInfo, kOut, 0x2000, 0x2000}), 0);
    CHECK(std::memcmp(g_mem.translate(kBuf), g_mem.translate(kOut), 0x2000) == 0);
    CHECK_EQ(call("CARDRead", {kInfo, kOut, 0x2000, 0x6000}), -11);  // past the end
    CHECK_EQ(call("CARDWrite", {kInfo, kBuf, 100, 0}), -128);

    // Close, reopen by name and by number; status carries length, name, game id and offsets.
    CHECK_EQ(call("CARDClose", {kInfo}), 0);
    CHECK_EQ(call("CARDRead", {kInfo, kOut, 0x200, 0}), -128);  // closed file info
    CHECK_EQ(call("CARDOpen", {0, kName, kInfo}), 0);
    CHECK_EQ(call("CARDFastOpen", {0, 0, kInfo + 0x40}), 0);
    CHECK_EQ(call("CARDFastOpen", {0, 1, kInfo + 0x40}), -4);
    CHECK_EQ(call("CARDGetStatus", {0, 0, kStat}), 0);
    CHECK(std::memcmp(g_mem.translate(kStat), "SYMPHONIA", 10) == 0);
    CHECK_EQ(g_mem.read32(kStat + 0x20), 3 * 0x2000);
    CHECK(std::memcmp(g_mem.translate(kStat + 0x28), "GQSF8P", 6) == 0);
    CHECK_EQ(g_mem.read32(kStat + 0x30), 0xFFFFFFFFu);  // no icon
    CHECK_EQ(g_mem.read32(kStat + 0x68), 0);            // data starts at offset 0

    // Banner (RGB5A3) plus two C8 icons: banner 96*32*2 = 6144 bytes, icons 32*32 = 1024 bytes each,
    // then one 512-byte palette shared by the icons, then the data.
    g_mem.write8(kStat + 0x2E, 2);
    g_mem.write32(kStat + 0x30, 0);
    g_mem.write16(kStat + 0x34, 0x0005);  // icons 0 and 1: C8 (1)
    g_mem.write16(kStat + 0x36, 0x0005);
    g_mem.write32(kStat + 0x38, 0x2000 - 64);
    CHECK_EQ(call("CARDSetStatus", {0, 0, kStat}), 0);
    for (uint32_t i = 0; i < 0x6C; ++i) g_mem.write8(kStat + i, 0);
    CHECK_EQ(call("CARDGetStatus", {0, 0, kStat}), 0);
    CHECK_EQ(g_mem.read8(kStat + 0x2E), 2);
    CHECK_EQ(g_mem.read32(kStat + 0x38), 0x2000 - 64);
    CHECK_EQ(g_mem.read32(kStat + 0x3C), 0);
    CHECK_EQ(g_mem.read32(kStat + 0x40), 0xFFFFFFFFu);  // RGB5A3 banner has no palette
    CHECK_EQ(g_mem.read32(kStat + 0x44), 6144);
    CHECK_EQ(g_mem.read32(kStat + 0x48), 7168);
    CHECK_EQ(g_mem.read32(kStat + 0x4C), 0xFFFFFFFFu);
    CHECK_EQ(g_mem.read32(kStat + 0x64), 8192);
    CHECK_EQ(g_mem.read32(kStat + 0x68), 8192 + 512);
    g_mem.write32(kStat + 0x30, 0x6000);  // icon address outside the file
    g_mem.write32(kStat + 0x30, 3 * 0x2000);
    CHECK_EQ(call("CARDSetStatus", {0, 0, kStat}), -128);
    g_cb_calls = 0;
    g_mem.write32(kStat + 0x30, 0);
    CHECK_EQ(call("CARDSetStatusAsync", {0, 0, kStat, kCbAddr}), 0);
    drain();
    CHECK_EQ(g_cb_calls, 1);

    // Another game id cannot touch the save.
    std::memcpy(g_mem.translate(0x80000000), "GZLE01", 6);
    CHECK_EQ(call("CARDOpen", {0, kName, kInfo + 0x80}), -10);
    CHECK_EQ(call("CARDDelete", {0, kName}), -10);
    std::memcpy(g_mem.translate(0x80000000), "GQSF8P", 6);

    // The image on disk is what a second runtime would see: re-point at the file and read the save.
    set_card_image(img.string());
    CHECK_EQ(call("CARDMount", {0, 0x80630000, 0}), 0);
    CHECK_EQ(call("CARDOpen", {0, kName, kInfo}), 0);
    CHECK_EQ(call("CARDRead", {kInfo, kOut, 0x2000, 0x2000}), 0);
    CHECK(std::memcmp(g_mem.translate(kBuf), g_mem.translate(kOut), 0x2000) == 0);

    // CARDCheckEx reports no bytes to repair; delete frees the space and the name.
    g_mem.write32(kOut, 77);
    CHECK_EQ(call("CARDCheckEx", {0, kOut}), 0);
    CHECK_EQ(g_mem.read32(kOut), 0);
    g_cb_calls = 0;
    CHECK_EQ(call("CARDDeleteAsync", {0, kName, kCbAddr}), 0);
    drain();
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(call("CARDOpen", {0, kName, kInfo}), -4);
    CHECK_EQ(call("CARDFreeBlocks", {0, kOut, kOut + 4}), 0);
    CHECK_EQ(g_mem.read32(kOut), 123 * 0x2000);
    CHECK_EQ(g_mem.read32(kOut + 4), 127);

    // Unmounted: operations report no card; with the switch off the card disappears again.
    CHECK_EQ(call("CARDUnmount", {0}), 0);
    CHECK_EQ(call("CARDOpen", {0, kName, kInfo}), -3);
    g_card_inserted = false;
    CHECK_EQ(call("CARDProbeEx", {0, kMem, kSec}), -3);
    CHECK_EQ(call("CARDMount", {0, 0x80630000, 0}), -3);

    fs::remove(img);
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all card HLE tests passed\n");
    return 0;
}
