// SPDX-License-Identifier: MIT
//
// DVD status and asynchronous read HLE.
//
// The SDK's DVD driver is a state machine driven by DI interrupts. With no DI hardware, recompiled
// driver code never advances. These replacements report an idle drive with a disc inserted, and
// transfer asynchronous reads at once by reusing the synchronous DVDReadPrio replacement and run their callbacks
// afterwards.
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>

#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"

namespace gcrecomp {

namespace {

constexpr uint32_t kStateEnd = 0;  // DVD_STATE_END: idle, last command finished

// DVDGetDriveStatus(void) -> DVD_STATE_END
// Games spin on it (the disc error handler loop), so it is a point where other threads may run.
void dvd_get_drive_status(PPCContext* ctx, Memory* mem) {
    sched::poll(ctx, mem);
    if (sched::interrupts_enabled()) dvd_run_pending_callbacks(ctx, mem);
    ctx->r[3] = kStateEnd;
}

// DVDGetCommandBlockStatus(DVDCommandBlock*) -> DVD_STATE_END
void dvd_get_command_block_status(PPCContext* ctx, Memory* mem) {
    if (sched::interrupts_enabled()) dvd_run_pending_callbacks(ctx, mem);
    ctx->r[3] = kStateEnd;
}

// DVDCheckDisk(void) -> TRUE
void dvd_check_disk(PPCContext* ctx, Memory*) { ctx->r[3] = 1; }

// Asynchronous reads. The data is transferred at once, but the callback is only queued: on the console the drive
// raises an interrupt long after DVDReadAsyncPrio returned, and the game relies on it. Tales of Symphonia's file
// loader marks its slot "loading" (state 2) after the call returns and its callback marks it "loaded" (state 3):
// a callback run inside the call is overwritten by the caller, the slot stays "loading" forever and the first field
// scene never fades in. The queued callbacks run from the VI retrace interrupt (threads.cpp) and when the game polls
// the drive status, the way a spinning loop learns that the command finished.
struct Pending {
    uint32_t callback;
    int32_t result;
    uint32_t file;
};
std::mutex g_mu;
std::deque<Pending> g_pending;

// DVDReadAsyncPrio(DVDFileInfo* file, void* addr, s32 length, s32 offset, DVDCallback cb, s32 prio)
// DVDReadAsync(file, addr, length, offset, cb) is the same with priority 2.
// DVDCallback: void (*)(s32 result, DVDFileInfo* file); result is the number of bytes transferred, -1 on failure.
void read_async(PPCContext* ctx, Memory* mem, uint32_t prio) {
    RecompiledFunc read_prio = lookup_os_func("DVDReadPrio");
    if (!read_prio) {
        std::printf("[DVD] DVDReadAsyncPrio: no DVDReadPrio replacement\n");
        ctx->r[3] = 0;
        return;
    }
    const uint32_t file = ctx->r[3];
    const int32_t length = (int32_t)ctx->r[5];
    const uint32_t callback = ctx->r[7];
    ctx->r[7] = prio;  // DVDReadPrio takes the priority as its fifth argument (r7)
    read_prio(ctx, mem);
    int32_t result = (int32_t)ctx->r[3];
    if (result == 0 && length > 0) result = -1;  // nothing transferred: unknown file or past the end of the disc
    if (callback) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pending.push_back({callback, result, file});
    }
    ctx->r[3] = 1;  // the request was accepted
}

void dvd_read_async_prio(PPCContext* ctx, Memory* mem) { read_async(ctx, mem, ctx->r[8]); }
void dvd_read_async(PPCContext* ctx, Memory* mem) { read_async(ctx, mem, 2); }

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"DVDGetDriveStatus", dvd_get_drive_status},
    {"DVDGetCommandBlockStatus", dvd_get_command_block_status},
    {"DVDCheckDisk", dvd_check_disk},
    {"DVDReadAsync", dvd_read_async},
    {"DVDReadAsyncPrio", dvd_read_async_prio},
    {nullptr, nullptr},
};

}  // namespace

void dvd_run_pending_callbacks(PPCContext* ctx, Memory* mem) {
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
        PPCContext saved = *ctx;
        ctx->r[3] = (uint32_t)p.result;
        ctx->r[4] = p.file;
        g_func_table.call(p.callback, ctx, mem);
        *ctx = saved;
    }
    running = false;
}

RecompiledFunc lookup_dvd_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
