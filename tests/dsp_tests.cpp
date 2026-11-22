// SPDX-License-Identifier: MIT
// The DSP interface device: mailboxes, control register, ROM handshake, ARAM DMA and interrupt
// delivery to a registered handler. No display, no disc.
#include <cstdio>
#include <vector>

#include "gcrecomp/hw/dsp.h"
#include "gcrecomp/interrupts.h"
#include "gcrecomp/runtime.h"

using namespace gcrecomp;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

namespace {
std::vector<uint8_t> g_ram(1 << 20);

void w16(DspInterface& d, uint32_t off, uint32_t v) { d.write(off, v, 2, g_ram.data(), g_ram.size()); }
void w32(DspInterface& d, uint32_t off, uint32_t v) { d.write(off, v, 4, g_ram.data(), g_ram.size()); }
void send(DspInterface& d, uint32_t mail) {
    w16(d, 0x00, mail >> 16);
    w16(d, 0x02, mail & 0xFFFF);
}
uint32_t take(DspInterface& d) {
    const uint32_t hi = d.read(0x04, 2);
    return (hi << 16) | d.read(0x06, 2);
}

void test_reset_handshake() {
    DspInterface d;
    CHECK(d.read(0x04, 2) == 0);  // nothing before reset
    w16(d, 0x0A, DspInterface::CR_RESET | DspInterface::CR_HALT);
    CHECK((d.control() & DspInterface::CR_RESET) == 0);  // reset completes at once
    CHECK(d.control() & DspInterface::CR_HALT);
    CHECK(d.read(0x04, 2) & 0x8000);                     // valid
    const uint32_t hi = d.read(0x04, 2);
    CHECK(hi == 0x8071);
    CHECK(d.read(0x06, 2) == 0xFEED);
    CHECK((d.read(0x04, 2) & 0x8000) == 0);              // low read consumed it
    CHECK(d.read(0x06, 2) == 0xFEED);                    // value stays, flag does not
}

void test_mail_to_dsp_consumed_by_rom() {
    DspInterface d;
    w16(d, 0x0A, DspInterface::CR_RESET);
    take(d);
    send(d, 0x80F3A001);
    CHECK((d.read(0x00, 2) & 0x8000) == 0);  // the ROM took it: the boot task's poll exits
    CHECK(d.rom_running());
}

void test_boot_protocol_and_init() {
    DspInterface d;
    w16(d, 0x0A, DspInterface::CR_RESET);
    take(d);
    send(d, 0x80F3A001); send(d, 0x00100000);
    send(d, 0x80F3C002); send(d, 0x000019E0);
    send(d, 0x80F3A002); send(d, 0x00000000);
    send(d, 0x80F3B002); send(d, 0x00000000);
    CHECK(d.rom_running());
    CHECK(d.boot_fields().size() == 4);
    CHECK(d.boot_fields()[1].tag == 0xC002 && d.boot_fields()[1].data == 0x19E0);
    CHECK((d.read(0x04, 2) & 0x8000) == 0);  // the ROM said nothing yet
    send(d, 0x80F3D001); send(d, 0x00000010);
    CHECK(!d.rom_running());
    CHECK(take(d) == (DspInterface::kDspInitMail | DspInterface::kMailValid));
    CHECK((d.control() & DspInterface::CR_DSPINT) != 0);
    CHECK(!d.dsp_interrupt_pending());  // masked
    w16(d, 0x0A, DspInterface::CR_DSPINTMSK | DspInterface::CR_DSPINT);  // unmask and clear
    CHECK((d.control() & DspInterface::CR_DSPINT) == 0);
    CHECK(d.control() & DspInterface::CR_DSPINTMSK);
}

void test_init_mail_value() {
    DspInterface d;
    w16(d, 0x0A, DspInterface::CR_RESET);
    take(d);
    send(d, 0x80F3D001); send(d, 0x10);
    CHECK(d.read(0x04, 2) == 0xDCD1);  // bit 15 is the valid flag, set
    CHECK(d.read(0x06, 2) == 0x0000);
}

void test_ucode_mails_stay_pending() {
    DspInterface d;
    w16(d, 0x0A, DspInterface::CR_RESET);
    take(d);
    send(d, 0x80F3D001); send(d, 0x10);
    take(d);
    send(d, 0xBABE0038);              // bit 15 of the high half is data, not the flag
    CHECK(d.read(0x00, 2) & 0x8000);  // nobody read it: pending
    CHECK(d.read(0x02, 2) == 0x0038);
    CHECK((d.read(0x00, 2) & 0x7FFF) == 0x3ABE);
    send(d, 0x12345678);
    CHECK(d.read(0x00, 2) == (0x1234 | 0x8000));
}

void test_control_w1c_and_masks() {
    DspInterface d;
    d.raise(DspInterface::CR_ARINT);
    d.raise(DspInterface::CR_DSPINT);
    CHECK(!d.aram_interrupt_pending() && !d.dsp_interrupt_pending());
    w16(d, 0x0A, DspInterface::CR_ARINTMSK);  // unmask; flags not written as 1 stay
    CHECK(d.aram_interrupt_pending());
    CHECK(!d.dsp_interrupt_pending());
    w16(d, 0x0A, DspInterface::CR_ARINTMSK | DspInterface::CR_ARINT);  // clear ARINT only
    CHECK(!d.aram_interrupt_pending());
    CHECK(d.control() & DspInterface::CR_DSPINT);
    w16(d, 0x0A, DspInterface::CR_DSPDMA | DspInterface::CR_INIT);  // status/strobe bits do not stick
    CHECK((d.control() & (DspInterface::CR_DSPDMA | DspInterface::CR_INIT)) == 0);
}

void test_aram_dma() {
    DspInterface d;
    for (int i = 0; i < 64; ++i) g_ram[0x1000 + i] = uint8_t(i + 1);
    w32(d, 0x20, 0x80001000);  // main address (cached alias wraps to physical)
    w32(d, 0x24, 0x00004000);  // ARAM address
    w32(d, 0x28, 64);          // main -> ARAM, 64 bytes: starts
    CHECK(d.aram()[0x4000] == 1 && d.aram()[0x403F] == 64);
    CHECK(d.control() & DspInterface::CR_ARINT);
    CHECK(d.read(0x28, 4) == 0);  // done
    w16(d, 0x0A, DspInterface::CR_ARINT);
    CHECK((d.control() & DspInterface::CR_ARINT) == 0);

    // two 16-bit halves: the low one starts, ARAM -> main
    w32(d, 0x20, 0x2000);
    w32(d, 0x24, 0x4000);
    w16(d, 0x28, 0x8000);  // direction, count high
    w16(d, 0x2A, 32);
    CHECK(g_ram[0x2000] == 1 && g_ram[0x201F] == 32);
    CHECK(g_ram[0x2020] == 0);
    CHECK(d.control() & DspInterface::CR_ARINT);
}

void test_registers_and_ar_mode() {
    DspInterface d;
    CHECK(d.read(0x16, 2) & 1);
    w16(d, 0x30, 0x0001);
    w16(d, 0x32, 0x2340);
    CHECK(d.read(0x30, 2) == 0x0001 && d.read(0x32, 2) == 0x2340);
    CHECK(d.read(0x30, 4) == 0x00012340);
    w16(d, 0x36, 0x8010);
    CHECK(d.read(0x36, 2) == 0x8010);
    CHECK(d.read(0x3A, 2) == 0);
}

// Interrupt delivery to the handler the game registered.
static unsigned g_handler_runs = 0;
static uint32_t g_handler_arg = 0;
static void handler(PPCContext* ctx, Memory*) {
    ++g_handler_runs;
    g_handler_arg = ctx->r[3];
    g_dsp.write(0x0A, DspInterface::CR_DSPINT | DspInterface::CR_DSPINTMSK, 2, nullptr, 0);  // acknowledge
    ctx->r[3] = 0xDEAD;  // clobbers registers like any handler
}

void test_interrupt_delivery() {
    g_mem.init();
    g_dsp.reset();
    g_dsp.set_trace(false);
    constexpr uint32_t kHandler = 0x80004000;
    g_func_table.register_func(kHandler, handler);
    g_interrupts.handlers[kOsIntrDspDsp] = 0;

    // Through the memory map, as the recompiled code reaches it.
    g_mem.write16(0xCC00500A, DspInterface::CR_RESET | DspInterface::CR_DSPINTMSK);
    CHECK(g_mem.read16(0xCC005004) == 0x8071 && g_mem.read16(0xCC005006) == 0xFEED);
    g_mem.write32(0xCC005000, 0x80F3D001);
    g_mem.write32(0xCC005000, 0x00000010);
    CHECK(g_dsp.dsp_interrupt_pending());

    g_ctx.r[3] = 1234;
    dsp_poll(&g_ctx, &g_mem);        // no handler registered yet: nothing happens
    CHECK(g_handler_runs == 0 && g_ctx.r[3] == 1234);

    g_interrupts.handlers[kOsIntrDspDsp] = kHandler;
    dsp_poll(&g_ctx, &g_mem);
    CHECK(g_handler_runs == 1);
    CHECK(g_handler_arg == (uint32_t)kOsIntrDspDsp);
    CHECK(g_ctx.r[3] == 1234);       // registers restored
    CHECK(!g_dsp.dsp_interrupt_pending());
    dsp_poll(&g_ctx, &g_mem);        // acknowledged: not delivered twice
    CHECK(g_handler_runs == 1);
    g_interrupts.handlers[kOsIntrDspDsp] = 0;
}

void test_memory_map() {
    g_mem.init();
    g_dsp.reset();
    g_mem.write16(0xCC005028, 0);
    CHECK(g_mem.read16(0xCC005016) & 1);
    g_mem.write32(0xCC005030, 0x00012340);
    CHECK(g_mem.read32(0xCC005030) == 0x00012340);
}
}  // namespace

int main() {
    test_reset_handshake();
    test_mail_to_dsp_consumed_by_rom();
    test_boot_protocol_and_init();
    test_init_mail_value();
    test_ucode_mails_stay_pending();
    test_control_w1c_and_masks();
    test_aram_dma();
    test_registers_and_ar_mode();
    test_interrupt_delivery();
    test_memory_map();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dsp tests passed\n");
    return 0;
}
