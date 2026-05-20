// =============================================================================
// Control Flow Graph Builder
// Analyzes disassembled PPC code to identify functions, basic blocks,
// and call targets. This is how we turn a flat binary into structured C.
// =============================================================================

#include "gcrecomp/cfg.h"
#include <cstdio>
#include <queue>
#include <algorithm>

namespace gcrecomp {

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
                if (dol.is_code(target) && (offset > 0x100 || offset < -0x100)) {
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

            if (dol.is_code(val) && (val & 3) == 0) {
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
    before = call_targets.size();
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

            if (matched && (addr & 3u) == 0 && dol.is_code(addr)) {
                call_targets.insert(addr);
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
                    // Indirect branch via CTR — could be switch table
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
