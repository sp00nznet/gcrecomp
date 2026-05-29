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
#include "gcrecomp/dol.h"
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
    return load_from_buffer(buf.data(), buf.size(), path);
}

bool RELFile::load_from_buffer(const uint8_t* data, size_t size,
                                const std::string& name_hint) {
    if (size < 0x40 || !data) {
        fprintf(stderr, "[REL] buffer too small (%zu bytes) [%s]\n",
                size, name_hint.c_str());
        return false;
    }
    // Treat input as the buffer body for the parsing code below; the
    // original load(path) path expected a std::vector<uint8_t>. Adapt by
    // wrapping a span-like view via local variables.
    const std::vector<uint8_t> buf(data, data + size);
    const long fsize_l = (long)size;
    const std::string path = name_hint;  // for log messages

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
    // One stream per import. Each stream is terminated by R_DOLPHIN_END.
    reloc_streams.clear();
    reloc_streams.reserve(imports.size());
    if (header.rel_offset != 0) {
        for (const auto& imp : imports) {
            RELRelocStream stream;
            stream.module_id = imp.module_id;
            uint32_t cur = imp.offset;
            while (cur + 8 <= buf.size()) {
                const uint8_t* e = buf.data() + cur;
                RELRelocation r{};
                r.offset  = read_be16(e + 0);
                r.type    = (RELRelocType)e[2];
                r.section = e[3];
                r.addend  = read_be32(e + 4);
                stream.entries.push_back(r);
                cur += 8;
                if (r.type == RELRelocType::R_DOLPHIN_END) break;
            }
            reloc_streams.push_back(std::move(stream));
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
    size_t total_relocs = 0;
    for (const auto& s : reloc_streams) total_relocs += s.entries.size();
    printf("[REL]   imports=%zu  reloc_streams=%zu  total_relocs=%zu\n",
           imports.size(), reloc_streams.size(), total_relocs);
}

// ---- REL → DOL adapter ------------------------------------------------------

static inline uint32_t align_up(uint32_t v, uint32_t a) {
    if (a < 2) return v;
    return (v + (a - 1)) & ~(a - 1);
}

bool rel_to_dol(RELFile& rel, uint32_t base_addr,
                const DOLFile* host_dol,
                DOLFile& out)
{
    // ---- Pass 1: assign virtual addresses to each section ----
    const uint32_t section_align = rel.header.align ? rel.header.align : 4;
    const uint32_t bss_align     = rel.header.bss_align ? rel.header.bss_align : 4;

    rel.section_addresses.assign(rel.sections.size(), 0);
    uint32_t cursor = align_up(base_addr, section_align);

    // Place non-BSS sections first.
    for (size_t i = 0; i < rel.sections.size(); ++i) {
        auto& s = rel.sections[i];
        if (s.size == 0) continue;
        if (s.offset == 0) continue;  // BSS — defer
        cursor = align_up(cursor, section_align);
        rel.section_addresses[i] = cursor;
        cursor += s.size;
    }
    // Then BSS.
    for (size_t i = 0; i < rel.sections.size(); ++i) {
        auto& s = rel.sections[i];
        if (s.size == 0) continue;
        if (s.offset != 0) continue;  // already placed
        cursor = align_up(cursor, bss_align);
        rel.section_addresses[i] = cursor;
        cursor += s.size;
    }

    // ---- Pass 2: apply relocations ----
    // Each stream is stateful: cur_section starts at 0, cur_offset starts
    // at 0, advanced by entry.offset for every reloc. R_DOLPHIN_SECTION
    // resets cur_offset to 0 and changes cur_section to entry.section.
    auto patch_be32 = [&](size_t sec_idx, uint32_t off, uint32_t val) {
        if (sec_idx >= rel.sections.size()) return;
        auto& s = rel.sections[sec_idx];
        if (off + 4 > s.data.size()) return;
        s.data[off + 0] = (uint8_t)(val >> 24);
        s.data[off + 1] = (uint8_t)(val >> 16);
        s.data[off + 2] = (uint8_t)(val >> 8);
        s.data[off + 3] = (uint8_t)val;
    };
    auto read_be32_at = [&](size_t sec_idx, uint32_t off) -> uint32_t {
        if (sec_idx >= rel.sections.size()) return 0;
        auto& s = rel.sections[sec_idx];
        if (off + 4 > s.data.size()) return 0;
        return (uint32_t(s.data[off + 0]) << 24) |
               (uint32_t(s.data[off + 1]) << 16) |
               (uint32_t(s.data[off + 2]) << 8)  |
                uint32_t(s.data[off + 3]);
    };
    auto patch_be16 = [&](size_t sec_idx, uint32_t off, uint16_t val) {
        if (sec_idx >= rel.sections.size()) return;
        auto& s = rel.sections[sec_idx];
        if (off + 2 > s.data.size()) return;
        s.data[off + 0] = (uint8_t)(val >> 8);
        s.data[off + 1] = (uint8_t)val;
    };

    int internal_applied = 0;
    int external_applied = 0;
    int external_skipped = 0;

    for (const auto& stream : rel.reloc_streams) {
        bool is_internal = (stream.module_id == rel.header.module_id);
        bool is_main_dol = (stream.module_id == 0);

        uint32_t cur_section_idx = 0;
        uint32_t cur_offset      = 0;

        for (const auto& r : stream.entries) {
            cur_offset += r.offset;

            if (r.type == RELRelocType::R_DOLPHIN_NOP) continue;
            if (r.type == RELRelocType::R_DOLPHIN_SECTION) {
                cur_section_idx = r.section;
                cur_offset      = 0;
                continue;
            }
            if (r.type == RELRelocType::R_DOLPHIN_END) break;

            // Compute target address (S + A).
            uint32_t S = 0;
            bool resolved = false;
            if (is_internal) {
                if (r.section < rel.section_addresses.size()) {
                    S = rel.section_addresses[r.section];
                    resolved = (S != 0 || rel.sections[r.section].size != 0);
                }
            } else if (is_main_dol && host_dol) {
                // Main DOL references: addend is the absolute address.
                // (The "section" field is a section index in the DOL.)
                // The simplest correct interpretation is "addend already
                // is the full address" since the main DOL has fixed
                // load addresses.
                S = 0;
                resolved = true;
            } else {
                external_skipped++;
                continue;
            }
            uint32_t T = S + r.addend;

            // Apply patch by reloc type.
            switch (r.type) {
                case RELRelocType::R_PPC_ADDR32: {
                    patch_be32(cur_section_idx, cur_offset, T);
                    break;
                }
                case RELRelocType::R_PPC_ADDR24: {
                    // Branch target replacing low 26 bits, masked to 24
                    // (LK and AA bits preserved).
                    uint32_t insn = read_be32_at(cur_section_idx, cur_offset);
                    uint32_t hi   = insn & 0xFC000003u;
                    uint32_t off  = T & 0x03FFFFFCu;
                    patch_be32(cur_section_idx, cur_offset, hi | off);
                    break;
                }
                case RELRelocType::R_PPC_ADDR16:
                case RELRelocType::R_PPC_ADDR16_LO: {
                    patch_be16(cur_section_idx, cur_offset, (uint16_t)T);
                    break;
                }
                case RELRelocType::R_PPC_ADDR16_HI: {
                    patch_be16(cur_section_idx, cur_offset, (uint16_t)(T >> 16));
                    break;
                }
                case RELRelocType::R_PPC_ADDR16_HA: {
                    uint32_t hi = T >> 16;
                    if (T & 0x8000) hi += 1;
                    patch_be16(cur_section_idx, cur_offset, (uint16_t)hi);
                    break;
                }
                case RELRelocType::R_PPC_ADDR14: {
                    uint32_t insn = read_be32_at(cur_section_idx, cur_offset);
                    uint32_t hi   = insn & 0xFFFF0003u;
                    uint32_t off  = T & 0x0000FFFCu;
                    patch_be32(cur_section_idx, cur_offset, hi | off);
                    break;
                }
                case RELRelocType::R_PPC_REL24: {
                    uint32_t patch_va = rel.section_addresses[cur_section_idx]
                                      + cur_offset;
                    int32_t  delta    = (int32_t)(T - patch_va);
                    uint32_t insn     = read_be32_at(cur_section_idx, cur_offset);
                    uint32_t hi       = insn & 0xFC000003u;
                    uint32_t off      = ((uint32_t)delta) & 0x03FFFFFCu;
                    patch_be32(cur_section_idx, cur_offset, hi | off);
                    break;
                }
                case RELRelocType::R_PPC_REL14: {
                    uint32_t patch_va = rel.section_addresses[cur_section_idx]
                                      + cur_offset;
                    int32_t  delta    = (int32_t)(T - patch_va);
                    uint32_t insn     = read_be32_at(cur_section_idx, cur_offset);
                    uint32_t hi       = insn & 0xFFFF0003u;
                    uint32_t off      = ((uint32_t)delta) & 0x0000FFFCu;
                    patch_be32(cur_section_idx, cur_offset, hi | off);
                    break;
                }
                default:
                    // Unknown — log and skip.
                    fprintf(stderr, "[REL] unknown reloc type %u at "
                            "sec %u offset 0x%X\n",
                            (unsigned)r.type, cur_section_idx, cur_offset);
                    break;
            }

            if (resolved) {
                if (is_internal) internal_applied++;
                else             external_applied++;
            }
        }
    }

    fprintf(stderr, "[REL] reloc summary: %d internal, %d external resolved, "
                    "%d external skipped\n",
            internal_applied, external_applied, external_skipped);

    // ---- Pass 3: build DOLFile view ----
    out = DOLFile{};
    out.bss_address = 0;
    out.bss_size    = 0;
    out.entry_point = 0;
    if (rel.header.prolog_section < rel.sections.size()) {
        uint32_t base = rel.section_addresses[rel.header.prolog_section];
        if (base != 0) {
            out.entry_point = base + rel.header.prolog_offset;
        }
    }

    uint32_t mem_lo = 0xFFFFFFFFu;
    uint32_t mem_hi = 0u;

    for (size_t i = 0; i < rel.sections.size(); ++i) {
        const auto& rs = rel.sections[i];
        if (rs.size == 0) continue;
        DOLSection ds;
        ds.file_offset = rs.offset;
        ds.address     = rel.section_addresses[i];
        ds.size        = rs.size;
        ds.is_text     = rs.executable;
        ds.index       = (int)i;
        ds.data        = rs.data;  // already patched in-place above
        if (ds.address < mem_lo) mem_lo = ds.address;
        if (ds.address + ds.size > mem_hi) mem_hi = ds.address + ds.size;
        out.sections.push_back(std::move(ds));
    }

    if (out.sections.empty()) {
        fprintf(stderr, "[REL] no sections to emit\n");
        return false;
    }

    out.memory_base = mem_lo;
    out.memory_end  = mem_hi;
    out.memory.assign(mem_hi - mem_lo, 0);
    for (const auto& ds : out.sections) {
        uint32_t off = ds.address - mem_lo;
        if (off + ds.size > out.memory.size()) continue;
        memcpy(out.memory.data() + off, ds.data.data(), ds.size);
    }
    return true;
}

} // namespace gcrecomp
