// =============================================================================
// gcrecomp - REL (Relocatable Module) Parser
// =============================================================================
//
// REL files are GameCube's DLL equivalent — dynamically loaded code modules.
// WW uses them for actors, scene-specific code, event scripts; without REL
// support those modules can't be loaded and the canonical create-request
// pipeline's phase_Load (fpcLd_Load → cDyl_LinkASync) returns ERROR.
//
// Format reference:
//   - WiiBrew: https://wiibrew.org/wiki/REL
//   - decomp-toolkit (encounter/decomp-toolkit)
//   - YAGCD section on .rel
// =============================================================================

#include "gcrecomp/rel.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace gcrecomp {

static inline uint16_t read_be16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}
static inline uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

bool RELFile::load(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        fprintf(stderr, "[REL] open failed: %s\n", path.c_str());
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long fsize_l = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize_l < 0x40) {
        fprintf(stderr, "[REL] %s too small (%ld bytes)\n",
                path.c_str(), fsize_l);
        fclose(fp);
        return false;
    }
    std::vector<uint8_t> buf((size_t)fsize_l);
    if (fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
        fclose(fp);
        fprintf(stderr, "[REL] read failed: %s\n", path.c_str());
        return false;
    }
    fclose(fp);

    // ---- Parse header ----
    const uint8_t* h = buf.data();
    header.module_id          = read_be32(h + 0x00);
    header.next               = read_be32(h + 0x04);
    header.prev               = read_be32(h + 0x08);
    header.num_sections       = read_be32(h + 0x0C);
    header.section_offset     = read_be32(h + 0x10);
    header.name_offset        = read_be32(h + 0x14);
    header.name_size          = read_be32(h + 0x18);
    header.version            = read_be32(h + 0x1C);
    header.bss_size           = read_be32(h + 0x20);
    header.rel_offset         = read_be32(h + 0x24);
    header.imp_offset         = read_be32(h + 0x28);
    header.imp_size           = read_be32(h + 0x2C);
    header.prolog_section     = h[0x30];
    header.epilog_section     = h[0x31];
    header.unresolved_section = h[0x32];
    header.bss_section        = h[0x33];
    header.prolog_offset      = read_be32(h + 0x34);
    header.epilog_offset      = read_be32(h + 0x38);
    header.unresolved_offset  = read_be32(h + 0x3C);
    if (header.version >= 2 && fsize_l >= 0x48) {
        header.align     = read_be32(h + 0x40);
        header.bss_align = read_be32(h + 0x44);
    } else {
        header.align = header.bss_align = 0;
    }
    if (header.version >= 3 && fsize_l >= 0x4C) {
        header.fix_size = read_be32(h + 0x48);
    } else {
        header.fix_size = 0;
    }

    if (header.version > 3) {
        fprintf(stderr, "[REL] unexpected version %u in %s\n",
                header.version, path.c_str());
        return false;
    }

    // ---- Module name (often absent in WW; some games embed it) ----
    if (header.name_offset != 0 && header.name_size > 0 &&
        header.name_offset + header.name_size <= buf.size())
    {
        name.assign(reinterpret_cast<const char*>(buf.data() + header.name_offset),
                    header.name_size);
        // Strip any null terminator inside the size.
        size_t nz = name.find('\0');
        if (nz != std::string::npos) name.resize(nz);
    }

    // ---- Section table ----
    if ((uint64_t)header.section_offset + (uint64_t)header.num_sections * 8u >
        buf.size())
    {
        fprintf(stderr, "[REL] section table out of bounds in %s\n", path.c_str());
        return false;
    }
    sections.clear();
    sections.reserve(header.num_sections);
    for (uint32_t i = 0; i < header.num_sections; ++i) {
        const uint8_t* e = buf.data() + header.section_offset + i * 8;
        RELSection s{};
        uint32_t raw_off = read_be32(e + 0);
        s.size       = read_be32(e + 4);
        s.executable = (raw_off & 1u) != 0;
        s.offset     = raw_off & ~1u;  // strip exec flag

        // Section i may be either BSS (offset==0 && size>0) or a real
        // section with file data.
        if (s.offset == 0 && s.size > 0) {
            // BSS-like — allocate but no file data.
            s.data.assign(s.size, 0);
        } else if (s.offset != 0 && s.size > 0) {
            if ((uint64_t)s.offset + (uint64_t)s.size > buf.size()) {
                fprintf(stderr, "[REL] section %u out of bounds in %s\n",
                        i, path.c_str());
                return false;
            }
            s.data.assign(buf.data() + s.offset,
                          buf.data() + s.offset + s.size);
        }
        sections.push_back(std::move(s));
    }

    // ---- Import table ----
    if (header.imp_size % 8u != 0 ||
        (uint64_t)header.imp_offset + (uint64_t)header.imp_size > buf.size())
    {
        fprintf(stderr, "[REL] imports out of bounds in %s\n", path.c_str());
        return false;
    }
    imports.clear();
    imports.reserve(header.imp_size / 8u);
    for (uint32_t off = 0; off + 8 <= header.imp_size; off += 8) {
        const uint8_t* e = buf.data() + header.imp_offset + off;
        RELImport imp{};
        imp.module_id = read_be32(e + 0);
        imp.offset    = read_be32(e + 4);
        imports.push_back(imp);
    }

    // ---- Relocations ----
    // The reloc stream is one big concatenation; imports[i].offset points
    // into it. Walk it once and store everything; consumers can group by
    // import if they need to.
    relocations.clear();
    // Conservative pre-reserve: imp_size / 8 entries on average have a
    // handful of relocs each. Don't over-think.
    if (header.rel_offset != 0) {
        // We don't know the total reloc table size ahead of time — walk
        // each import's section, terminated by R_DOLPHIN_END (203).
        for (const auto& imp : imports) {
            uint32_t cur = imp.offset;
            while (cur + 8 <= buf.size()) {
                const uint8_t* e = buf.data() + cur;
                RELRelocation r{};
                r.offset  = read_be16(e + 0);
                r.type    = (RELRelocType)e[2];
                r.section = e[3];
                r.addend  = read_be32(e + 4);
                relocations.push_back(r);
                cur += 8;
                if (r.type == RELRelocType::R_DOLPHIN_END) break;
            }
        }
    }
    return true;
}

void RELFile::print_info() const {
    printf("[REL] module_id=%u version=%u\n",
           header.module_id, header.version);
    if (!name.empty()) {
        printf("[REL]   name='%s'\n", name.c_str());
    }
    printf("[REL]   sections=%u bss_size=%u align=%u bss_align=%u fix_size=%u\n",
           header.num_sections, header.bss_size, header.align,
           header.bss_align, header.fix_size);
    printf("[REL]   prolog: section=%u offset=0x%X  "
           "epilog: section=%u offset=0x%X  "
           "unresolved: section=%u offset=0x%X\n",
           header.prolog_section, header.prolog_offset,
           header.epilog_section, header.epilog_offset,
           header.unresolved_section, header.unresolved_offset);
    for (size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        printf("[REL]   [%zu] offset=0x%X size=0x%X %s%s\n",
               i, s.offset, s.size,
               s.executable ? "exec " : "",
               (s.offset == 0 && s.size > 0) ? "(BSS)" : "");
    }
    printf("[REL]   imports=%zu  relocations=%zu\n",
           imports.size(), relocations.size());
}

} // namespace gcrecomp
