// =============================================================================
// PowerPC to C Static Recompiler
// The magic: translates each PPC instruction into a C expression that
// operates on a PPCContext struct. The generated C code compiles with
// any modern C compiler and runs natively on x86-64.
//
// Example:
//   addi r3, r4, 0x20  ->  ctx->r[3] = (int32_t)ctx->r[4] + 0x20;
//   lwz r5, 0x10(r6)   ->  ctx->r[5] = MEM_READ32(ctx->r[6] + 0x10);
//   bl func_80123456    ->  func_80123456(ctx, mem);
// =============================================================================

#include "gcrecomp/ppc_to_c.h"
#include "gcrecomp/symbol_map.h"
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <set>
#include <map>

namespace gcrecomp {

    void PPCToCEmitter::emit(const char* fmt, ...) {
        for (int i = 0; i < indent_level; i++) fprintf(out, "    ");
        va_list args;
        va_start(args, fmt);
        vfprintf(out, fmt, args);
        va_end(args);
        fprintf(out, "\n");
    }

    void PPCToCEmitter::emit_raw(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vfprintf(out, fmt, args);
        va_end(args);
    }

    // A single-precision scalar result (fadds, fmuls, fmadds, frsp, fres ...): rounded to single and written to both
    // halves of the register, ps0 (the double f[d]) and ps1. `fmt` is the expression, in terms of the operands'
    // double registers; operands are NOT rounded to single first, which would break the common int-to-float
    // trick (subtracting 2^52 from 2^52 + n).
    static void emit_fs(PPCToCEmitter& e, unsigned d, const char* fmt, ...) {
        char expr[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(expr, sizeof expr, fmt, args);
        va_end(args);
        e.emit("{ float r = (float)(%s); ctx->f[%u] = (double)r; ctx->ps1[%u] = r; }", expr, d, d);
    }

    // A paired-single instruction: both result lanes are computed from the operands as they were before the
    // instruction (the destination may be one of the sources), then written. `ops` lists the operands the
    // expressions use ("a", "b", "c" for frA, frB, frC); the expressions name their lanes a0 a1 b0 b1 c0 c1.
    static void emit_ps(PPCToCEmitter& e, const char* ops, const PPCInsn& insn, const char* e0, const char* e1) {
        std::string line = "{";
        for (const char* o = ops; *o; ++o) {
            const unsigned r = *o == 'a' ? insn.ra : *o == 'b' ? insn.rb : insn.rc_reg;
            char buf[160];
            std::snprintf(buf, sizeof buf, " [[maybe_unused]] const float %c0 = (float)ctx->f[%u], %c1 = ctx->ps1[%u];", *o, r, *o, r);
            line += buf;
        }
        char buf[320];
        std::snprintf(buf, sizeof buf, " const float r0 = %s, r1 = %s; ctx->f[%u] = (double)r0; ctx->ps1[%u] = r1; }", e0, e1,
                      insn.rd, insn.rd);
        e.emit("%s", (line + buf).c_str());
    }

    std::string PPCToCEmitter::name_for(uint32_t addr) const {
        // A Ghidra symbol map, when supplied, carries real names rather
        // than func_XXXXXXXX. Checked first so symbols apply at every
        // naming site, not just direct branches.
        if (syms) {
            std::string n = syms->get_name(addr);
            if (!n.empty() && n.rfind("func_", 0) != 0) return n;
        }
        if (func_map) {
            auto it = func_map->find(addr);
            if (it != func_map->end() && !it->second.name.empty())
                return it->second.name;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "func_%08X", addr);
        return buf;
    }

    void PPCToCEmitter::emit_insn(const PPCInsn& insn) {
        // Comment with original instruction
        emit("// 0x%08X: %s", insn.address, insn.mnemonic.c_str());

        switch (insn.type) {
        // ==== Integer Arithmetic ====
        case PPCInsnType::ADDI:
            if (insn.ra == 0)
                emit("ctx->r[%u] = 0x%X;", insn.rd, (uint32_t)(int32_t)insn.simm);
            else
                emit("ctx->r[%u] = (int32_t)ctx->r[%u] + %d;", insn.rd, insn.ra, insn.simm);
            break;

        case PPCInsnType::ADDIS:
            if (insn.ra == 0)
                emit("ctx->r[%u] = 0x%X;", insn.rd, (uint32_t)((int32_t)insn.simm << 16));
            else
                emit("ctx->r[%u] = (int32_t)ctx->r[%u] + 0x%X;", insn.rd, insn.ra, (uint32_t)((int32_t)insn.simm << 16));
            break;

        case PPCInsnType::ADD:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u];", insn.rd, insn.ra, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::ADDC:
            emit("{ uint64_t t = (uint64_t)ctx->r[%u] + (uint64_t)ctx->r[%u]; ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rb, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::ADDE:
            emit("{ uint64_t t = (uint64_t)ctx->r[%u] + (uint64_t)ctx->r[%u] + ctx->xer_ca(); ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rb, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::ADDZE:
            emit("{ uint64_t t = (uint64_t)ctx->r[%u] + ctx->xer_ca(); ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::ADDME:
            emit("{ uint64_t t = (uint64_t)ctx->r[%u] + (uint64_t)0xFFFFFFFF + ctx->xer_ca(); ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::ADDIC:
            emit("{ uint64_t t = (uint64_t)ctx->r[%u] + (uint64_t)(uint32_t)%d; ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.simm, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::SUBF:
            emit("ctx->r[%u] = ctx->r[%u] - ctx->r[%u];", insn.rd, insn.rb, insn.ra);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::SUBFC:
            emit("{ uint64_t t = (uint64_t)ctx->r[%u] - (uint64_t)ctx->r[%u]; ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(!(t >> 32)); }",
                 insn.rb, insn.ra, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::SUBFE:
            emit("{ uint64_t t = (uint64_t)~ctx->r[%u] + (uint64_t)ctx->r[%u] + ctx->xer_ca(); ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rb, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::SUBFIC:
            emit("{ uint64_t t = (uint64_t)(uint32_t)%d - (uint64_t)ctx->r[%u]; ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(!(t >> 32)); }",
                 insn.simm, insn.ra, insn.rd);
            break;

        case PPCInsnType::SUBFZE:
            emit("{ uint64_t t = (uint64_t)~ctx->r[%u] + ctx->xer_ca(); ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::SUBFME:
            emit("{ uint64_t t = (uint64_t)~ctx->r[%u] + (uint64_t)0xFFFFFFFF + ctx->xer_ca(); ctx->r[%u] = (uint32_t)t; ctx->set_xer_ca(t >> 32); }",
                 insn.ra, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::NEG:
            emit("ctx->r[%u] = -ctx->r[%u];", insn.rd, insn.ra);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::MULLI:
            emit("ctx->r[%u] = (int32_t)ctx->r[%u] * %d;", insn.rd, insn.ra, insn.simm);
            break;

        case PPCInsnType::MULLW:
            emit("ctx->r[%u] = (int32_t)ctx->r[%u] * (int32_t)ctx->r[%u];", insn.rd, insn.ra, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::MULHW:
            emit("ctx->r[%u] = (uint32_t)((int64_t)(int32_t)ctx->r[%u] * (int64_t)(int32_t)ctx->r[%u] >> 32);",
                 insn.rd, insn.ra, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::MULHWU:
            emit("ctx->r[%u] = (uint32_t)((uint64_t)ctx->r[%u] * (uint64_t)ctx->r[%u] >> 32);",
                 insn.rd, insn.ra, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::DIVW:
            // x86 traps on division by zero and INT_MIN / -1; PowerPC leaves the result undefined.
            // Gekko returns -1 for a negative dividend and 0 otherwise in those cases.
            emit("{ int32_t a = (int32_t)ctx->r[%u]; int32_t b = (int32_t)ctx->r[%u]; "
                 "ctx->r[%u] = (b == 0 || (a == INT32_MIN && b == -1)) ? (a < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(a / b); }",
                 insn.ra, insn.rb, insn.rd);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        case PPCInsnType::DIVWU:
            emit("ctx->r[%u] = ctx->r[%u] == 0 ? 0u : ctx->r[%u] / ctx->r[%u];", insn.rd, insn.rb, insn.ra, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.rd);
            break;

        // ==== Integer Compare ====
        case PPCInsnType::CMPI:
            emit("{ int32_t a = (int32_t)ctx->r[%u]; int32_t b = %d; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : 2; if (ctx->xer_so()) c |= 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.simm, insn.crfd);
            break;

        case PPCInsnType::CMP:
            emit("{ int32_t a = (int32_t)ctx->r[%u]; int32_t b = (int32_t)ctx->r[%u]; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : 2; if (ctx->xer_so()) c |= 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.rb, insn.crfd);
            break;

        case PPCInsnType::CMPLI:
            emit("{ uint32_t a = ctx->r[%u]; uint32_t b = %u; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : 2; if (ctx->xer_so()) c |= 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.uimm, insn.crfd);
            break;

        case PPCInsnType::CMPL:
            emit("{ uint32_t a = ctx->r[%u]; uint32_t b = ctx->r[%u]; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : 2; if (ctx->xer_so()) c |= 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.rb, insn.crfd);
            break;

        // ==== Integer Logical ====
        case PPCInsnType::ORI:
            emit("ctx->r[%u] = ctx->r[%u] | 0x%X;", insn.ra, insn.rs, insn.uimm);
            break;

        case PPCInsnType::ORIS:
            emit("ctx->r[%u] = ctx->r[%u] | 0x%X;", insn.ra, insn.rs, insn.uimm << 16);
            break;

        case PPCInsnType::ANDI:
            emit("ctx->r[%u] = ctx->r[%u] & 0x%X;", insn.ra, insn.rs, insn.uimm);
            emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::ANDIS:
            emit("ctx->r[%u] = ctx->r[%u] & 0x%X;", insn.ra, insn.rs, insn.uimm << 16);
            emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::XORI:
            emit("ctx->r[%u] = ctx->r[%u] ^ 0x%X;", insn.ra, insn.rs, insn.uimm);
            break;

        case PPCInsnType::XORIS:
            emit("ctx->r[%u] = ctx->r[%u] ^ 0x%X;", insn.ra, insn.rs, insn.uimm << 16);
            break;

        case PPCInsnType::AND:
            emit("ctx->r[%u] = ctx->r[%u] & ctx->r[%u];", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::OR:
            emit("ctx->r[%u] = ctx->r[%u] | ctx->r[%u];", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::XOR:
            emit("ctx->r[%u] = ctx->r[%u] ^ ctx->r[%u];", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::NOR:
            emit("ctx->r[%u] = ~(ctx->r[%u] | ctx->r[%u]);", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::NAND:
            emit("ctx->r[%u] = ~(ctx->r[%u] & ctx->r[%u]);", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::ANDC:
            emit("ctx->r[%u] = ctx->r[%u] & ~ctx->r[%u];", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::ORC:
            emit("ctx->r[%u] = ctx->r[%u] | ~ctx->r[%u];", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::EQV:
            emit("ctx->r[%u] = ~(ctx->r[%u] ^ ctx->r[%u]);", insn.ra, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::EXTSB:
            emit("ctx->r[%u] = (uint32_t)(int32_t)(int8_t)ctx->r[%u];", insn.ra, insn.rs);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::EXTSH:
            emit("ctx->r[%u] = (uint32_t)(int32_t)(int16_t)ctx->r[%u];", insn.ra, insn.rs);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::CNTLZW:
            emit("ctx->r[%u] = PPC_CNTLZW(ctx->r[%u]);", insn.ra, insn.rs);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        // ==== Shift/Rotate ====
        case PPCInsnType::SLW:
            emit("ctx->r[%u] = (ctx->r[%u] & 0x20) ? 0 : (ctx->r[%u] << (ctx->r[%u] & 0x1F));",
                 insn.ra, insn.rb, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::SRW:
            emit("ctx->r[%u] = (ctx->r[%u] & 0x20) ? 0 : (ctx->r[%u] >> (ctx->r[%u] & 0x1F));",
                 insn.ra, insn.rb, insn.rs, insn.rb);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::SRAW:
            emit("{ int32_t s = (int32_t)ctx->r[%u]; uint32_t n = ctx->r[%u] & 0x3F; if (n > 31) { ctx->r[%u] = s >> 31; ctx->set_xer_ca(s < 0); } else { ctx->r[%u] = (uint32_t)(s >> n); ctx->set_xer_ca(s < 0 && (s & ((1 << n) - 1))); } }",
                 insn.rs, insn.rb, insn.ra, insn.ra);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::SRAWI:
            emit("{ int32_t s = (int32_t)ctx->r[%u]; ctx->r[%u] = (uint32_t)(s >> %u); ctx->set_xer_ca(s < 0 && (s & ((1 << %u) - 1))); }",
                 insn.rs, insn.ra, insn.sh, insn.sh);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;

        case PPCInsnType::RLWINM: {
            // Rotate left word immediate then AND with mask
            uint32_t mask = 0;
            for (uint32_t i = insn.mb; i != ((insn.me + 1) & 31) || mask == 0; i = (i + 1) & 31) {
                mask |= (1u << (31 - i));
                if (i == insn.me) break;
            }
            if (insn.sh == 0)
                emit("ctx->r[%u] = ctx->r[%u] & 0x%08X;", insn.ra, insn.rs, mask);
            else
                emit("ctx->r[%u] = PPC_ROTL32(ctx->r[%u], %u) & 0x%08X;", insn.ra, insn.rs, insn.sh, mask);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;
        }

        case PPCInsnType::RLWIMI: {
            uint32_t mask = 0;
            for (uint32_t i = insn.mb; i != ((insn.me + 1) & 31) || mask == 0; i = (i + 1) & 31) {
                mask |= (1u << (31 - i));
                if (i == insn.me) break;
            }
            emit("ctx->r[%u] = (PPC_ROTL32(ctx->r[%u], %u) & 0x%08X) | (ctx->r[%u] & 0x%08X);",
                 insn.ra, insn.rs, insn.sh, mask, insn.ra, ~mask);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;
        }

        case PPCInsnType::RLWNM: {
            uint32_t mask = 0;
            for (uint32_t i = insn.mb; i != ((insn.me + 1) & 31) || mask == 0; i = (i + 1) & 31) {
                mask |= (1u << (31 - i));
                if (i == insn.me) break;
            }
            emit("ctx->r[%u] = PPC_ROTL32(ctx->r[%u], ctx->r[%u] & 0x1F) & 0x%08X;",
                 insn.ra, insn.rs, insn.rb, mask);
            if (insn.rc) emit("ctx->update_cr0((int32_t)ctx->r[%u]);", insn.ra);
            break;
        }

        // ==== Load Integer ====
        case PPCInsnType::LBZ:
            emit("ctx->r[%u] = MEM_READ8(%s + %d);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm);
            break;
        case PPCInsnType::LHZ:
            emit("ctx->r[%u] = MEM_READ16(%s + %d);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm);
            break;
        case PPCInsnType::LHA:
            emit("ctx->r[%u] = (uint32_t)(int32_t)(int16_t)MEM_READ16(%s + %d);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm);
            break;
        case PPCInsnType::LWZ:
            emit("ctx->r[%u] = MEM_READ32(%s + %d);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm);
            break;

        // Load with update
        case PPCInsnType::LWZU:
            emit("ctx->r[%u] += %d; ctx->r[%u] = MEM_READ32(ctx->r[%u]);",
                 insn.ra, insn.simm, insn.rd, insn.ra);
            break;
        case PPCInsnType::LBZU:
            emit("ctx->r[%u] += %d; ctx->r[%u] = MEM_READ8(ctx->r[%u]);",
                 insn.ra, insn.simm, insn.rd, insn.ra);
            break;
        case PPCInsnType::LHZU:
            emit("ctx->r[%u] += %d; ctx->r[%u] = MEM_READ16(ctx->r[%u]);",
                 insn.ra, insn.simm, insn.rd, insn.ra);
            break;
        case PPCInsnType::LHAU:
            emit("ctx->r[%u] += %d; ctx->r[%u] = (uint32_t)(int32_t)(int16_t)MEM_READ16(ctx->r[%u]);",
                 insn.ra, insn.simm, insn.rd, insn.ra);
            break;

        // Load indexed
        case PPCInsnType::LBZX:
            emit("ctx->r[%u] = MEM_READ8(ctx->r[%u] + ctx->r[%u]);", insn.rd, insn.ra ? insn.ra : 0, insn.rb);
            break;
        case PPCInsnType::LHZX:
            emit("ctx->r[%u] = MEM_READ16(ctx->r[%u] + ctx->r[%u]);", insn.rd, insn.ra ? insn.ra : 0, insn.rb);
            break;
        case PPCInsnType::LWZX:
            emit("ctx->r[%u] = MEM_READ32(ctx->r[%u] + ctx->r[%u]);", insn.rd, insn.ra ? insn.ra : 0, insn.rb);
            break;
        case PPCInsnType::LHAX:
            emit("ctx->r[%u] = (uint32_t)(int32_t)(int16_t)MEM_READ16(%s + ctx->r[%u]);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb);
            break;

        // Load with update (indexed)
        case PPCInsnType::LBZUX:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u]; ctx->r[%u] = MEM_READ8(ctx->r[%u]);",
                 insn.ra, insn.ra, insn.rb, insn.rd, insn.ra);
            break;
        case PPCInsnType::LHZUX:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u]; ctx->r[%u] = MEM_READ16(ctx->r[%u]);",
                 insn.ra, insn.ra, insn.rb, insn.rd, insn.ra);
            break;
        case PPCInsnType::LWZUX:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u]; ctx->r[%u] = MEM_READ32(ctx->r[%u]);",
                 insn.ra, insn.ra, insn.rb, insn.rd, insn.ra);
            break;
        case PPCInsnType::LHAUX:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u]; ctx->r[%u] = (uint32_t)(int32_t)(int16_t)MEM_READ16(ctx->r[%u]);",
                 insn.ra, insn.ra, insn.rb, insn.rd, insn.ra);
            break;

        // Load multiple words
        case PPCInsnType::LMW:
            emit("{ uint32_t ea = %s + %d; for (int i = %u; i < 32; i++) { ctx->r[i] = MEM_READ32(ea); ea += 4; } }",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rd);
            break;

        // Load byte-reversed
        case PPCInsnType::LHBRX:
            emit("{ uint16_t v = MEM_READ16(%s + ctx->r[%u]); ctx->r[%u] = (v >> 8) | (v << 8); }",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rd);
            break;
        case PPCInsnType::LWBRX:
            emit("{ uint32_t v = MEM_READ32(%s + ctx->r[%u]); ctx->r[%u] = (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rd);
            break;

        // Load word and reserve
        case PPCInsnType::LWARX:
            emit("ctx->r[%u] = MEM_READ32(%s + ctx->r[%u]); // lwarx (reservation ignored)",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb);
            break;

        // ==== Store Integer ====
        case PPCInsnType::STB:
            emit("MEM_WRITE8(%s + %d, (uint8_t)ctx->r[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rs);
            break;
        case PPCInsnType::STH:
            emit("MEM_WRITE16(%s + %d, (uint16_t)ctx->r[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rs);
            break;
        case PPCInsnType::STW:
            emit("MEM_WRITE32(%s + %d, ctx->r[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rs);
            break;

        // Store with update (used heavily for stack frames: stwu r1, -X(r1))
        // PPC: EA = rA + d; MEM[EA] = rS; rA = EA (store BEFORE update)
        case PPCInsnType::STWU:
            emit("{ uint32_t ea = ctx->r[%u] + %d; MEM_WRITE32(ea, ctx->r[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.simm, insn.rs, insn.ra);
            break;
        case PPCInsnType::STBU:
            emit("{ uint32_t ea = ctx->r[%u] + %d; MEM_WRITE8(ea, (uint8_t)ctx->r[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.simm, insn.rs, insn.ra);
            break;
        case PPCInsnType::STHU:
            emit("{ uint32_t ea = ctx->r[%u] + %d; MEM_WRITE16(ea, (uint16_t)ctx->r[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.simm, insn.rs, insn.ra);
            break;

        // Store indexed
        case PPCInsnType::STBX:
            emit("MEM_WRITE8(%s + ctx->r[%u], (uint8_t)ctx->r[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rs);
            break;
        case PPCInsnType::STHX:
            emit("MEM_WRITE16(%s + ctx->r[%u], (uint16_t)ctx->r[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rs);
            break;
        case PPCInsnType::STWX:
            emit("MEM_WRITE32(%s + ctx->r[%u], ctx->r[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rs);
            break;

        // Store with update (indexed) -- EA = rA + rB; MEM[EA] = rS; rA = EA
        case PPCInsnType::STBUX:
            emit("{ uint32_t ea = ctx->r[%u] + ctx->r[%u]; MEM_WRITE8(ea, (uint8_t)ctx->r[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.rb, insn.rs, insn.ra);
            break;
        case PPCInsnType::STHUX:
            emit("{ uint32_t ea = ctx->r[%u] + ctx->r[%u]; MEM_WRITE16(ea, (uint16_t)ctx->r[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.rb, insn.rs, insn.ra);
            break;
        case PPCInsnType::STWUX:
            emit("{ uint32_t ea = ctx->r[%u] + ctx->r[%u]; MEM_WRITE32(ea, ctx->r[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.rb, insn.rs, insn.ra);
            break;

        // Store byte-reversed
        case PPCInsnType::STHBRX:
            emit("{ uint16_t v = (uint16_t)ctx->r[%u]; MEM_WRITE16(%s + ctx->r[%u], (v >> 8) | (v << 8)); }",
                 insn.rs, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb);
            break;
        case PPCInsnType::STWBRX:
            emit("{ uint32_t v = ctx->r[%u]; MEM_WRITE32(%s + ctx->r[%u], (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24)); }",
                 insn.rs, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb);
            break;

        // Store multiple
        case PPCInsnType::STMW:
            emit("{ uint32_t ea = %s + %d; for (int i = %u; i < 32; i++) { MEM_WRITE32(ea, ctx->r[i]); ea += 4; } }",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rs);
            break;

        // Store word conditional
        case PPCInsnType::STWCX:
            emit("MEM_WRITE32(%s + ctx->r[%u], ctx->r[%u]); ctx->set_cr_field(0, 0x2 | (ctx->xer_so() ? 1 : 0)); // stwcx. (always succeeds)",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rs);
            break;

        // ==== Load/Store Float ====
        // A single-precision load fills ps0 and ps1 with the value; ps0 is f[]. A double load leaves ps1 alone.
        case PPCInsnType::LFS:
            emit("{ float v = MEM_READF32(%s + %d); ctx->f[%u] = (double)v; ctx->ps1[%u] = v; }",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rd, insn.rd);
            break;

        case PPCInsnType::LFD:
            emit("ctx->f[%u] = MEM_READF64(%s + %d);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm);
            break;

        case PPCInsnType::STFS:
            emit("MEM_WRITEF32(%s + %d, (float)ctx->f[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rs);
            break;

        case PPCInsnType::STFD:
            emit("MEM_WRITEF64(%s + %d, ctx->f[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.simm, insn.rs);
            break;

        // Float load/store with update
        case PPCInsnType::LFSU:
            emit("ctx->r[%u] += %d; { float v = MEM_READF32(ctx->r[%u]); ctx->f[%u] = (double)v; ctx->ps1[%u] = v; }",
                 insn.ra, insn.simm, insn.ra, insn.rd, insn.rd);
            break;
        case PPCInsnType::LFDU:
            emit("ctx->r[%u] += %d; ctx->f[%u] = MEM_READF64(ctx->r[%u]);",
                 insn.ra, insn.simm, insn.rd, insn.ra);
            break;
        case PPCInsnType::STFSU:
            emit("{ uint32_t ea = ctx->r[%u] + %d; MEM_WRITEF32(ea, (float)ctx->f[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.simm, insn.rs, insn.ra);
            break;
        case PPCInsnType::STFDU:
            emit("{ uint32_t ea = ctx->r[%u] + %d; MEM_WRITEF64(ea, ctx->f[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.simm, insn.rs, insn.ra);
            break;

        // Float load/store indexed
        case PPCInsnType::LFSX:
            emit("{ float v = MEM_READF32(%s + ctx->r[%u]); ctx->f[%u] = (double)v; ctx->ps1[%u] = v; }",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rd, insn.rd);
            break;
        case PPCInsnType::LFDX:
            emit("ctx->f[%u] = MEM_READF64(%s + ctx->r[%u]);",
                 insn.rd, insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb);
            break;
        case PPCInsnType::STFSX:
            emit("MEM_WRITEF32(%s + ctx->r[%u], (float)ctx->f[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rs);
            break;
        case PPCInsnType::STFDX:
            emit("MEM_WRITEF64(%s + ctx->r[%u], ctx->f[%u]);",
                 insn.ra ? (std::string("ctx->r[") + std::to_string(insn.ra) + "]").c_str() : "0", insn.rb, insn.rs);
            break;

        // Float indexed with update
        case PPCInsnType::LFSUX:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u]; { float v = MEM_READF32(ctx->r[%u]); ctx->f[%u] = (double)v; ctx->ps1[%u] = v; }",
                 insn.ra, insn.ra, insn.rb, insn.ra, insn.rd, insn.rd);
            break;
        case PPCInsnType::LFDUX:
            emit("ctx->r[%u] = ctx->r[%u] + ctx->r[%u]; ctx->f[%u] = MEM_READF64(ctx->r[%u]);",
                 insn.ra, insn.ra, insn.rb, insn.rd, insn.ra);
            break;
        case PPCInsnType::STFSUX:
            emit("{ uint32_t ea = ctx->r[%u] + ctx->r[%u]; MEM_WRITEF32(ea, (float)ctx->f[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.rb, insn.rs, insn.ra);
            break;
        case PPCInsnType::STFDUX:
            emit("{ uint32_t ea = ctx->r[%u] + ctx->r[%u]; MEM_WRITEF64(ea, ctx->f[%u]); ctx->r[%u] = ea; }",
                 insn.ra, insn.rb, insn.rs, insn.ra);
            break;

        // ==== Floating Point Arithmetic ====
        // Single-precision operations take the double-precision register values as they are and
        // round the result to single. Rounding the operands first would break the common
        // int-to-float trick (subtracting 2^52 from 2^52 + n).
        case PPCInsnType::FADDS:
            emit_fs(*this, insn.rd, "ctx->f[%u] + ctx->f[%u]", insn.ra, insn.rb);
            break;
        case PPCInsnType::FSUBS:
            emit_fs(*this, insn.rd, "ctx->f[%u] - ctx->f[%u]", insn.ra, insn.rb);
            break;
        case PPCInsnType::FMULS:
            emit_fs(*this, insn.rd, "(float)ctx->f[%u] * (float)ctx->f[%u]", insn.ra, insn.rc_reg);
            break;
        case PPCInsnType::FDIVS:
            emit_fs(*this, insn.rd, "(float)ctx->f[%u] / (float)ctx->f[%u]", insn.ra, insn.rb);
            break;
        case PPCInsnType::FADD:
            emit("ctx->f[%u] = ctx->f[%u] + ctx->f[%u];", insn.rd, insn.ra, insn.rb);
            break;
        case PPCInsnType::FSUB:
            emit("ctx->f[%u] = ctx->f[%u] - ctx->f[%u];", insn.rd, insn.ra, insn.rb);
            break;
        case PPCInsnType::FMUL:
            emit("ctx->f[%u] = ctx->f[%u] * ctx->f[%u];", insn.rd, insn.ra, insn.rc_reg);
            break;
        case PPCInsnType::FDIV:
            emit("ctx->f[%u] = ctx->f[%u] / ctx->f[%u];", insn.rd, insn.ra, insn.rb);
            break;

        case PPCInsnType::FMADDS:
            emit_fs(*this, insn.rd, "ctx->f[%u] * ctx->f[%u] + ctx->f[%u]", insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FMSUBS:
            emit_fs(*this, insn.rd, "ctx->f[%u] * ctx->f[%u] - ctx->f[%u]", insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FNMSUBS:
            emit_fs(*this, insn.rd, "-(ctx->f[%u] * ctx->f[%u] - ctx->f[%u])", insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FNMADDS:
            emit_fs(*this, insn.rd, "-(ctx->f[%u] * ctx->f[%u] + ctx->f[%u])", insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FMADD:
            emit("ctx->f[%u] = ctx->f[%u] * ctx->f[%u] + ctx->f[%u];", insn.rd, insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FMSUB:
            emit("ctx->f[%u] = ctx->f[%u] * ctx->f[%u] - ctx->f[%u];", insn.rd, insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FNMADD:
            emit("ctx->f[%u] = -(ctx->f[%u] * ctx->f[%u] + ctx->f[%u]);", insn.rd, insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FNMSUB:
            emit("ctx->f[%u] = -(ctx->f[%u] * ctx->f[%u] - ctx->f[%u]);", insn.rd, insn.ra, insn.rc_reg, insn.rb);
            break;

        case PPCInsnType::FRES:
            emit_fs(*this, insn.rd, "1.0 / ctx->f[%u]", insn.rb);
            break;
        case PPCInsnType::FRSQRTE:
            emit("ctx->f[%u] = 1.0 / sqrt(ctx->f[%u]);", insn.rd, insn.rb);
            break;
        case PPCInsnType::FSEL:
            emit("ctx->f[%u] = (ctx->f[%u] >= 0.0) ? ctx->f[%u] : ctx->f[%u];",
                 insn.rd, insn.ra, insn.rc_reg, insn.rb);
            break;
        case PPCInsnType::FMR:
            emit("ctx->f[%u] = ctx->f[%u];", insn.rd, insn.rb);
            break;
        case PPCInsnType::FNEG:
            emit("ctx->f[%u] = -ctx->f[%u];", insn.rd, insn.rb);
            break;
        case PPCInsnType::FABS:
            emit("ctx->f[%u] = fabs(ctx->f[%u]);", insn.rd, insn.rb);
            break;
        case PPCInsnType::FNABS:
            emit("ctx->f[%u] = -fabs(ctx->f[%u]);", insn.rd, insn.rb);
            break;
        case PPCInsnType::FRSP:
            emit_fs(*this, insn.rd, "ctx->f[%u]", insn.rb);
            break;
        // The integer goes to the low word; conversion saturates (NaN gives 0x80000000).
        case PPCInsnType::FCTIWZ:
            emit("{ uint64_t tmp = ppc_fctiwz(ctx->f[%u]); std::memcpy(&ctx->f[%u], &tmp, 8); }", insn.rb, insn.rd);
            break;
        case PPCInsnType::FCTIW:
            emit("{ uint64_t tmp = ppc_fctiw(ctx->f[%u]); std::memcpy(&ctx->f[%u], &tmp, 8); }", insn.rb, insn.rd);
            break;
        case PPCInsnType::MFFS:
            emit("{ uint64_t v = (uint64_t)ctx->fpscr; std::memcpy(&ctx->f[%u], &v, 8); }", insn.rd);
            break;
        case PPCInsnType::MTFSF:
            emit("{ uint64_t v; std::memcpy(&v, &ctx->f[%u], 8); ctx->fpscr = (uint32_t)v; }", insn.rb);
            break;

        // ==== Float Compare ====
        case PPCInsnType::FCMPU:
        case PPCInsnType::FCMPO:
            emit("{ double a = ctx->f[%u]; double b = ctx->f[%u]; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.rb, insn.crfd);
            break;

        // ==== Paired Singles (the GameCube special sauce) ====
        case PPCInsnType::PS_ADD:
            emit_ps(*this, "ab", insn, "a0 + b0", "a1 + b1");
            break;
        case PPCInsnType::PS_SUB:
            emit_ps(*this, "ab", insn, "a0 - b0", "a1 - b1");
            break;
        case PPCInsnType::PS_MUL:
            emit_ps(*this, "ac", insn, "a0 * c0", "a1 * c1");
            break;
        case PPCInsnType::PS_DIV:
            emit_ps(*this, "ab", insn, "a0 / b0", "a1 / b1");
            break;
        // The fused forms compute the product exactly (24 x 24 bits fit a double) and round once.
        case PPCInsnType::PS_MADD:
            emit_ps(*this, "abc", insn, "(float)((double)a0 * c0 + b0)", "(float)((double)a1 * c1 + b1)");
            break;
        case PPCInsnType::PS_MSUB:
            emit_ps(*this, "abc", insn, "(float)((double)a0 * c0 - b0)", "(float)((double)a1 * c1 - b1)");
            break;
        case PPCInsnType::PS_NMADD:
            emit_ps(*this, "abc", insn, "-(float)((double)a0 * c0 + b0)", "-(float)((double)a1 * c1 + b1)");
            break;
        case PPCInsnType::PS_NMSUB:
            emit_ps(*this, "abc", insn, "-(float)((double)a0 * c0 - b0)", "-(float)((double)a1 * c1 - b1)");
            break;
        case PPCInsnType::PS_MULS0:
            emit_ps(*this, "ac", insn, "a0 * c0", "a1 * c0");
            break;
        case PPCInsnType::PS_MULS1:
            emit_ps(*this, "ac", insn, "a0 * c1", "a1 * c1");
            break;
        case PPCInsnType::PS_MADDS0:
            emit_ps(*this, "abc", insn, "(float)((double)a0 * c0 + b0)", "(float)((double)a1 * c0 + b1)");
            break;
        case PPCInsnType::PS_MADDS1:
            emit_ps(*this, "abc", insn, "(float)((double)a0 * c1 + b0)", "(float)((double)a1 * c1 + b1)");
            break;
        case PPCInsnType::PS_SUM0:
            emit_ps(*this, "abc", insn, "a0 + b1", "c1");
            break;
        case PPCInsnType::PS_SUM1:
            emit_ps(*this, "abc", insn, "c0", "a0 + b1");
            break;
        case PPCInsnType::PS_MERGE00:
            emit_ps(*this, "ab", insn, "a0", "b0");
            break;
        case PPCInsnType::PS_MERGE01:
            emit_ps(*this, "ab", insn, "a0", "b1");
            break;
        case PPCInsnType::PS_MERGE10:
            emit_ps(*this, "ab", insn, "a1", "b0");
            break;
        case PPCInsnType::PS_MERGE11:
            emit_ps(*this, "ab", insn, "a1", "b1");
            break;
        case PPCInsnType::PS_MR:
            emit_ps(*this, "b", insn, "b0", "b1");
            break;
        case PPCInsnType::PS_NEG:
            emit_ps(*this, "b", insn, "-b0", "-b1");
            break;
        case PPCInsnType::PS_ABS:
            emit_ps(*this, "b", insn, "fabsf(b0)", "fabsf(b1)");
            break;
        case PPCInsnType::PS_NABS:
            emit_ps(*this, "b", insn, "-fabsf(b0)", "-fabsf(b1)");
            break;
        case PPCInsnType::PS_RES:
            emit_ps(*this, "b", insn, "1.0f / b0", "1.0f / b1");
            break;
        case PPCInsnType::PS_RSQRTE:
            emit_ps(*this, "b", insn, "1.0f / sqrtf(b0)", "1.0f / sqrtf(b1)");
            break;
        case PPCInsnType::PS_SEL:
            emit_ps(*this, "abc", insn, "a0 >= 0.0f ? c0 : b0", "a1 >= 0.0f ? c1 : b1");
            break;

        // Paired singles compare
        case PPCInsnType::PS_CMPU0:
        case PPCInsnType::PS_CMPO0:
            emit("{ float a = (float)ctx->f[%u]; float b = (float)ctx->f[%u]; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.rb, insn.crfd);
            break;
        case PPCInsnType::PS_CMPU1:
        case PPCInsnType::PS_CMPO1:
            emit("{ float a = ctx->ps1[%u]; float b = ctx->ps1[%u]; uint32_t c = (a < b) ? 8 : (a > b) ? 4 : (a == b) ? 2 : 1; ctx->set_cr_field(%u, c); }",
                 insn.ra, insn.rb, insn.crfd);
            break;

        // ==== Paired Singles Quantized Load/Store ====
        // For GQR=0 (most common), quantization type is float and scale is 1.
        // We emit a runtime helper call that reads the GQR register.
        case PPCInsnType::PSQ_L:
        case PPCInsnType::PSQ_LU:
        case PPCInsnType::PSQ_LX:
        case PPCInsnType::PSQ_LUX:
        case PPCInsnType::PSQ_ST:
        case PPCInsnType::PSQ_STU:
        case PPCInsnType::PSQ_STX:
        case PPCInsnType::PSQ_STUX: {
            const bool indexed = insn.type == PPCInsnType::PSQ_LX || insn.type == PPCInsnType::PSQ_LUX ||
                                 insn.type == PPCInsnType::PSQ_STX || insn.type == PPCInsnType::PSQ_STUX;
            const bool update = insn.type == PPCInsnType::PSQ_LU || insn.type == PPCInsnType::PSQ_LUX ||
                                insn.type == PPCInsnType::PSQ_STU || insn.type == PPCInsnType::PSQ_STUX;
            const bool store = insn.type == PPCInsnType::PSQ_ST || insn.type == PPCInsnType::PSQ_STU ||
                               insn.type == PPCInsnType::PSQ_STX || insn.type == PPCInsnType::PSQ_STUX;
            // Effective address: (rA|0) + d, or (rA|0) + rB; the update forms use rA itself (rA != 0).
            char ea[96];
            const std::string base = insn.ra ? "ctx->r[" + std::to_string(insn.ra) + "]" : "0";
            if (indexed) std::snprintf(ea, sizeof ea, "%s + ctx->r[%u]", base.c_str(), insn.rb);
            else std::snprintf(ea, sizeof ea, "%s + %d", base.c_str(), insn.simm);
            const uint32_t reg = store ? insn.rs : insn.rd;
            // A quantized access moves ps0 and ps1 (W = 0) or only ps0 (W = 1; a load then sets ps1 to 1.0).
            // ps0 is the register's double, ps1 its second slot.
            emit("{ uint32_t ea = %s;", ea);
            if (update) emit("  ctx->r[%u] = ea;", insn.ra);
            if (store) {
                if (insn.psq_w) emit("  PSQ_STORE_ONE((float)ctx->f[%u], ea, ctx->gqr[%u]);", reg, insn.psq_i);
                else emit("  PSQ_STORE_PAIR((float)ctx->f[%u], ctx->ps1[%u], ea, ctx->gqr[%u]);", reg, reg, insn.psq_i);
            } else if (insn.psq_w) {
                emit("  float v0 = PSQ_LOAD_ONE(ea, ctx->gqr[%u]); ctx->f[%u] = (double)v0; ctx->ps1[%u] = 1.0f;",
                     insn.psq_i, reg, reg);
            } else {
                emit("  float v0, v1; PSQ_LOAD_PAIR(&v0, &v1, ea, ctx->gqr[%u]); ctx->f[%u] = (double)v0; ctx->ps1[%u] = v1;",
                     insn.psq_i, reg, reg);
            }
            emit("}");
            break;
        }

        // ==== Condition Register Operations ====
        case PPCInsnType::CRXOR:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, a ^ b); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CROR:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, a | b); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CRAND:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, a & b); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CRANDC:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, a & !b); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CREQV:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, !(a ^ b)); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CRNAND:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, !(a & b)); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CRNOR:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, !(a | b)); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::CRORC:
            emit("{ bool a = ctx->get_cr_bit(%u); bool b = ctx->get_cr_bit(%u); ctx->set_cr_bit(%u, a | !b); }",
                 PPC_CRBA(insn.raw), PPC_CRBB(insn.raw), PPC_CRBD(insn.raw));
            break;
        case PPCInsnType::MCRF:
            emit("ctx->set_cr_field(%u, ctx->get_cr_field(%u));", insn.crfd, insn.crfs);
            break;

        // ==== SPR Access ====
        case PPCInsnType::MFSPR:
            if (insn.spr == 8)       emit("ctx->r[%u] = ctx->lr;", insn.rd);
            else if (insn.spr == 9)  emit("ctx->r[%u] = ctx->ctr;", insn.rd);
            else if (insn.spr == 1)  emit("ctx->r[%u] = ctx->xer;", insn.rd);
            else if (insn.spr >= 912 && insn.spr <= 919)
                emit("ctx->r[%u] = ctx->gqr[%u];", insn.rd, insn.spr - 912);
            else if (insn.spr == 268) emit("ctx->r[%u] = PPC_MFTBL(); // mftb (TBL)", insn.rd);
            else if (insn.spr == 269) emit("ctx->r[%u] = PPC_MFTBU(); // mftbu (TBU)", insn.rd);
            else emit("ctx->r[%u] = 0; // mfspr %u (unhandled)", insn.rd, insn.spr);
            break;

        case PPCInsnType::MTSPR:
            // Note: for mtspr, the source register is in the rd field position (bits 21-25)
            if (insn.spr == 8)       emit("ctx->lr = ctx->r[%u];", insn.rd);
            else if (insn.spr == 9)  emit("ctx->ctr = ctx->r[%u];", insn.rd);
            else if (insn.spr == 1)  emit("ctx->xer = ctx->r[%u];", insn.rd);
            else if (insn.spr >= 912 && insn.spr <= 919)
                emit("ctx->gqr[%u] = ctx->r[%u];", insn.spr - 912, insn.rd);
            else emit("// mtspr %u, r%u (unhandled)", insn.spr, insn.rd);
            break;

        case PPCInsnType::MFCR:
            emit("ctx->r[%u] = ctx->cr;", insn.rd);
            break;

        case PPCInsnType::MTCRF:
            emit("{ uint32_t mask = 0; uint32_t crm = (0x%08X >> 12) & 0xFF; for (int i = 0; i < 8; i++) if (crm & (1 << (7-i))) mask |= 0xF << (28 - i*4); ctx->cr = (ctx->cr & ~mask) | (ctx->r[%u] & mask); }",
                 insn.raw, insn.rs);
            break;

        // ==== Branch ====
        case PPCInsnType::B:
            if (insn.link) {
                bool known = func_map && func_map->find(insn.branch_target) != func_map->end();
                if (known) {
                    emit("ctx->lr = 0x%08Xu; %s(ctx, mem); // bl", insn.address + 4, name_for(insn.branch_target).c_str());
                } else {
                    // Target not in CFG (e.g., `bla 0x60` into a low/vector address).
                    // Route through the function table so it becomes a runtime
                    // unresolved-call rather than a link-time error.
                    emit("ctx->lr = 0x%08Xu; CALL_INDIRECT(0x%08Xu, ctx, mem); // bl (unresolved target)",
                         insn.address + 4, insn.branch_target);
                }
            } else {
                emit("goto label_%08X; // b", insn.branch_target);
            }
            break;

        case PPCInsnType::BC: {
            // Conditional branch: BO field encodes condition
            // bit 4 (0x10): 1=don't test CR  bit 3 (0x08): CR sense (1=true)
            // bit 2 (0x04): 1=don't decrement CTR  bit 1 (0x02): CTR sense (1=branch if CTR==0)
            uint32_t bo = insn.bo;
            uint32_t bi = insn.bi;
            bool skip_ctr = (bo & 0x04) != 0;
            bool skip_cr  = (bo & 0x10) != 0;

            if (skip_ctr && skip_cr) {
                // BO=20: unconditional (shouldn't normally appear as bc, but handle it)
                emit("goto label_%08X; // bc (always)", insn.branch_target);
            } else if (skip_ctr && !skip_cr) {
                // Pure CR test (most common: BO=4 bf, BO=12 bt)
                bool cr_sense = (bo & 0x08) != 0;
                if (cr_sense)
                    emit("if (ctx->get_cr_bit(%u)) goto label_%08X; // bt cr%u", bi, insn.branch_target, bi);
                else
                    emit("if (!ctx->get_cr_bit(%u)) goto label_%08X; // bf cr%u", bi, insn.branch_target, bi);
            } else if (!skip_ctr && skip_cr) {
                // CTR decrement only (BO=16 bdnz, BO=18 bdz)
                bool ctr_zero = (bo & 0x02) != 0;
                if (ctr_zero)
                    emit("if (--ctx->ctr == 0) goto label_%08X; // bdz", insn.branch_target);
                else
                    emit("if (--ctx->ctr != 0) goto label_%08X; // bdnz", insn.branch_target);
            } else {
                // Both CTR and CR test (rare)
                bool cr_sense = (bo & 0x08) != 0;
                bool ctr_zero = (bo & 0x02) != 0;
                emit("{ ctx->ctr--; if (ctx->ctr %s 0 && %sctx->get_cr_bit(%u)) goto label_%08X; } // bc %u, %u",
                     ctr_zero ? "==" : "!=", cr_sense ? "" : "!", bi, insn.branch_target, bo, bi);
            }
            break;
        }

        case PPCInsnType::BCLR: {
            if (insn.is_return()) {
                emit("return; // blr");
            } else if (insn.link) {
                emit("{ uint32_t target = ctx->lr; ctx->lr = 0x%08Xu; CALL_INDIRECT(target, ctx, mem); } // blrl", insn.address + 4);
            } else {
                // Conditional return via LR
                uint32_t bo = insn.bo;
                uint32_t bi = insn.bi;
                bool skip_ctr = (bo & 0x04) != 0;
                bool skip_cr  = (bo & 0x10) != 0;

                if (skip_ctr && !skip_cr) {
                    bool cr_sense = (bo & 0x08) != 0;
                    if (cr_sense)
                        emit("if (ctx->get_cr_bit(%u)) return; // bclr bt cr%u", bi, bi);
                    else
                        emit("if (!ctx->get_cr_bit(%u)) return; // bclr bf cr%u", bi, bi);
                } else if (!skip_ctr && skip_cr) {
                    bool ctr_zero = (bo & 0x02) != 0;
                    if (ctr_zero)
                        emit("if (--ctx->ctr == 0) return; // bdzlr");
                    else
                        emit("if (--ctx->ctr != 0) return; // bdnzlr");
                } else {
                    emit("return; // bclr (unconditional fallback)");
                }
            }
            break;
        }

        case PPCInsnType::BCCTR:
            if (insn.link) {
                emit("ctx->lr = 0x%08Xu; CALL_INDIRECT(ctx->ctr, ctx, mem); // bctrl", insn.address + 4);
            } else {
                // Switch table: use resolved jump table targets if available,
                // otherwise fall back to all block addresses in this function.
                // Jump table detection based on approach from ExpansionPak/GCRecompiler.
                const auto& targets = current_block->jump_table_targets.empty()
                    ? block_addrs : current_block->jump_table_targets;
                emit("// bctr -- switch table dispatch%s",
                     current_block->jump_table_targets.empty() ? " (unresolved)" : "");
                emit("switch (ctx->ctr) {");
                // A jump table routinely maps several indices onto one case
                // body, so the same target appears more than once; C needs
                // each case label exactly once.
                std::set<uint32_t> emitted;
                for (uint32_t addr : targets) {
                    if (!emitted.insert(addr).second) continue;
                    emit("    case 0x%08Xu: goto label_%08X;", addr, addr);
                }
                emit("    default: CALL_INDIRECT(ctx->ctr, ctx, mem); break; // fallback: tail call");
                emit("}");
            }
            break;

        // ==== System ====
        case PPCInsnType::SC:
            emit("// sc (system call - not expected in game code)");
            break;
        case PPCInsnType::RFI:
            emit("return; // rfi (return from interrupt)");
            break;
        case PPCInsnType::TWI:
        case PPCInsnType::TW:
            emit("// %s (trap - ignored)", insn.mnemonic.c_str());
            break;
        case PPCInsnType::MFMSR:
            emit("ctx->r[%u] = 0; // mfmsr (supervisor, no-op)", insn.rd);
            break;
        case PPCInsnType::MTMSR:
            emit("// mtmsr r%u (supervisor, no-op)", insn.rs);
            break;
        case PPCInsnType::DCBI:
        case PPCInsnType::DCBZ_L:
        case PPCInsnType::TLBIE:
        case PPCInsnType::TLBSYNC:
            emit("// %s (supervisor/cache, no-op)", insn.mnemonic.c_str());
            break;

        // ==== No-ops and cache ops (safe to ignore) ====
        case PPCInsnType::NOP:
        case PPCInsnType::SYNC:
        case PPCInsnType::ISYNC:
        case PPCInsnType::EIEIO:
        case PPCInsnType::DCBF:
        case PPCInsnType::DCBST:
        case PPCInsnType::DCBT:
        case PPCInsnType::DCBTST:
        case PPCInsnType::ICBI:
            emit("// %s (no-op on x86)", insn.mnemonic.c_str());
            break;

        case PPCInsnType::DCBZ:
            emit("std::memset((void*)((uintptr_t)mem->translate(ctx->r[%u] + ctx->r[%u]) & ~31), 0, 32); // dcbz",
                 insn.ra, insn.rb);
            break;

        default:
            emit("// UNIMPLEMENTED: %s (0x%08X)", insn.mnemonic.c_str(), insn.raw);
            break;
        }
    }

void emit_file_header(FILE* out, const char* project_name) {
    fprintf(out, "// =============================================================\n");
    fprintf(out, "// %s Static Recompilation - Auto-generated C code\n", project_name);
    fprintf(out, "// Source: GameCube DOL PowerPC 750CXe (Gekko)\n");
    fprintf(out, "// Target: x86-64 Windows (MSVC/Clang/GCC)\n");
    fprintf(out, "// DO NOT EDIT - Generated by gcrecomp\n");
    fprintf(out, "// =============================================================\n\n");
    fprintf(out, "#include \"recomp_common.h\"\n");
    fprintf(out, "#include \"recomp_funcs.h\"\n\n");
}

} // namespace gcrecomp
