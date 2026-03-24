// =============================================================================
// gcrecomp DOL Info — GameCube/Triforce DOL file analyzer
//
// Dumps information about a DOL executable:
//   - Section layout (text, data, BSS)
//   - Entry point, memory bounds
//   - Disassembly statistics
//   - Quick instruction type census
//
// Usage: gcrecomp_dol_info <dol_file>
// =============================================================================

#include "gcrecomp/dol.h"
#include "gcrecomp/ppc.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>

using namespace gcrecomp;

int main(int argc, char* argv[]) {
    printf("gcrecomp DOL Info v0.1.0\n\n");

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dol_file> [--disasm] [--stats]\n", argv[0]);
        return 1;
    }

    bool show_stats = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--stats") == 0) show_stats = true;
    }

    DOLFile dol;
    if (!dol.load(argv[1])) {
        fprintf(stderr, "Failed to load DOL: %s\n", argv[1]);
        return 1;
    }

    dol.print_info();

    if (show_stats) {
        printf("\n=== Instruction Statistics ===\n");

        uint32_t total_insn = 0;
        uint32_t unknown_insn = 0;
        uint32_t branch_insn = 0;
        uint32_t call_insn = 0;
        uint32_t ps_insn = 0;
        std::map<std::string, uint32_t> mnemonic_counts;

        for (const auto& sec : dol.sections) {
            if (!sec.is_text) continue;
            auto insns = ppc_disasm_range(sec.data.data(), sec.address, sec.size);
            for (const auto& insn : insns) {
                total_insn++;
                mnemonic_counts[insn.mnemonic]++;
                if (insn.type == PPCInsnType::UNKNOWN) unknown_insn++;
                if (insn.is_branch()) branch_insn++;
                if (insn.is_call()) call_insn++;
                if (insn.mnemonic.substr(0, 2) == "ps" ||
                    insn.mnemonic.substr(0, 3) == "psq") ps_insn++;
            }
        }

        printf("Total instructions: %u\n", total_insn);
        printf("Unknown:            %u (%.2f%%)\n", unknown_insn,
               total_insn ? 100.0 * unknown_insn / total_insn : 0.0);
        printf("Branches:           %u\n", branch_insn);
        printf("Calls (bl/blr):     %u\n", call_insn);
        printf("Paired Singles:     %u\n", ps_insn);
        printf("Coverage:           %.2f%%\n",
               total_insn ? 100.0 * (total_insn - unknown_insn) / total_insn : 0.0);

        // Estimate function count from call targets
        printf("\nEstimated function entries: ~%u (based on bl targets)\n", call_insn);

        // Top 20 mnemonics
        printf("\nTop 20 instructions:\n");
        std::vector<std::pair<uint32_t, std::string>> sorted;
        for (const auto& [name, count] : mnemonic_counts)
            sorted.push_back({count, name});
        std::sort(sorted.begin(), sorted.end(), std::greater<>());
        for (int i = 0; i < 20 && i < (int)sorted.size(); i++) {
            printf("  %-12s %8u  (%.1f%%)\n", sorted[i].second.c_str(),
                   sorted[i].first, 100.0 * sorted[i].first / total_insn);
        }
    }

    return 0;
}
