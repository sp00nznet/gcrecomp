// SPDX-License-Identifier: MIT
//
// ppc-diff runner: executes every case of the generated table (ppc_diff_gen) on the code the recompiler emits and
// compares registers, CR, LR, CTR and the memory window with the reference results from Unicorn / the Gekko model.
//
// usage: ppc_diff_run [--verbose] [--inputs] [--max-report N] [--only NAME[,NAME]] [--ignore NAME[,NAME]]
//   --verbose        print every mismatch, not only the first few per instruction
//   --inputs         with each mismatch, print the full input state (to reproduce it by hand)
//   --max-report N   mismatches printed per instruction name (default 3)
//   --only / --ignore  restrict / exclude instruction names (names are the generator's, e.g. rlwinm, psq_l)
// Exit status: 0 when every non-ignored case matches, 1 otherwise.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "gcrecomp/ppc_helpers.h"
#include "gcrecomp/runtime.h"

using gcrecomp::g_mem;
using gcrecomp::Memory;
using gcrecomp::PPCContext;

// The same bindings the recompiler writes to recomp_common.h.
#define MEM_READ8(addr)         g_mem.read8(static_cast<uint32_t>(addr))
#define MEM_READ16(addr)        g_mem.read16(static_cast<uint32_t>(addr))
#define MEM_READ32(addr)        g_mem.read32(static_cast<uint32_t>(addr))
#define MEM_READF32(addr)       g_mem.readf32(static_cast<uint32_t>(addr))
#define MEM_READF64(addr)       g_mem.readf64(static_cast<uint32_t>(addr))
#define MEM_WRITE8(addr, val)   g_mem.write8(static_cast<uint32_t>(addr), static_cast<uint8_t>(val))
#define MEM_WRITE16(addr, val)  g_mem.write16(static_cast<uint32_t>(addr), static_cast<uint16_t>(val))
#define MEM_WRITE32(addr, val)  g_mem.write32(static_cast<uint32_t>(addr), static_cast<uint32_t>(val))
#define MEM_WRITEF32(addr, val) g_mem.writef32(static_cast<uint32_t>(addr), static_cast<float>(val))
#define MEM_WRITEF64(addr, val) g_mem.writef64(static_cast<uint32_t>(addr), static_cast<double>(val))
#define PSQ_LOAD_ONE(addr, gqr)             psq_load_one(g_mem, static_cast<uint32_t>(addr), gqr)
#define PSQ_LOAD_PAIR(ps0, ps1, addr, gqr)  psq_load_pair(g_mem, ps0, ps1, static_cast<uint32_t>(addr), gqr)
#define PSQ_STORE_ONE(val, addr, gqr)       psq_store_one(g_mem, val, static_cast<uint32_t>(addr), gqr)
#define PSQ_STORE_PAIR(v0, v1, addr, gqr)   psq_store_pair(g_mem, v0, v1, static_cast<uint32_t>(addr), gqr)

struct PdEntry {
    uint8_t kind;  // 0 GPR, 1 FPR ps0, 2 FPR ps1, 3 CR, 5 LR, 6 CTR, 8 memory word, 255 end
    uint8_t idx;
    uint64_t value;
};
struct PdCase {
    const char* name;
    int ps1;
    const char* words;
    const char* dis;
    void (*fn)(PPCContext*, Memory*);
    const PdEntry* in;
    const PdEntry* out;
};

#include "ppc_diff_cases.inc"

static constexpr uint32_t kMem = 0x80002000u;
static constexpr int kMemWords = 32;

struct Snapshot {
    uint32_t gpr[32];
    uint64_t fpr0[32];
    uint32_t fpr1[32];
    uint32_t cr, lr, ctr;
    uint32_t mem[kMemWords];
};

