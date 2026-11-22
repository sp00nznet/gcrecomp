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

/// The identifier of a microcode image: Dolphin's HashEctor over the code bytes (the DSP HLE tells the AX versions apart
/// by it, see AxUcode::kCrcOldGc).
uint32_t ucode_crc(const uint8_t* code, size_t length);

class AxUcode {
public:
    /// The first GameCube AX microcode. It differs from the later ones in the meaning of the voice parameter block's
    /// mixer control word (bit 0 = send to aux A, bit 1 = aux B, bit 2 = surround, bit 3 = ramp, bit 4 = Dolby Pro Logic II
    /// mixing; the main L/R buses are always mixed) and in its parameter block, which has no low-pass filter section in
    /// main memory. Later ones have one bit per bus (bit 0 = main L, bit 1 = main R, ...).
    static constexpr uint32_t kCrcOldGc = 0x4E8A8B21;

    static constexpr uint32_t kMailCmdList = 0xBABE0000;
    static constexpr uint32_t kDspResume = 0xDCD10001;
    static constexpr uint32_t kDspYield = 0xDCD10002;

    /// Samples per frame and bus: 5 ms at 32 kHz.
    static constexpr size_t kFrame = 5 * 32;

    /// `crc` is ucode_crc() of the microcode the game uploaded; 0 (unknown) selects the later, one-bit-per-bus mixer control.
    explicit AxUcode(uint32_t crc = 0);
    uint32_t crc() const { return crc_; }

    /// Handles one mail from the CPU; returns the mail the microcode answers with, if any.
    std::optional<uint32_t> handle_mail(uint32_t mail, AxMemory& mem);

    /// Command lists rendered so far.
    uint64_t frames() const { return frames_; }
    /// Commands met that the model does not know (the list is abandoned at the first one).
    uint64_t unknown_commands() const { return unknown_cmds_; }
    /// The command list rendered last (words, as the CPU wrote them), for traces.
    const uint16_t* last_cmdlist() const { return cmdlist_; }
    size_t last_cmdlist_size() const { return cmd_size_; }
    /// What the lists contained, for judging what the game asks of the DSP (GCRECOMP_AX_TRACE prints it).
    struct Stats {
        uint64_t commands[32] = {};   ///< times each command id 0..31 was met
        uint64_t voice_ms = 0;        ///< 1 ms slices of running voices processed
        uint64_t voice_ms_loud = 0;   ///< of those, slices whose resampled output was not all zero
        int voice_peak = 0;           ///< largest |sample| a voice produced, after the voice envelope
        /// Sum of squares and sample count at points of the mix, to see which stage a signal dies at.
        enum Tap { kUploadL, kUploadR, kUploadS, kOppositeIn, kOutL, kOutR, kOutS, kTaps };
        double tap_energy[kTaps] = {};
        uint64_t tap_samples[kTaps] = {};
    };
    const Stats& stats() const { return stats_; }
    /// Zeroes the tap accumulators (for windowed measurements).
    void reset_taps() { for (int i = 0; i < Stats::kTaps; ++i) stats_.tap_energy[i] = 0, stats_.tap_samples[i] = 0; }
    /// Called with a parameter block (122 words, as the CPU sees it) the first time a running voice at
    /// that address is processed in a list; for traces. Not called when null.
    using VoiceObserver = void (*)(uint32_t pb_addr, const uint16_t* pb);
    void set_voice_observer(VoiceObserver f) { voice_observer_ = f; }
    /// The CPU asked the microcode to hand the DSP back to the ROM.
    bool reset_requested() const { return reset_requested_; }

private:
    enum class MailState { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };
    static constexpr size_t kPbWords = 122;
    using Pb = uint16_t[kPbWords];
    /// The low-pass filter section of the parameter block: words [kLpfFirst, kLpfEnd).
    static constexpr size_t kLpfFirst = 93;
    static constexpr size_t kLpfEnd = 97;

    void tap(int which, const int32_t* samples);
    void copy_cmdlist(uint32_t addr, size_t size, const AxMemory& mem);
    void run_cmdlist(AxMemory& mem);
    void setup(uint32_t init_addr, const AxMemory& mem);
    void download_and_mix(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb, const AxMemory& mem);
    void read_pb(Pb pb, uint32_t addr, const AxMemory& mem) const;
    void write_pb(const Pb pb, uint32_t addr, AxMemory& mem) const;
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
    uint32_t crc_ = 0;
    uint64_t frames_ = 0;
    Stats stats_;
    VoiceObserver voice_observer_ = nullptr;
    uint32_t cur_pb_addr_ = 0;
    uint64_t unknown_cmds_ = 0;
    bool reset_requested_ = false;
};

}  // namespace gcrecomp::audio
