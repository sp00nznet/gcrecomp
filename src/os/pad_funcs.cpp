// SPDX-License-Identifier: MIT
//
// Controller (PAD) HLE.
//
// The SDK's PAD code talks to the serial interface with interrupts. These replacements report one
// connected controller on port 0 and fill PADStatus from g_pad[0], which the window layer updates
// from the keyboard each frame.
#include <cstring>

#include "gcrecomp/runtime.h"

namespace gcrecomp {

PadState g_pad[4] = {};

namespace {

constexpr int8_t kErrNone = 0;
constexpr int8_t kErrNoController = -1;

// PADInit(void) / PADReset(u32 mask) / PADRecalibrate(u32 mask) -> TRUE
void pad_true(PPCContext* ctx, Memory*) { ctx->r[3] = 1; }
void pad_nothing(PPCContext*, Memory*) {}

// PADRead(PADStatus* status): four entries of 12 bytes:
//   u16 button; s8 stickX, stickY, substickX, substickY; u8 triggerL, triggerR, analogA, analogB; s8 err
void pad_read(PPCContext* ctx, Memory* mem) {
    const uint32_t status = ctx->r[3];
    for (uint32_t port = 0; port < 4; ++port) {
        const uint32_t p = status + port * 12;
        const PadState& s = g_pad[port];
        const bool connected = port == 0;
        mem->write16(p, connected ? s.buttons : 0);
        mem->write8(p + 2, connected ? uint8_t(s.stick_x) : 0);
        mem->write8(p + 3, connected ? uint8_t(s.stick_y) : 0);
        mem->write8(p + 4, connected ? uint8_t(s.substick_x) : 0);
        mem->write8(p + 5, connected ? uint8_t(s.substick_y) : 0);
        mem->write8(p + 6, connected ? s.trigger_l : 0);
        mem->write8(p + 7, connected ? s.trigger_r : 0);
        mem->write8(p + 8, 0);
        mem->write8(p + 9, 0);
        mem->write8(p + 10, uint8_t(connected ? kErrNone : kErrNoController));
        mem->write8(p + 11, 0);
    }
    ctx->r[3] = 0;  // no channel reports an error
}

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"PADInit", pad_true},
    {"PADReset", pad_true},
    {"PADRecalibrate", pad_true},
    {"PADRead", pad_read},
    {"PADSetSpec", pad_nothing},
    {"PADControlMotor", pad_nothing},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_pad_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