static uint64_t bits64(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}
static double from_bits(uint64_t b) {
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
static uint32_t bits32(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
static float from_bits32(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

static void apply(Snapshot& s, const PdEntry* e) {
    for (; e->kind != 255; ++e) {
        switch (e->kind) {
            case 0: s.gpr[e->idx] = static_cast<uint32_t>(e->value); break;
            case 1: s.fpr0[e->idx] = e->value; break;
            case 2: s.fpr1[e->idx] = bits32(static_cast<float>(from_bits(e->value))); break;
            case 3: s.cr = static_cast<uint32_t>(e->value); break;
            case 5: s.lr = static_cast<uint32_t>(e->value); break;
            case 6: s.ctr = static_cast<uint32_t>(e->value); break;
            case 8: s.mem[e->idx] = static_cast<uint32_t>(e->value); break;
        }
    }
}

// The upper word of fctiw/fctiwz/mffs results is implementation-defined (the Gekko puts 0xFFF80000 there; the
// emitted code leaves zero); games read the low word only.
static bool low_word_only(const std::string& name) { return name == "fctiw" || name == "fctiwz" || name == "mffs"; }

static std::set<std::string> split_names(const char* s) {
    std::set<std::string> r;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, ',')) r.insert(t);
    return r;
}

int main(int argc, char** argv) {
    int max_report = 3;
    bool verbose = false, show_inputs = false;
    std::set<std::string> only, ignore;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--verbose") verbose = true;
        else if (a == "--inputs") show_inputs = true;
        else if (a == "--max-report" && i + 1 < argc) max_report = std::atoi(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = split_names(argv[++i]);
        else if (a == "--ignore" && i + 1 < argc) ignore = split_names(argv[++i]);
        else {
            std::printf(
                "usage: ppc_diff_run [--verbose] [--inputs] [--max-report N] [--only NAME,..] [--ignore NAME,..]\n"
                "Runs the instruction cases compiled in from the vectors file and compares them with the reference\n"
                "results (see tools/ppc-diff/README.md).\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (!g_mem.init()) {
        std::printf("cannot initialise memory\n");
        return 2;
    }

    struct Stat {
        int run = 0, bad = 0, reported = 0;
    };
    std::map<std::string, Stat> stats;
    int failed_total = 0;
    for (int ci = 0; ci < pd_ncases; ++ci) {
        const PdCase& c = pd_cases[ci];
        if (!only.empty() && !only.count(c.name)) continue;
        const bool ignored = ignore.count(c.name) != 0;

        Snapshot exp;
        std::memset(&exp, 0, sizeof exp);
        apply(exp, c.in);
        PPCContext ctx;
        std::memset(&ctx, 0, sizeof ctx);
        for (int i = 0; i < 32; ++i) {
            ctx.r[i] = exp.gpr[i];
            ctx.f[i] = from_bits(exp.fpr0[i]);
            ctx.ps1[i] = from_bits32(exp.fpr1[i]);
        }
        ctx.cr = exp.cr;
        ctx.lr = exp.lr;
        ctx.ctr = exp.ctr;
        for (int i = 0; i < kMemWords; ++i) g_mem.write32(kMem + 4 * i, exp.mem[i]);

        apply(exp, c.out);  // exp is now the expected final state
        c.fn(&ctx, &g_mem);

        Snapshot got;
        std::memset(&got, 0, sizeof got);
        for (int i = 0; i < 32; ++i) {
            got.gpr[i] = ctx.r[i];
            got.fpr0[i] = bits64(ctx.f[i]);
            got.fpr1[i] = bits32(ctx.ps1[i]);
        }
        got.cr = ctx.cr;
        got.lr = ctx.lr;
        got.ctr = ctx.ctr;
        for (int i = 0; i < kMemWords; ++i) got.mem[i] = g_mem.read32(kMem + 4 * i);

        const bool lowonly = low_word_only(c.name);
        std::string diff;
        char buf[160];
        for (int i = 0; i < 32; ++i) {
            if (got.gpr[i] != exp.gpr[i]) {
                std::snprintf(buf, sizeof buf, "  r%d = %08x, expected %08x\n", i, got.gpr[i], exp.gpr[i]);
                diff += buf;
            }
            uint64_t g = got.fpr0[i], e = exp.fpr0[i];
            if (lowonly) g &= 0xFFFFFFFFull, e &= 0xFFFFFFFFull;
            // NaNs compare by class only: the payload of a generated NaN is not architecturally defined for
            // our purposes.
            const bool both_nan = std::isnan(from_bits(g)) && std::isnan(from_bits(e)) && !lowonly;
            if (g != e && !both_nan) {
                std::snprintf(buf, sizeof buf, "  f%d.ps0 = %016llx, expected %016llx\n", i, (unsigned long long)g,
                              (unsigned long long)e);
                diff += buf;
            }
            if (c.ps1) {
                const bool nan2 = std::isnan(from_bits32(got.fpr1[i])) && std::isnan(from_bits32(exp.fpr1[i]));
                if (got.fpr1[i] != exp.fpr1[i] && !nan2) {
                    std::snprintf(buf, sizeof buf, "  f%d.ps1 = %08x, expected %08x\n", i, got.fpr1[i], exp.fpr1[i]);
                    diff += buf;
                }
            }
        }
        if (got.cr != exp.cr) {
            std::snprintf(buf, sizeof buf, "  cr = %08x, expected %08x\n", got.cr, exp.cr);
            diff += buf;
        }
        if (got.lr != exp.lr) {
            std::snprintf(buf, sizeof buf, "  lr = %08x, expected %08x\n", got.lr, exp.lr);
            diff += buf;
        }
        if (got.ctr != exp.ctr) {
            std::snprintf(buf, sizeof buf, "  ctr = %08x, expected %08x\n", got.ctr, exp.ctr);
            diff += buf;
        }
        for (int i = 0; i < kMemWords; ++i)
            if (got.mem[i] != exp.mem[i]) {
                std::snprintf(buf, sizeof buf, "  mem[+0x%02x] = %08x, expected %08x\n", 4 * i, got.mem[i], exp.mem[i]);
                diff += buf;
            }

        Stat& st = stats[c.name];
        ++st.run;
        if (!diff.empty()) {
            ++st.bad;
            if (!ignored) ++failed_total;
            if (st.reported < max_report || verbose) {
                ++st.reported;
                std::printf("MISMATCH %s%s: %s\n  words %s\n%s", c.name, ignored ? " (ignored)" : "", c.dis, c.words,
                            diff.c_str());
                if (show_inputs) {  // the full input state, to reproduce the case by hand
                    std::printf("  inputs:");
                    for (const PdEntry* e = c.in; e->kind != 255; ++e)
                        std::printf(" %u.%u=%llx", e->kind, e->idx, (unsigned long long)e->value);
                    std::printf("\n");
                }
            }
        }
    }

    std::printf("\n%-14s %6s %6s\n", "instruction", "cases", "wrong");
    int total = 0;
    for (const auto& kv : stats) {
        total += kv.second.run;
        std::printf("%-14s %6d %6d%s\n", kv.first.c_str(), kv.second.run, kv.second.bad,
                    kv.second.bad ? (ignore.count(kv.first) ? "  (ignored)" : "  <== MISMATCH") : "");
    }
    std::printf("\nppc_diff: %d cases, %d mismatching (%d with a branch and %d undecodable skipped by the generator)\n",
                total, failed_total, pd_skipped_branch, pd_skipped_unknown);
    return failed_total ? 1 : 0;
}
