#pragma once
// =============================================================================
// gcrecomp — PowerPC 750CXe (Gekko) Instruction Definitions
//
// The Gekko is a modified IBM PowerPC 750 running at 486 MHz.
// It includes the Paired Singles SIMD extension for fast 3D math.
// Used in the GameCube, Triforce arcade board, and Wii (Broadway).
//
// Reference: PowerPC Microprocessor Family Programming Environments Manual
//            IBM Gekko User's Manual (Paired Singles extension)
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace gcrecomp {

// ---- Instruction field extraction ------------------------------------------

inline uint32_t PPC_OP(uint32_t inst)    { return (inst >> 26) & 0x3F; }
inline uint32_t PPC_XO(uint32_t inst)    { return (inst >> 1) & 0x3FF; }
inline uint32_t PPC_RD(uint32_t inst)    { return (inst >> 21) & 0x1F; }
inline uint32_t PPC_RS(uint32_t inst)    { return (inst >> 21) & 0x1F; }
inline uint32_t PPC_RA(uint32_t inst)    { return (inst >> 16) & 0x1F; }
inline uint32_t PPC_RB(uint32_t inst)    { return (inst >> 11) & 0x1F; }
inline uint32_t PPC_RC(uint32_t inst)    { return (inst >> 6)  & 0x1F; }
inline int16_t  PPC_SIMM(uint32_t inst)  { return (int16_t)(inst & 0xFFFF); }
inline uint16_t PPC_UIMM(uint32_t inst)  { return inst & 0xFFFF; }
inline int32_t  PPC_LI(uint32_t inst)    { int32_t v = inst & 0x03FFFFFC; if (v & 0x02000000) v |= 0xFC000000; return v; }
inline int16_t  PPC_BD(uint32_t inst)    { int16_t v = inst & 0xFFFC; return v; }
inline uint32_t PPC_BO(uint32_t inst)    { return (inst >> 21) & 0x1F; }
inline uint32_t PPC_BI(uint32_t inst)    { return (inst >> 16) & 0x1F; }
inline bool     PPC_LK(uint32_t inst)    { return inst & 1; }
inline bool     PPC_AA(uint32_t inst)    { return (inst >> 1) & 1; }
inline bool     PPC_RC_FLAG(uint32_t inst) { return inst & 1; }
inline bool     PPC_OE(uint32_t inst)    { return (inst >> 10) & 1; }
inline uint32_t PPC_SPR(uint32_t inst)   { return ((inst >> 16) & 0x1F) | (((inst >> 11) & 0x1F) << 5); }
inline uint32_t PPC_FD(uint32_t inst)    { return (inst >> 21) & 0x1F; }
inline uint32_t PPC_FS(uint32_t inst)    { return (inst >> 21) & 0x1F; }
inline uint32_t PPC_FA(uint32_t inst)    { return (inst >> 16) & 0x1F; }
inline uint32_t PPC_FB(uint32_t inst)    { return (inst >> 11) & 0x1F; }
inline uint32_t PPC_FC(uint32_t inst)    { return (inst >> 6)  & 0x1F; }
inline uint32_t PPC_SH(uint32_t inst)    { return (inst >> 11) & 0x1F; }
inline uint32_t PPC_MB(uint32_t inst)    { return (inst >> 6)  & 0x1F; }
inline uint32_t PPC_ME(uint32_t inst)    { return (inst >> 1)  & 0x1F; }
inline uint32_t PPC_CRFD(uint32_t inst)  { return (inst >> 23) & 0x7; }
inline uint32_t PPC_CRFS(uint32_t inst)  { return (inst >> 18) & 0x7; }
inline uint32_t PPC_CRBD(uint32_t inst)  { return (inst >> 21) & 0x1F; }
inline uint32_t PPC_CRBA(uint32_t inst)  { return (inst >> 16) & 0x1F; }
inline uint32_t PPC_CRBB(uint32_t inst)  { return (inst >> 11) & 0x1F; }

// ---- Instruction type enumeration ------------------------------------------

enum class PPCInsnType {
    // Integer Arithmetic
    ADD, ADDI, ADDIS, ADDIC, ADDC, ADDE, ADDZE, ADDME,
    SUBF, SUBFIC, SUBFC, SUBFE, SUBFZE, SUBFME,
    MULLI, MULLW, MULHW, MULHWU,
    DIVW, DIVWU,
    NEG,

    // Integer Compare
    CMPI, CMP, CMPLI, CMPL,

    // Integer Logical
    ANDI, ANDIS, ORI, ORIS, XORI, XORIS,
    AND, OR, XOR, NAND, NOR, EQV, ANDC, ORC,
    EXTSB, EXTSH, CNTLZW,

    // Integer Shift/Rotate
    RLWINM, RLWIMI, RLWNM,
    SLW, SRW, SRAW, SRAWI,

    // Floating Point Arithmetic
    FADD, FADDS, FSUB, FSUBS, FMUL, FMULS, FDIV, FDIVS,
    FMADD, FMADDS, FMSUB, FMSUBS, FNMADD, FNMADDS, FNMSUB, FNMSUBS,
    FRES, FRSQRTE, FSEL,
    FABS, FNABS, FNEG, FMR, FRSP,
    FCTIW, FCTIWZ,

    // Paired Singles (Gekko extension)
    PS_ADD, PS_SUB, PS_MUL, PS_DIV,
    PS_MADD, PS_MSUB, PS_NMADD, PS_NMSUB,
    PS_SUM0, PS_SUM1, PS_MULS0, PS_MULS1,
    PS_MADDS0, PS_MADDS1,
    PS_MERGE00, PS_MERGE01, PS_MERGE10, PS_MERGE11,
    PS_MR, PS_NEG, PS_ABS, PS_NABS, PS_RES, PS_RSQRTE, PS_SEL,
    PSQ_L, PSQ_LU, PSQ_LX, PSQ_LUX,
    PSQ_ST, PSQ_STU, PSQ_STX, PSQ_STUX,
    PS_CMPU0, PS_CMPU1, PS_CMPO0, PS_CMPO1,

    // Float Compare
    FCMPU, FCMPO,

    // Load Integer
    LBZ, LBZU, LBZX, LBZUX,
    LHZ, LHZU, LHZX, LHZUX,
    LHA, LHAU, LHAX, LHAUX,
    LWZ, LWZU, LWZX, LWZUX,
    LHBRX, LWBRX,
    LMW, LSWI, LSWX, LWARX,

    // Store Integer
    STB, STBU, STBX, STBUX,
    STH, STHU, STHX, STHUX,
    STW, STWU, STWX, STWUX,
    STHBRX, STWBRX,
    STMW, STSWI, STSWX, STWCX,

    // Load/Store Float
    LFS, LFSU, LFSX, LFSUX,
    LFD, LFDU, LFDX, LFDUX,
    STFS, STFSU, STFSX, STFSUX,
    STFD, STFDU, STFDX, STFDUX,

    // Branch
    B, BC, BCLR, BCCTR,

    // Condition Register
    CRAND, CRANDC, CREQV, CRNAND, CRNOR, CROR, CRORC, CRXOR,
    MCRF, MCRFS, MCRXR, MTCRF, MFCR,

    // System
    MFSPR, MTSPR, MFMSR, MTMSR,
    SC, RFI, SYNC, ISYNC, EIEIO, ICBI, DCBI,
    DCBF, DCBST, DCBT, DCBTST, DCBZ, DCBZ_L,

    // TLB
    TLBIE, TLBSYNC,

    // Float status
    MFFS, MTFSF,

    // Misc
    NOP, TWI, TW,

    UNKNOWN,
};

// ---- Decoded instruction ---------------------------------------------------

struct PPCInsn {
    uint32_t    raw;
    uint32_t    address;
    PPCInsnType type;
    std::string mnemonic;

    // Decoded fields (populated based on type)
    uint32_t rd, rs, ra, rb, rc_reg;
    int32_t  simm;
    uint32_t uimm;
    uint32_t bo, bi;
    int32_t  branch_target;
    bool     link;
    bool     aa;
    bool     rc;
    bool     oe;
    uint32_t spr;
    uint32_t sh, mb, me;
    uint32_t crfd, crfs;
    uint32_t psq_w;     // PSQ W bit: 0=paired, 1=single
    uint32_t psq_i;     // PSQ GQR index (0-7)

    bool is_branch() const;
    bool is_call() const;
    bool is_return() const;
    bool is_conditional() const;
};

// Disassemble a single instruction
PPCInsn ppc_disasm(uint32_t raw, uint32_t address);

// Disassemble a range of instructions from memory
std::vector<PPCInsn> ppc_disasm_range(const uint8_t* data, uint32_t base_addr, uint32_t size);

} // namespace gcrecomp
