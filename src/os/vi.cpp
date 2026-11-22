// SPDX-License-Identifier: MIT
//
// Video Interface: the retrace clock and the register bits the SDK's retrace handler looks at.
// See gcrecomp/vi.h for the model.
#include "gcrecomp/vi.h"

namespace gcrecomp::vi {

RetraceTimer& retrace_timer() {
    static RetraceTimer timer;
    return timer;
}

namespace {

uint32_t load32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void store32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

void store16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

}  // namespace

bool raise_display_interrupts(uint8_t* regs) {
    for (uint32_t i = 0; i < 2; ++i) {
        uint8_t* r = regs + kRegDispInt0 + 4 * i;
        uint32_t v = load32(r);
        if (v & kDispIntEnb) store32(r, v | kDispIntInt);
    }
    return display_interrupt_pending(regs);
}

bool display_interrupt_pending(const uint8_t* regs) {
    for (uint32_t i = 0; i < kDispIntCount; ++i) {
        uint32_t v = load32(regs + kRegDispInt0 + 4 * i);
        if ((v & kDispIntInt) && (v & kDispIntEnb)) return true;
    }
    return false;
}

void write_beam(uint8_t* regs, RetraceTimer::Beam beam) {
    store16(regs + kRegVertCount, beam.vcount);
    store16(regs + kRegVertCount + 2, beam.hcount);
}

void refresh_beam(uint8_t* block) {
    RetraceTimer& t = retrace_timer();
    auto now = RetraceTimer::Clock::now();
    t.start(now);
    write_beam(block, t.beam(now));
}

}  // namespace gcrecomp::vi
