// =============================================================================
// Memory System Implementation
//
// Emulates the GameCube's 24 MB main RAM with big-endian byte-order access.
// The GameCube's Gekko CPU is big-endian, so all multi-byte reads and writes
// in this module store bytes in network (big-endian) order within the backing
// buffer, matching what the original PowerPC code expects.
//
// Virtual address translation:
//   0x80000000 - 0x817FFFFF  -> physical offset 0x00000000 - 0x017FFFFF (cached)
//   0xC0000000 - 0xC17FFFFF  -> physical offset 0x00000000 - 0x017FFFFF (uncached)
//
// Both ranges map to the same 24 MB host-side buffer. Out-of-range accesses
// log an error and return the buffer base to avoid segfaults during debugging.
//
// References:
//   - libogc memory map (ogc/system.h)
//   - Pureikyubu / Dolwin memory subsystem
// =============================================================================

#include "gcrecomp/runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gcrecomp {

Memory g_mem;

bool Memory::init() {
    ram = (uint8_t*)calloc(1, MAIN_RAM_SIZE);
    if (!ram) {
        fprintf(stderr, "[Memory] Failed to allocate %u MB\n", MAIN_RAM_SIZE / (1024 * 1024));
        return false;
    }
    printf("[Memory] Allocated %u MB main RAM\n", MAIN_RAM_SIZE / (1024 * 1024));
    return true;
}

void Memory::shutdown() {
    if (ram) {
        free(ram);
        ram = nullptr;
    }
}

uint8_t* Memory::translate(uint32_t addr) {
    // Cached region: 0x80000000 - 0x817FFFFF
    if (addr >= MAIN_RAM_BASE && addr < MAIN_RAM_END) {
        return ram + (addr - MAIN_RAM_BASE);
    }
    // Uncached region: 0xC0000000 - 0xC17FFFFF (same physical memory)
    if (addr >= UNCACHED_BASE && addr < UNCACHED_BASE + MAIN_RAM_SIZE) {
        return ram + (addr - UNCACHED_BASE);
    }
    // Physical address: 0x00000000 - 0x017FFFFF (some init code uses physical addrs)
    if (addr < MAIN_RAM_SIZE) {
        return ram + addr;
    }
    // Hardware registers: 0xCC000000 - 0xCC00FFFF
    if (addr >= HW_REG_BASE && addr < HW_REG_BASE + HW_REG_SIZE) {
        return hw_regs + (addr - HW_REG_BASE);
    }
    // Out of range - rate-limit warnings
    static int bad_count = 0;
    bad_count++;
    if (bad_count <= 10) {
        fprintf(stderr, "[Memory] Bad address: 0x%08X (occurrence #%d)\n", addr, bad_count);
    }
    return ram; // Don't crash, return base (will produce wrong results but won't segfault)
}

const uint8_t* Memory::translate(uint32_t addr) const {
    return const_cast<Memory*>(this)->translate(addr);
}

// Big-endian reads (GameCube byte order)
uint8_t Memory::read8(uint32_t addr) const {
    return *translate(addr);
}

uint16_t Memory::read16(uint32_t addr) const {
    const uint8_t* p = translate(addr);
    return ((uint16_t)p[0] << 8) | p[1];
}

uint32_t Memory::read32(uint32_t addr) const {
    const uint8_t* p = translate(addr);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

uint64_t Memory::read64(uint32_t addr) const {
    return ((uint64_t)read32(addr) << 32) | read32(addr + 4);
}

float Memory::readf32(uint32_t addr) const {
    uint32_t v = read32(addr);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

double Memory::readf64(uint32_t addr) const {
    uint64_t v = read64(addr);
    double d;
    memcpy(&d, &v, 8);
    return d;
}

// ---- Hardware Register HLE ----
// When the game writes to certain HW registers, we need to simulate
// the hardware's response. Otherwise the game busy-waits forever.

// EXI (External Interface) registers - 3 channels, 0x14 bytes each
// Channel 0: 0xCC006800, Channel 1: 0xCC006814, Channel 2: 0xCC006828
static constexpr uint32_t EXI_BASE = 0xCC006800;
static constexpr uint32_t EXI_CH_SIZE = 0x14;
// Offsets within each channel:
//   +0x00 = CSR (Channel Status)
//   +0x04 = MAR (DMA Memory Address)
//   +0x08 = LENGTH (DMA Transfer Length)
//   +0x0C = CR (Control Register) — bit 0 = TSTART
//   +0x10 = DATA (Immediate data)

static void hw_write32_hle(Memory* mem, uint32_t addr, uint32_t val) {
    // EXI DMA Control Register: when TSTART (bit 0) is set, immediately
    // complete the transfer by clearing it. On real hardware the EXI
    // controller does the DMA and clears TSTART when done.
    for (int ch = 0; ch < 3; ch++) {
        uint32_t cr_addr = EXI_BASE + ch * EXI_CH_SIZE + 0x0C;
        if (addr == cr_addr && (val & 1)) {
            val &= ~1u; // Clear TSTART — transfer "complete"
            break;
        }
    }

    // PI (Processor Interface) Interrupt Cause: 0xCC003000
    // Writing 1 bits clears them (write-to-clear register)
    if (addr == 0xCC003000) {
        uint32_t old = mem->read32(addr);
        val = old & ~val; // Clear the bits that were written as 1
    }

    // Store the value
    uint8_t* p = mem->hw_regs + (addr - Memory::HW_REG_BASE);
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)val;
}

// Big-endian writes
void Memory::write8(uint32_t addr, uint8_t val) {
    *translate(addr) = val;
}

void Memory::write16(uint32_t addr, uint16_t val) {
    uint8_t* p = translate(addr);
    p[0] = (uint8_t)(val >> 8);
    p[1] = (uint8_t)val;
}

void Memory::write32(uint32_t addr, uint32_t val) {
    // Intercept HW register writes for HLE
    if (addr >= HW_REG_BASE && addr < HW_REG_BASE + HW_REG_SIZE) {
        hw_write32_hle(this, addr, val);
        return;
    }
    uint8_t* p = translate(addr);
    p[0] = (uint8_t)(val >> 24);
    p[1] = (uint8_t)(val >> 16);
    p[2] = (uint8_t)(val >> 8);
    p[3] = (uint8_t)val;
}

void Memory::write64(uint32_t addr, uint64_t val) {
    write32(addr, (uint32_t)(val >> 32));
    write32(addr + 4, (uint32_t)val);
}

void Memory::writef32(uint32_t addr, float val) {
    uint32_t v;
    memcpy(&v, &val, 4);
    write32(addr, v);
}

void Memory::writef64(uint32_t addr, double val) {
    uint64_t v;
    memcpy(&v, &val, 8);
    write64(addr, v);
}

} // namespace gcrecomp
