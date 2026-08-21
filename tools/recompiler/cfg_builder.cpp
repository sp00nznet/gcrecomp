// =============================================================================
// Control Flow Graph Builder
// Analyzes disassembled PPC code to identify functions, basic blocks,
// and call targets. This is how we turn a flat binary into structured C.
// =============================================================================

#include "gcrecomp/cfg.h"
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <algorithm>

namespace gcrecomp {

namespace {

// Entry gating and jump-table resolution are on by default. Verified against
// Wind Waker: 2583 -> 327 functions popping frames they never pushed, 254 of
// 282 jump tables resolved, and the game still runs. GCRECOMP_LEGACY_CFG=1
// restores the old promote-everything behaviour if a title needs it.
bool experimental_cfg() {
    static const bool legacy = std::getenv("GCRECOMP_LEGACY_CFG") != nullptr;
    return !legacy;
}

// Does `addr` begin a function, or land mid-way through one?
//
// The tail-call heuristic below promotes distant `b` targets to function
// entries. In a codebase with multi-kilobyte functions a long intra-function
// branch to a shared epilogue is completely ordinary, and promoting one
// produces a "function" that pops a stack frame it never pushed: called
// through the dispatch table it leaks r1 and restores callee-saved registers
// from whatever occupies those slots, silently corrupting its caller.
//
// So walk forward and look at what the code does to r1 first. A real entry
// either establishes a frame (stwu r1, -N(r1)) or, if it is a leaf, never
// touches r1 at all. Reaching a pop (addi r1, r1, +N) or an mtlr with no
// preceding push means we are standing in somebody else's epilogue.
bool looks_like_function_entry(uint32_t addr, const DOLFile& dol) {
    if (!experimental_cfg()) return true;
    constexpr int kMaxScan = 96;   // longest plausible prologue-to-epilogue run
    constexpr uint32_t kSprLR = 8;

    uint32_t at = addr;
    for (int i = 0; i < kMaxScan; i++) {
        if (!dol.is_code(at)) return false;
        PPCInsn insn = ppc_disasm(dol.read32(at), at);

        // Frame established: this is an entry.
        if (insn.type == PPCInsnType::STWU && insn.ra == 1 && insn.rs == 1 &&
            insn.simm < 0) {
            return true;
        }
        // Frame torn down before any was set up: mid-function.
        if (insn.type == PPCInsnType::ADDI && insn.rd == 1 && insn.ra == 1 &&
            insn.simm > 0) {
            return false;
        }
        // Restoring the return address without having saved it: mid-function.
        if (insn.type == PPCInsnType::MTSPR && insn.spr == kSprLR) {
            return false;
        }
        // A leaf that returns without ever touching r1 is a legitimate entry.
        if (insn.is_return()) return true;

        // An unconditional branch continues the straight-line run somewhere
        // else, so follow it. This is the case that matters: a jump-table
        // case body is a couple of instructions and a hop to the shared
        // epilogue, and only by following do we reach the pop that gives it
        // away. A bl returns here, so it just advances. The iteration budget
        // bounds any loop we walk into.
        if (insn.type == PPCInsnType::B && !insn.link) {
            at = (uint32_t)insn.branch_target;
            continue;
        }
        at += 4;
    }
    return true;
}


}  // namespace


void CFG::build(const DOLFile& dol) {
    scan_targets(dol);
    build_functions(dol);
}

void CFG::scan_targets(const DOLFile& dol) {
    printf("[CFG] Building control flow graph...\n");

    // Start with the entry point
    call_targets.insert(dol.entry_point);

    // Phase 1: Linear scan for bl (branch-and-link) targets
    std::set<uint32_t> tail_call_candidates;
    for (const auto& sec : dol.sections) {
        if (!sec.is_text) continue;

        auto insns = ppc_disasm_range(sec.data.data(), sec.address, sec.size);
        for (const auto& insn : insns) {
            if (insn.type == PPCInsnType::B && insn.link) {
                uint32_t target = insn.branch_target;
                if (dol.is_code(target)) {
                    call_targets.insert(target);
                }
            }
            else if (insn.type == PPCInsnType::B && !insn.link) {
                uint32_t target = insn.branch_target;
                int32_t offset = (int32_t)target - (int32_t)insn.address;
                // Distance alone does not distinguish a tail call from a long
                // branch inside one big function, so also require the target to
                // actually look like an entry. See looks_like_function_entry().
                if (dol.is_code(target) && (offset > 0x100 || offset < -0x100) &&
                    looks_like_function_entry(target, dol)) {
                    tail_call_candidates.insert(target);
                }
            }
        }
    }

    size_t tail_calls_added = 0;
    for (uint32_t tc : tail_call_candidates) {
        if (!call_targets.count(tc)) {
            call_targets.insert(tc);
            tail_calls_added++;
        }
    }

    printf("[CFG] Phase 1: Found %zu call targets from bl scan (+%zu tail calls)\n",
           call_targets.size() - tail_calls_added, tail_calls_added);

    // Phase 1.5: Scan data sections for function pointers
    size_t before = call_targets.size();
    for (const auto& sec : dol.sections) {
        if (sec.is_text) continue;
        if (sec.size < 4) continue;

        for (uint32_t off = 0; off + 4 <= sec.size; off += 4) {
            const uint8_t* p = sec.data.data() + off;
            uint32_t val = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                           ((uint32_t)p[2] << 8) | p[3];

            // A word in .data that happens to look like a code address is
            // not necessarily a function: jump tables live here too, and
            // their entries point mid-function.
            if (dol.is_code(val) && (val & 3) == 0 &&
                looks_like_function_entry(val, dol)) {
                call_targets.insert(val);
            }
        }
    }
    printf("[CFG] Phase 1.5: Found %zu additional targets from data scan (%zu total)\n",
           call_targets.size() - before, call_targets.size());

    // Phase 1.6: Scan code for function prologues (stwu r1, -X(r1) = 0x9421xxxx)
    before = call_targets.size();
    for (const auto& sec : dol.sections) {
        if (!sec.is_text) continue;
        for (uint32_t off = 0; off + 4 <= sec.size; off += 4) {
            const uint8_t* p = sec.data.data() + off;
            uint32_t raw = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                           ((uint32_t)p[2] << 8) | p[3];
            if ((raw >> 16) == 0x9421) {
                uint32_t addr = sec.address + off;
                call_targets.insert(addr);
            }
        }
    }
    printf("[CFG] Phase 1.6: Found %zu additional targets from prologue scan (%zu total)\n",
           call_targets.size() - before, call_targets.size());

    // Phase 1.7: Scan text for `lis rD, hi` / `addi rD, rD, lo` and
    // `lis rD, hi` / `ori rD, rD, lo` pairs that materialize a 32-bit
    // absolute address. Catches function pointers stored in tables that
    // get loaded into registers at runtime (vtables, jump tables, callback
    // registries) which Phase 1.5's static data scan can miss when the
    // pointer is computed rather than stored.
    //
    // PPC encoding:
    //   lis  rD, imm     = 0x3C00_0000 | (rD<<21) | imm16    (opcode 15, rA=0)
    //   addi rD, rA, simm= 0x3800_0000 | (rD<<21) | (rA<<16) | imm16 (opcode 14)
    //   ori  rA, rS, imm = 0x6000_0000 | (rS<<21) | (rA<<16) | imm16 (opcode 24)
    // Phase 1.7 is opt-in with the rest of the in-progress CFG work: it is the
    // only discovery difference between this recompiler and the fork in ww,
    // and the fork is the one whose output runs.
    before = call_targets.size();
    if (experimental_cfg()) {
    auto extract_reg = [](uint32_t raw, int shift) -> uint32_t {
        return (raw >> shift) & 0x1F;
    };
    for (const auto& sec : dol.sections) {
        if (!sec.is_text) continue;
        // Need at least 8 bytes for a pair.
        for (uint32_t off = 0; off + 8 <= sec.size; off += 4) {
            const uint8_t* p1 = sec.data.data() + off;
            uint32_t i1 = ((uint32_t)p1[0] << 24) | ((uint32_t)p1[1] << 16) |
                          ((uint32_t)p1[2] << 8) | p1[3];
            // lis: primary opcode 15, rA == 0 (so addis becomes lis).
            // 0x3C = primary opcode 15 with the high register bit cleared
            // (rA hidden in low 5 bits of 1st byte must be 0).
            if ((i1 & 0xFC1F0000u) != 0x3C000000u) continue;
            uint32_t hi_reg = extract_reg(i1, 21);
            uint32_t hi_imm = i1 & 0xFFFFu;

            const uint8_t* p2 = sec.data.data() + off + 4;
            uint32_t i2 = ((uint32_t)p2[0] << 24) | ((uint32_t)p2[1] << 16) |
                          ((uint32_t)p2[2] << 8) | p2[3];

            uint32_t lo_simm   = i2 & 0xFFFFu;
            uint32_t addr      = 0;
            bool     matched   = false;

            uint32_t op2  = i2 >> 26;
            uint32_t lo_rD = extract_reg(i2, 21);
            uint32_t lo_rA = extract_reg(i2, 16);

            // addi rD, rD, lo with rD == hi_reg, rA == hi_reg
            // (opcode 14 = 0x38000000)
            if (op2 == 14 && lo_rD == hi_reg && lo_rA == hi_reg) {
                int32_t signed_lo = (int32_t)(int16_t)lo_simm;
                addr = (hi_imm << 16) + (uint32_t)signed_lo;
                matched = true;
            }
            // ori rD, rS, lo with rD == rS == hi_reg
            // (opcode 24 = 0x60000000)
            else if (op2 == 24 && lo_rA == hi_reg && lo_rD == hi_reg) {
                addr = (hi_imm << 16) | lo_simm;
                matched = true;
            }

            if (matched && (addr & 3u) == 0 && dol.is_code(addr) &&
                looks_like_function_entry(addr, dol)) {
                call_targets.insert(addr);
            }
        }
    }
}
    printf("[CFG] Phase 1.7: Found %zu additional targets from "
           "lis/addi+ori pair scan (%zu total)\n",
           call_targets.size() - before, call_targets.size());
}

void CFG::build_functions(const DOLFile& dol) {
    discover_functions(dol);
    printf("[CFG] Phase 2: Built %zu functions\n", functions.size());
}

void CFG::add_extra_entries(const std::vector<uint32_t>& addrs) {
    size_t added = 0;
    for (uint32_t addr : addrs) {
        if (call_targets.insert(addr).second) added++;
    }
    printf("[CFG] Added %zu extra function entries (%zu were duplicates)\n",
           added, addrs.size() - added);
}

void CFG::discover_functions(const DOLFile& dol) {
    for (uint32_t entry : call_targets) {
        if (!dol.is_code(entry)) continue;

        Function func;
        func.entry = entry;
        func.is_leaf = true;

        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "func_%08X", entry);
        func.name = name_buf;

        build_blocks(func, dol);
        functions[entry] = std::move(func);
    }
}

