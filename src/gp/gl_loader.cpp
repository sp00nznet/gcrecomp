// SPDX-License-Identifier: MIT
// Resolves the OpenGL entry points declared in gl_loader.h through SDL_GL_GetProcAddress.
#include "gl_loader.h"

#include <SDL3/SDL.h>

#include <cstdio>

#define GCRECOMP_GL_DEFINE(ret, name, params) ret(GCRECOMP_GLAPI* name) params = nullptr;
GCRECOMP_GL_FUNCTIONS(GCRECOMP_GL_DEFINE)

namespace gcrecomp::gp {

bool gl_load_functions() {
    bool ok = true;
#define GCRECOMP_GL_LOAD(ret, name, params)                                                         \
    name = reinterpret_cast<decltype(name)>(reinterpret_cast<void*>(SDL_GL_GetProcAddress(#name))); \
    if (!name) {                                                                                     \
        std::fprintf(stderr, "[GL] missing OpenGL entry point %s\n", #name);                         \
        ok = false;                                                                                  \
    }
    GCRECOMP_GL_FUNCTIONS(GCRECOMP_GL_LOAD)
#undef GCRECOMP_GL_LOAD
    return ok;
}

}  // namespace gcrecomp::gp
