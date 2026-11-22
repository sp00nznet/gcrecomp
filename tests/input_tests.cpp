// SPDX-License-Identifier: MIT
// Unit tests for src/gp/input.cpp and the PAD replacements, with fake input state (no window,
// no device, no disc).
#include <SDL3/SDL_scancode.h>

#include <cstdio>
#include <cstring>

#include "gcrecomp/runtime.h"
#include "input.h"

namespace gcrecomp {
RecompiledFunc lookup_pad_func(const char* name);
}

using namespace gcrecomp;
using namespace gcrecomp::gp::input;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

static void test_stick_scaling() {
    int8_t x, y;
    scale_stick(0, 0, x, y);
    CHECK(x == 0 && y == 0);
    scale_stick(kStickDeadZone, 0, x, y);  // on the dead zone edge
    CHECK(x == 0 && y == 0);
    scale_stick(32767, 0, x, y);
    CHECK(x == 127 && y == 0);
    scale_stick(-32768, 0, x, y);
    CHECK(x == -128 && y == 0);
    scale_stick(0, -32768, x, y);  // SDL up is negative, SDK up is positive
    CHECK(x == 0 && y == 127);
    scale_stick(0, 32767, x, y);
    CHECK(x == 0 && y == -128);
    scale_stick(-32768, -32768, x, y);  // a diagonal corner is clamped to the circle, not past it
    CHECK(x < 0 && y > 0 && x >= -128 && y <= 127);
    CHECK(-x == y + 1 || -x == y);
    scale_stick(kStickDeadZone + 1500, 0, x, y);  // just past the dead zone: small, not a jump
    CHECK(x > 0 && x < 10);
    int8_t px, py, nx, ny;  // symmetric
    scale_stick(12345, 6789, px, py);
    scale_stick(-12345, -6789, nx, ny);
    CHECK(std::abs(px + nx) <= 1 && std::abs(py + ny) <= 1);
}

static void test_trigger_scaling() {
    CHECK(scale_trigger(0) == 0);
    CHECK(scale_trigger(-5) == 0);
    CHECK(scale_trigger(32767) == 255);
    CHECK(scale_trigger(16384) == 128);
    uint8_t prev = 0;
    for (int v = 0; v <= 32767; v += 17) {  // monotonic
        const uint8_t t = scale_trigger(int16_t(v));
        CHECK(t >= prev);
        prev = t;
    }
}

static void test_gamepad_layout() {
    GamepadSnapshot g;
    PadState p = gamepad_to_pad(g);
    CHECK(is_neutral(p));

    g.south = true;
    CHECK(gamepad_to_pad(g).buttons == kA);
    g = {};
    g.west = true;
    CHECK(gamepad_to_pad(g).buttons == kB);
    g = {};
    g.east = true;
    CHECK(gamepad_to_pad(g).buttons == kX);
    g = {};
    g.north = true;
    CHECK(gamepad_to_pad(g).buttons == kY);
    g = {};
    g.start = g.right_shoulder = true;
    CHECK(gamepad_to_pad(g).buttons == (kStart | kZ));
    g = {};
    g.dpad_up = g.dpad_left = true;
    CHECK(gamepad_to_pad(g).buttons == (kUp | kLeft));
    g = {};
    g.dpad_down = g.dpad_right = true;
    CHECK(gamepad_to_pad(g).buttons == (kDown | kRight));

    // Analog triggers: value always reported, digital bit only past the click point.
    g = {};
    g.left_trigger = 8192;  // ~64/255
    p = gamepad_to_pad(g);
    CHECK(p.trigger_l == scale_trigger(8192) && !(p.buttons & kL));
    g.left_trigger = 32767;
    g.right_trigger = 30000;
    p = gamepad_to_pad(g);
    CHECK(p.trigger_l == 255 && (p.buttons & kL));
    CHECK(p.trigger_r == scale_trigger(30000) && (p.buttons & kR));
    g = {};
    g.left_shoulder = true;  // pads without an analog trigger path still reach L
    p = gamepad_to_pad(g);
    CHECK(p.trigger_l == 255 && (p.buttons & kL));

    g = {};
    g.left_x = 32767;
    g.right_y = -32768;
    p = gamepad_to_pad(g);
    CHECK(p.stick_x == 127 && p.stick_y == 0 && p.substick_x == 0 && p.substick_y == 127);
}

static bool press_all(const KeyMap& m, Control c, PadState& out) {
    bool keys[SDL_SCANCODE_COUNT] = {};
    for (int k : m.keys[c]) keys[k] = true;
    out = keyboard_to_pad(m, keys, SDL_SCANCODE_COUNT);
    return !m.keys[c].empty();
}

