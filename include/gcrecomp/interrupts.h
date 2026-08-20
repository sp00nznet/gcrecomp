#pragma once
// =============================================================================
// gcrecomp — Periodic interrupt delivery
//
// GameCube games written against the Dolphin OS expect periodic VI vblank
// interrupts (~60 Hz NTSC, ~50 Hz PAL) to drive the thread scheduler. The
// idle thread polls `RunQueueBits`; without something to set a thread
// runnable, that poll spins forever.
//
// On real hardware the interrupt preempts the CPU. Here we run cooperatively:
// `tick()` is called from `Memory::translate` every ~256K memory ops, and
// once enough virtual time has elapsed since the last fire, it invokes the
// registered handler synchronously with the active PPCContext.
//
// Typical setup (in the game's launcher):
//   1. Bind a handler-registration hook (e.g. HLE `__OSSetInterruptHandler`)
//      that calls `g_interrupts.set_handler(id, addr)` as the game wires up
//      its dispatchers.
//   2. Call `g_interrupts.set_external_dispatcher(addr_of_ExternalInterruptHandler)`
//      so the scheduler can invoke the OS-level dispatcher when an
//      interrupt fires.
//   3. Optional: tune `ops_per_frame` if the default doesn't match the
//      recompiled-code op rate on your machine.
// =============================================================================

#include "gcrecomp/ppc.h"
#include <atomic>
#include <cstdint>

namespace gcrecomp {

class FuncTable;
struct PPCContext;
class Memory;

struct InterruptScheduler {
    // Address of the OS's external-interrupt dispatcher (e.g. the function
    // that reads the PI cause register and routes to the right handler).
    // 0 means "don't dispatch via dispatcher; call the captured handlers
    // directly". Set this once the game's symbol map is loaded.
    uint32_t external_dispatcher = 0;

    // PI cause register bit for VI vblank. Bit 8 on real GameCube hardware.
    static constexpr uint32_t PI_CAUSE_VI_BIT = 0x100;

    // Per-OS-interrupt-id handler addresses captured from
    // `__OSSetInterruptHandler`. ID space is 0..63 in practice; size 256
    // is generous and matches what OS implementations expect.
    static constexpr int MAX_INTR_ID = 256;
    uint32_t handlers[MAX_INTR_ID] = { 0 };

    // How many `tick()` invocations between vblank fires. `Memory::translate`
    // calls tick once per ~256K raw mem ops, so on a typical machine the
    // recompiled code does ~65 ticks/sec. Default of 1 fires every tick
    // (~60 Hz), which roughly matches NTSC vblank. Tune via set_ops_per_frame.
    uint64_t ops_per_frame = 1;

    // Op counter and next-fire deadline (both in mem-op units).
    std::atomic<uint64_t> op_counter{0};
    uint64_t next_vi_op = 0;

    // Reentry guard — we mustn't fire an interrupt while one is already running.
    bool inside_handler = false;

    // Hook points called by the project code.
    void set_handler(int interrupt_id, uint32_t handler_addr);
    void set_external_dispatcher(uint32_t addr) { external_dispatcher = addr; }
    void set_ops_per_frame(uint64_t ops) { ops_per_frame = ops; next_vi_op = op_counter.load() + ops; }

    // Master enable. Off by default; flip it on once you've wired up the
    // dispatcher and the game has registered its handlers.
    std::atomic<bool> enabled{false};
    void enable()  { enabled.store(true,  std::memory_order_relaxed); next_vi_op = op_counter.load() + ops_per_frame; }
    void disable() { enabled.store(false, std::memory_order_relaxed); }

    // Called from `Memory::translate` once every block of ops (the hot path).
    // Returns immediately if the deadline hasn't elapsed.
    void tick(PPCContext* ctx, Memory* mem);

    // The actual fire path — exposed so a test harness or a custom poll
    // hook can force-fire without waiting for the op counter.
    void fire_vi(PPCContext* ctx, Memory* mem);
};

extern InterruptScheduler g_interrupts;

} // namespace gcrecomp
