// SPDX-License-Identifier: MIT
#include "input.h"

#include "gcrecomp/gp/gp.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gcrecomp::gp::input {

// ---- Scaling and gamepad layout -----------------------------------------------------------------

void scale_stick(int16_t sdl_x, int16_t sdl_y, int8_t& x, int8_t& y) {
    const double fx = sdl_x, fy = -double(sdl_y);  // SDL's y grows downwards, the SDK's upwards
    const double mag = std::hypot(fx, fy);
    if (mag <= kStickDeadZone) {
        x = y = 0;
        return;
    }
    const double k = std::min(1.0, (mag - kStickDeadZone) / (32767.0 - kStickDeadZone)) / mag;
    auto to_i8 = [](double v) { return int8_t(std::clamp(std::lround(v >= 0 ? v * 127.0 : v * 128.0), -128L, 127L)); };
    x = to_i8(fx * k);
    y = to_i8(fy * k);
}

uint8_t scale_trigger(int16_t sdl) {
    if (sdl <= 0) return 0;
    return uint8_t((int(sdl) * 255 + 16383) / 32767);
}

PadState gamepad_to_pad(const GamepadSnapshot& g) {
    PadState p;
    uint16_t b = 0;
    if (g.dpad_left) b |= kLeft;
    if (g.dpad_right) b |= kRight;
    if (g.dpad_down) b |= kDown;
    if (g.dpad_up) b |= kUp;
    if (g.south) b |= kA;
    if (g.west) b |= kB;
    if (g.east) b |= kX;
    if (g.north) b |= kY;
    if (g.start) b |= kStart;
    if (g.right_shoulder) b |= kZ;
    p.trigger_l = g.left_shoulder ? 255 : scale_trigger(g.left_trigger);
    p.trigger_r = scale_trigger(g.right_trigger);
    if (p.trigger_l >= kTriggerClick) b |= kL;
    if (p.trigger_r >= kTriggerClick) b |= kR;
    p.buttons = b;
    scale_stick(g.left_x, g.left_y, p.stick_x, p.stick_y);
    scale_stick(g.right_x, g.right_y, p.substick_x, p.substick_y);
    return p;
}

// ---- Keyboard -----------------------------------------------------------------------------------

namespace {

const char* const kControlNames[kControlCount] = {
    "a", "b", "x", "y", "start", "z", "l", "r",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "stick_up", "stick_down", "stick_left", "stick_right",
    "cstick_up", "cstick_down", "cstick_left", "cstick_right",
};

constexpr int kKeyboardStick = 80;

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }

KeyMap default_keymap() {
    KeyMap m;
    m.keys[kCtlA] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_Z};
    m.keys[kCtlB] = {SDL_SCANCODE_X, SDL_SCANCODE_BACKSPACE};
    m.keys[kCtlX] = {SDL_SCANCODE_C};
    m.keys[kCtlY] = {SDL_SCANCODE_V};
    m.keys[kCtlStart] = {SDL_SCANCODE_SPACE};
    m.keys[kCtlZ] = {SDL_SCANCODE_W};
    m.keys[kCtlL] = {SDL_SCANCODE_Q};
    m.keys[kCtlR] = {SDL_SCANCODE_E};
    m.keys[kCtlDpadUp] = m.keys[kCtlStickUp] = {SDL_SCANCODE_UP};
    m.keys[kCtlDpadDown] = m.keys[kCtlStickDown] = {SDL_SCANCODE_DOWN};
    m.keys[kCtlDpadLeft] = m.keys[kCtlStickLeft] = {SDL_SCANCODE_LEFT};
    m.keys[kCtlDpadRight] = m.keys[kCtlStickRight] = {SDL_SCANCODE_RIGHT};
    return m;
}

