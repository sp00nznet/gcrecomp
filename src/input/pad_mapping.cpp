// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// PAD Mapping - GX PAD API Implementation
//
// Intercepts recompiled game code's calls to PADRead and returns input
// state from the host platform's input devices. This bridges the gap
// between the GameCube's Serial Interface (SI) controller protocol and
// modern input APIs.
//
// When a recompiled game calls PADRead(), execution is redirected here.
// The function reads the current input state and writes a PADStatus[4]
// array into the emulated GameCube memory at the address specified in
// the PPC context's r3 register.
//
// References:
//   - libogc — PADRead function signature and PADStatus layout
//   - Pureikyubu — SI (Serial Interface) controller protocol
// =============================================================================

#include "gcrecomp/input/input.h"
#include "gcrecomp/runtime.h"
#include <cstring>

namespace gcrecomp::input {

// Called by recompiled game code via OS function replacement.
// Writes PADStatus struct to GameCube memory at the address in r3.
void pad_read_hook(gcrecomp::PPCContext* ctx, gcrecomp::Memory* mem) {
    input_update();

    // r3 = pointer to PADStatus[4] array
    uint32_t status_addr = ctx->r[3];

    for (int i = 0; i < 4; i++) {
        PadStatus pad = input_get_pad(i);
        uint32_t addr = status_addr + i * 12; // PADStatus is 12 bytes

        mem->write16(addr + 0, pad.button);
        mem->write8(addr + 2, (uint8_t)pad.stick_x);
        mem->write8(addr + 3, (uint8_t)pad.stick_y);
        mem->write8(addr + 4, (uint8_t)pad.substick_x);
        mem->write8(addr + 5, (uint8_t)pad.substick_y);
        mem->write8(addr + 6, pad.trigger_l);
        mem->write8(addr + 7, pad.trigger_r);
        mem->write8(addr + 8, pad.analog_a);
        mem->write8(addr + 9, pad.analog_b);
        mem->write8(addr + 10, (uint8_t)pad.err);
        mem->write8(addr + 11, 0); // padding
    }

    // Return value: bitmask of connected controllers
    ctx->r[3] = 0x1; // Only controller 0 connected
}

} // namespace gcrecomp::input
