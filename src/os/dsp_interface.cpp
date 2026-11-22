// SPDX-License-Identifier: MIT
//
// DSP interface device: see gcrecomp/hw/dsp.h. The mailbox/control model is ported from the
// owner's own Rust runtime (fepor-recomp, crates/runtime/src/dsp.rs); the register meanings follow
// the SDK's dolphin/dsp.h.
#include "gcrecomp/hw/dsp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <unordered_set>

#include "gcrecomp/interrupts.h"
#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"

namespace gcrecomp {

namespace {

constexpr uint32_t kMailToDspHi = 0x00;
constexpr uint32_t kMailToDspLo = 0x02;
constexpr uint32_t kMailFromDspHi = 0x04;
constexpr uint32_t kMailFromDspLo = 0x06;
constexpr uint32_t kControl = 0x0A;
constexpr uint32_t kArMode = 0x16;
constexpr uint32_t kArDmaMain = 0x20;
constexpr uint32_t kArDmaAram = 0x24;
constexpr uint32_t kArDmaCount = 0x28;
constexpr uint32_t kArDmaCountLo = 0x2A;
constexpr uint32_t kAiBytesLeft = 0x3A;
constexpr uint32_t kInitCodeDmaLen = 0x20;
constexpr uint32_t kInitCodeMail = 0x00544348;

// Boot-task commands sent to the ROM: a tag mail (0x80F3xxxx) followed by one data mail, except the
// start command whose data mail is the start vector.
constexpr uint32_t kBootTagMask = 0xFFFF0000;
constexpr uint32_t kBootTag = 0x80F30000;
constexpr uint16_t kBootStart = 0xD001;
constexpr uint16_t kBootCodeAddress = 0xA001;  // where the microcode is in main memory
constexpr uint16_t kBootCodeLength = 0xA002;   // its size in bytes

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

DspInterface g_dsp;

namespace {

// GCRECOMP_AX_TRACE=1: the first running voices (parameter block address, source format and addresses,
// resampling ratio, mixer control) and, once a second, what the command lists contained.
bool ax_trace_enabled() {
    static const bool on = env_flag("GCRECOMP_AX_TRACE");
    return on;
}

void trace_voice(uint32_t pb_addr, const uint16_t* pb) {
    static std::unordered_set<uint32_t> seen;
    if (seen.size() >= 64 || !seen.insert(pb_addr).second) return;
    auto hilo = [&](int i) { return uint32_t(pb[i]) << 16 | pb[i + 1]; };
    std::fprintf(stderr,
                 "[AX] voice pb %08X: format %04X (srctype %u) start %08X end %08X cur %08X ratio %08X mixctl %04X "
                 "loop %u vol %04X\n",
                 pb_addr, pb[56], pb[4], hilo(57), hilo(59), hilo(61), hilo(83), pb[6], pb[55], pb[50]);
}

void trace_ax_stats(audio::AxUcode& ax) {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    const auto& st = ax.stats();
    std::fprintf(stderr, "[AX] lists %llu, voice ms %llu (loud %llu, peak %d), unknown cmds %llu, commands:",
                 (unsigned long long)ax.frames(), (unsigned long long)st.voice_ms, (unsigned long long)st.voice_ms_loud,
                 st.voice_peak, (unsigned long long)ax.unknown_commands());
    for (unsigned i = 0; i < 32; ++i)
        if (st.commands[i]) std::fprintf(stderr, " %02X:%llu", i, (unsigned long long)st.commands[i]);
    std::fprintf(stderr, "\n");
    static const char* const kTapNames[] = {"uploadL", "uploadR", "uploadS", "oppositeIn", "outL", "outR", "outS"};
    std::fprintf(stderr, "[AX] rms per stage (last second):");
    for (int i = 0; i < audio::AxUcode::Stats::kTaps; ++i) {
        const double n = double(st.tap_samples[i]);
        std::fprintf(stderr, " %s %.0f", kTapNames[i], n > 0 ? std::sqrt(st.tap_energy[i] / n) : 0.0);
    }
    std::fprintf(stderr, "\n");
    ax.reset_taps();
}

}  // namespace

DspInterface::DspInterface() : aram_(kAramSize, 0) { trace_ = env_flag("DSP_TRACE"); }

void DspInterface::reset() {
    std::memset(regs_, 0, sizeof regs_);
    std::memset(aram_.data(), 0, aram_.size());
    control_ = 0;
    to_dsp_ = from_dsp_ = 0;
    to_pending_ = false;
    rom_running_ = true;
    init_code_loaded_ = false;
    expect_data_for_ = 0;
    boot_fields_.clear();
    ax_ = audio::AxUcode();
}

bool DspInterface::dsp_interrupt_pending() const {
    return (control_ & CR_DSPINT) && (control_ & CR_DSPINTMSK);
}

bool DspInterface::aram_interrupt_pending() const {
    return (control_ & CR_ARINT) && (control_ & CR_ARINTMSK);
}

uint32_t DspInterface::raw_read(uint32_t offset, uint32_t size) const {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size && offset + i < kSize; ++i) v = v << 8 | regs_[offset + i];
    return v;
}

void DspInterface::raw_write(uint32_t offset, uint32_t value, uint32_t size) {
    for (uint32_t i = 0; i < size && offset + i < kSize; ++i)
        regs_[offset + i] = uint8_t(value >> (8 * (size - 1 - i)));
}

uint32_t DspInterface::read(uint32_t offset, uint32_t size) {
    if (offset >= kSize) return 0;
    if (size == 4) {
        const uint32_t hi = read(offset, 2);
        return hi << 16 | read(offset + 2, 2);
    }
    if (size == 2) {
        switch (offset) {
        case kControl: return control_;
        case kArMode: return raw_read(offset, 2) | 1;  // the SDK spins on "ARAM controller ready"
        case kAiBytesLeft: return 0;
        case kMailFromDspHi: return from_dsp_ >> 16;
        case kMailFromDspLo: {
            const uint32_t v = from_dsp_ & 0xFFFF;
            if (trace_) std::fprintf(stderr, "[dsp] cpu reads mail %08X\n", from_dsp_);
            from_dsp_ &= ~kMailValid;  // reading the low half consumes the mail
            return v;
        }
        case kMailToDspHi: return (to_dsp_ >> 16 & 0x7FFF) | (to_pending_ ? 0x8000u : 0u);
        case kMailToDspLo: return to_dsp_ & 0xFFFF;
        default: break;
        }
    }
    return raw_read(offset, size);
}

void DspInterface::write(uint32_t offset, uint32_t value, uint32_t size, uint8_t* ram, size_t ram_size) {
    if (offset >= kSize) return;
    if (size == 4) {
        if (offset == kArDmaCount) {  // one store: count and control together
            raw_write(offset, value, 4);
            aram_dma(ram, ram_size);
            return;
        }
        write(offset, value >> 16, 2, ram, ram_size);
        write(offset + 2, value & 0xFFFF, 2, ram, ram_size);
        return;
    }
    if (size == 2) {
        switch (offset) {
        case kControl: write_control(uint16_t(value)); return;
        case kMailToDspHi:
            if (trace_) std::fprintf(stderr, "[dsp] cpu writes mailbox high %04X\n", uint16_t(value));
            to_dsp_ = (to_dsp_ & 0x0000FFFF) | (value & 0xFFFF) << 16;
            return;
        case kMailToDspLo:
            to_dsp_ = (to_dsp_ & 0xFFFF0000) | (value & 0xFFFF);
            mail_from_cpu(to_dsp_, ram, ram_size);
            return;
        case kArDmaCountLo:
            // The SDK writes the count as two halves; the low one starts the transfer.
            raw_write(offset, value, 2);
            if (raw_read(kArDmaCount, 4) & 0x7FFFFFFF) aram_dma(ram, ram_size);
            return;
        default: break;
        }
    }
    raw_write(offset, value, size);
}

void DspInterface::write_control(uint16_t v) {
    if (trace_) std::fprintf(stderr, "[dsp] control <- %04X (was %04X)\n", v, control_);
    // Interrupt flags clear by writing 1. RESET and INIT are strobes the DSP completes at once;
    // DSPDMA is a status bit the CPU cannot set.
    const uint16_t flags = control_ & kInterruptFlags & ~(v & kInterruptFlags);
    const bool unhalted = (control_ & CR_HALT) && !(v & CR_HALT);
    control_ = uint16_t((v & ~kInterruptFlags & ~CR_RESET & ~CR_INIT & ~CR_DSPDMA) | flags);
    if (v & CR_RESET) {
        // Reset: the ROM runs again and announces itself.
        rom_running_ = true;
        expect_data_for_ = 0;
        boot_fields_.clear();
        from_dsp_ = kRomReadyMail;
        to_dsp_ = 0;
        init_code_loaded_ = false;
        if (trace_) std::fprintf(stderr, "[dsp] reset: rom sends %08X\n", from_dsp_);
    } else if (unhalted && rom_running_ && init_code_loaded_) {
        // The SDK's init code (__OSInitAudioSystem, 0x80 bytes) runs: it waits for its mailbox to be free,
        // sends one mail ("\0TCH", decoded by hand from the code; the SDK discards the value) and halts.
        init_code_loaded_ = false;
        from_dsp_ = kInitCodeMail | kMailValid;
        control_ |= CR_HALT;
        if (trace_) std::fprintf(stderr, "[dsp] init code runs: mail %08X, halts\n", from_dsp_);
    }
}

void DspInterface::mail_from_cpu(uint32_t mail, uint8_t* ram, size_t ram_size) {
    // The mail as the DSP reads it: the high half's bit 15 is what the CPU wrote, not the pending flag.
    if (trace_) std::fprintf(stderr, "[dsp] cpu sends mail %08X%s\n", mail, rom_running_ ? " (rom)" : " (ucode)");
    if (!rom_running_) {
        // The game's microcode takes every mail at once (the CPU polls the pending flag before the
        // next one) and may answer with a mail and a DSP interrupt.
        to_pending_ = false;
        axucode_mail(mail, ram, ram_size);
        return;
    }
    // The ROM takes every mail at once: the pending flag never stays set.
    to_pending_ = false;
    if (expect_data_for_) {
        const uint16_t tag = expect_data_for_;
        expect_data_for_ = 0;
        boot_fields_.push_back({tag, mail});
        if (trace_) std::fprintf(stderr, "[dsp] rom: command %04X data %08X\n", tag, mail);
        if (tag == kBootStart) {
            // The uploaded microcode starts and reports in with an interrupt.
            rom_running_ = false;
            ax_ = audio::AxUcode(boot_ucode_crc(ram, ram_size));
            from_dsp_ = kDspInitMail | kMailValid;
            control_ |= CR_DSPINT;
            if (trace_ || ax_trace_enabled())
                std::fprintf(stderr, "[dsp] ucode (crc %08X) starts at %04X: DSP_INIT + interrupt\n", ax_.crc(), mail & 0xFFFF);
        }
        return;
    }
    if ((mail & kBootTagMask) == kBootTag) expect_data_for_ = uint16_t(mail);
}

uint32_t DspInterface::boot_ucode_crc(const uint8_t* ram, size_t ram_size) const {
    uint32_t address = 0, length = 0;
    for (const BootField& f : boot_fields_) {
        if (f.tag == kBootCodeAddress) address = f.data & 0x03FFFFFF;
        if (f.tag == kBootCodeLength) length = f.data & 0xFFFF;
    }
    if (!ram || address >= ram_size || length > ram_size - address) return 0;
    return audio::ucode_crc(ram + address, length);
}

void DspInterface::axucode_mail(uint32_t mail, uint8_t* ram, size_t ram_size) {
    audio::AxMemory mem;
    mem.ram = ram;
    mem.ram_size = ram ? ram_size : 0;
    mem.aram = aram_.data();
    mem.aram_size = aram_.size();
    if (ax_trace_enabled()) ax_.set_voice_observer(trace_voice);
    const std::optional<uint32_t> reply = ax_.handle_mail(mail, mem);
    if (ax_trace_enabled()) trace_ax_stats(ax_);
    if (reply) {
        post_mail_to_cpu(*reply);
        control_ |= CR_DSPINT;
        if (trace_) std::fprintf(stderr, "[dsp] ucode answers %08X + interrupt (list %llu)\n", *reply,
                                 (unsigned long long)ax_.frames());
    }
    if (trace_ && reply && *reply == audio::AxUcode::kDspYield && ax_.frames() % 500 == 1) {
        std::fprintf(stderr, "[dsp] ax list %llu (%zu words):", (unsigned long long)ax_.frames(), ax_.last_cmdlist_size());
        for (size_t i = 0; i < ax_.last_cmdlist_size() && i < 64; ++i) std::fprintf(stderr, " %04X", ax_.last_cmdlist()[i]);
        std::fprintf(stderr, "\n");
    }
    if (ax_.reset_requested()) {
        // The CPU hands the DSP back to the ROM: the next boot task upload starts a new microcode.
        rom_running_ = true;
        expect_data_for_ = 0;
        boot_fields_.clear();
        ax_ = audio::AxUcode();
    }
}

void DspInterface::aram_dma(uint8_t* ram, size_t ram_size) {
    const uint32_t main_addr = raw_read(kArDmaMain, 4) & 0x03FFFFFF;
    const uint32_t aram_addr = raw_read(kArDmaAram, 4) & uint32_t(kAramSize - 1);
    const uint32_t cnt = raw_read(kArDmaCount, 4);
    const size_t len = cnt & 0x7FFFFFFF;
    const bool to_main = cnt >> 31;
    if (trace_)
        std::fprintf(stderr, "[dsp] aram dma %s main %08X aram %06X len %zX\n", to_main ? "aram->main" : "main->aram",
                     main_addr, aram_addr, len);
    size_t n = len;
    if (n > aram_.size() - aram_addr) n = aram_.size() - aram_addr;
    n = main_addr >= ram_size ? 0 : (n < ram_size - main_addr ? n : ram_size - main_addr);
    if (ram && n) {
        if (to_main) std::memcpy(ram + main_addr, &aram_[aram_addr], n);
        else std::memcpy(&aram_[aram_addr], ram + main_addr, n);
    }
    // __OSInitAudioSystem hands its init code to the DSP through the ARAM DMA registers: 0x20 bytes to ARAM 0.
    if (!to_main && aram_addr == 0 && len == kInitCodeDmaLen) init_code_loaded_ = true;
    raw_write(kArDmaCount, 0, 4);  // transfer complete
    control_ |= CR_ARINT;
}

namespace {
bool g_in_dsp_handler = false;

void deliver(int id, PPCContext* ctx, Memory* mem) {
    const uint32_t handler = g_interrupts.handlers[id];
    if (!handler) return;
    // The handler runs as an exception would: all registers are the interrupted code's.
    PPCContext saved = *ctx;
    // An interrupt handler runs with interrupts masked, and the interrupted code gets its own state
    // back afterwards (the handler's OSDisableInterrupts/OSRestoreInterrupts pairs are not balanced
    // against the exception return that real hardware performs).
    const bool was_enabled = sched::interrupts_exchange(false);
    ctx->r[3] = uint32_t(id);
    ctx->r[4] = mem->read32(0x800000D4);  // the interrupted context (OSGetCurrentContext)
    g_func_table.call(handler, ctx, mem);
    *ctx = saved;
    sched::interrupts_exchange(was_enabled);
}
}  // namespace

void dsp_poll(PPCContext* ctx, Memory* mem) {
    if (g_in_dsp_handler || !ctx || !mem) return;
    if (!g_dsp.dsp_interrupt_pending() && !g_dsp.aram_interrupt_pending()) return;
    if (!sched::interrupts_enabled()) {  // the guest masked interrupts: it stays pending
        if (g_dsp.tracing()) {
            static unsigned n = 0;
            if (n++ < 5) std::fprintf(stderr, "[dsp] interrupt pending, guest interrupts disabled\n");
        }
        return;
    }
    if (g_dsp.tracing())
        std::fprintf(stderr, "[dsp] deliver interrupt (cr %04X) to %08X\n", g_dsp.control(),
                     g_interrupts.handlers[kOsIntrDspDsp]);
    g_in_dsp_handler = true;
    if (g_dsp.aram_interrupt_pending()) deliver(kOsIntrDspAram, ctx, mem);
    if (g_dsp.dsp_interrupt_pending()) deliver(kOsIntrDspDsp, ctx, mem);
    g_in_dsp_handler = false;
}

}  // namespace gcrecomp