namespace {

struct Cursor {
    const std::string& s;
    size_t i = 0;
    bool done() const { return i >= s.size(); }
    char peek() const { return s[i]; }
    void skip_space() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
};

bool parse_string(Cursor& c, std::string& out, std::string& err) {
    if (c.done() || c.peek() != '"') {
        err = "expected a quoted string";
        return false;
    }
    ++c.i;
    out.clear();
    while (!c.done() && c.peek() != '"') {
        char ch = c.s[c.i++];
        if (ch == '\\') {
            if (c.done()) break;
            const char e = c.s[c.i++];
            if (e == '"' || e == '\\') ch = e;
            else if (e == 't') ch = '\t';
            else {
                err = "unsupported escape sequence";
                return false;
            }
        }
        out += ch;
    }
    if (c.done()) {
        err = "unterminated string";
        return false;
    }
    ++c.i;
    return true;
}

// Rest of the line after a value: spaces and an optional comment.
bool end_of_value(Cursor& c, std::string& err) {
    c.skip_space();
    if (!c.done() && c.peek() != '#' && c.peek() != '\r') {
        err = "unexpected text after the value";
        return false;
    }
    return true;
}

bool parse_value(Cursor& c, std::vector<std::string>& out, std::string& err) {
    out.clear();
    c.skip_space();
    if (!c.done() && c.peek() == '[') {
        ++c.i;
        for (;;) {
            c.skip_space();
            if (c.done()) {
                err = "unterminated array (arrays must fit on one line)";
                return false;
            }
            if (c.peek() == ']') {
                ++c.i;
                break;
            }
            std::string v;
            if (!parse_string(c, v, err)) return false;
            out.push_back(v);
            c.skip_space();
            if (!c.done() && c.peek() == ',') ++c.i;
            else if (c.done() || c.peek() != ']') {
                err = "expected ',' or ']'";
                return false;
            }
        }
        return end_of_value(c, err);
    }
    std::string v;
    if (!parse_string(c, v, err)) return false;
    out.push_back(v);
    return end_of_value(c, err);
}

}  // namespace

bool parse_keymap(const std::string& toml, KeyMap& map, std::string& error) {
    KeyMap result = map;
    std::istringstream in(toml);
    std::string line;
    bool in_keyboard = false;
    for (int n = 1; std::getline(in, line); ++n) {
        std::string err;
        auto fail = [&](const std::string& what) {
            error = "line " + std::to_string(n) + ": " + what;
            return false;
        };
        Cursor c{line};
        c.skip_space();
        if (c.done() || c.peek() == '#' || c.peek() == '\r') continue;
        if (c.peek() == '[') {
            const size_t close = line.find(']', c.i);
            if (close == std::string::npos) return fail("unterminated table header");
            std::string name = line.substr(c.i + 1, close - c.i - 1);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (name != "keyboard") return fail("unknown table [" + name + "]");
            in_keyboard = true;
            c.i = close + 1;
            if (!end_of_value(c, err)) return fail(err);
            continue;
        }
        if (!in_keyboard) return fail("key outside the [keyboard] table");
        const size_t eq = line.find('=', c.i);
        if (eq == std::string::npos) return fail("expected 'name = value'");
        std::string key = line.substr(c.i, eq - c.i);
        key.erase(key.find_last_not_of(" \t") + 1);
        int control = -1;
        for (int i = 0; i < kControlCount; ++i)
            if (key == kControlNames[i]) control = i;
        if (control < 0) return fail("unknown control '" + key + "'");
        c.i = eq + 1;
        std::vector<std::string> names;
        if (!parse_value(c, names, err)) return fail(err);
        std::vector<int> codes;
        for (const std::string& name : names) {
            const SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) return fail("unknown key name \"" + name + "\"");
            codes.push_back(int(sc));
        }
        result.keys[control] = std::move(codes);
    }
    map = std::move(result);
    return true;
}

PadState keyboard_to_pad(const KeyMap& map, const bool* keys, int count) {
    auto down = [&](Control c) {
        for (int k : map.keys[c])
            if (k >= 0 && k < count && keys[k]) return true;
        return false;
    };
    PadState p;
    uint16_t b = 0;
    if (down(kCtlDpadLeft)) b |= kLeft;
    if (down(kCtlDpadRight)) b |= kRight;
    if (down(kCtlDpadDown)) b |= kDown;
    if (down(kCtlDpadUp)) b |= kUp;
    if (down(kCtlZ)) b |= kZ;
    if (down(kCtlR)) b |= kR;
    if (down(kCtlL)) b |= kL;
    if (down(kCtlA)) b |= kA;
    if (down(kCtlB)) b |= kB;
    if (down(kCtlX)) b |= kX;
    if (down(kCtlY)) b |= kY;
    if (down(kCtlStart)) b |= kStart;
    p.buttons = b;
    auto axis = [&](Control pos, Control neg) { return int8_t((down(pos) ? kKeyboardStick : 0) - (down(neg) ? kKeyboardStick : 0)); };
    p.stick_x = axis(kCtlStickRight, kCtlStickLeft);
    p.stick_y = axis(kCtlStickUp, kCtlStickDown);
    p.substick_x = axis(kCtlCStickRight, kCtlCStickLeft);
    p.substick_y = axis(kCtlCStickUp, kCtlCStickDown);
    p.trigger_l = (b & kL) ? 255 : 0;
    p.trigger_r = (b & kR) ? 255 : 0;
    return p;
}

