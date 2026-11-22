// SPDX-License-Identifier: MIT
//
// GX synchronization HLE.
//
// The GP module consumes the command stream synchronously as the game writes it, so by the time a
// GX call returns the GPU has finished. The SDK waits for the "draw done" interrupt by putting the
// thread to sleep, which never wakes without interrupt delivery; these replacements return at once.
#include <cstring>

#include "gcrecomp/runtime.h"

namespace gcrecomp {

namespace {

// GXDrawDone(void)
void gx_draw_done(PPCContext*, Memory*) {}

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"GXDrawDone", gx_draw_done},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_gx_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
