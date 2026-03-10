#pragma once
// =============================================================================
// gcrecomp — REL (Relocatable Module) Parser
// =============================================================================
//
// REL files are the GameCube's equivalent of DLLs — dynamically loaded code
// modules. Many GameCube games use them extensively:
//   - Wind Waker: actors, scenes, event scripts
//   - Melee: stage-specific code
//   - Pikmin 2: enemy AI modules
//
// REL modules contain relocatable code sections, an import table (referencing
// the main DOL and other RELs), and a relocation table for patching addresses
// at load time.
//
// For static recompilation, REL modules need to be:
//   1. Parsed to discover code sections
//   2. Relocated against the DOL's address space
//   3. Recompiled just like DOL functions
//
// Reference: decomp-toolkit (https://github.com/encounter/decomp-toolkit)
//            Dolphin emulator (for the R_DOLPHIN_* relocation extensions)
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

namespace gcrecomp {

struct RELHeader {
    uint32_t module_id;         // Unique module identifier
    uint32_t next;              // Runtime: next module pointer (linked list)
    uint32_t prev;              // Runtime: prev module pointer
    uint32_t num_sections;
    uint32_t section_offset;    // File offset to section table
    uint32_t name_offset;       // File offset to module name (often 0)
    uint32_t name_size;
    uint32_t version;           // REL format version (1, 2, or 3)
    uint32_t bss_size;
    uint32_t rel_offset;        // File offset to relocation table
    uint32_t imp_offset;        // File offset to import table
    uint32_t imp_size;
    uint8_t  prolog_section;    // Section containing _prolog (constructor)
    uint8_t  epilog_section;    // Section containing _epilog (destructor)
    uint8_t  unresolved_section;
    uint8_t  bss_section;       // BSS section index (v2+)
    uint32_t prolog_offset;
    uint32_t epilog_offset;
    uint32_t unresolved_offset;
    // v2+ fields:
    uint32_t align;
    uint32_t bss_align;
    // v3+ fields:
    uint32_t fix_size;          // Size of fixed-position data
};

struct RELSection {
    uint32_t offset;            // File offset (bit 0 = executable flag)
    uint32_t size;
    bool     executable;
    std::vector<uint8_t> data;
};

// PowerPC ELF relocation types + Dolphin extensions
enum class RELRelocType : uint8_t {
    R_PPC_NONE       = 0,
    R_PPC_ADDR32     = 1,   // Absolute 32-bit address
    R_PPC_ADDR24     = 2,   // Branch target (24-bit, shifted)
    R_PPC_ADDR16     = 3,
    R_PPC_ADDR16_LO  = 4,   // Low 16 bits
    R_PPC_ADDR16_HI  = 5,   // High 16 bits
    R_PPC_ADDR16_HA  = 6,   // High 16 bits, adjusted for sign extension
    R_PPC_ADDR14     = 7,
    R_PPC_REL24      = 10,  // Relative branch (24-bit)
    R_PPC_REL14      = 11,
    R_DOLPHIN_NOP    = 201, // Skip N bytes (advance write pointer)
    R_DOLPHIN_SECTION= 202, // Switch target section
    R_DOLPHIN_END    = 203, // End of relocation list
};

struct RELRelocation {
    uint16_t     offset;     // Offset from previous relocation
    RELRelocType type;
    uint8_t      section;    // Target section index
    uint32_t     addend;     // Symbol offset / addend
};

struct RELImport {
    uint32_t module_id;      // 0 = main DOL, otherwise REL module ID
    uint32_t offset;         // File offset to relocations for this import
};

struct RELFile {
    RELHeader header;
    std::vector<RELSection>    sections;
    std::vector<RELImport>     imports;
    std::vector<RELRelocation> relocations;
    std::string                name;

    bool load(const std::string& path);
    void print_info() const;
};

} // namespace gcrecomp
