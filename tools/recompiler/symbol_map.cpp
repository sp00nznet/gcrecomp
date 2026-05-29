// =============================================================================
// Symbol Map — Load function names from external symbol files
// =============================================================================

#include "gcrecomp/symbol_map.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace gcrecomp {

// -----------------------------------------------------------------------------
// Name sanitization
// -----------------------------------------------------------------------------
// Ghidra names freely contain '::', '<T>', '~', spaces, and operator overload
// punctuation. We need bare identifiers that survive C++ symbol naming AND
// match across forward decl / def / register table call sites.
//
// Strategy: replace any non-[A-Za-z0-9_] run with a single '_', collapse,
// strip leading/trailing underscores, prepend '_' if the result starts with
// a digit, and prepend 'fn_' if the sanitized form would be empty.
std::string sanitize_c_identifier(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    bool last_underscore = false;
    for (char c : raw) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (ok) {
            out.push_back(c);
            last_underscore = false;
        } else if (!last_underscore && !out.empty()) {
            out.push_back('_');
            last_underscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) return "fn_anon";
    if (out[0] >= '0' && out[0] <= '9') out.insert(out.begin(), '_');
    return out;
}

// -----------------------------------------------------------------------------
// Minimal JSON value extractor for our Ghidra script outputs.
// The script writes one JSON object with an "addr" and "name" field per entry,
// pretty-printed with indent=1 so each field is on its own line. We don't
// need a full JSON parser; a streaming scan that pairs the most recent "addr"
// with the next "name" inside the same object is enough — and avoids dragging
// in a third-party JSON library.
// -----------------------------------------------------------------------------
namespace {

bool extract_quoted(const std::string& s, size_t pos, std::string& out) {
    size_t a = s.find('"', pos);
    if (a == std::string::npos) return false;
    size_t b = a + 1;
    std::string tmp;
    while (b < s.size()) {
        char c = s[b];
        if (c == '\\' && b + 1 < s.size()) {
            char n = s[b + 1];
            if (n == 'n') tmp.push_back('\n');
            else if (n == 't') tmp.push_back('\t');
            else if (n == '"') tmp.push_back('"');
            else if (n == '\\') tmp.push_back('\\');
            else tmp.push_back(n);
            b += 2;
            continue;
        }
        if (c == '"') {
            out = tmp;
            return true;
        }
        tmp.push_back(c);
        b += 1;
    }
    return false;
}

// Read whole file into a string. Returns empty on failure.
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Walk the JSON text and emit each (addr, name) pair where addr is a string
// formatted "0x" + hex digits. Pairs are tracked at the object level: when we
// hit a '{' we start a fresh pending addr/name, when we hit '}' we commit if
// both are present.
template <typename Emit>
void walk_addr_name_pairs(const std::string& text, Emit&& emit) {
    int depth = 0;
    std::vector<std::pair<uint32_t, std::string>> stack;
    std::vector<bool> have_addr_stack, have_name_stack;
    stack.push_back({0u, {}});
    have_addr_stack.push_back(false);
    have_name_stack.push_back(false);

    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '{') {
            depth++;
            stack.push_back({0u, {}});
            have_addr_stack.push_back(false);
            have_name_stack.push_back(false);
            i++;
            continue;
        }
        if (c == '}') {
            if (have_addr_stack.back() && have_name_stack.back()) {
                emit(stack.back().first, stack.back().second);
            }
            stack.pop_back();
            have_addr_stack.pop_back();
            have_name_stack.pop_back();
            depth--;
            i++;
            continue;
        }
        if (c == '"') {
            std::string key;
            if (!extract_quoted(text, i, key)) break;
            i = text.find('"', i) + 1;
            i = text.find('"', i) + 1;  // skip past closing quote of key
            // After key, expect ':' then value.
            size_t colon = text.find(':', i);
            if (colon == std::string::npos) break;
            i = colon + 1;
            // Skip whitespace.
            while (i < text.size() && (text[i] == ' ' || text[i] == '\n' ||
                                       text[i] == '\r' || text[i] == '\t')) i++;
            if (i >= text.size()) break;
            if (key == "addr") {
                std::string val;
                if (extract_quoted(text, i, val)) {
                    uint32_t a = (uint32_t)strtoul(val.c_str(), nullptr, 16);
                    stack.back().first = a;
                    have_addr_stack.back() = true;
                    i = text.find('"', i) + 1;
                    i = text.find('"', i) + 1;
                }
            } else if (key == "name") {
                std::string val;
                if (extract_quoted(text, i, val)) {
                    stack.back().second = val;
                    have_name_stack.back() = true;
                    i = text.find('"', i) + 1;
                    i = text.find('"', i) + 1;
                }
            }
            continue;
        }
        i++;
    }
}

bool is_ghidra_default_name(const std::string& name) {
    // Ghidra emits these as placeholders we don't want clogging the map.
    static const char* prefixes[] = {"FUN_", "SUB_", "LAB_", "DAT_",
                                     "switchD_", "caseD_"};
    for (const char* p : prefixes) {
        if (name.rfind(p, 0) == 0) return true;
    }
    return false;
}

} // namespace

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

bool SymbolMap::load_ghidra_json(const std::string& functions_json,
                                 const std::string& symbols_json) {
    auto ingest = [&](const std::string& path, bool as_function) {
        if (path.empty()) return 0;
        std::string text = slurp(path);
        if (text.empty()) {
            fprintf(stderr, "[SymbolMap] Ghidra JSON missing or empty: %s\n",
                    path.c_str());
            return 0;
        }
        int n = 0;
        walk_addr_name_pairs(text, [&](uint32_t addr, const std::string& name) {
            if (addr == 0 || name.empty()) return;
            if (is_ghidra_default_name(name)) return;
            Symbol sym;
            sym.address = addr;
            sym.size = 0;
            sym.name = sanitize_c_identifier(name);
            sym.is_function = as_function;
            // Functions win over symbols if both files name the same address.
            auto it = symbols.find(addr);
            if (it != symbols.end() && it->second.is_function && !as_function) {
                return;
            }
            symbols[addr] = std::move(sym);
            n++;
        });
        printf("[SymbolMap] Loaded %d Ghidra %s entries from %s\n",
               n, as_function ? "function" : "symbol", path.c_str());
        return n;
    };
    int total = 0;
    total += ingest(functions_json, /*as_function=*/true);
    total += ingest(symbols_json,   /*as_function=*/false);
    return total > 0;
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
