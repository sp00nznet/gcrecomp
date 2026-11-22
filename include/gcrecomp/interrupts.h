#pragma once
// =============================================================================
// gcrecomp — OS interrupt handler table
//
// __OSSetInterruptHandler (HLE) records the handlers the game registers here, indexed by OS interrupt number
// (OS_INTR_*: 24 is PI_VI, the VI retrace; 6/7 are DSP/ARAM, 8 AI...). The devices that raise an interrupt look
// the handler up and run it at a safe point: the DSP block (dsp_interface.cpp) and the VI retrace
// (threads.cpp, see gcrecomp/threads.h and gcrecomp/vi.h).
// =============================================================================

#include <cstdint>

namespace gcrecomp {

struct InterruptScheduler {
    // Per-OS-interrupt-id handler addresses captured from `__OSSetInterruptHandler`.
    static constexpr int MAX_INTR_ID = 256;
    uint32_t handlers[MAX_INTR_ID] = { 0 };

    void set_handler(int interrupt_id, uint32_t handler_addr);
};

extern InterruptScheduler g_interrupts;

} // namespace gcrecomp
