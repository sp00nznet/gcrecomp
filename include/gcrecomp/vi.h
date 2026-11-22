#pragma once
// SPDX-License-Identifier: MIT
// =============================================================================
// Video Interface timing and display-interrupt registers.
//
// The VI raises one interrupt per field (OS interrupt 24, PI cause bit 8) when the beam reaches the line
// programmed into one of the four display interrupt registers (VI_DISP_INT0..3 at 0xCC002030, 0x34, 0x38,
// 0x3C) and that register is enabled. The SDK's retrace handler (__VIRetraceHandler) reads the registers,
// clears the status bit of those that are set, then runs the pre/post retrace callbacks, counts the retrace and
// wakes the threads blocked in VIWaitForRetrace. The handler is the game's own recompiled code: this module only
// supplies the clock and the register bits it reads.
//
// Register layout (32 bits, big-endian; the SDK accesses the high half-word):
//     bit 31 INT   status, set by the hardware, cleared by writing 0
//     bit 28 ENB   enable
//     bits 16..26  VCT vertical match position
//     bits 0..10   HCT horizontal match position
// The matching position is not modelled: an enabled register raises its interrupt once per retrace.
// Registers 0 and 1 are the ones the SDK uses for the retrace; 2 and 3 serve the light-pen position callback
// and stay untouched.
// =============================================================================

#include <chrono>
#include <cstdint>

namespace gcrecomp::vi {

constexpr uint32_t kRegBase = 0xCC002000;  // VI register block
constexpr uint32_t kRegVertCount = 0x2C;   // VI_VERT_COUNT (16 bit); VI_HORIZ_COUNT follows at 0x2E
constexpr uint32_t kRegDispInt0 = 0x30;
constexpr uint32_t kDispIntCount = 4;
constexpr uint32_t kDispIntInt = 0x80000000;  // status
constexpr uint32_t kDispIntEnb = 0x10000000;  // enable
constexpr uint32_t kRetraceInterrupt = 24;    // OS_INTR_PI_VI
constexpr uint32_t kPiCauseVi = 0x100;        // PI_INTERRUPT_CAUSE bit
constexpr uint32_t kPiRegCause = 0xCC003000;
constexpr uint32_t kPiRegMask = 0xCC003004;

// NTSC field rate: 60000/1001 Hz.
constexpr std::chrono::nanoseconds kFieldPeriod{16683333};
constexpr uint32_t kLinesPerField = 263;
constexpr uint32_t kHalfLinesPerLine = 858;  // the horizontal counter runs 1..858 per line

// The wall-clock source of the retrace. It free-runs from start(); each period boundary is one retrace.
class RetraceTimer {
public:
    using Clock = std::chrono::steady_clock;

    explicit RetraceTimer(std::chrono::nanoseconds period = kFieldPeriod) : period_(period) {}

    // Begins the first field at `now` (the first retrace is one period later). Idempotent.
    void start(Clock::time_point now) {
        if (started_) return;
        started_ = true;
        field_start_ = now;
    }
    bool started() const { return started_; }
    void reset() { *this = RetraceTimer(period_); }

    // When the next retrace happens (meaningful once started).
    Clock::time_point next() const { return field_start_ + period_; }
    bool due(Clock::time_point now) const { return started_ && now >= next(); }

    // Consumes the retrace that is due. Returns false if none is. A host that fell behind loses the retraces it
    // missed, as the hardware cannot queue them: the field restarts on the period boundary before `now`.
    bool take(Clock::time_point now) {
        if (!due(now)) return false;
        auto behind = (now - field_start_) / period_;  // whole periods elapsed (>= 1)
        field_start_ += period_ * behind;
        ++count_;
        return true;
    }
    uint64_t count() const { return count_; }

    // Beam position at `now`: the line within the field (1..kLinesPerField) and the position within the line
    // (1..kHalfLinesPerLine). The field starts at line 1 on each retrace.
    struct Beam {
        uint16_t vcount;
        uint16_t hcount;
    };
    Beam beam(Clock::time_point now) const {
        if (!started_ || now < field_start_) return {1, 1};
        double phase = std::chrono::duration<double>(now - field_start_) / period_;
        if (phase >= 1.0) phase = 0.999999;
        double line = phase * kLinesPerField;
        uint32_t v = 1 + uint32_t(line);
        uint32_t h = 1 + uint32_t((line - uint32_t(line)) * kHalfLinesPerLine);
        return {uint16_t(v), uint16_t(h)};
    }

private:
    std::chrono::nanoseconds period_;
    bool started_ = false;
    Clock::time_point field_start_{};
    uint64_t count_ = 0;
};

// The retrace clock shared by the scheduler (interrupt delivery) and the register reads (beam position).
RetraceTimer& retrace_timer();

// ---- Register bits. `regs` points at the VI block (host address of 0xCC002000), big-endian. ----

// Sets the status bit of the enabled display interrupt registers 0 and 1. Returns true if one is pending
// (status and enable both set) afterwards.
bool raise_display_interrupts(uint8_t* regs);

// True if a display interrupt register has both its status and enable bits set.
bool display_interrupt_pending(const uint8_t* regs);

// Writes the beam position into VI_VERT_COUNT / VI_HORIZ_COUNT.
void write_beam(uint8_t* regs, RetraceTimer::Beam beam);

// Refreshes the position registers from the retrace clock. `block` is the host address of 0xCC002000. Called on
// reads of those registers.
void refresh_beam(uint8_t* block);

}  // namespace gcrecomp::vi
