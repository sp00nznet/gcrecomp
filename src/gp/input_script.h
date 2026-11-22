// SPDX-License-Identifier: MIT
//
// Scripted controller input: a small language that presses buttons and waits for events (a frame
// count, a file the game opens from the disc, the picture changing or settling, wall time), so a
// run can be driven through menus without knowing at which frame each screen appears.
//
// The engine is pure: Script::step() takes an Observation (what happened since the previous frame)
// and returns the buttons to hold, so it is tested without a window, a device or a disc. The host
// (input.cpp) fills the Observation. The language is documented in docs/INPUT_SCRIPT.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gcrecomp::gp::input {

// A coarse picture of a presented frame: kSigW x kSigH average luminances (0-255).
constexpr int kSigW = 32, kSigH = 18;
using FrameSignature = std::vector<uint8_t>;

// Averages the kSigW x kSigH cells of a packed RGB image (width w, height h, rows top to bottom).
FrameSignature frame_signature(const uint8_t* rgb, int w, int h);
// Mean absolute difference of two signatures, as a fraction of full scale (0 = identical, 1 = black
// against white). Signatures of different sizes differ completely (1).
double signature_diff(const FrameSignature& a, const FrameSignature& b);

// What the host reports each frame (retrace tick).
struct Observation {
    int frame = 0;                          // 1-based count of calls to step()
    double seconds = 0;                     // wall time since the run started
    uint64_t presented = 0;                 // frames presented to the window so far
    const FrameSignature* sig = nullptr;    // latest signature computed (null before the first)
    uint64_t sig_at = 0;                    // value of `presented` when `sig` was computed
    std::vector<std::string> files;         // disc files opened since the previous step, in order
};

enum class CmdKind { At, Press, Wait, WaitSeconds, WaitPresented, WaitFile, WaitChange, WaitStill, Shot, PressUntilFile, TraceFrame, Stick };

struct Command {
    CmdKind kind = CmdKind::Wait;
    uint16_t mask = 0;       // Press: PAD_BUTTON_* bits
    int8_t sx = 0, sy = 0;   // Stick: deflection (SDK units, -128..127, y up)
    bool sub = false;        // Stick: the C-stick instead of the main stick
    std::vector<uint16_t> cycle;  // PressUntilFile: the presses, used in turn
    int count = 0;           // At: frame; Press: hold frames; Wait*/WaitStill: how many
    int every = 120;         // PressUntilFile: frames between presses
    double seconds = 0;      // WaitSeconds: duration
    double threshold = 0.05; // WaitChange / WaitStill: signature difference that counts as a change
    double timeout = 0;      // seconds of wall time after which a wait gives up (0 = never)
    std::string text;        // WaitFile: path substring; Shot: file name; (the source text of the command otherwise)
    std::string source;      // the command as written, for log lines
};

class Script {
public:
    // Parses `text`; commands are separated by ';', ',' or a newline, '#' starts a comment. Returns
    // false and sets `error` (naming the command) on a malformed script; the script is then empty.
    bool parse(const char* text, std::string& error);

    // Runs every command that is ready, then returns the buttons held this frame.
    uint16_t step(const Observation& o);
    // The sticks held this frame, valid after step(): main stick and C-stick, SDK units (-128..127, y up).
    struct Sticks { int8_t x = 0, y = 0, sub_x = 0, sub_y = 0; };
    const Sticks& sticks() const { return sticks_; }

    bool empty() const { return cmds_.empty(); }
    bool finished() const { return pc_ >= cmds_.size(); }
    // True while the current wait compares pictures (the host then reads the frame back).
    bool needs_signature() const;
    // Frames to trace (GP_TRACE_FRAME style) requested since the last call, as "the Nth next frame".
    std::vector<int> take_traces();
    // Screenshot names requested since the last call.
    std::vector<std::string> take_shots();
    // One line per command that started a wait or ran, for the host's log (drained by the caller).
    std::vector<std::string> take_log();

    const std::vector<Command>& commands() const { return cmds_; }

private:
    struct Hold { uint16_t mask; int until; int8_t sx = 0, sy = 0; bool stick = false, sub = false; };
    std::vector<Command> cmds_;
    size_t pc_ = 0;
    bool started_ = false;
    // State of the running wait.
    int start_frame_ = 0;
    double start_seconds_ = 0;
    uint64_t start_presented_ = 0;
    FrameSignature ref_;
    uint64_t last_sig_presented_ = 0;
    int still_count_ = 0;
    int next_press_ = 0;
    size_t cycle_pos_ = 0;
    size_t file_cursor_ = 0;  // index in the current Observation::files not yet consumed
    std::vector<Hold> holds_;
    Sticks sticks_;
    std::vector<std::string> shots_, log_;
    std::vector<int> traces_;
};

}  // namespace gcrecomp::gp::input
