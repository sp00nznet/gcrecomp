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
#include "gcrecomp/rel.h"
#include "gcrecomp/yaz0.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

using namespace gcrecomp;

static void print_usage(const char* prog) {
    printf("gcrecomp Static Recompiler (Gekko PowerPC -> C)\n");
    printf("================================================\n\n");
    printf("Usage: %s <input.dol> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --map <file>         Load Dolphin symbol map\n");
    printf("  --csv <file>         Load CSV symbol map (addr,name)\n");
    printf("  --ghidra <dir>       Load Ghidra headless JSON exports from <dir>\n");
    printf("                       (expects functions.json + symbols.json)\n");
    printf("  --output <dir>       Output directory (default: ./recompiled)\n");
    printf("  --extra-funcs <file> Force-add function entries (one hex addr/line)\n");
    printf("  --funcs-per-file <n> Functions per output file (default: 200)\n");
    printf("  --project <name>     Project name in file headers\n");
    printf("  --info               Print DOL info and exit\n");
    printf("  --stats              Print CFG statistics and exit\n");
    printf("  --trace              Emit TRACE_ENTER(addr) at every function entry\n");
    printf("                       (records calls in gcrecomp's trace ring)\n");
    printf("  --namespace <name>   Wrap recompiled code in a C++ namespace\n");
    printf("                       (avoids collisions with C stdlib main/memcpy/etc.)\n");
    printf("  --rel <path>         Recompile a REL module (instead of DOL).\n");
    printf("                       Input path is the .rel file; positional\n");
    printf("                       arg is the host DOL used to resolve\n");
    printf("                       module_id==0 references.\n");
    printf("  --rel-base <addr>    Load address for REL sections (default 0x82000000)\n");
    printf("  --rel-name <name>    Output basename (default: REL file stem)\n");
    printf("  --help               Show this help\n");
}

// Read whole file into a vector. Yaz0-aware: if file starts with "Yaz0",
// decompresses in-place and returns the decompressed buffer.
static bool slurp_maybe_yaz0(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", path.c_str());
        return false;
    }
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (sz >= 4 && buf[0] == 'Y' && buf[1] == 'a' && buf[2] == 'z' && buf[3] == '0') {
        uint32_t out_sz = (uint32_t(buf[4]) << 24) | (uint32_t(buf[5]) << 16) |
                          (uint32_t(buf[6]) << 8)  |  uint32_t(buf[7]);
        std::vector<uint8_t> dec(out_sz);
        if (!gcrecomp::yaz0_decompress(buf.data(), buf.size(),
                                        dec.data(), dec.size())) {
            fprintf(stderr, "Yaz0 decompress failed for %s\n", path.c_str());
            return false;
        }
        out = std::move(dec);
    } else {
        out = std::move(buf);
    }
    return true;
}

