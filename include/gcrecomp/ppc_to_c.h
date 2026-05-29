#pragma once
// =============================================================================
// gcrecomp — PPC to C Emitter
//
// Translates each PowerPC instruction into a C expression operating
// on a PPCContext struct. The generated code compiles with any modern
// C/C++ compiler and runs natively.
// =============================================================================

#include "gcrecomp/ppc.h"
#include <cstdio>
#include <vector>

namespace gcrecomp {

struct SymbolMap;  // fwd; defined in symbol_map.h

class PPCToCEmitter {
public:
    FILE* out;
    int   indent_level;
    std::vector<uint32_t> block_addrs;
    // Optional: when set, direct branches resolve target addresses to
    // symbolic names. Unset addresses fall back to func_XXXXXXXX.
    const SymbolMap* syms = nullptr;

    PPCToCEmitter(FILE* f) : out(f), indent_level(1) {}

    void emit(const char* fmt, ...);
    void emit_raw(const char* fmt, ...);
    void emit_insn(const PPCInsn& insn);
};

void emit_file_header(FILE* out, const char* project_name = "gcrecomp");

} // namespace gcrecomp
