# Scripted input (`TOS_SCRIPT`)

A script presses buttons on controller port 1 and waits for events, so a run can drive a game through
menus without knowing at which frame each screen shows up. The engine is `src/gp/input_script.*` (pure,
unit-tested in `tests/input_script_test.cpp` without a window, a device or a disc); `src/gp/input.cpp`
feeds it once per retrace.

```
TOS_SCRIPT='wait-file mov/op; wait 300; press S' ./your_boot ...
TOS_SCRIPT=@script.txt ./your_boot ...             # the script is read from the file
```

The host program has to call `gp::pump_events()` once per frame, as the boot harnesses do. Everything the
script does is logged on stderr as `[script] frame N: <command>`, and so is every disc file the game opens (`[script] frame N: game opened PATH`), which gives a timeline to write the script against; a wait that gives up logs `timed out`.

## Format

Commands are separated by `;`, `,` or a newline. `#` starts a comment that runs to the end of the line.
Words are separated by blanks. Commands run in order; a *wait* blocks the following commands until its
condition holds, `press` does not block (the button is held while later commands run).

| Command | Meaning |
| --- | --- |
| `press BTN[+BTN...] [hold=N]` | Holds the buttons for `N` frames (default 5) from this frame. |
| `at N` | Waits until retrace frame `N` (1-based, counted from the start of the run). |
| `wait N` | Waits `N` retrace frames. |
| `wait-seconds S` | Waits `S` seconds of wall time. |
| `wait-presented N` | Waits until `N` more frames have been presented to the window. |
| `wait-file TEXT` | Waits until the game opens (DVDOpen / DVDFastOpen) a disc file whose path contains `TEXT`, ignoring case. Only opens after the wait began count; `TEXT` has no blanks. |
| `press-until-file TEXT BTN[+BTN...]... [every=N] [hold=N]` | Every `N` frames (default 120) presses the next of the listed buttons (held `hold` frames, default 10), in turn, until the game opens a matching file: for a menu that ignores a press made before it is ready, or a dialog that may or may not show (`D A` picks the second entry). |
| `wait-change [T]` | Waits until the picture differs from the one on screen when the wait began by more than `T` (default 0.05). |
| `wait-still N [T]` | Waits until `N` consecutive presented frames each differ from the previous one by at most `T` (default 0.05): the screen has settled. |
| `trace-frame [N]` | Traces the draws of the `N`th next presented frame (default 1, the frame in progress) as `GP_TRACE_FRAME` does (see DEBUGGING.md), so a frame reached by events can be inspected without knowing its number. |
| `shot NAME` | Writes the next presented frame as `NAME.ppm` in the screenshot directory (`TOS_SCREENSHOT_DIR` of the harness). |

Every wait accepts `timeout=SECONDS` (wall time), after which it is considered done, so one missed event
does not hang a run. `wait-change` and `wait-still` also accept `threshold=T` instead of the positional value.

Buttons: `A B X Y Z`, `S` or `start`, `U`/`up`, `D`/`down`, `L`/`left`, `R`/`right` (the single letters
`L` and `R` are the D-pad, as in the first format), `trigger-l`, `trigger-r`. Unknown names are an error.

The first format `FRAME:BUTTON,FRAME:BUTTON` (button held 5 frames) still works; it is `at FRAME; press
BUTTON`.

### The picture comparison

A presented frame is reduced to a 32x18 grid of average luminances. The difference of two pictures is the
mean absolute difference of the cells over full scale (0 identical, 1 black against white). So 0.05 is "a
dialog popped up or a scene changed", not "a sprite moved". The window is read back only while a
`wait-change` / `wait-still` is running, so other scripts cost nothing. The picture is the window's (after
scaling), so a screen saver or a compositor overlay does not matter but the window must be mapped.

### Example: the intro of a game with a start menu

```
wait-file mov/op.h4m timeout=120    # the movie starts
wait 120
press S                             # skip it
wait-still 30                       # the title screen has settled
shot title
press A hold=8
wait-change timeout=20              # the next screen
shot next
```

## Memory card

`GCRECOMP_CARD_IMAGE=path[:size]` inserts a card image in slot A for a host program that did not call
`set_card_image()` itself (a missing file starts as a blank, unformatted card the game offers to format).

## Tools

* `tools/run-scripted.sh`: runs a harness with a script file, a fresh card image, screenshots and a log.
* `tools/guest-stacks.sh PID [DEPTH]`: gdb backtraces of the guest threads of a running harness (guest call chain as `func_XXXXXXXX`).
* `tools/input-scripts/tos-new-game.txt`: example, Tales of Symphonia from a blank card to the first field scene.
