// =============================================================================
// gcrecomp Static Recompiler — PPC-to-C translation tool
//
// Generic recompiler for any Gekko-based platform (GameCube, Triforce, Wii).
// Reads a DOL executable, builds a control flow graph, and emits C code
// that operates on the gcrecomp PPCContext and Memory runtime.
//
// Usage: gcrecomp_recompiler <input.dol> [options]
//   --map <file>       Load symbol map (Dolphin format)
//   --csv <file>       Load symbol map (CSV format)
//   --output <dir>     Output directory (default: ./recompiled)
//   --extra-funcs <f>  Force-add function entries (one hex addr per line)
//   --info             Print DOL info and exit
//   --stats            Print CFG statistics and exit
// =============================================================================

#include "gcrecomp/dol.h"
#include "gcrecomp/ppc.h"
#include "gcrecomp/cfg.h"
#include "gcrecomp/symbol_map.h"
#include "gcrecomp/ppc_to_c.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gcrecomp;

static void print_usage(const char* prog) {
    printf("gcrecomp Static Recompiler (Gekko PowerPC -> C)\n");
    printf("================================================\n\n");
    printf("Usage: %s <input.dol> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --map <file>         Load Dolphin symbol map\n");
    printf("  --csv <file>         Load CSV symbol map (addr,name)\n");
    printf("  --output <dir>       Output directory (default: ./recompiled)\n");
    printf("  --extra-funcs <file> Force-add function entries (one hex addr/line)\n");
    printf("  --funcs-per-file <n> Functions per output file (default: 200)\n");
    printf("  --project <name>     Project name in file headers\n");
    printf("  --info               Print DOL info and exit\n");
    printf("  --stats              Print CFG statistics and exit\n");
    printf("  --help               Show this help\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string dol_path;
    std::string map_path;
    std::string csv_path;
    std::string extra_funcs_path;
    std::string output_dir = "recompiled";
    std::string project_name = "gcrecomp";
    int funcs_per_file = 200;
    bool info_only = false;
    bool stats_only = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (strcmp(argv[i], "--extra-funcs") == 0 && i + 1 < argc) {
            extra_funcs_path = argv[++i];
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_name = argv[++i];
        } else if (strcmp(argv[i], "--funcs-per-file") == 0 && i + 1 < argc) {
            funcs_per_file = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--info") == 0) {
            info_only = true;
        } else if (strcmp(argv[i], "--stats") == 0) {
            stats_only = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            dol_path = argv[i];
        }
    }

    if (dol_path.empty()) {
        fprintf(stderr, "Error: No DOL file specified\n");
        return 1;
    }

    // ---- Load DOL ----
    printf("\n  gcrecomp Static Recompiler\n");
    printf("  Gekko PowerPC 750CXe -> Native C\n\n");

    DOLFile dol;
    printf("[*] Loading DOL: %s\n", dol_path.c_str());
    if (!dol.load(dol_path)) {
        fprintf(stderr, "Failed to load DOL file\n");
        return 1;
    }
    dol.print_info();

    if (info_only) return 0;

    // ---- Load symbols ----
    SymbolMap syms;
    if (!map_path.empty()) syms.load_dolphin_map(map_path);
    if (!csv_path.empty()) syms.load_csv(csv_path);

    // ---- Build CFG ----
    printf("\n[*] Building control flow graph...\n");
    CFG cfg;
    cfg.scan_targets(dol);

    // Load extra function entries if specified
    if (!extra_funcs_path.empty()) {
        FILE* ef = fopen(extra_funcs_path.c_str(), "r");
        if (ef) {
            std::vector<uint32_t> extras;
            char line[256];
            while (fgets(line, sizeof(line), ef)) {
                if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
                uint32_t addr = 0;
                if (sscanf(line, "%x", &addr) == 1 && addr != 0) {
                    extras.push_back(addr);
                }
            }
            fclose(ef);
            cfg.add_extra_entries(extras);
        } else {
            fprintf(stderr, "Warning: Could not open extra-funcs file: %s\n",
                    extra_funcs_path.c_str());
        }
    }

    cfg.build_functions(dol);
    cfg.print_stats();

    if (stats_only) return 0;

    // ---- Emit C code ----
    printf("\n[*] Generating C code -> %s/\n", output_dir.c_str());

    std::string mkdir_cmd = "mkdir -p " + output_dir;
    system(mkdir_cmd.c_str());

    int file_index = 0;
    int func_count = 0;
    FILE* current_file = nullptr;

    // Generate recomp_common.h — shared macros and includes for all recompiled code
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/recomp_common.h", output_dir.c_str());
        FILE* common = fopen(path, "w");
        if (common) {
            fprintf(common, "#pragma once\n");
            fprintf(common, "// Auto-generated by gcrecomp recompiler\n");
            fprintf(common, "#include \"gcrecomp/runtime.h\"\n");
            fprintf(common, "#include \"gcrecomp/ppc_helpers.h\"\n\n");
            fprintf(common, "using gcrecomp::PPCContext;\n");
            fprintf(common, "using gcrecomp::Memory;\n");
            fprintf(common, "using gcrecomp::FuncTable;\n");
            fprintf(common, "using gcrecomp::g_mem;\n");
            fprintf(common, "using gcrecomp::g_func_table;\n\n");
            fprintf(common, "// Memory access macros\n");
            fprintf(common, "#define MEM_READ8(addr)         g_mem.read8(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READ16(addr)        g_mem.read16(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READ32(addr)        g_mem.read32(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READF32(addr)       g_mem.readf32(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_READF64(addr)       g_mem.readf64(static_cast<uint32_t>(addr))\n");
            fprintf(common, "#define MEM_WRITE8(addr, val)   g_mem.write8(static_cast<uint32_t>(addr), static_cast<uint8_t>(val))\n");
            fprintf(common, "#define MEM_WRITE16(addr, val)  g_mem.write16(static_cast<uint32_t>(addr), static_cast<uint16_t>(val))\n");
            fprintf(common, "#define MEM_WRITE32(addr, val)  g_mem.write32(static_cast<uint32_t>(addr), static_cast<uint32_t>(val))\n");
            fprintf(common, "#define MEM_WRITEF32(addr, val) g_mem.writef32(static_cast<uint32_t>(addr), static_cast<float>(val))\n");
            fprintf(common, "#define MEM_WRITEF64(addr, val) g_mem.writef64(static_cast<uint32_t>(addr), static_cast<double>(val))\n\n");
            fprintf(common, "// Indirect call macro\n");
            fprintf(common, "#define CALL_INDIRECT(addr, ctx, mem) g_func_table.call(static_cast<uint32_t>(addr), ctx, mem)\n\n");
            fprintf(common, "// PSQ wrapper macros (delegate to gcrecomp runtime helpers with g_mem)\n");
            fprintf(common, "#define PSQ_LOAD_ONE(addr, gqr)              psq_load_one(g_mem, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_LOAD_PAIR(ps0, ps1, addr, gqr)   psq_load_pair(g_mem, ps0, ps1, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_STORE_ONE(val, addr, gqr)         psq_store_one(g_mem, val, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_STORE_PAIR(v0, v1, addr, gqr)     psq_store_pair(g_mem, v0, v1, static_cast<uint32_t>(addr), gqr)\n");
            fclose(common);
        }
    }

    // Generate forward declarations header
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/recomp_funcs.h", output_dir.c_str());
        FILE* hdr = fopen(path, "w");
        if (hdr) {
            fprintf(hdr, "#pragma once\n");
            fprintf(hdr, "// Auto-generated: forward declarations for all recompiled functions\n");
            fprintf(hdr, "#include \"recomp_common.h\"\n\n");
            for (auto& [addr, func] : cfg.functions) {
                std::string fname = syms.get_name(addr);
                func.name = fname;
                fprintf(hdr, "void %s(PPCContext* ctx, Memory* mem);\n", fname.c_str());
            }
            fprintf(hdr, "\n// Register all recompiled functions into the function table\n");
            fprintf(hdr, "void register_recompiled_functions(FuncTable& table);\n");
            fclose(hdr);
        }

        // Generate function registration table
        snprintf(path, sizeof(path), "%s/recomp_register.cpp", output_dir.c_str());
        FILE* reg = fopen(path, "w");
        if (reg) {
            fprintf(reg, "// Auto-generated: registers all recompiled functions\n");
            fprintf(reg, "#include \"recomp_funcs.h\"\n\n");
            fprintf(reg, "void register_recompiled_functions(FuncTable& table) {\n");
            for (auto& [addr, func] : cfg.functions) {
                fprintf(reg, "    table.register_func(0x%08X, %s);\n", addr, func.name.c_str());
            }
            fprintf(reg, "}\n");
            fclose(reg);
        }
    }

    // Generate recompiled function files
    for (auto& [addr, func] : cfg.functions) {
        if (func_count % funcs_per_file == 0) {
            if (current_file) fclose(current_file);
            char filename[512];
            snprintf(filename, sizeof(filename), "%s/recomp_%04d.cpp",
                     output_dir.c_str(), file_index++);
            current_file = fopen(filename, "w");
            if (!current_file) {
                fprintf(stderr, "Failed to create %s\n", filename);
                return 1;
            }
            emit_file_header(current_file, project_name.c_str());
        }

        fprintf(current_file, "\n// ---- %s @ 0x%08X ----\n", func.name.c_str(), addr);
        fprintf(current_file, "void %s(PPCContext* ctx, Memory* mem) {\n", func.name.c_str());

        PPCToCEmitter emitter(current_file);
        emitter.block_addrs = func.block_addrs;

        for (uint32_t block_addr : func.block_addrs) {
            auto& block = func.blocks[block_addr];
            emitter.current_block = &block;
            fprintf(current_file, "label_%08X:\n", block_addr);

            for (const auto& insn : block.instructions) {
                emitter.emit_insn(insn);
            }
        }
        emitter.current_block = nullptr;

        fprintf(current_file, "}\n");
        func_count++;
    }

    if (current_file) fclose(current_file);

    printf("\n[*] Done! Generated %d files with %d functions.\n", file_index, func_count);

    return 0;
}
