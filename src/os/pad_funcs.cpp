// SPDX-License-Identifier: MIT
//
// Controller (PAD) HLE.
//
// The SDK's PAD code talks to the serial interface with interrupts. These replacements fill PADStatus
// from g_pad[0..3], which the window layer (src/gp/input.cpp) updates from the keyboard and gamepads
// each frame, and record rumble requests in g_pad[].rumble for it to forward to the device.
#include <cstring>

#include "gcrecomp/runtime.h"

namespace gcrecomp {

namespace {
PadState connected_pad() {
    PadState s;
    s.connected = true;
    return s;
}
}  // namespace

// Port 0 reports a controller even before any host device was used; the others when assigned.
PadState g_pad[4] = {connected_pad(), {}, {}, {}};

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
        const bool connected = s.connected;
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

// PADControlMotor(s32 chan, u32 command): 0 = stop, 1 = rumble, 2 = stop and brake.
void set_motor(uint32_t chan, uint32_t command) {
    if (chan < 4) g_pad[chan].rumble = command == 1;
}
void pad_control_motor(PPCContext* ctx, Memory*) { set_motor(ctx->r[3], ctx->r[4]); }
// PADControlAllMotors(const u32 commands[4])
void pad_control_all_motors(PPCContext* ctx, Memory* mem) {
    for (uint32_t chan = 0; chan < 4; ++chan) set_motor(chan, mem->read32(ctx->r[3] + chan * 4));
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
    {"PADControlMotor", pad_control_motor},
    {"PADControlAllMotors", pad_control_all_motors},
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
