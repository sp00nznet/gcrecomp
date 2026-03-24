// =============================================================================
// Symbol Map — Load function names from external symbol files
// =============================================================================

#include "gcrecomp/symbol_map.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace gcrecomp {

bool SymbolMap::load_dolphin_map(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) {
        fprintf(stderr, "[SymbolMap] Failed to open: %s\n", path.c_str());
        return false;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Dolphin format: addr size unk flags name
        uint32_t addr, size, unk, flags;
        char name[512];
        if (sscanf(line, "%x %x %x %u %511s", &addr, &size, &unk, &flags, name) >= 5) {
            Symbol sym;
            sym.address = addr;
            sym.size = size;
            sym.name = name;
            sym.is_function = (flags & 1) != 0;
            symbols[addr] = std::move(sym);
        }
    }

    fclose(fp);
    printf("[SymbolMap] Loaded %zu symbols from %s\n", symbols.size(), path.c_str());
    return true;
}

bool SymbolMap::load_csv(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) {
        fprintf(stderr, "[SymbolMap] Failed to open: %s\n", path.c_str());
        return false;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // CSV format: addr,name or 0xaddr,name
        char* comma = strchr(line, ',');
        if (!comma) continue;

        *comma = '\0';
        uint32_t addr = (uint32_t)strtoul(line, nullptr, 16);
        char* name = comma + 1;
        // Trim trailing whitespace
        size_t len = strlen(name);
        while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r' || name[len-1] == ' '))
            name[--len] = '\0';

        if (addr != 0 && len > 0) {
            Symbol sym;
            sym.address = addr;
            sym.size = 0;
            sym.name = name;
            sym.is_function = true;
            symbols[addr] = std::move(sym);
        }
    }

    fclose(fp);
    printf("[SymbolMap] Loaded %zu symbols from %s\n", symbols.size(), path.c_str());
    return true;
}

const Symbol* SymbolMap::find(uint32_t addr) const {
    auto it = symbols.find(addr);
    return (it != symbols.end()) ? &it->second : nullptr;
}

std::string SymbolMap::get_name(uint32_t addr) const {
    auto it = symbols.find(addr);
    if (it != symbols.end()) return it->second.name;
    char buf[32];
    snprintf(buf, sizeof(buf), "func_%08X", addr);
    return buf;
}

} // namespace gcrecomp
