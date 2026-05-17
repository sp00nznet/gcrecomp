// =============================================================================
// gcrecomp — Periodic interrupt delivery
// =============================================================================

#include "gcrecomp/interrupts.h"
#include "gcrecomp/runtime.h"
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

void InterruptScheduler::tick(PPCContext* ctx, Memory* mem) {
    if (!enabled.load(std::memory_order_relaxed)) return;
    if (inside_handler) return;

    // Increment the op counter in coarse chunks so the atomic isn't on the
    // hot path of every translate() call — callers should invoke us once
    // per N raw memory ops (see Memory::translate).
    uint64_t n = op_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < next_vi_op) return;
    next_vi_op = n + ops_per_frame;

    fire_vi(ctx, mem);
}

void InterruptScheduler::fire_vi(PPCContext* ctx, Memory* mem) {
    if (inside_handler) return;
    if (!external_dispatcher) return;

    inside_handler = true;
    static uint64_t fire_count = 0;
    if (++fire_count <= 3 || (fire_count & 0x3FF) == 0) {
        printf("[Intr] vblank fire #%llu (dispatcher=0x%08X)\n",
               (unsigned long long)fire_count, external_dispatcher);
        fflush(stdout);
    }

    // Set PI cause register VI bit so the dispatcher (which masks against
    // PI mask) will see VI as pending. The cause register is at 0xCC003000
    // big-endian 32-bit; VI is bit 8 from the MSB (== 0x00000100 in the
    // 32-bit big-endian view, stored as 0x01 in byte[2]).
    auto store_be32 = [](uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)(v >> 24);
        p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);
        p[3] = (uint8_t)v;
    };
    auto load_be32 = [](const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  | p[3];
    };
    uint8_t* cause_p = mem->hw_regs + (0xCC003000 - Memory::HW_REG_BASE);
    uint8_t* mask_p  = mem->hw_regs + (0xCC003004 - Memory::HW_REG_BASE);
    store_be32(cause_p, load_be32(cause_p) | PI_CAUSE_VI_BIT);
    // Also force the PI interrupt mask's VI bit. Until the game's VI
    // library runs `__OSUnmaskInterrupt(OS_INTR_PI_VI)`, the mask register
    // is all-zero. With cause set but mask clear, the OS dispatcher reads
    // it as "spurious interrupt" and falls through to `OSLoadContext(0)`,
    // which trashes the CPU register file. Forcing the mask bit means
    // (cause & mask) != 0 and the dispatcher takes the real path.
    store_be32(mask_p, load_be32(mask_p) | PI_CAUSE_VI_BIT);

    // FULL register-file save/restore around the dispatcher call. On real
    // hardware the OS exception entry vector saves all GPRs/FPRs/CR/XER to
    // a saved-context struct before invoking ExternalInterruptHandler, then
    // restores them via `rfi`. The recompiled handler only saves r29-r31
    // (its own callee-saved usage) — so without us snapshotting the full
    // ctx, anything it clobbers (r0-r12, r14-r28, CR, XER, etc.) leaks
    // back into the interrupted code and trashes register-dependent loads
    // (we'd see reads from random addresses like 0x3031xxxx).
    //
    // The handler's job here is to mutate memory (set runqueue bits, wake
    // threads, post messages); those writes persist correctly because we
    // don't restore the Memory object. We restore registers only.
    PPCContext saved = *ctx;

    // OS exception-glue convention: r3 = exception number (4 = external),
    // r4 = pointer to saved-context struct. We pass 4/0 and rely on the
    // dispatcher reaching the cause-matched branch before any OSLoadContext
    // dereference of r4 (see the mask-forcing logic above).
    ctx->r[3] = 4;
    ctx->r[4] = 0;

    g_func_table.call(external_dispatcher, ctx, mem);

    *ctx = saved;

    inside_handler = false;
}

} // namespace gcrecomp
