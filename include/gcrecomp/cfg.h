#pragma once
// =============================================================================
// gcrecomp — Control Flow Graph
//
// Analyzes disassembled PPC code to identify functions, basic blocks,
// and call targets. Used by the PPC-to-C code generator.
// =============================================================================

#include "gcrecomp/ppc.h"
#include "gcrecomp/dol.h"
#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <map>

namespace gcrecomp {

struct BasicBlock {
    uint32_t start;
    uint32_t end;
    std::vector<uint32_t> successors;
    std::vector<uint32_t> predecessors;
    std::vector<PPCInsn>  instructions;
    std::vector<uint32_t> jump_table_targets; // Resolved targets from bctr switch tables
    bool     is_entry;
    bool     is_return;
};

struct Function {
    uint32_t entry;
    std::string name;
    std::vector<uint32_t> block_addrs;
    std::map<uint32_t, BasicBlock> blocks;
    std::set<uint32_t> calls;
    bool     is_leaf;
};

struct CFG {
    std::map<uint32_t, Function> functions;
    std::set<uint32_t> call_targets;

    void build(const DOLFile& dol);
    void scan_targets(const DOLFile& dol);
    void add_extra_entries(const std::vector<uint32_t>& addrs);
    void build_functions(const DOLFile& dol);
    void discover_functions(const DOLFile& dol);
    void build_blocks(Function& func, const DOLFile& dol);
    std::vector<uint32_t> detect_jump_table(const Function& func, uint32_t bctr_addr, const DOLFile& dol);
    void print_stats() const;
};

} // namespace gcrecomp
