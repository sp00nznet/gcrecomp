#pragma once
// SPDX-License-Identifier: MIT
//
// High-level emulation of the AX audio microcode, the SDK's voice mixer that runs on the GameCube's
// DSP. The CPU sends two mails per 5 ms frame, 0xBABE0000|size and the address of a command list in
// main memory; the microcode walks the list (voice parameter blocks, aux buses, output) and answers
// with DSP_YIELD. This class reproduces that on main memory and ARAM; it knows nothing of the
// machine beyond those two arrays, so it is unit-testable.
//
// Ported from the author's own Rust implementation (fepor-recomp, crates/runtime/src/ax.rs), whose
// structure and arithmetic follow the DSP HLE of Dolphin (GPL-2.0-or-later) so that results match it
// sample for sample. The polyphase resampler's coefficient table is a DSP ROM dump that cannot be
// shipped: its three sets are regenerated from a windowed-sinc fit.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace gcrecomp::audio {

/// Guest memory the microcode works on. `ram` is main memory (addresses are physical, wrapped to
/// 26 bits), `aram` the DSP's memory (size a power of two).
struct AxMemory {
    uint8_t* ram = nullptr;
    size_t ram_size = 0;
    const uint8_t* aram = nullptr;
    size_t aram_size = 0;
};

class AxUcode {
public:
    static constexpr uint32_t kMailCmdList = 0xBABE0000;
    static constexpr uint32_t kDspResume = 0xDCD10001;
    static constexpr uint32_t kDspYield = 0xDCD10002;

    /// Samples per frame and bus: 5 ms at 32 kHz.
    static constexpr size_t kFrame = 5 * 32;

    AxUcode();

    /// Handles one mail from the CPU; returns the mail the microcode answers with, if any.
    std::optional<uint32_t> handle_mail(uint32_t mail, AxMemory& mem);

    /// Command lists rendered so far.
    uint64_t frames() const { return frames_; }
    /// Commands met that the model does not know (the list is abandoned at the first one).
    uint64_t unknown_commands() const { return unknown_cmds_; }
    /// The command list rendered last (words, as the CPU wrote them), for traces.
    const uint16_t* last_cmdlist() const { return cmdlist_; }
    size_t last_cmdlist_size() const { return cmd_size_; }
    /// The CPU asked the microcode to hand the DSP back to the ROM.
    bool reset_requested() const { return reset_requested_; }

private:
    enum class MailState { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };
    static constexpr size_t kPbWords = 122;
    using Pb = uint16_t[kPbWords];

    void copy_cmdlist(uint32_t addr, size_t size, const AxMemory& mem);
    void run_cmdlist(AxMemory& mem);
    void setup(uint32_t init_addr, const AxMemory& mem);
    void download_and_mix(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb, const AxMemory& mem);
    void process_pb_list(uint32_t pb_addr, AxMemory& mem);
    void process_voice(Pb pb, size_t ms, const AxMemory& mem);
    void mix_aux(size_t aux_id, uint32_t write_addr, uint32_t read_addr, AxMemory& mem);
    void upload_lrs(uint32_t dst, AxMemory& mem);
    void set_main_lr(uint32_t src, const AxMemory& mem);
    void set_opposite_lr(uint32_t src, const AxMemory& mem);
    void compressor(uint16_t threshold, uint16_t release_frames, uint32_t table, const AxMemory& mem);
    void output(uint32_t lr, uint32_t surround, AxMemory& mem);
    void mix_auxb_lr(uint32_t ul, uint32_t dl, AxMemory& mem);
    void send_aux_and_mix(const uint32_t a[6], AxMemory& mem);

    MailState state_ = MailState::WaitingForCmdListSize;
    size_t cmd_size_ = 0;
    uint16_t cmdlist_[512] = {};
    /// main L/R/S, auxA L/R/S, auxB L/R/S, one frame each
    int32_t bufs_[9][kFrame] = {};
    uint16_t compressor_pos_ = 0;
    /// Mails left to swallow while the CPU uploads a new microcode.
    uint32_t upload_mails_ = 0;
    uint64_t frames_ = 0;
    uint64_t unknown_cmds_ = 0;
    bool reset_requested_ = false;
};

}  // namespace gcrecomp::audio
