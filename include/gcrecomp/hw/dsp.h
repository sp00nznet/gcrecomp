// SPDX-License-Identifier: MIT
//
// DSP interface (0xCC005000..0xCC00503F): mailboxes, control/status register, ARAM DMA registers
// and the AI DMA registers that share the block.
//
// There is no DSP core. The mask ROM's boot protocol is answered directly: after a reset the
// ROM announces itself (0x8071FEED), takes the boot task's command mails, and when told to start
// the uploaded microcode it reports DSP_INIT (0xDCD10000) with a DSP interrupt. From then on the
// microcode belongs to the game: its mails are handed to the AX microcode HLE (audio/ax.h), which
// renders the command lists the CPU sends into main memory and answers DSP_YIELD with an interrupt.
//
// The model follows the register layout used by the SDK (dolphin/dsp.h):
//   +0x00/+0x02  CPU->DSP mailbox, high/low (bit 15 of the high half reads as: mail pending)
//   +0x04/+0x06  DSP->CPU mailbox, high/low (bit 15 of the high half: mail valid; reading the
//                low half consumes the mail)
//   +0x0A        control/status (see Cr below)
//   +0x16        ARAM mode (bit 0 reads as set: the ARAM controller is ready)
//   +0x20..+0x2B ARAM DMA main address, ARAM address, control/count (starts on write)
//   +0x30..+0x3B AI DMA start address, length, bytes left (stored; the AI HLE mirrors into them)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gcrecomp/audio/ax.h"

namespace gcrecomp {

class DspInterface {
public:
    static constexpr uint32_t kBase = 0xCC005000;
    static constexpr uint32_t kSize = 0x40;
    static constexpr size_t kAramSize = 16 * 1024 * 1024;

    // Control/status register bits.
    enum Cr : uint16_t {
        CR_RESET = 1 << 0,
        CR_PIINT = 1 << 1,
        CR_HALT = 1 << 2,
        CR_AIDINT = 1 << 3,
        CR_AIDINTMSK = 1 << 4,
        CR_ARINT = 1 << 5,
        CR_ARINTMSK = 1 << 6,
        CR_DSPINT = 1 << 7,
        CR_DSPINTMSK = 1 << 8,
        CR_DSPDMA = 1 << 9,
        CR_INIT = 1 << 11,
    };
    static constexpr uint16_t kInterruptFlags = CR_AIDINT | CR_ARINT | CR_DSPINT;  // write 1 to clear

    static constexpr uint32_t kMailValid = 0x80000000;
    static constexpr uint32_t kRomReadyMail = 0x8071FEED;
    static constexpr uint32_t kDspInitMail = 0xDCD10000;

    DspInterface();

    // Back to power-on state (ARAM is cleared too). The trace setting is kept.
    void reset();

    // Register access at `offset` (0..kSize-1), `size` 1, 2 or 4 bytes. Mails are 16-bit
    // registers: 4-byte accesses are the high half then the low half.
    uint32_t read(uint32_t offset, uint32_t size);
    // `ram` / `ram_size` is main memory, for the ARAM DMA (addresses are physical, wrapped to 26 bits).
    void write(uint32_t offset, uint32_t value, uint32_t size, uint8_t* ram, size_t ram_size);

    uint16_t control() const { return control_; }
    // An unmasked ARAM or DSP interrupt flag is set (the AI flag is the AI HLE's business).
    bool dsp_interrupt_pending() const;
    bool aram_interrupt_pending() const;

    // Raises a flag as the hardware would: the DSP side (tests, microcode HLE).
    void raise(uint16_t cr_flag) { control_ |= (cr_flag & kInterruptFlags); }
    // The DSP side posts a mail; readable by the CPU until it reads the low half.
    void post_mail_to_cpu(uint32_t mail) { from_dsp_ = mail | kMailValid; }

    uint8_t* aram() { return aram_.data(); }
    size_t aram_size() const { return aram_.size(); }

    // The boot ROM is answering (false once the uploaded microcode runs).
    bool rom_running() const { return rom_running_; }
    // Boot task fields the ROM collected: command tag low half -> data mail.
    struct BootField { uint16_t tag; uint32_t data; };
    const std::vector<BootField>& boot_fields() const { return boot_fields_; }
    // The AX microcode model that answers the game's mails once the ROM is done.
    const audio::AxUcode& ax() const { return ax_; }

    // Mail and control traces on stderr. The global device reads DSP_TRACE (non-empty, not "0").
    void set_trace(bool on) { trace_ = on; }
    bool tracing() const { return trace_; }

private:
    uint32_t raw_read(uint32_t offset, uint32_t size) const;
    void raw_write(uint32_t offset, uint32_t value, uint32_t size);
    void write_control(uint16_t value);
    void mail_from_cpu(uint32_t mail, uint8_t* ram, size_t ram_size);
    void axucode_mail(uint32_t mail, uint8_t* ram, size_t ram_size);
    void aram_dma(uint8_t* ram, size_t ram_size);

    uint8_t regs_[kSize] = {};
    uint16_t control_ = 0;
    uint32_t to_dsp_ = 0;     // last mail the CPU wrote, as written
    bool to_pending_ = false; // the DSP has not taken it yet
    uint32_t from_dsp_ = 0;   // mail for the CPU (bit 31: valid)
    std::vector<uint8_t> aram_;
    bool rom_running_ = true;
    bool init_code_loaded_ = false;  // the SDK's init code was DMA'd in since the last reset
    uint16_t expect_data_for_ = 0;  // command tag whose data mail comes next (0: none)
    std::vector<BootField> boot_fields_;
    // ucode_crc() of the microcode the boot task fields point to (0 if they do not name a range of memory).
    uint32_t boot_ucode_crc(const uint8_t* ram, size_t ram_size) const;
    audio::AxUcode ax_;  // the game's microcode, once the boot task started it
    bool trace_ = false;
};

extern DspInterface g_dsp;

// OS interrupt numbers of the DSP block (dolphin/os.h).
constexpr int kOsIntrDspAi = 5;
constexpr int kOsIntrDspAram = 6;
constexpr int kOsIntrDspDsp = 7;

}  // namespace gcrecomp
