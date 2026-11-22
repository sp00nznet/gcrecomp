// SPDX-License-Identifier: MIT
//
// GP (graphics processor) emulation for recompiled GameCube code.
//
// The Dolphin SDK's GX functions only encode commands: they write CP, XF and BP register loads and
// vertex data to the write-gather pipe at 0xCC008000. Instead of replacing every GX function, the
// recompiled SDK code runs unchanged and this module consumes the command stream it produces,
// then draws it with OpenGL through SDL3.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gcrecomp::gp {

struct Config {
    int window_width = 1280;
    int window_height = 960;
    // When set, every presented frame is also written as <dir>/frame_NNNN.ppm (for tests).
    const char* screenshot_dir = nullptr;
    // Stop writing screenshots after this many frames (0 = unlimited).
    int max_screenshots = 0;
    // TOML file with the keyboard mapping (see src/gp/input.h); null or missing = defaults.
    const char* input_config = nullptr;
};

// Creates the window and GL context and installs the write-gather pipe sink. Returns false when no
// window can be created; the runtime then keeps running without graphics.
bool init(const Config& config);
void shutdown();

// A store to the write-gather pipe: `size` is 1, 2, 4 or 8 bytes, `value` holds them big-endian.
void wgp_write(uint32_t size, uint64_t value);

// Processes window events. Returns false once the window was closed.
bool pump_events();

// Number of frames presented so far (copies to the external frame buffer).
uint64_t frames_presented();
// Traces the draws of the frame `frames_presented() + n` as GP_TRACE_FRAME does (n >= 1; 1 is the frame in
// progress). Used by the script command `trace-frame` to look at a frame reached by events.
void request_trace_frame(uint64_t n);

// Counters for diagnosing what the game submits.
struct Stats {
    uint64_t commands = 0;
    uint64_t draws = 0;
    uint64_t vertices = 0;
    uint64_t bp_writes = 0;
    uint64_t xf_writes = 0;
    uint64_t cp_writes = 0;
    uint64_t unknown_commands = 0;
};
const Stats& stats();

}  // namespace gcrecomp::gp
