#pragma once
// =============================================================================
// GameCube Dolphin OS Definitions
//
// Struct layouts and constants for OS-level data structures that games built
// with the Dolphin SDK expect to find in emulated memory. These definitions
// are used by the static recompilation runtime to set up and interact with
// the emulated GameCube memory space.
//
// References:
//   - libogc (devkitPro, zlib license):
//       Canonical open-source implementation of the Dolphin OS SDK.
//       Struct sizes and field offsets are derived from libogc headers
//       (ogc/os.h, ogc/dvd.h, ogc/card.h, ogc/pad.h, ogc/cache.h).
//   - Pureikyubu / Dolwin (CC0 1.0 Universal):
//       GameCube emulator with public-domain OS HLE code. Thread control
//       block layout and low-memory constants sourced from its OS module.
//
// These structs represent the *layout* of data in emulated memory. They are
// not instantiated directly as C++ objects -- instead, the runtime reads and
// writes their fields via big-endian Memory accessors at known offsets.
// =============================================================================

#include <cstdint>

namespace gcrecomp::os {

// =============================================================================
// OSThread - Thread control block
//
// The Dolphin OS uses a cooperative threading model. Each thread has a control
// block (TCB) of 0x318 bytes stored in emulated memory. The recompilation
// runtime does not implement actual preemptive threading; instead it writes
// valid TCB structures so that SDK initialization code (OSInit, OSCreateThread)
// sees consistent state.
//
// Reference: libogc ogc/lwp_threads.h, Pureikyubu HLE/os.cpp
// =============================================================================
struct OSThread {
    // The full struct is 0x318 bytes. Key fields:
    // 0x000: OSContext (register save area, 0x2C8 bytes)
    // 0x2C8: state (u16)     -- current scheduling state
    // 0x2CA: attributes (u16) -- detach/joinable flags
    // 0x2CC: suspend count (s32) -- incremented on OSSuspendThread
    // 0x2D0: effective priority (s32) -- may be boosted by mutex inheritance
    // 0x2D4: base priority (s32)  -- priority set at creation time
    // 0x2D8: exit value (void*)   -- value passed to OSExitThread
    // 0x2DC-0x2E3: thread queue links (next/prev pointers)
    // 0x2F4: stack base (void*)   -- top of stack (highest address)
    // 0x2F8: stack end (void*)    -- bottom of stack (lowest address)
    static constexpr uint32_t SIZE = 0x318;