bool is_neutral(const PadState& p) {
    return p.buttons == 0 && p.stick_x == 0 && p.stick_y == 0 && p.substick_x == 0 && p.substick_y == 0 &&
           p.trigger_l == 0 && p.trigger_r == 0;
}

// ---- Ports --------------------------------------------------------------------------------------

int PortAssigner::port_for(uint32_t device, bool active) {
    for (int i = 0; i < 4; ++i)
        if (owner_[i] == device) return i;
    if (!active) return -1;
    for (int i = 0; i < 4; ++i) {
        if (owner_[i] == 0) {
            owner_[i] = device;
            return i;
        }
    }
    return -1;
}

void PortAssigner::release(uint32_t device) {
    for (uint32_t& o : owner_)
        if (o == device) o = 0;
}

// ---- SDL layer ----------------------------------------------------------------------------------

namespace {

constexpr uint32_t kKeyboardDevice = 0xFFFFFFFFu;  // SDL joystick ids are small; this one is never one

struct Device {
    SDL_JoystickID id;
    SDL_Gamepad* pad;
};

KeyMap g_keymap = default_keymap();
std::vector<Device> g_gamepads;
PortAssigner g_ports;
Script g_script;
FrameSignature g_signature;
uint64_t g_presented = 0, g_signature_at = 0;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
int g_frame = 0;
bool g_rumbling[4] = {};

GamepadSnapshot read_gamepad(SDL_Gamepad* g) {
    GamepadSnapshot s;
    auto b = [&](SDL_GamepadButton btn) { return SDL_GetGamepadButton(g, btn); };
    auto a = [&](SDL_GamepadAxis ax) { return SDL_GetGamepadAxis(g, ax); };
    s.south = b(SDL_GAMEPAD_BUTTON_SOUTH);
    s.east = b(SDL_GAMEPAD_BUTTON_EAST);
    s.west = b(SDL_GAMEPAD_BUTTON_WEST);
    s.north = b(SDL_GAMEPAD_BUTTON_NORTH);
    s.start = b(SDL_GAMEPAD_BUTTON_START);
    s.left_shoulder = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    s.right_shoulder = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    s.dpad_up = b(SDL_GAMEPAD_BUTTON_DPAD_UP);
    s.dpad_down = b(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    s.dpad_left = b(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    s.dpad_right = b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    s.left_x = a(SDL_GAMEPAD_AXIS_LEFTX);
    s.left_y = a(SDL_GAMEPAD_AXIS_LEFTY);
    s.right_x = a(SDL_GAMEPAD_AXIS_RIGHTX);
    s.right_y = a(SDL_GAMEPAD_AXIS_RIGHTY);
    s.left_trigger = a(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    s.right_trigger = a(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    return s;
}

void open_gamepad(SDL_JoystickID id) {
    for (const Device& d : g_gamepads)
        if (d.id == id) return;
    if (SDL_Gamepad* pad = SDL_OpenGamepad(id)) {
        g_gamepads.push_back({id, pad});
        std::fprintf(stderr, "[input] gamepad connected: %s\n", SDL_GetGamepadName(pad));
    }
}

void stop_rumble(int port) {
    if (!g_rumbling[port]) return;
    g_rumbling[port] = false;
    for (const Device& d : g_gamepads)
        if (d.id == g_ports.owner(port)) SDL_RumbleGamepad(d.pad, 0, 0, 0);
}


// TOS_SCRIPT is the script text, or "@path" to read it from a file.
void load_script() {
    const char* env = std::getenv("TOS_SCRIPT");
    g_start = std::chrono::steady_clock::now();
    if (!env || !*env) return;
    std::string text = env;
    if (env[0] == '@') {
        std::ifstream f(env + 1);
        if (!f) {
            std::fprintf(stderr, "[script] cannot read %s\n", env + 1);
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    std::string error;
    if (!g_script.parse(text.c_str(), error)) std::fprintf(stderr, "[script] %s\n", error.c_str());
    else std::fprintf(stderr, "[script] %zu command(s) loaded\n", g_script.commands().size());
}

uint16_t run_script() {
    if (g_script.empty()) return 0;
    Observation o;
    o.frame = g_frame;
    o.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    o.presented = g_presented;
    o.sig = g_signature.empty() ? nullptr : &g_signature;
    o.sig_at = g_signature_at;
    take_opened_disc_files(o.files);
    for (const std::string& f : o.files) std::fprintf(stderr, "[script] frame %d: game opened %s\n", g_frame, f.c_str());
    const uint16_t buttons = g_script.step(o);
    for (int n : g_script.take_traces()) request_trace_frame(uint64_t(n));
    for (const std::string& line : g_script.take_log()) std::fprintf(stderr, "[script] %s\n", line.c_str());
    return buttons;
}

}  // namespace

void init(const char* toml_path) {
    g_keymap = default_keymap();
    if (toml_path && *toml_path) {
        std::ifstream f(toml_path);
        if (f) {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string error;
            if (parse_keymap(ss.str(), g_keymap, error)) std::fprintf(stderr, "[input] keyboard map loaded from %s\n", toml_path);
            else std::fprintf(stderr, "[input] %s: %s; using the default keyboard map\n", toml_path, error.c_str());
        }
    }
    load_script();
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "[input] gamepads unavailable: %s\n", SDL_GetError());
        return;
    }
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; ++i) open_gamepad(ids[i]);
        SDL_free(ids);
    }
}

void shutdown() {
    for (int p = 0; p < 4; ++p) stop_rumble(p);
    for (const Device& d : g_gamepads) SDL_CloseGamepad(d.pad);
    g_gamepads.clear();
    g_ports = PortAssigner();
}

void handle_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
        open_gamepad(e.gdevice.which);
    } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (size_t i = 0; i < g_gamepads.size(); ++i) {
            if (g_gamepads[i].id != e.gdevice.which) continue;
            for (int p = 0; p < 4; ++p)
                if (g_ports.owner(p) == g_gamepads[i].id) g_rumbling[p] = false;  // the device is gone
            g_ports.release(g_gamepads[i].id);
            SDL_CloseGamepad(g_gamepads[i].pad);
            g_gamepads.erase(g_gamepads.begin() + long(i));
            std::fprintf(stderr, "[input] gamepad disconnected\n");
            break;
        }
    }
}

