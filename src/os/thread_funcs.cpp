// SPDX-License-Identifier: MIT
//
// Thread scheduler HLE (single-threaded).
//
// The runtime does not run guest threads: OSCreateThread only fills the thread structure. The SDK's
// scheduler (SelectThread and friends) spins forever when no thread is runnable, so the calls that
// reach it are replaced by no-ops. A game whose own logic depends on its other threads running will
// not work with these; real threads need a scheduler that runs each guest thread on its own host
// thread.
#include <cstring>

#include "gcrecomp/runtime.h"

namespace gcrecomp {

namespace {

void ret_zero(PPCContext* ctx, Memory*) { ctx->r[3] = 0; }
void ret_one(PPCContext* ctx, Memory*) { ctx->r[3] = 1; }
void nothing(PPCContext*, Memory*) {}

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"OSSuspendThread", ret_zero},      // returns the previous suspend count
    {"OSDisableScheduler", ret_zero},   // returns the previous disable count
    {"OSEnableScheduler", ret_zero},
    {"OSSetThreadPriority", ret_one},   // TRUE
    {"OSExitThread", nothing},
    {"OSCancelThread", nothing},
    {"OSYieldThread", nothing},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_thread_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