void CFG::build_blocks(Function& func, const DOLFile& dol) {
    std::set<uint32_t> block_starts;
    std::queue<uint32_t> work;

    block_starts.insert(func.entry);
    work.push(func.entry);

    // First pass: discover block boundaries
    while (!work.empty()) {
        uint32_t addr = work.front();
        work.pop();

        uint32_t pc = addr;
        while (dol.is_code(pc)) {
            uint32_t raw = dol.read32(pc);
            PPCInsn insn = ppc_disasm(raw, pc);

            if (insn.is_branch()) {
                if (insn.type == PPCInsnType::B && !insn.link) {
                    // Unconditional branch (not a call)
                    uint32_t target = insn.branch_target;
                    if (dol.is_code(target) && !block_starts.count(target)) {
                        block_starts.insert(target);
                        work.push(target);
                    }
                    break;
                }
                else if (insn.type == PPCInsnType::BC) {
                    // Conditional branch
                    uint32_t target = insn.branch_target;
                    uint32_t fall = pc + 4;
                    if (dol.is_code(target) && !block_starts.count(target)) {
                        block_starts.insert(target);
                        work.push(target);
                    }
                    if (!block_starts.count(fall)) {
                        block_starts.insert(fall);
                        work.push(fall);
                    }
                    break;
                }
                else if (insn.type == PPCInsnType::B && insn.link) {
                    // Function call (bl) — continue, execution returns
                    func.calls.insert(insn.branch_target);
                    func.is_leaf = false;
                }
                else if (insn.type == PPCInsnType::BCCTR && insn.link) {
                    // Indirect call via CTR (bctrl) — continue, execution returns
                    func.is_leaf = false;
                }
                else if (insn.type == PPCInsnType::BCLR && insn.link) {
                    // Indirect call via LR (blrl) — continue, execution returns
                    func.is_leaf = false;
                }
                else if (insn.is_return()) {
                    break;
                }
                else if (insn.type == PPCInsnType::BCLR && !insn.link) {
                    // Conditional return via LR — block terminator
                    // Fall-through is a new block (if condition not met)
                    uint32_t fall = pc + 4;
                    if (dol.is_code(fall) && !block_starts.count(fall)) {
                        block_starts.insert(fall);
                        work.push(fall);
                    }
                    break;
                }
                else if (insn.type == PPCInsnType::BCCTR && !insn.link) {
                    // Indirect branch via CTR — try to resolve switch table
                    auto jt_targets = detect_jump_table(func, pc, dol);
                    for (uint32_t target : jt_targets) {
                        if (dol.is_code(target) && !block_starts.count(target)) {
                            block_starts.insert(target);
                            work.push(target);
                        }
                    }
                    break;
                }
            }

            pc += 4;
        }
    }

    // Second pass: build actual basic blocks
    std::vector<uint32_t> sorted_starts(block_starts.begin(), block_starts.end());
    std::sort(sorted_starts.begin(), sorted_starts.end());
    func.block_addrs = sorted_starts;

    for (size_t i = 0; i < sorted_starts.size(); i++) {
        uint32_t start = sorted_starts[i];
        uint32_t limit = (i + 1 < sorted_starts.size()) ? sorted_starts[i + 1] : start + 0x10000;

        BasicBlock block;
        block.start = start;
        block.is_entry = (start == func.entry);
        block.is_return = false;

        uint32_t pc = start;
        while (pc < limit && dol.is_code(pc)) {
            uint32_t raw = dol.read32(pc);
            PPCInsn insn = ppc_disasm(raw, pc);
            block.instructions.push_back(insn);

            if (insn.is_branch()) {
                if (insn.type == PPCInsnType::B && insn.link) {
                    // Function call (bl) — not a block terminator, execution continues
                    pc += 4;
                    continue;
                }
                if (insn.type == PPCInsnType::BCLR && insn.link) {
                    // Indirect call via LR (blrl) — not a block terminator
                    pc += 4;
                    continue;
                }
                if (insn.type == PPCInsnType::BCCTR && insn.link) {
                    // Indirect call via CTR (bctrl) — not a block terminator
                    pc += 4;
                    continue;
                }
                if (insn.is_return()) {
                    block.is_return = true;
                } else if (insn.type == PPCInsnType::B && !insn.link) {
                    block.successors.push_back(insn.branch_target);
                } else if (insn.type == PPCInsnType::BC) {
                    block.successors.push_back(insn.branch_target);
                    block.successors.push_back(pc + 4);
                } else if (insn.type == PPCInsnType::BCLR && !insn.link) {
                    // Conditional return — terminates block
                    block.is_return = true;
                } else if (insn.type == PPCInsnType::BCCTR && !insn.link) {
                    // Switch table — resolve targets and wire as successors
                    auto jt_targets = detect_jump_table(func, pc, dol);
                    block.jump_table_targets = jt_targets;
                    for (uint32_t target : jt_targets) {
                        block.successors.push_back(target);
                    }
                }
                pc += 4;
                break;
            }
            pc += 4;
        }

        block.end = pc;
        func.blocks[start] = std::move(block);
    }

    // Wire up predecessors
    for (auto& [addr, block] : func.blocks) {
        for (uint32_t succ : block.successors) {
            if (func.blocks.count(succ)) {
                func.blocks[succ].predecessors.push_back(addr);
            }
        }
    }
}

