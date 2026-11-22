// SPDX-License-Identifier: MIT
//
// ARAM (auxiliary RAM) and ARQ (ARAM request queue) HLE.
//
// The SDK's AR code probes the ARAM size by DMA-ing patterns through hardware registers and
// waiting for the DMA engine; recompiled code spins forever on emulated registers. These
// replacements keep ARAM as a host buffer, perform DMA copies immediately, and call completion
// callbacks synchronously.
#include <cstdio>
#include <cstring>

#include "gcrecomp/hw/dsp.h"
#include "gcrecomp/runtime.h"

namespace gcrecomp {

namespace {

constexpr uint32_t kAramSize = uint32_t(DspInterface::kAramSize);
constexpr uint32_t kAramInternal = 0x4000;  // first 16 KB are used by the DSP

// ARAM is the DSP interface's: the register-level DMA and these replacements see the same memory.
uint8_t* aram() { return g_dsp.aram(); }

uint32_t g_dma_callback = 0;

// Calls a guest function pointer, keeping the caller's argument registers intact.
void call_guest(uint32_t fn, PPCContext* ctx, Memory* mem, uint32_t arg) {
    if (!fn) return;
    PPCContext saved = *ctx;
    ctx->r[3] = arg;
    g_func_table.call(fn, ctx, mem);
    *ctx = saved;
}

// type 0: main memory -> ARAM, type 1: ARAM -> main memory.
bool dma(Memory* mem, uint32_t type, uint32_t main_addr, uint32_t aram_addr, uint32_t length) {
    if (aram_addr > kAramSize || length > kAramSize - aram_addr) {
        std::printf("[AR] DMA out of range: aram=0x%08X length=0x%X\n[bt]", aram_addr, length);
        print_call_chain(stdout);
        std::printf("\n");
        return false;
    }
    uint8_t* host = mem->translate(main_addr);
    if (!host) {
        std::printf("[AR] DMA with invalid main memory address 0x%08X\n", main_addr);
        return false;
    }
    if (type == 0) std::memcpy(aram() + aram_addr, host, length);
    else std::memcpy(host, aram() + aram_addr, length);
    return true;
}

// ARInit(u32* stack_index_addr, u32 num_entries) -> first free ARAM address
void ar_init(PPCContext* ctx, Memory*) {
    std::printf("[AR] ARInit\n");
    ctx->r[3] = kAramInternal;
}

// ARStartDMA(u32 type, u32 mainmem, u32 aram, u32 length)
void ar_start_dma(PPCContext* ctx, Memory* mem) {
    dma(mem, ctx->r[3], ctx->r[4], ctx->r[5], ctx->r[6]);
    call_guest(g_dma_callback, ctx, mem, 0);
}

void ar_get_dma_status(PPCContext* ctx, Memory*) { ctx->r[3] = 0; }
void ar_get_size(PPCContext* ctx, Memory*) { ctx->r[3] = kAramSize; }
void ar_get_base_address(PPCContext* ctx, Memory*) { ctx->r[3] = kAramInternal; }
void ar_get_internal_size(PPCContext* ctx, Memory*) { ctx->r[3] = kAramInternal; }
void ar_check_init(PPCContext* ctx, Memory*) { ctx->r[3] = 1; }

void ar_register_dma_callback(PPCContext* ctx, Memory*) {
    const uint32_t old = g_dma_callback;
    g_dma_callback = ctx->r[3];
    ctx->r[3] = old;
}

// ARQInit(void)
void arq_init(PPCContext*, Memory*) { std::printf("[ARQ] ARQInit\n"); }

// ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority,
//                u32 source, u32 dest, u32 length, ARQCallback callback)
// ARQRequest layout: next, owner, type, priority, source, dest, length, callback (4 bytes each).
void arq_post_request(PPCContext* ctx, Memory* mem) {
    const uint32_t request = ctx->r[3];
    const uint32_t type = ctx->r[5];
    const uint32_t source = ctx->r[7];
    const uint32_t dest = ctx->r[8];
    const uint32_t length = ctx->r[9];
    const uint32_t callback = ctx->r[10];
    if (request) {
        mem->write32(request + 4, ctx->r[4]);
        mem->write32(request + 8, type);
        mem->write32(request + 12, ctx->r[6]);
        mem->write32(request + 16, source);
        mem->write32(request + 20, dest);
        mem->write32(request + 24, length);
        mem->write32(request + 28, callback);
    }
    // ARQ type 0 = main memory to ARAM: source is main memory, dest is ARAM.
    // type 1 = ARAM to main memory: source is ARAM, dest is main memory.
    if (type == 0) dma(mem, 0, source, dest, length);
    else dma(mem, 1, dest, source, length);
    call_guest(callback, ctx, mem, request);
    ctx->r[3] = 1;
}

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"ARInit", ar_init},
    {"ARStartDMA", ar_start_dma},
    {"ARGetDMAStatus", ar_get_dma_status},
    {"ARGetSize", ar_get_size},
    {"ARGetBaseAddress", ar_get_base_address},
    {"ARGetInternalSize", ar_get_internal_size},
    {"ARCheckInit", ar_check_init},
    {"ARRegisterDMACallback", ar_register_dma_callback},
    {"ARQInit", arq_init},
    {"ARQPostRequest", arq_post_request},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_ar_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
