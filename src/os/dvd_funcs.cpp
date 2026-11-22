// SPDX-License-Identifier: MIT
//
// DVD status and asynchronous read HLE.
//
// The SDK's DVD driver is a state machine driven by DI interrupts. With no DI hardware, recompiled
// driver code never advances. These replacements report an idle drive with a disc inserted, and
// complete asynchronous reads at once by reusing the synchronous DVDReadPrio replacement.
#include <cstdio>
#include <cstring>

#include "gcrecomp/runtime.h"

namespace gcrecomp {

namespace {

constexpr uint32_t kStateEnd = 0;  // DVD_STATE_END: idle, last command finished

// DVDGetDriveStatus(void) -> DVD_STATE_END
void dvd_get_drive_status(PPCContext* ctx, Memory*) { ctx->r[3] = kStateEnd; }

// DVDGetCommandBlockStatus(DVDCommandBlock*) -> DVD_STATE_END
void dvd_get_command_block_status(PPCContext* ctx, Memory*) { ctx->r[3] = kStateEnd; }

// DVDCheckDisk(void) -> TRUE
void dvd_check_disk(PPCContext* ctx, Memory*) { ctx->r[3] = 1; }

// DVDReadAsyncPrio(DVDFileInfo* file, void* addr, s32 length, s32 offset, DVDCallback cb, s32 prio)
// DVDCallback: void (*)(s32 result, DVDFileInfo* file)
void dvd_read_async_prio(PPCContext* ctx, Memory* mem) {
    RecompiledFunc read_prio = lookup_os_func("DVDReadPrio");
    if (!read_prio) {
        std::printf("[DVD] DVDReadAsyncPrio: no DVDReadPrio replacement\n");
        ctx->r[3] = 0;
        return;
    }
    const uint32_t file = ctx->r[3];
    const uint32_t callback = ctx->r[7];
    ctx->r[7] = ctx->r[8];  // DVDReadPrio takes the priority as its fifth argument (r7)
    read_prio(ctx, mem);
    const uint32_t result = ctx->r[3];
    if (callback) {
        PPCContext saved = *ctx;
        ctx->r[3] = result;
        ctx->r[4] = file;
        g_func_table.call(callback, ctx, mem);
        *ctx = saved;
    }
    ctx->r[3] = 1;  // the request was accepted
}

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"DVDGetDriveStatus", dvd_get_drive_status},
    {"DVDGetCommandBlockStatus", dvd_get_command_block_status},
    {"DVDCheckDisk", dvd_check_disk},
    {"DVDReadAsyncPrio", dvd_read_async_prio},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_dvd_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
