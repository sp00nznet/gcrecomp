#pragma once
// =============================================================================
// gcrecomp - GameCube Static Recompilation Toolkit
// Input System Header
//
// Maps keyboard/mouse/gamepad input to GameCube controller state.
// The GameCube controller has:
//   - 2 analog sticks (main stick + C-stick)
//   - A, B, X, Y buttons
//   - Z trigger button
//   - L and R analog triggers (with digital click at full press)
//   - D-pad (4 directions)
//   - Start button
//   - Rumble motor
//
// This module is game-agnostic. The default keyboard mapping can be
// customized per game by the integrating project.
//
// References:
//   - libogc — PADStatus structure and PAD button bit definitions
//   - Pureikyubu — Serial Interface (SI) controller documentation
// =============================================================================

#include <cstdint>

namespace gcrecomp::input {

/// GameCube controller button bit flags.
/// These match the PADStatus button field layout from the Nintendo SDK
/// and libogc. The bit positions are fixed by the hardware SI protocol.
/// Reference: libogc pad.h PAD_BUTTON_* constants.
enum GCButton : uint16_t {
    PAD_BUTTON_LEFT  = 0x0001, ///< D-pad left
    PAD_BUTTON_RIGHT = 0x0002, ///< D-pad right
    PAD_BUTTON_DOWN  = 0x0004, ///< D-pad down
    PAD_BUTTON_UP    = 0x0008, ///< D-pad up
    PAD_TRIGGER_Z    = 0x0010, ///< Z trigger button
    PAD_TRIGGER_R    = 0x0020, ///< R trigger (digital)
    PAD_TRIGGER_L    = 0x0040, ///< L trigger (digital)
    PAD_BUTTON_A     = 0x0100, ///< A button (main action)
    PAD_BUTTON_B     = 0x0200, ///< B button (secondary action)
    PAD_BUTTON_X     = 0x0400, ///< X button
    PAD_BUTTON_Y     = 0x0800, ///< Y button
    PAD_BUTTON_START = 0x1000, ///< Start/Pause button
};

/// Mirrors the GameCube PADStatus struct layout.
/// This structure is filled by the input system each frame and can be
/// written directly into recompiled game memory at the PADRead call site.
/// Reference: libogc pad.h PADStatus structure.
struct PadStatus {
    uint16_t button;        ///< Button bit flags (OR of GCButton values)
    int8_t   stick_x;       ///< Main stick X axis (-128 to 127)
    int8_t   stick_y;       ///< Main stick Y axis (-128 to 127)
    int8_t   substick_x;    ///< C-stick X axis (-128 to 127)
    int8_t   substick_y;    ///< C-stick Y axis (-128 to 127)
    uint8_t  trigger_l;     ///< L trigger analog value (0-255)
    uint8_t  trigger_r;     ///< R trigger analog value (0-255)
    uint8_t  analog_a;      ///< A button pressure (rarely used by games)
    uint8_t  analog_b;      ///< B button pressure (rarely used by games)
    int8_t   err;           ///< Error status (0 = OK, negative = error)
};

/// Default keyboard mapping:
///   WASD        = Main stick (character movement)
///   Arrow keys  = C-stick (camera control)
///   Space       = A button
///   Left Shift  = B button
///   E           = X button
///   Q           = Y button
///   R           = Z trigger
///   Tab         = R trigger
///   F           = L trigger
///   Enter       = Start
///   XInput gamepad is also supported when connected.

/// Initialize the input system.
bool input_init();

/// Shut down the input system.
void input_shutdown();

/// Poll input devices and update pad state (call once per frame).
void input_update();

/// Get the current pad state for a controller port (0-3).
PadStatus input_get_pad(int controller = 0);

/// Check if a button was just pressed this frame (rising edge).
bool input_pressed(GCButton btn, int controller = 0);

/// Check if a button was just released this frame (falling edge).
bool input_released(GCButton btn, int controller = 0);

/// Control the rumble motor for a controller.
/// @param controller Controller port (0-3)
/// @param on True to start rumble, false to stop
void input_set_rumble(int controller, bool on);

} // namespace gcrecomp::input