    // State values (matches OS_THREAD_STATE_* in libogc)
    static constexpr uint16_t STATE_READY    = 1;  // Eligible to run
    static constexpr uint16_t STATE_RUNNING  = 2;  // Currently executing
    static constexpr uint16_t STATE_WAITING  = 4;  // Blocked on a queue
    static constexpr uint16_t STATE_MORIBUND = 8;  // Terminated, awaiting join
};

// Offsets within OSThread for field access via Memory read/write.
// These let the runtime manipulate thread structures without needing
// to map the full C struct into host memory.
namespace thread_off {
    constexpr uint32_t STATE          = 0x2C8;
    constexpr uint32_t ATTR           = 0x2CA;
    constexpr uint32_t SUSPEND_COUNT  = 0x2CC;
    constexpr uint32_t PRIORITY       = 0x2D0;
    constexpr uint32_t BASE_PRIORITY  = 0x2D4;
    constexpr uint32_t EXIT_VALUE     = 0x2D8;
    constexpr uint32_t QUEUE_NEXT     = 0x2DC;
    constexpr uint32_t QUEUE_PREV     = 0x2E0;
    constexpr uint32_t LINK_NEXT      = 0x2E4;
    constexpr uint32_t LINK_PREV      = 0x2E8;
    constexpr uint32_t STACK_BASE     = 0x2F4;
    constexpr uint32_t STACK_END      = 0x2F8;
}

// =============================================================================
// OSContext - CPU register save area
//
// When the OS switches threads or handles an exception, it saves the full
// Gekko register state into an OSContext structure (0x2C8 bytes). This is
// embedded at offset 0 of each OSThread.
//
// Reference: libogc ogc/context.h, Pureikyubu HLE/os.h
// =============================================================================
namespace ctx_off {
    constexpr uint32_t GPR_BASE       = 0x00C;  // r0-r31 (32 x 4 bytes)
    constexpr uint32_t CR             = 0x080;  // Condition Register
    constexpr uint32_t LR             = 0x084;  // Link Register
    constexpr uint32_t CTR            = 0x088;  // Count Register
    constexpr uint32_t XER            = 0x08C;  // Fixed-Point Exception Register
    constexpr uint32_t FPR_BASE       = 0x090;  // f0-f31 (32 x 8 bytes)
    constexpr uint32_t FPSCR          = 0x190;  // FP Status and Control Register
    constexpr uint32_t SRR0           = 0x198;  // Machine status save/restore register 0
    constexpr uint32_t SRR1           = 0x19C;  // Machine status save/restore register 1
    constexpr uint32_t STATE          = 0x1A2;  // Context state flags
    constexpr uint32_t GQR_BASE       = 0x1A8;  // GQR0-GQR7 (8 x 4 bytes, paired-single quantization)
    constexpr uint32_t PS_BASE        = 0x1C8;  // Paired singles ps0-ps31 (32 x 8 bytes)
    constexpr uint32_t SIZE           = 0x2C8;  // Total OSContext size
}

// =============================================================================
// OSAlarm - Timer alarm structure
//
// Used by OSSetAlarm / OSSetPeriodicAlarm to schedule callbacks at specific
// timebase tick counts. The runtime stubs these as no-ops.
//
// Reference: libogc ogc/system.h
// =============================================================================
struct OSAlarm {
    static constexpr uint32_t SIZE = 0x28;
};

// =============================================================================
// OSMutex - Mutual exclusion lock
//
// Used by the SDK for thread-safe resource access. In a single-threaded
// recompilation environment, lock/unlock operations are no-ops.
//
// Reference: libogc ogc/mutex.h
// =============================================================================
struct OSMutex {
    static constexpr uint32_t SIZE = 0x18;
};

// =============================================================================
// OSMessageQueue - Inter-thread message passing
//
// Provides a bounded FIFO queue for sending messages between threads.
// Stubbed in single-threaded recompilation.
//
// Reference: libogc ogc/message.h
// =============================================================================
struct OSMessageQueue {
    static constexpr uint32_t SIZE = 0x20;
};

// =============================================================================
// DVD structures
//
// DVDFileInfo is the handle structure returned by DVDOpen. It contains a
// DVDCommandBlock (used for async DMA) followed by the file's disc offset,
// length, and an optional completion callback.
//
// Reference: libogc ogc/dvd.h
// =============================================================================
struct DVDFileInfo {
    // 0x00: DVDCommandBlock (0x30 bytes, internal DMA state)
    // 0x30: start address (file offset on disc, in bytes)
    // 0x34: length (file size in bytes)
    // 0x38: callback (function pointer for async completion)
    static constexpr uint32_t SIZE         = 0x3C;
    static constexpr uint32_t OFF_START    = 0x30;
    static constexpr uint32_t OFF_LENGTH   = 0x34;
    static constexpr uint32_t OFF_CALLBACK = 0x38;
};

// =============================================================================
// Heap structures
//
// The Dolphin OS provides a simple heap allocator (OSAlloc / OSFree) that
// manages free blocks via an intrusive linked list. Each heap has a header
// stored in a low-memory array, and each free block starts with a cell header.
//
// Reference: libogc ogc/system.h (OSCreateHeap, OSAlloc, OSFree)
// =============================================================================

// OS heap header (in emulated memory)
struct OSHeapHeader {
    // Heap descriptor stored in low memory heap array
    static constexpr uint32_t SIZE = 0x10;
    // 0x00: size (total heap size)
    // 0x04: free list head
    // 0x08: allocated list head
    // 0x0C: padding
};

// Free block header (16 bytes, stored at the start of each free region)
struct OSHeapCell {
    // 0x00: prev (pointer to previous free cell)
    // 0x04: next (pointer to next free cell)
    // 0x08: size (size of this free block including header)
    static constexpr uint32_t SIZE = 0x10;
};

// =============================================================================
// Card (memory card) structures
//
// CARDFileInfo is the handle for an open memory card file. The SDK uses
// channels 0 and 1 for slots A and B respectively.
//
// Reference: libogc ogc/card.h
// =============================================================================
struct CARDFileInfo {
    static constexpr uint32_t SIZE = 0x14;
    // 0x00: channel (s32) -- 0 = slot A, 1 = slot B
    // 0x04: file number (s32) -- directory entry index
    // 0x08: offset -- current read/write offset
    // 0x0C: length -- file length
    // 0x10: iBlock (internal) -- current block index
};

// =============================================================================
// PAD (controller) status - matches the in-memory layout
//
// PADRead fills an array of 4 PADStatus structs (one per controller port).
// The button field uses a bitmask; stick values are signed 8-bit.
//
// Reference: libogc ogc/pad.h
// =============================================================================
struct PADStatus {
    uint16_t button;      // Button bitmask (A, B, X, Y, Start, D-pad, triggers)
    int8_t   stick_x;     // Main analog stick X (-128 to 127)
    int8_t   stick_y;     // Main analog stick Y (-128 to 127)
    int8_t   substick_x;  // C-stick X (-128 to 127)
    int8_t   substick_y;  // C-stick Y (-128 to 127)
    uint8_t  trigger_l;   // Left trigger analog (0-255)
    uint8_t  trigger_r;   // Right trigger analog (0-255)
    uint8_t  analog_a;    // Analog A button (0-255)
    uint8_t  analog_b;    // Analog B button (0-255)
    int8_t   err;         // Error code (0 = ok, negative = error)
    uint8_t  padding;