void update() {
    PadState next[4];
    auto place = [&](uint32_t device, const PadState& state) {
        const int port = g_ports.port_for(device, !is_neutral(state));
        if (port >= 0) next[port] = state;
    };
    int key_count = 0;
    const bool* keys = SDL_GetKeyboardState(&key_count);
    place(kKeyboardDevice, keyboard_to_pad(g_keymap, keys, key_count));
    for (const Device& d : g_gamepads) place(d.id, gamepad_to_pad(read_gamepad(d.pad)));

    ++g_frame;
    next[0].buttons |= run_script();

    for (int p = 0; p < 4; ++p) {
        const bool owned = g_ports.owner(p) != 0;
        PadState& out = g_pad[p];
        const bool rumble = out.rumble;
        out = next[p];
        out.rumble = rumble;
        out.connected = p == 0 || owned;
        if (!out.connected) out.rumble = false;
        if (out.rumble) {
            for (const Device& d : g_gamepads) {
                if (d.id != g_ports.owner(p)) continue;
                SDL_RumbleGamepad(d.pad, 0xC000, 0xC000, 250);  // refreshed every frame while requested
                g_rumbling[p] = true;
            }
        } else {
            stop_rumble(p);
        }
    }
}

bool wants_signature() { return g_script.needs_signature(); }

void notify_frame(const FrameSignature* sig) {
    ++g_presented;
    if (sig) {
        g_signature = *sig;
        g_signature_at = g_presented;
    }
}

std::vector<std::string> take_shot_requests() { return g_script.take_shots(); }

}  // namespace gcrecomp::gp::input
