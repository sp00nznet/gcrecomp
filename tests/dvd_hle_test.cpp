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
