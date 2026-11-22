// =============================================================================
// gcrecomp — OS interrupt handler table
// =============================================================================

#include "gcrecomp/interrupts.h"
#include <cstdio>

namespace gcrecomp {

InterruptScheduler g_interrupts;

void InterruptScheduler::set_handler(int interrupt_id, uint32_t handler_addr) {
    if (interrupt_id < 0 || interrupt_id >= MAX_INTR_ID) return;
    uint32_t prev = handlers[interrupt_id];
    handlers[interrupt_id] = handler_addr;
    printf("[Intr] set_handler(id=%d, addr=0x%08X) (prev=0x%08X)\n",
           interrupt_id, handler_addr, prev);
    fflush(stdout);
}

} // namespace gcrecomp
