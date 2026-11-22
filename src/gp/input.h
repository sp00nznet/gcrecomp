// SPDX-License-Identifier: MIT
//
// Host input -> GameCube controller state.
//
// The pure parts (scaling, keyboard map, TOML parsing, port assignment; the scripted input is in input_script.h) take plain data
// so they can be tested without a window or a device; the SDL part (`init`, `handle_event`,
// `update`) reads the keyboard and gamepads and writes gcrecomp::g_pad[0..3].
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gcrecomp/runtime.h"
#include "input_script.h"

union SDL_Event;

namespace gcrecomp::gp::input {

// PAD_BUTTON_* bits of PADStatus.button.
enum PadButton : uint16_t {
    kLeft = 0x0001, kRight = 0x0002, kDown = 0x0004, kUp = 0x0008,
    kZ = 0x0010, kR = 0x0020, kL = 0x0040,
    kA = 0x0100, kB = 0x0200, kX = 0x0400, kY = 0x0800, kStart = 0x1000,
};

// A trigger reads as pressed (digital L/R bit set) from this analog value on.
constexpr uint8_t kTriggerClick = 170;
// Radial dead zone of gamepad sticks, in SDL units (of 32767).
constexpr int kStickDeadZone = 4096;

// What a standard SDL gamepad reports; axes in SDL's units (sticks -32768..32767, up is negative;
// triggers 0..32767).
struct GamepadSnapshot {
    bool south = false, east = false, west = false, north = false;
    bool start = false, left_shoulder = false, right_shoulder = false;
    bool dpad_up = false, dpad_down = false, dpad_left = false, dpad_right = false;
    int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    int16_t left_trigger = 0, right_trigger = 0;
};

// SDL stick (x right, y down, -32768..32767) -> SDK stick (x right, y up, -128..127) with a radial
// dead zone that is rescaled so the output still reaches the extremes.
void scale_stick(int16_t sdl_x, int16_t sdl_y, int8_t& x, int8_t& y);
// SDL trigger 0..32767 -> 0..255.
uint8_t scale_trigger(int16_t sdl);

// Layout: south = A, west = B, east = X, north = Y (the GameCube's physical arrangement), start =
// Start, right shoulder = Z, analog triggers = L/R (the digital bit is set past kTriggerClick), left
// shoulder = L at full travel.
PadState gamepad_to_pad(const GamepadSnapshot& g);

// ---- Keyboard -------------------------------------------------------------------------------

enum Control {
    kCtlA, kCtlB, kCtlX, kCtlY, kCtlStart, kCtlZ, kCtlL, kCtlR,
    kCtlDpadUp, kCtlDpadDown, kCtlDpadLeft, kCtlDpadRight,
    kCtlStickUp, kCtlStickDown, kCtlStickLeft, kCtlStickRight,
    kCtlCStickUp, kCtlCStickDown, kCtlCStickLeft, kCtlCStickRight,
    kControlCount,
};

// Key names accepted in the TOML file, indexed by Control: "a", "dpad_up", "cstick_left", ...
const char* control_name(Control c);

// Scancodes (SDL_Scancode values) bound to each control.
struct KeyMap {
    std::vector<int> keys[kControlCount];
};

// Arrows = D-pad and control stick, Enter or Z = A, X or Backspace = B, C = X, V = Y, Space = Start,
// W = Z, Q = L, E = R.
KeyMap default_keymap();

// Applies a TOML text over `map`. Supported subset: `#` comments, a `[keyboard]` table, and
// `name = "Key"` or `name = ["Key", "Key"]` where Key is an SDL key name ("Return", "Left", "A",
// "Keypad 4", ...). Controls that are not mentioned keep their binding; `name = []` unbinds.
// Returns false and fills `error` ("line N: ...") on the first problem; `map` is then untouched.
bool parse_keymap(const std::string& toml, KeyMap& map, std::string& error);

// Keyboard state (SDL scancode-indexed, `count` entries) -> controller. Keyboard sticks deflect to 80.
PadState keyboard_to_pad(const KeyMap& map, const bool* keys, int count);

// True when nothing is pressed or deflected (used to decide when a device starts being "used").
bool is_neutral(const PadState& p);

// ---- Ports ----------------------------------------------------------------------------------

// Hands out the four ports in the order devices are first used. Device ids are arbitrary non-zero.
class PortAssigner {
public:
    // Port of `device`, assigning the lowest free one when `active` and it has none; -1 when it has
    // none (and is idle, or all ports are taken).
    int port_for(uint32_t device, bool active);
    void release(uint32_t device);
    uint32_t owner(int port) const { return owner_[port]; }

private:
    uint32_t owner_[4] = {};
};

// ---- SDL layer ------------------------------------------------------------------------------

// Opens gamepad support and loads the keyboard map from `toml_path` (may be null or missing: the
// defaults apply; a malformed file is reported on stderr and ignored).
void init(const char* toml_path);
void shutdown();
// Gamepad hot plug.
void handle_event(const SDL_Event& e);
// Once per frame: reads devices, runs the script (TOS_SCRIPT, see docs/INPUT_SCRIPT.md), writes
// g_pad and drives rumble.
void update();

// Script support for the renderer. notify_frame() is called for every presented frame, with the
// frame's signature when wants_signature() said it is needed (null otherwise). take_shot_requests()
// returns the screenshot names the script asked for since the last call.
bool wants_signature();
void notify_frame(const FrameSignature* sig);
std::vector<std::string> take_shot_requests();

}  // namespace gcrecomp::gp::input
