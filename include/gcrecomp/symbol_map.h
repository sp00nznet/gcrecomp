#pragma once
// =============================================================================
// gcrecomp — Symbol Map
//
// Loads function names and addresses from external symbol files.
// Supports Dolphin emulator .map format and simple CSV format.
// =============================================================================

#include <cstdint>
#include <map>
#include <string>

namespace gcrecomp {

struct Symbol {
    uint32_t address;
    uint32_t size;
    std::string name;
    bool is_function;
};

struct SymbolMap {
    std::map<uint32_t, Symbol> symbols;

    // Load from Dolphin emulator .map format:
    //   addr size unk flags name
    bool load_dolphin_map(const std::string& path);

    // Load from CSV format:
    //   0xaddr,name
    bool load_csv(const std::string& path);

    // Load function and label names from Ghidra headless exports.
    // Either path may be empty to skip that file. Names are sanitized to
    // valid C identifiers (Ghidra namespaces and templates are flattened).
    // Default Ghidra names (FUN_xxxxxxxx etc.) are dropped — we'd rather
    // keep our own func_XXXXXXXX fallback than persist Ghidra's.
    bool load_ghidra_json(const std::string& functions_json,
                          const std::string& symbols_json);

    // Lookup
    const Symbol* find(uint32_t addr) const;

    // Get name for address, or generate "func_XXXXXXXX" default
    std::string get_name(uint32_t addr) const;

    size_t size() const { return symbols.size(); }
};

// Sanitize an arbitrary string into a valid C identifier.
// Handles common Ghidra patterns: ::, <T>, ~, operator overloads, etc.
// Exposed so the codegen can sanitize forward decls consistently.
std::string sanitize_c_identifier(const std::string& raw);

} // namespace gcrecomp