    static constexpr uint32_t SIZE = 12;
};
static_assert(sizeof(PADStatus) == 12, "PADStatus must be 12 bytes");

// =============================================================================
// Error codes
//
// Standard return values used across the Dolphin SDK. Functions return 0
// on success and negative values on failure.
//
// Reference: libogc ogc/system.h, ogc/dvd.h, ogc/card.h
// =============================================================================
constexpr int32_t OS_ERROR_OK        =  0;
constexpr int32_t OS_ERROR_INVALID   = -1;
constexpr int32_t OS_ERROR_NO_MEM    = -2;
constexpr int32_t OS_ERROR_BUSY      = -3;

constexpr int32_t DVD_RESULT_OK      =  0;
constexpr int32_t DVD_RESULT_FATAL   = -1;
constexpr int32_t DVD_RESULT_IGNORED = -2;
constexpr int32_t DVD_RESULT_CANCELED = -3;

constexpr int32_t CARD_RESULT_READY       =  0;
constexpr int32_t CARD_RESULT_BUSY        = -1;
constexpr int32_t CARD_RESULT_WRONGDEVICE = -2;
constexpr int32_t CARD_RESULT_NOCARD      = -3;
constexpr int32_t CARD_RESULT_NOFILE      = -4;
constexpr int32_t CARD_RESULT_IOERROR     = -5;
constexpr int32_t CARD_RESULT_BROKEN      = -6;
constexpr int32_t CARD_RESULT_EXIST       = -7;
constexpr int32_t CARD_RESULT_NOENT       = -8;
constexpr int32_t CARD_RESULT_INSSPACE    = -9;
constexpr int32_t CARD_RESULT_NOPERM      = -10;
constexpr int32_t CARD_RESULT_LIMIT       = -11;
constexpr int32_t CARD_RESULT_NAMETOOLONG = -12;

} // namespace gcrecomp::os
