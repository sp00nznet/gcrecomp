// =============================================================================
// Memory System Implementation
//
// Emulates Gekko-based system RAM with big-endian byte-order access.
// The Gekko CPU is big-endian, so all multi-byte reads and writes store
// bytes in network (big-endian) order within the backing buffer.
//
// Virtual address translation:
//   0x80000000 - 0x80xxxxxx  -> physical offset (cached)
//   0xC0000000 - 0xC0xxxxxx  -> physical offset (uncached, same memory)
//
// The RAM size is configurable at init() time:
//   - GameCube:  24 MB (default)
//   - Triforce:  48 MB
//
// Out-of-range accesses log an error and return the buffer base to avoid
// segfaults during debugging.
//
// References:
//   - libogc memory map (ogc/system.h)
//   - Pureikyubu / Dolwin memory subsystem
// =============================================================================

#include "gcrecomp/runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace gcrecomp {

Memory g_mem;

bool Memory::init(uint32_t size) {
    ram_size = size;
    ram_end = MAIN_RAM_BASE + ram_size;

    ram = (uint8_t*)calloc(1, ram_size);
    if (!ram) {
        fprintf(stderr, "[Memory] Failed to allocate %u MB\n", ram_size / (1024 * 1024));
        return false;
    }
    printf("[Memory] Allocated %u MB main RAM\n", ram_size / (1024 * 1024));
    return true;
}

void Memory::shutdown() {
    if (ram) {
        free(ram);
        ram = nullptr;
    }
}

uint8_t* Memory::translate(uint32_t addr) {
    // Cached region: 0x80000000 - 0x80000000+ram_size
    if (addr >= MAIN_RAM_BASE && addr < ram_end) {
        return ram + (addr - MAIN_RAM_BASE);
    }
    // Uncached region: 0xC0000000 - 0xC0000000+ram_size (same physical memory)
    if (addr >= UNCACHED_BASE && addr < UNCACHED_BASE + ram_size) {
        return ram + (addr - UNCACHED_BASE);
    }
    // Physical address: 0x00000000 - ram_size (some init code uses physical addrs)
    if (addr < ram_size) {
        return ram + addr;
    }
    // Hardware registers: 0xCC000000 - 0xCC00FFFF
    if (addr >= HW_REG_BASE && addr < HW_REG_BASE + HW_REG_SIZE) {
        return hw_regs + (addr - HW_REG_BASE);
    }
    // Out of range - rate-limit warnings
    static uint32_t bad_count = 0;
    if (bad_count < 10) {
        bad_count++;
        fprintf(stderr, "[Memory] Bad address: 0x%08X (occurrence #%u)\n", addr, bad_count);
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
    // HW register HLE for reads
    if (addr >= HW_REG_BASE && addr < HW_REG_BASE + HW_REG_SIZE) {
        static std::unordered_map<uint32_t, int> hw_read_count;
        int& count = hw_read_count[addr];
        if (count < 3) {
            const uint8_t* p = hw_regs + (addr - HW_REG_BASE);
            uint32_t val = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                           ((uint32_t)p[2] << 8) | p[3];
            printf("[HW] Read32 0x%08X = 0x%08X\n", addr, val);
            fflush(stdout);
        }
        count++;
        if (count == 100) {
            printf("[HW] Read32 0x%08X spinning (100 reads)!\n", addr);
            fflush(stdout);
        }

        // VI: Simulate advancing half-line counter so VIWaitForRetrace works.
        // Real VI generates interrupts at vertical blank (line 263/525 for NTSC).
        // 0xCC002000+0x24 = 0xCC002024 = VI_VCOUNT (vertical beam position)
        // 0xCC002000+0x2C = 0xCC00202C = VI_DISPLAY_INT0 (retrace match)
        if (addr == 0xCC002024 || addr == 0xCC00202C) {
            // Increment line counter to simulate video scan
            static uint16_t vi_line = 0;
            vi_line = (vi_line + 1) % 526;
            uint8_t* p = const_cast<uint8_t*>(hw_regs + (0xCC002024 - HW_REG_BASE));
            p[0] = 0; p[1] = 0; p[2] = (uint8_t)(vi_line >> 8); p[3] = (uint8_t)vi_line;
        }

        // SI: Report TCINT (transfer complete) for SI poll commands.
        // 0xCC006434 = SI Ch0 Status — bit 31 = TCINT (transfer complete)
        if (addr == 0xCC006434) {
            uint8_t* p = const_cast<uint8_t*>(hw_regs + (addr - HW_REG_BASE));
            p[0] |= 0x80;  // Set TCINT (transfer complete)
        }

        // DI (DVD Interface) registers
        // 0xCC006000 = DISR — DVD Status (bit 0 = TCINT, bit 2 = BRKINT, bit 4 = DEINT)
        // 0xCC006004 = DICVR — DVD Cover (bit 0 = CVRINT, bit 2 = CVR state)
        // 0xCC006024 = DICVR alias or DVD Cover Register
        if (addr == 0xCC006000 || addr == 0xCC006024) {
            uint8_t* p = const_cast<uint8_t*>(hw_regs + (addr - HW_REG_BASE));
            p[3] |= 0x01;  // TCINT = transfer complete
        }
        // DI Cover register — report disc inserted (CVR = 0 means closed)
        if (addr == 0xCC006004) {
            uint8_t* p = const_cast<uint8_t*>(hw_regs + (addr - HW_REG_BASE));
            p[3] &= ~0x04;  // CVR bit 2 = 0 means cover closed / disc present
        }
    }
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
    // Log HW register accesses (rate-limited)
    static std::unordered_map<uint32_t, int> hw_access_count;
    int& count = hw_access_count[addr];
    if (count < 3) {
        printf("[HW] Write32 0x%08X = 0x%08X\n", addr, val);
        fflush(stdout);
    }
    count++;
    if (count == 100 || count == 1000 || count == 10000) {
        printf("[HW] Write32 0x%08X hit %d times\n", addr, count);
        fflush(stdout);
    }

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