// Derive a clean basename from a REL path (no dir, no extension, no
// non-identifier chars).
static std::string sanitize_name(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string stem = (slash == std::string::npos) ? path
                                                    : path.substr(slash + 1);
    size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    for (auto& c : stem) {
        if (!(isalnum((unsigned char)c) || c == '_')) c = '_';
    }
    if (stem.empty()) stem = "rel";
    return stem;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string dol_path;
    std::string map_path;
    std::string csv_path;
    std::string ghidra_dir;
    std::string extra_funcs_path;
    std::string output_dir = "recompiled";
    std::string project_name = "gcrecomp";
    std::string rel_path;
    std::string rel_name;
    uint32_t    rel_base = 0x82000000u;
    int funcs_per_file = 200;
    bool info_only = false;
    bool stats_only = false;
    bool trace = false;
    std::string ns_name;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (strcmp(argv[i], "--ghidra") == 0 && i + 1 < argc) {
            ghidra_dir = argv[++i];
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
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--namespace") == 0 && i + 1 < argc) {
            ns_name = argv[++i];
        } else if (strcmp(argv[i], "--rel") == 0 && i + 1 < argc) {
            rel_path = argv[++i];
        } else if (strcmp(argv[i], "--rel-base") == 0 && i + 1 < argc) {
            rel_base = (uint32_t)strtoul(argv[++i], nullptr, 0);
        } else if (strcmp(argv[i], "--rel-name") == 0 && i + 1 < argc) {
            rel_name = argv[++i];
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

    DOLFile host_dol;
    printf("[*] Loading DOL: %s\n", dol_path.c_str());
    if (!host_dol.load(dol_path)) {
        fprintf(stderr, "Failed to load DOL file\n");
        return 1;
    }
    host_dol.print_info();

    if (info_only) return 0;

    // ---- REL mode: replace the working DOL with a REL-derived view ----
    // The recompilation pipeline below works on a DOLFile reference. When
    // --rel is set, we load + decompress the REL, apply relocations, and
    // build a DOLFile that contains only the REL's sections. The host DOL
    // is consulted for module_id==0 references during relocation.
    DOLFile rel_view_dol;
    DOLFile* dol_ptr = &host_dol;  // default
    RELFile  rel;
    bool     rel_mode = !rel_path.empty();
    if (rel_mode) {
        printf("[*] Loading REL: %s (base=0x%08X)\n",
               rel_path.c_str(), rel_base);
        std::vector<uint8_t> rel_bytes;
        if (!slurp_maybe_yaz0(rel_path, rel_bytes)) return 1;
        if (!rel.load_from_buffer(rel_bytes.data(), rel_bytes.size(),
                                   rel_path)) {
            fprintf(stderr, "REL parse failed\n");
            return 1;
        }
        rel.print_info();
        if (!rel_to_dol(rel, rel_base, &host_dol, rel_view_dol)) {
            fprintf(stderr, "REL → DOL conversion failed\n");
            return 1;
        }
        printf("[*] REL mapped: sections=%zu memory_base=0x%08X memory_end=0x%08X "
               "entry=0x%08X\n",
               rel_view_dol.sections.size(), rel_view_dol.memory_base,
               rel_view_dol.memory_end, rel_view_dol.entry_point);
        dol_ptr = &rel_view_dol;
        if (rel_name.empty()) rel_name = sanitize_name(rel_path);
    }
    DOLFile& dol = *dol_ptr;

    // ---- Load symbols ----
    SymbolMap syms;
    if (!map_path.empty()) syms.load_dolphin_map(map_path);
    if (!csv_path.empty()) syms.load_csv(csv_path);
    if (!ghidra_dir.empty()) {
        std::string fns_path = ghidra_dir + "/functions.json";
        std::string sym_path = ghidra_dir + "/symbols.json";
        syms.load_ghidra_json(fns_path, sym_path);
    }

    // ---- Build CFG ----
    printf("\n[*] Building control flow graph...\n");
    CFG cfg;
    cfg.scan_targets(dol);

    // Promote every symbol-map function entry to a CFG entry. Static
    // analysis misses tiny vtable stubs (`blr` only, called via bclrl)
    // because no `bl` ever references them and no data scan picks them up.
    {
        std::vector<uint32_t> sym_entries;
        for (const auto& [addr, sym] : syms.symbols) {
            if (sym.is_function && addr >= dol.sections.front().address) {
                sym_entries.push_back(addr);
            }
        }
        if (!sym_entries.empty()) {
            printf("[CFG] Adding %zu symbol-map function entries as CFG seeds.\n",
                   sym_entries.size());
            cfg.add_extra_entries(sym_entries);
        }
    }

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

    // Generate recomp_common.h — shared macros and includes for all recompiled code.
    // In REL mode we don't regenerate this; the DOL recompile already produced it.
    if (!rel_mode) {
        char path[512];
        snprintf(path, sizeof(path), "%s/recomp_common.h", output_dir.c_str());
        // Do not clobber an existing one. This header is what binds every
        // generated function to a particular runtime -- which g_mem and
        // g_func_table they touch -- so a project that supplies its own
        // must keep it. Overwriting it compiles cleanly and then reads and
        // writes an entirely different, uninitialised Memory at run time.
        if (FILE* existing = fopen(path, "r")) {
            fclose(existing);
            printf("[*] Keeping existing %s (project-supplied runtime bindings)\n",
                   path);
        } else {
        FILE* common = fopen(path, "w");
        if (common) {
            fprintf(common, "#pragma once\n");
            fprintf(common, "// Auto-generated by gcrecomp recompiler\n");
            fprintf(common, "#include \"gcrecomp/runtime.h\"\n");
            fprintf(common, "#include \"gcrecomp/ppc_helpers.h\"\n");
            fprintf(common, "#include <cstring>  // std::memcpy/std::memset used in recompiled code\n\n");
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
            if (trace) {
                fprintf(common, "// Function-entry trace (--trace was passed to gcrecomp_recompiler)\n");
                fprintf(common, "#define TRACE_ENTER(addr) gcrecomp::trace_enter(static_cast<uint32_t>(addr))\n\n");
            } else {
                fprintf(common, "// Function-entry trace disabled (rebuild with --trace to enable)\n");
                fprintf(common, "#define TRACE_ENTER(addr) ((void)0)\n\n");
            }
            fprintf(common, "// PSQ wrapper macros (delegate to gcrecomp runtime helpers with g_mem)\n");
            fprintf(common, "#define PSQ_LOAD_ONE(addr, gqr)              psq_load_one(g_mem, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_LOAD_PAIR(ps0, ps1, addr, gqr)   psq_load_pair(g_mem, ps0, ps1, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_STORE_ONE(val, addr, gqr)         psq_store_one(g_mem, val, static_cast<uint32_t>(addr), gqr)\n");
            fprintf(common, "#define PSQ_STORE_PAIR(v0, v1, addr, gqr)     psq_store_pair(g_mem, v0, v1, static_cast<uint32_t>(addr), gqr)\n");
            fclose(common);
        }
        }
    }

    // ---- Assign function names (used by both DOL and REL emit paths) ----
    for (auto& [addr, func] : cfg.functions) {
        func.name = syms.get_name(addr);
    }

    if (!rel_mode) {
        // ---- DOL: forward decl header + registration ----
        char path[512];
        snprintf(path, sizeof(path), "%s/recomp_funcs.h", output_dir.c_str());
        FILE* hdr = fopen(path, "w");
        if (hdr) {
            fprintf(hdr, "#pragma once\n");
            fprintf(hdr, "// Auto-generated: forward declarations for all recompiled functions\n");
            fprintf(hdr, "#include \"recomp_common.h\"\n\n");
            if (!ns_name.empty()) fprintf(hdr, "namespace %s {\n\n", ns_name.c_str());
            for (auto& [addr, func] : cfg.functions) {
                fprintf(hdr, "void %s(PPCContext* ctx, Memory* mem);\n", func.name.c_str());
            }
            if (!ns_name.empty()) fprintf(hdr, "\n} // namespace %s\n", ns_name.c_str());
            fprintf(hdr, "\n// Register all recompiled functions into the function table\n");
            fprintf(hdr, "void register_recompiled_functions(FuncTable& table);\n");
            fclose(hdr);
        }

        snprintf(path, sizeof(path), "%s/recomp_register.cpp", output_dir.c_str());
        FILE* reg = fopen(path, "w");
        if (reg) {
            fprintf(reg, "// Auto-generated: registers all recompiled functions\n");
            fprintf(reg, "#include \"recomp_funcs.h\"\n\n");
            fprintf(reg, "void register_recompiled_functions(FuncTable& table) {\n");
            std::string qual_prefix = ns_name.empty() ? "" : (ns_name + "::");
            for (auto& [addr, func] : cfg.functions) {
                fprintf(reg, "    table.register_func(0x%08X, %s%s);\n",
                        addr, qual_prefix.c_str(), func.name.c_str());
            }
            fprintf(reg, "}\n");
            fclose(reg);
        }

        // Generate recompiled function files (one per N functions).
        for (auto& [addr, func] : cfg.functions) {
            if (func_count % funcs_per_file == 0) {
                if (current_file) {
                    if (!ns_name.empty())
                        fprintf(current_file, "\n} // namespace %s\n", ns_name.c_str());
                    fclose(current_file);
                }
                char filename[512];
                snprintf(filename, sizeof(filename), "%s/recomp_%04d.cpp",
                         output_dir.c_str(), file_index++);
                current_file = fopen(filename, "w");
                if (!current_file) {
                    fprintf(stderr, "Failed to create %s\n", filename);
                    return 1;
                }
                emit_file_header(current_file, project_name.c_str());
                if (!ns_name.empty()) fprintf(current_file, "namespace %s {\n", ns_name.c_str());
            }

            fprintf(current_file, "\n// ---- %s @ 0x%08X ----\n",
                    func.name.c_str(), addr);
            fprintf(current_file, "void %s(PPCContext* ctx, Memory* mem) {\n",
                    func.name.c_str());
            if (trace) fprintf(current_file, "    TRACE_ENTER(0x%08Xu);\n", addr);

            PPCToCEmitter emitter(current_file);
            emitter.block_addrs = func.block_addrs;
            emitter.syms = &syms;
            emitter.func_map = &cfg.functions;
            for (uint32_t block_addr : func.block_addrs) {
                auto& block = func.blocks[block_addr];
                emitter.current_block = &block;
                fprintf(current_file, "label_%08X:\n", block_addr);
                for (const auto& insn : block.instructions) {
                    emitter.emit_insn(insn);
                }
            }
            fprintf(current_file, "}\n");
            func_count++;
        }
        if (current_file) {
            if (!ns_name.empty())
                fprintf(current_file, "\n} // namespace %s\n", ns_name.c_str());
            fclose(current_file);
        }

        printf("\n[*] Done! Generated %d files with %d functions.\n",
               file_index, func_count);
    } else {
        // ---- REL: single .cpp + register fn + entry-point header ----
        char path[512];

        snprintf(path, sizeof(path), "%s/rel_%s.cpp", output_dir.c_str(),
                 rel_name.c_str());
        FILE* cpp = fopen(path, "w");
        if (!cpp) {
            fprintf(stderr, "Failed to create %s\n", path);
            return 1;
        }
        emit_file_header(cpp, project_name.c_str());
        // Include the per-REL header for internal function forward decls.
        // The header also brings in recomp_common.h transitively.
        fprintf(cpp, "#include \"rel_%s.h\"\n\n", rel_name.c_str());
        fprintf(cpp, "// REL module: %s\n", rel_path.c_str());
        fprintf(cpp, "// module_id=%u version=%u base=0x%08X\n",
                rel.header.module_id, rel.header.version, rel_base);
        fprintf(cpp, "// prolog: section=%u offset=0x%X  epilog: section=%u "
                     "offset=0x%X  unresolved: section=%u offset=0x%X\n\n",
                rel.header.prolog_section, rel.header.prolog_offset,
                rel.header.epilog_section, rel.header.epilog_offset,
                rel.header.unresolved_section, rel.header.unresolved_offset);

        // Collect all call targets across REL functions. Any address NOT
        // already in cfg.functions is an external reference (most likely
        // a function in the host DOL or another REL). Emit forward
        // declarations for them so the REL cpp links standalone.
        std::set<uint32_t> external_calls;
        for (auto& [addr, func] : cfg.functions) {
            for (uint32_t call_addr : func.calls) {
                if (cfg.functions.find(call_addr) == cfg.functions.end()) {
                    external_calls.insert(call_addr);
                }
            }
        }
        if (!external_calls.empty()) {
            fprintf(cpp, "// Forward declarations for external functions "
                         "(host DOL / other RELs).\n");
            fprintf(cpp, "// These are resolved at link time by the host "
                         "DOL's recomp_funcs.h or the appropriate REL.\n");
            for (uint32_t a : external_calls) {
                fprintf(cpp, "extern void %s(PPCContext* ctx, "
                             "Memory* mem);\n", syms.get_name(a).c_str());
            }
            fprintf(cpp, "\n");
        }

        for (auto& [addr, func] : cfg.functions) {
            fprintf(cpp, "\n// ---- %s @ 0x%08X ----\n",
                    func.name.c_str(), addr);
            fprintf(cpp, "void %s(PPCContext* ctx, Memory* mem) {\n",
                    func.name.c_str());
            if (trace) fprintf(cpp, "    TRACE_ENTER(0x%08Xu);\n", addr);

            PPCToCEmitter emitter(cpp);
            emitter.block_addrs = func.block_addrs;
            emitter.syms = &syms;
            emitter.func_map = &cfg.functions;
            for (uint32_t block_addr : func.block_addrs) {
                auto& block = func.blocks[block_addr];
                emitter.current_block = &block;
                fprintf(cpp, "label_%08X:\n", block_addr);
                for (const auto& insn : block.instructions) {
                    emitter.emit_insn(insn);
                }
            }
            fprintf(cpp, "}\n");
            func_count++;
        }
        fclose(cpp);

        // Header with entry-point macros and the register fn forward decl.
        snprintf(path, sizeof(path), "%s/rel_%s.h", output_dir.c_str(),
                 rel_name.c_str());
        FILE* hdr = fopen(path, "w");
        if (hdr) {
            fprintf(hdr, "#pragma once\n");
            fprintf(hdr, "// Auto-generated header for REL module '%s'\n",
                    rel_name.c_str());
            fprintf(hdr, "#include \"recomp_common.h\"\n\n");
            uint32_t prolog_va = 0, epilog_va = 0, unresolved_va = 0;
            if (rel.header.prolog_section < rel.section_addresses.size()) {
                uint32_t base = rel.section_addresses[rel.header.prolog_section];
                if (base) prolog_va = base + rel.header.prolog_offset;
            }
            if (rel.header.epilog_section < rel.section_addresses.size()) {
                uint32_t base = rel.section_addresses[rel.header.epilog_section];
                if (base) epilog_va = base + rel.header.epilog_offset;
            }
            if (rel.header.unresolved_section < rel.section_addresses.size()) {
                uint32_t base = rel.section_addresses[rel.header.unresolved_section];
                if (base) unresolved_va = base + rel.header.unresolved_offset;
            }
            fprintf(hdr, "#define REL_%s_MODULE_ID      0x%08Xu\n",
                    rel_name.c_str(), rel.header.module_id);
            fprintf(hdr, "#define REL_%s_BASE           0x%08Xu\n",
                    rel_name.c_str(), rel_base);
            fprintf(hdr, "#define REL_%s_PROLOG_VA      0x%08Xu\n",
                    rel_name.c_str(), prolog_va);
            fprintf(hdr, "#define REL_%s_EPILOG_VA      0x%08Xu\n",
                    rel_name.c_str(), epilog_va);
            fprintf(hdr, "#define REL_%s_UNRESOLVED_VA  0x%08Xu\n\n",
                    rel_name.c_str(), unresolved_va);
            for (auto& [addr, func] : cfg.functions) {
                fprintf(hdr, "void %s(PPCContext* ctx, Memory* mem);\n",
                        func.name.c_str());
            }
            fprintf(hdr, "\nvoid rel_%s_register(FuncTable& table);\n",
                    rel_name.c_str());
            fclose(hdr);
        }

        // Stubs file: any external function the REL calls that's not in
        // the host DOL's recomp_funcs.h will be unresolved at link time.
        // Generate weak no-op stubs for all of them. The host can override
        // any by providing a real definition elsewhere; the linker will
        // pick that over the weak stub.
        // (Note: MSVC's /alternatename has different semantics from GNU
        // weak symbols — we use plain functions and rely on the host to
        // exclude this file when overriding. Simpler approach: only emit
        // stubs for symbols NOT in cfg's known set.)
        snprintf(path, sizeof(path), "%s/rel_%s_external_stubs.cpp",
                 output_dir.c_str(), rel_name.c_str());
        FILE* stubs = fopen(path, "w");
        if (stubs) {
            fprintf(stubs, "// Auto-generated external function stubs for "
                           "REL '%s'.\n", rel_name.c_str());
            fprintf(stubs, "// Each call target referenced by the REL but "
                           "not provided by the\n");
            fprintf(stubs, "// host DOL recompilation gets a no-op stub so "
                           "the linker resolves.\n");
            fprintf(stubs, "// To override a specific stub with real "
                           "behavior, exclude this file\n");
            fprintf(stubs, "// from the build and provide your own "
                           "implementations.\n\n");
            fprintf(stubs, "#include \"recomp_common.h\"\n");
            fprintf(stubs, "#include \"recomp_funcs.h\"\n\n");
            // For each external call, emit a weak stub guarded by a
            // preprocessor check against the DOL's forward decls. We can't
            // easily test that in C++; instead, the recompiler driver
            // could compare against a known symbol list. For simplicity,
            // only emit stubs for symbols outside the DOL's normal range
            // (0x80xxxxxx) — those are clearly missing and won't conflict.
            int stub_count = 0;
            for (uint32_t a : external_calls) {
                // Skip DOL range — those are handled by recomp_funcs.h.
                if (a >= 0x80000000u && a < 0x81000000u) continue;
                fprintf(stubs, "void %s(PPCContext* ctx, Memory* mem) "
                               "{ (void)ctx; (void)mem; }\n",
                        syms.get_name(a).c_str());
                stub_count++;
            }
            fclose(stubs);
            printf("[*] Emitted %d external stubs.\n", stub_count);
        }

        // Registration cpp.
        snprintf(path, sizeof(path), "%s/rel_%s_register.cpp",
                 output_dir.c_str(), rel_name.c_str());
        FILE* reg = fopen(path, "w");
        if (reg) {
            fprintf(reg, "// Auto-generated registration for REL '%s'\n",
                    rel_name.c_str());
            fprintf(reg, "#include \"rel_%s.h\"\n\n", rel_name.c_str());
            fprintf(reg, "void rel_%s_register(FuncTable& table) {\n",
                    rel_name.c_str());
            for (auto& [addr, func] : cfg.functions) {
                fprintf(reg, "    table.register_func(0x%08X, %s);\n",
                        addr, func.name.c_str());
            }
            fprintf(reg, "}\n");
            fclose(reg);
        }

        printf("\n[*] Done! Generated rel_%s.cpp / .h / _register.cpp "
               "with %d functions.\n",
               rel_name.c_str(), func_count);
    }

    return 0;
}