static void test_keyboard_defaults() {
    const KeyMap m = default_keymap();
    bool keys[SDL_SCANCODE_COUNT] = {};
    CHECK(is_neutral(keyboard_to_pad(m, keys, SDL_SCANCODE_COUNT)));

    auto one = [&](SDL_Scancode sc) {
        bool k[SDL_SCANCODE_COUNT] = {};
        k[sc] = true;
        return keyboard_to_pad(m, k, SDL_SCANCODE_COUNT);
    };
    CHECK(one(SDL_SCANCODE_RETURN).buttons == kA);
    CHECK(one(SDL_SCANCODE_Z).buttons == kA);
    CHECK(one(SDL_SCANCODE_X).buttons == kB);
    CHECK(one(SDL_SCANCODE_BACKSPACE).buttons == kB);
    CHECK(one(SDL_SCANCODE_C).buttons == kX);
    CHECK(one(SDL_SCANCODE_V).buttons == kY);
    CHECK(one(SDL_SCANCODE_SPACE).buttons == kStart);
    CHECK(one(SDL_SCANCODE_W).buttons == kZ);
    PadState l = one(SDL_SCANCODE_Q), r = one(SDL_SCANCODE_E);
    CHECK(l.buttons == kL && l.trigger_l == 255 && r.buttons == kR && r.trigger_r == 255);
    PadState up = one(SDL_SCANCODE_UP);  // arrows are D-pad and control stick
    CHECK(up.buttons == kUp && up.stick_y == 80 && up.stick_x == 0);
    PadState left = one(SDL_SCANCODE_LEFT);
    CHECK(left.buttons == kLeft && left.stick_x == -80);
    PadState down = one(SDL_SCANCODE_DOWN);
    CHECK(down.buttons == kDown && down.stick_y == -80);
    PadState right = one(SDL_SCANCODE_RIGHT);
    CHECK(right.buttons == kRight && right.stick_x == 80);
    keys[SDL_SCANCODE_LEFT] = keys[SDL_SCANCODE_RIGHT] = true;  // opposite keys cancel
    CHECK(keyboard_to_pad(m, keys, SDL_SCANCODE_COUNT).stick_x == 0);
    // A key index past the end of the state array must not be read.
    CHECK(is_neutral(keyboard_to_pad(m, keys, 4)));
}

static void test_keymap_toml() {
    KeyMap m = default_keymap();
    std::string err;
    const char* toml =
        "# my layout\n"
        "[keyboard]\n"
        "a = \"J\"   # single key\n"
        "b = [\"K\", \"Left Shift\"]\n"
        "dpad_up = []\n"
        "cstick_left = [\"Keypad 4\"]\n";
    CHECK(parse_keymap(toml, m, err));
    CHECK(m.keys[kCtlA] == std::vector<int>{SDL_SCANCODE_J});
    CHECK((m.keys[kCtlB] == std::vector<int>{SDL_SCANCODE_K, SDL_SCANCODE_LSHIFT}));
    CHECK(m.keys[kCtlDpadUp].empty());
    CHECK(m.keys[kCtlStickUp] == default_keymap().keys[kCtlStickUp]);  // untouched keeps its default
    CHECK(m.keys[kCtlStart] == default_keymap().keys[kCtlStart]);
    PadState p;
    CHECK(press_all(m, kCtlCStickLeft, p) && p.substick_x == -80);
    CHECK(press_all(m, kCtlA, p) && p.buttons == kA);

    // Errors name the line and leave the map unchanged.
    const KeyMap before = m;
    CHECK(!parse_keymap("[keyboard]\na = \"J\"\nb = \"NoSuchKey\"\n", m, err));
    CHECK(err.rfind("line 3", 0) == 0);
    CHECK(m.keys[kCtlA] == before.keys[kCtlA]);  // the valid line before the error was not applied
    CHECK(!parse_keymap("a = \"J\"\n", m, err) && err.rfind("line 1", 0) == 0);
    CHECK(!parse_keymap("[keyboard]\nturbo = \"J\"\n", m, err) && err.rfind("line 2", 0) == 0);
    CHECK(!parse_keymap("[keyboard]\na = [\"J\"\n", m, err));
    CHECK(!parse_keymap("[pad]\n", m, err));
    CHECK(!parse_keymap("[keyboard]\na = \"J\" junk\n", m, err));
    CHECK(parse_keymap("", m, err));  // an empty file changes nothing
    CHECK(parse_keymap("[keyboard]\r\na = \"J\"\r\n", m, err));  // CRLF
}