// Detect jump tables by backscanning from a bctr instruction.
// Looks for the classic GCC pattern:
//   cmplwi rX, N          (bounds check: N = table size)
//   bc     ...            (branch if out of bounds)
//   lis    rY, hi(table)  (load table base high)
//   addi   rY, rY, lo(table) or ori rY, rY, lo(table)
//   rlwinm rZ, rX, 2, 0, 29  (index * 4)
//   lwzx   rZ, rY, rZ    (load target address)
//   mtctr  rZ             (move to CTR)
//   bctr                  (dispatch)
//
// Inspired by ExpansionPak/GCRecompiler's jump table detection approach.
std::vector<uint32_t> CFG::detect_jump_table(const Function& func, uint32_t bctr_addr, const DOLFile& dol) {
    (void)func;
    std::vector<uint32_t> targets;
    if (!experimental_cfg()) return targets;

    // Read the run leading up to the bctr straight out of the image rather
    // than from func.blocks. This is called both while block boundaries are
    // still being discovered (func.blocks empty) and while the block holding
    // the bctr is mid-construction and not yet inserted, so a window built
    // from func.blocks never contains the bctr and detection always failed.
    constexpr int kWindow = 24;
    std::vector<PPCInsn> window;
    for (int i = kWindow; i >= 0; i--) {
        uint32_t at = bctr_addr - (uint32_t)i * 4;
        if (at > bctr_addr || !dol.is_code(at)) { window.clear(); continue; }
        window.push_back(ppc_disasm(dol.read32(at), at));
    }
    if (window.empty()) return targets;

    // Scan forward, tracking what each register holds. Backwards would meet
    // the addi before the lis that feeds it, which is why the original pair
    // match never completed.
    uint32_t reg_val[32] = {0};
    bool     reg_set[32] = {false};
    uint32_t table_count = 0;
    bool     found_bounds = false;
    // The chain has to actually connect: the register moved to CTR must be
    // the one lwzx loaded, and lwzx must be indexing the base a lis/addi pair
    // built. Matching the four opcodes independently -- as this did -- fires on
    // any nearby lis, and a bogus table turns `bctr` into a goto to nowhere.
    uint32_t lwzx_rd = 32, lwzx_ra = 32, lwzx_rb = 32;
    uint32_t mtctr_rs = 32;

    for (const PPCInsn& insn : window) {
        const uint32_t raw = insn.raw;
        const uint32_t op = PPC_OP(raw), xo = PPC_XO(raw);
        const uint32_t rd = PPC_RD(raw), ra = PPC_RA(raw), rb = (raw >> 11) & 0x1F;
        const uint32_t imm = raw & 0xFFFF;

        if (op == 15 && ra == 0) {                    // lis rD, imm
            reg_val[rd] = imm << 16; reg_set[rd] = true;
        } else if (op == 14) {                        // addi rD, rA, simm
            if (reg_set[ra]) {
                reg_val[rd] = reg_val[ra] + (uint32_t)(int32_t)(int16_t)imm;
                reg_set[rd] = true;
            } else {
                reg_set[rd] = false;
            }
        } else if (op == 24) {                        // ori rA, rS, imm
            if (reg_set[rd]) { reg_val[ra] = reg_val[rd] | imm; reg_set[ra] = true; }
            else             { reg_set[ra] = false; }
        } else if (op == 31 && xo == 23) {            // lwzx rD, rA, rB
            lwzx_rd = rd; lwzx_ra = ra; lwzx_rb = rb;
        } else if (op == 31 && xo == 467 && PPC_SPR(raw) == 9) {  // mtctr rS
            mtctr_rs = rd;                            // rS sits in the rD field
        } else if (op == 10 || op == 11) {            // cmplwi / cmpwi rA, N
            table_count = (op == 10) ? imm : (uint32_t)(int32_t)(int16_t)imm;
            found_bounds = true;
        }
    }

    if (mtctr_rs == 32 || lwzx_rd == 32 || !found_bounds) return targets;
    if (mtctr_rs != lwzx_rd) return targets;          // CTR not fed by the load

    // Whichever operand of the lwzx carries the table address.
    uint32_t table_base = 0;
    if (lwzx_ra < 32 && reg_set[lwzx_ra])      table_base = reg_val[lwzx_ra];
    else if (lwzx_rb < 32 && reg_set[lwzx_rb]) table_base = reg_val[lwzx_rb];
    else return targets;
    const bool found_mtctr = true, found_lwzx = true, found_base = table_base != 0;

    if (!found_mtctr || !found_lwzx || !found_base || !found_bounds) return targets;
    if (table_count == 0 || table_count > 512) return targets;

    // The compare bounds the index but its exact form (bgt vs bge) decides
    // whether the last case is included, so read one extra entry and let the
    // is_code check end the table.
    for (uint32_t i = 0; i <= table_count; i++) {
        uint32_t target = dol.read32(table_base + i * 4);
        if (target == 0 || !dol.is_code(target)) break;
        targets.push_back(target);
    }
    return targets;
}


void CFG::print_stats() const {
    uint32_t total_blocks = 0;
    uint32_t total_insns = 0;
    uint32_t leaf_funcs = 0;

    for (const auto& [addr, func] : functions) {
        total_blocks += (uint32_t)func.blocks.size();
        for (const auto& [_, block] : func.blocks) {
            total_insns += (uint32_t)block.instructions.size();
        }
        if (func.is_leaf) leaf_funcs++;
    }

    printf("=== CFG Statistics ===\n");
    printf("Functions:    %zu\n", functions.size());
    printf("Leaf funcs:   %u\n", leaf_funcs);
    printf("Basic blocks: %u\n", total_blocks);
    printf("Instructions: %u\n", total_insns);
}

} // namespace gcrecomp
