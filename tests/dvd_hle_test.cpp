// SPDX-License-Identifier: MIT
//
// DVD HLE tests: opening a disc file by path (DVDOpen) and by entry number (DVDConvertPathToEntrynum +
// DVDFastOpen, which is how the Hudson HVQM4 movie player of Tales of Symphonia opens "mov/op.h4m").
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static int g_cb_calls = 0;
static int32_t g_cb_result = 0;
static uint32_t g_cb_file = 0;
static void test_callback(PPCContext* ctx, Memory*) {
    ++g_cb_calls;
    g_cb_result = (int32_t)ctx->r[3];
    g_cb_file = ctx->r[4];
}

static void put_string(uint32_t addr, const char* s) {
    do g_mem.write8(addr++, (uint8_t)*s); while (*s++);
}

int main() {
    CHECK(g_mem.init());
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "gcrecomp_dvd_hle_test";
    fs::remove_all(root);
    fs::create_directories(root / "MOV");
    std::ofstream(root / "MOV" / "op.h4m", std::ios::binary) << "HVQM4 1.5";
    set_game_root(root.string());

    const uint32_t kPath = 0x80600000, kInfo = 0x80600100, kInfo2 = 0x80600200, kBuf = 0x80610000;

    // The game asks for "mov/op.h4m"; the disc is case-insensitive, the host is not.
    put_string(kPath, "mov/op.h4m");
    const int32_t entry = call("DVDConvertPathToEntrynum", {kPath});
    CHECK(entry > 0);  // entry 0 is the root directory
    CHECK_EQ(call("DVDConvertPathToEntrynum", {kPath}), entry);  // stable for the same path

    put_string(kPath, "mov/missing.h4m");
    CHECK_EQ(call("DVDConvertPathToEntrynum", {kPath}), -1);

    CHECK_EQ(call("DVDFastOpen", {(uint32_t)entry, kInfo}), 1);
    CHECK_EQ(g_mem.read32(kInfo + 0x34), 9);  // DVDFileInfo.length
    CHECK_EQ(call("DVDReadPrio", {kInfo, kBuf, 5, 4, 2}), 5);
    CHECK(std::memcmp(g_mem.translate(kBuf), "4 1.5", 5) == 0);

    // The file is 9 bytes: its last 32 byte block holds all of it. A read that ends inside the block
    // transfers the whole request (zero padded) and reports it; one past the block is short.
    std::memset(g_mem.translate(kBuf), 0xAA, 64);
    CHECK_EQ(call("DVDReadPrio", {kInfo, kBuf, 32, 0, 2}), 32);
    CHECK(std::memcmp(g_mem.translate(kBuf), "HVQM4 1.5", 9) == 0);
    CHECK_EQ(g_mem.read8(kBuf + 9), 0);
    CHECK_EQ(g_mem.read8(kBuf + 31), 0);
    CHECK_EQ(g_mem.read8(kBuf + 32), 0xAA);  // not past the request
    CHECK_EQ(call("DVDReadPrio", {kInfo, kBuf, 64, 0, 2}), 9);
    CHECK_EQ(call("DVDClose", {kInfo}), 1);

    // Asynchronous reads: the data is there when the call returns, the callback runs afterwards (never inside the
    // call: the game's loader marks its slot "loading" after the call returns and its callback marks it "loaded").
    const uint32_t kCb = 0x80004000;
    g_func_table.register_func(kCb, test_callback);
    put_string(kPath, "mov/op.h4m");
    CHECK_EQ(call("DVDOpen", {kPath, kInfo}), 1);
    std::memset(g_mem.translate(kBuf), 0xAA, 64);
    CHECK_EQ(call("DVDReadAsyncPrio", {kInfo, kBuf, 5, 4, kCb, 2}), 1);
    CHECK_EQ(g_cb_calls, 0);
    CHECK(std::memcmp(g_mem.translate(kBuf), "4 1.5", 5) == 0);
    {
        PPCContext ctx = {};
        dvd_run_pending_callbacks(&ctx, &g_mem);
    }
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(g_cb_result, 5);  // bytes transferred
    CHECK_EQ(g_cb_file, kInfo);
    CHECK_EQ(call("DVDReadAsync", {kInfo, kBuf, 5, 0, kCb}), 1);  // plain DVDReadAsync: no priority argument
    CHECK_EQ(g_cb_calls, 1);
    CHECK_EQ(call("DVDGetCommandBlockStatus", {kInfo}), 0);  // polling the drive delivers the callback, then reports idle
    CHECK_EQ(g_cb_calls, 2);
    CHECK_EQ(call("DVDReadAsyncPrio", {0x80600300, kBuf, 5, 0, kCb, 2}), 1);  // not an open file: the callback reports -1
    CHECK_EQ(call("DVDGetDriveStatus", {}), 0);
    CHECK_EQ(g_cb_calls, 3);
    CHECK_EQ(g_cb_result, -1);
    call("DVDClose", {kInfo});

    CHECK_EQ(call("DVDFastOpen", {0, kInfo2}), 0);        // the root directory is not a file
    CHECK_EQ(call("DVDFastOpen", {999, kInfo2}), 0);      // no such entry

    put_string(kPath, "MOV/op.h4m");
    CHECK_EQ(call("DVDOpen", {kPath, kInfo2}), 1);
    CHECK_EQ(g_mem.read32(kInfo2 + 0x34), 9);
    call("DVDClose", {kInfo2});

    fs::remove_all(root);
    if (g_failures) std::printf("%d failure(s)\n", g_failures);
    else std::printf("dvd_hle: all checks passed\n");
    return g_failures ? 1 : 0;
}