static void test_port_assignment() {
    PortAssigner ports;
    CHECK(ports.port_for(10, false) == -1);  // idle, unassigned devices take no port
    CHECK(ports.port_for(20, true) == 0);    // first one used gets port 0, whatever its id
    CHECK(ports.port_for(10, true) == 1);
    CHECK(ports.port_for(20, false) == 0);   // stays assigned while idle
    CHECK(ports.port_for(30, true) == 2);
    CHECK(ports.port_for(40, true) == 3);
    CHECK(ports.port_for(50, true) == -1);   // no fifth port
    ports.release(10);                       // unplugged: its port is free again, others keep theirs
    CHECK(ports.port_for(20, false) == 0 && ports.port_for(30, false) == 2);
    CHECK(ports.port_for(50, true) == 1);
}

// The first script format ("FRAME:BUTTON,...", five frames each) still works; the rest of the language
// is in input_script_test.cpp.
static void test_script() {
    Script script;
    std::string error;
    CHECK(script.parse("10:A,20:S,30:U,40:Q,50:R,52:B", error));
    auto at = [&](int frame) {
        Observation o;
        o.frame = frame;
        return script.step(o);
    };
    uint16_t held[60] = {};
    for (int f = 1; f < 60; ++f) held[f] = at(f);
    CHECK(held[9] == 0);
    CHECK(held[10] == kA && held[14] == kA);
    CHECK(held[15] == 0);
    CHECK(held[20] == kStart);
    CHECK(held[30] == kUp);
    CHECK(held[40] == 0);  // unknown button holds nothing
    CHECK(held[50] == kRight);
    CHECK(held[53] == (kRight | kB));  // overlapping presses combine
    Script none;
    CHECK(none.parse(nullptr, error) && none.empty());
    CHECK(none.parse("", error) && none.empty());
}

static void test_pad_replacements() {
    RecompiledFunc motor = lookup_pad_func("PADControlMotor");
    RecompiledFunc all = lookup_pad_func("PADControlAllMotors");
    RecompiledFunc read = lookup_pad_func("PADRead");
    CHECK(motor && all && read);

    PPCContext ctx = {};
    Memory mem;
    CHECK(mem.init(1 << 20));

    for (PadState& p : g_pad) p.rumble = false;
    ctx.r[3] = 2;
    ctx.r[4] = 1;
    motor(&ctx, &mem);
    CHECK(g_pad[2].rumble && !g_pad[0].rumble);
    ctx.r[4] = 2;  // stop-and-brake also stops
    motor(&ctx, &mem);
    CHECK(!g_pad[2].rumble);
    ctx.r[3] = 9;  // out of range channel is ignored
    ctx.r[4] = 1;
    motor(&ctx, &mem);
    CHECK(!g_pad[0].rumble && !g_pad[1].rumble && !g_pad[2].rumble && !g_pad[3].rumble);

    const uint32_t cmds = 0x80001000;
    const uint32_t v[4] = {1, 0, 1, 2};
    for (int i = 0; i < 4; ++i) mem.write32(cmds + i * 4, v[i]);
    ctx.r[3] = cmds;
    all(&ctx, &mem);
    CHECK(g_pad[0].rumble && !g_pad[1].rumble && g_pad[2].rumble && !g_pad[3].rumble);

    // PADRead: connected ports carry their state, the others report PAD_ERR_NO_CONTROLLER (-1).
    for (PadState& p : g_pad) p = PadState();
    g_pad[0].connected = true;
    g_pad[0].buttons = kA | kStart;
    g_pad[0].stick_x = -100;
    g_pad[0].stick_y = 55;
    g_pad[0].substick_x = 127;
    g_pad[0].substick_y = -128;
    g_pad[0].trigger_l = 200;
    g_pad[0].trigger_r = 7;
    g_pad[2].connected = true;
    g_pad[2].buttons = kB;
    const uint32_t st = 0x80002000;
    ctx.r[3] = st;
    read(&ctx, &mem);
    CHECK(mem.read16(st) == (kA | kStart));
    CHECK(int8_t(mem.read8(st + 2)) == -100 && int8_t(mem.read8(st + 3)) == 55);
    CHECK(int8_t(mem.read8(st + 4)) == 127 && int8_t(mem.read8(st + 5)) == -128);
    CHECK(mem.read8(st + 6) == 200 && mem.read8(st + 7) == 7);
    CHECK(int8_t(mem.read8(st + 10)) == 0);
    CHECK(int8_t(mem.read8(st + 12 + 10)) == -1 && mem.read16(st + 12) == 0);
    CHECK(mem.read16(st + 24) == kB && int8_t(mem.read8(st + 24 + 10)) == 0);
    CHECK(int8_t(mem.read8(st + 36 + 10)) == -1);
    mem.shutdown();
}

int main() {
    test_stick_scaling();
    test_trigger_scaling();
    test_gamepad_layout();
    test_keyboard_defaults();
    test_keymap_toml();
    test_port_assignment();
    test_script();
    test_pad_replacements();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all input tests passed");
    return 0;
}
