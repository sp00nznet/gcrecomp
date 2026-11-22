// SPDX-License-Identifier: MIT
//
// Recompiler: every integer instruction with the record bit set (the "." forms) must update cr0 from
// its result. The 64-bit division helper of the Metrowerks runtime ends a loop with `subfe.`; with
// the record bit dropped the loop's `blt` read a stale cr0 and every division gave a wrong quotient,
// so strtod() and sprintf("%f") never converged.
#include <cstdio>
#include <string>

#include "gcrecomp/ppc.h"
#include "gcrecomp/ppc_to_c.h"

using namespace gcrecomp;

static int g_failures = 0;

static std::string emit_one(uint32_t raw) {
    FILE* f = std::tmpfile();
    PPCToCEmitter emitter(f);
    emitter.emit_insn(ppc_disasm(raw, 0x80000000));
    std::fflush(f);
    std::rewind(f);
    std::string text;
    char buf[512];
    while (std::fgets(buf, sizeof buf, f)) text += buf;
    std::fclose(f);
    return text;
}

static uint32_t xo_form(uint32_t xo, uint32_t rt, uint32_t ra, uint32_t rb, uint32_t rc) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}

struct Case {
    const char* name;
    uint32_t xo;
    bool dest_is_ra;  // logical, shift and extend forms write rA; arithmetic forms write rD
};

int main() {
    const Case cases[] = {
        {"addc", 10, false},  {"adde", 138, false}, {"addze", 202, false}, {"subfc", 8, false},
        {"subfe", 136, false}, {"mulhw", 75, false}, {"mulhwu", 11, false}, {"divw", 491, false},
        {"divwu", 459, false}, {"nand", 476, true},  {"andc", 60, true},    {"orc", 412, true},
        {"eqv", 284, true},    {"slw", 24, true},    {"srw", 536, true},    {"sraw", 792, true},
        {"srawi", 824, true},  {"cntlzw", 26, true},
    };
    for (const Case& c : cases) {
        const uint32_t rt = 3, ra = 4, rb = 5;
        const std::string with_rc = emit_one(xo_form(c.xo, rt, ra, rb, 1));
        const std::string without = emit_one(xo_form(c.xo, rt, ra, rb, 0));
        const std::string want = "update_cr0((int32_t)ctx->r[" + std::to_string(c.dest_is_ra ? ra : rt) + "])";
        if (with_rc.find(want) == std::string::npos) {
            std::printf("%s.: no cr0 update of the result\n%s", c.name, with_rc.c_str());
            ++g_failures;
        }
        if (without.find("update_cr0") != std::string::npos) {
            std::printf("%s: cr0 updated without the record bit\n%s", c.name, without.c_str());
            ++g_failures;
        }
    }
    // rlwimi. (opcode 20) and rlwnm. (opcode 21): rS=3 rA=4 SH/rB=5 MB=0 ME=31
    for (uint32_t opcd : {20u, 21u}) {
        const uint32_t base = (opcd << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (0u << 6) | (31u << 1);
        if (emit_one(base | 1).find("update_cr0((int32_t)ctx->r[4])") == std::string::npos) {
            std::printf("rlw op %u.: no cr0 update\n", opcd);
            ++g_failures;
        }
        if (emit_one(base).find("update_cr0") != std::string::npos) {
            std::printf("rlw op %u: cr0 updated without the record bit\n", opcd);
            ++g_failures;
        }
    }
    if (g_failures) std::printf("%d failure(s)\n", g_failures);
    else std::printf("recompiler_rc: all checks passed\n");
    return g_failures ? 1 : 0;
}
