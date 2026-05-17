#pragma once
// =============================================================================
// gcrecomp — PPC to C Emitter
//
// Translates each PowerPC instruction into a C expression operating
// on a PPCContext struct. The generated code compiles with any modern
// C/C++ compiler and runs natively.
// =============================================================================

#include "gcrecomp/ppc.h"
#include "gcrecomp/cfg.h"
#include <cstdio>
#include <vector>

namespace gcrecomp {

class PPCToCEmitter {
public:
    FILE* out;
    int   indent_level;
    std::vector<uint32_t> block_addrs;
    const BasicBlock* current_block = nullptr;
    const std::map<uint32_t, Function>* func_map = nullptr;

    PPCToCEmitter(FILE* f) : out(f), indent_level(1) {}

    void emit(const char* fmt, ...);
    void emit_raw(const char* fmt, ...);
    void emit_insn(const PPCInsn& insn);

    // Resolve a branch-target address to its declared function name
    // (uses func_map when available; falls back to "func_XXXXXXXX").
    std::string name_for(uint32_t addr) const;
};

void emit_file_header(FILE* out, const char* project_name = "gcrecomp");

} // namespace gcrecomp
