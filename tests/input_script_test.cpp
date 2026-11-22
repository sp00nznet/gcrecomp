// SPDX-License-Identifier: MIT
// Unit tests for the scripted input engine (src/gp/input_script.*): no window, no device, no disc.
#include <cstdio>
#include <string>

#include "input.h"

using namespace gcrecomp::gp::input;

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

// Drives a script frame by frame with a fake host.
struct Host {
    Script script;
    Observation o;
    FrameSignature sig;
    std::vector<std::string> pending_files;
    bool give_sig = true;

    explicit Host(const char* text) {
        std::string error;
        if (!script.parse(text, error)) std::fprintf(stderr, "parse: %s\n", error.c_str());
    }
    // One retrace tick; `present` says whether a new picture was shown during it.
    uint16_t tick(bool present = true, double dt = 1.0 / 60) {
        ++o.frame;
        o.seconds += dt;
        if (present) {
            ++o.presented;
            o.sig = give_sig ? &sig : nullptr;
            o.sig_at = o.presented;
        }
        o.files = pending_files;
        pending_files.clear();
        return script.step(o);
    }
};

static FrameSignature flat(uint8_t v) { return FrameSignature(size_t(kSigW) * kSigH, v); }

static void test_parse() {
    Script s;
    std::string e;
    CHECK(s.parse("press A; wait 10\npress S+A hold=3 # comment, with ; separators\n\n# only a comment\nwait-file mov/OP.h4m timeout=5", e));
    CHECK(s.commands().size() == 4);
    CHECK(s.commands()[0].kind == CmdKind::Press && s.commands()[0].mask == kA && s.commands()[0].count == 5);
    CHECK(s.commands()[1].kind == CmdKind::Wait && s.commands()[1].count == 10);
    CHECK(s.commands()[2].mask == (kStart | kA) && s.commands()[2].count == 3);
    CHECK(s.commands()[3].kind == CmdKind::WaitFile && s.commands()[3].text == "mov/op.h4m" && s.commands()[3].timeout == 5);

    CHECK(s.parse("wait-change; wait-change 0.2; wait-still 30; wait-still 30 0.01; wait-seconds 1.5; wait-presented 7; shot name", e));
    CHECK(s.commands().size() == 7);
    CHECK(s.commands()[0].threshold == 0.05 && s.commands()[1].threshold == 0.2);
    CHECK(s.commands()[3].count == 30 && s.commands()[3].threshold == 0.01);
    CHECK(s.commands()[4].seconds == 1.5);

    for (const char* bad : {"press", "press Q", "press A+", "wait", "wait x", "wait 1 2", "wait-file", "jump 3", "press A hold=0", "press A hold=x",
                            "wait 5 timeout=-1", "wait-still", "wait-still 0", "shot", "wait-change a b", "wait 3 colour=3"}) {
        CHECK(!s.parse(bad, e));
        CHECK(!e.empty() && s.empty());
    }
    CHECK(s.parse(nullptr, e) && s.empty());
}

static void test_press_and_wait() {
    Host h("press A; wait 10; press S hold=2; wait 3; press B+X");
    uint16_t held[40];
    for (auto& v : held) v = h.tick();
    CHECK(held[0] == kA && held[4] == kA && held[5] == 0);      // frames are 1-based: tick 0 is frame 1
    CHECK(held[10] == kStart && held[11] == kStart && held[12] == 0);
    CHECK(held[13] == (kB | kX) && held[17] == (kB | kX) && held[18] == 0);
    CHECK(h.script.finished());
}

static void test_at_and_seconds() {
    Host h("at 5; press A; wait-seconds 2; press B");
    uint16_t held[200];
    for (auto& v : held) v = h.tick();
    CHECK(held[3] == 0 && held[4] == kA);  // frame 5
    int first_b = -1;
    for (int i = 0; i < 200; ++i)
        if (held[i] & kB) { first_b = i + 1; break; }
    CHECK(first_b >= 5 + 119 && first_b <= 5 + 122);  // two seconds at 60 frames per second
}

static void test_wait_file() {
    Host h("wait-file mov/op; press S; wait-file Menu.dat; press A");
    for (int i = 0; i < 20; ++i) CHECK(h.tick() == 0);
    h.pending_files = {"s/other.bin", "mov/OP.h4m"};  // case-insensitive substring
    CHECK(h.tick() == kStart);
    for (int i = 0; i < 4; ++i) h.tick();
    CHECK(h.tick() == 0);
    h.pending_files = {"x/menu.dat"};
    CHECK(h.tick() == kA);

    // One batch can satisfy two consecutive waits only when it holds both files in order.
    Host in_order("wait-file a.bin; wait-file b.bin; press A");
    in_order.pending_files = {"a.bin", "b.bin"};
    CHECK(in_order.tick() == kA);
    Host reversed("wait-file a.bin; wait-file b.bin; press A");
    reversed.pending_files = {"b.bin", "a.bin"};
    CHECK(reversed.tick() == 0);
    reversed.pending_files = {"b.bin"};
    CHECK(reversed.tick() == kA);
    // A file seen before the wait began does not count.
    Host early("press B hold=1; wait 2; wait-file late.bin; press A");
    early.pending_files = {"late.bin"};
    early.tick();
    CHECK(early.tick() == 0 && early.tick() == 0 && early.tick() == 0);
}

static void test_press_until_file() {
    Host h("press-until-file next.bin A every=20 hold=3; press B hold=1");
    int presses = 0;
    uint16_t prev = 0;
    for (int i = 0; i < 70; ++i) {
        const uint16_t b = h.tick();
        CHECK((b & ~kA) == 0);
        if ((b & kA) && !(prev & kA)) ++presses;
        prev = b;
    }
    CHECK(presses == 4);  // frames 1, 21, 41, 61
    h.pending_files = {"s/NEXT.bin"};
    CHECK(h.tick() == kB);  // the file ends the loop and the next command runs at once
    CHECK(h.tick() == 0);
    Host seq("press-until-file n.bin D A+B every=10 hold=2");
    uint16_t at[40];
    for (auto& v : at) v = seq.tick();
    CHECK(at[0] == kDown && at[1] == kDown && at[2] == 0);
    CHECK(at[10] == (kA | kB) && at[20] == kDown && at[30] == (kA | kB));  // the buttons are used in turn
    Script s;
    std::string e;
    CHECK(!s.parse("press-until-file x", e) && !s.parse("press-until-file x Q", e) && !s.parse("press-until-file x A every=0", e));
}

static void test_wait_presented() {
    Host h("wait-presented 3; press A");
    CHECK(h.tick(false) == 0 && h.tick(false) == 0);
    CHECK(h.tick(true) == 0 && h.tick(true) == 0);
    CHECK(h.tick(true) == kA);
}

static void test_wait_change() {
    Host h("wait-change 0.1; press A hold=1");
    CHECK(h.script.needs_signature());
    h.sig = flat(20);
    CHECK(h.tick() == 0);                  // the first picture is the reference
    h.sig = flat(30);                       // 10/255 = 0.039, under the threshold
    CHECK(h.tick() == 0);
    h.sig = flat(100);                      // 80/255 = 0.31
    CHECK(h.tick() == kA);
    CHECK(!h.script.needs_signature());
    CHECK(h.tick() == 0);

    // The reference is the picture when the wait began, so a slow fade is a change in the end.
    Host fade("wait-change 0.1; press A hold=1");
    uint16_t last = 0;
    for (int v = 20; v < 120 && !last; v += 5) {
        fade.sig = flat(uint8_t(v));
        last = fade.tick();
    }
    CHECK(last == kA);

    // No new picture, no change.
    Host stale("wait-change; press A");
    stale.sig = flat(0);
    stale.tick();
    stale.sig = flat(255);
    for (int i = 0; i < 10; ++i) CHECK(stale.tick(false) == 0);
    CHECK(stale.tick(true) == kA);

    // Without a signature (the host did not read the frame back) the wait times out instead of hanging.
    Host blind("wait-change timeout=1; press A hold=1");
    blind.give_sig = false;
    uint16_t seen = 0;
    for (int i = 0; i < 70; ++i) seen |= blind.tick();
    CHECK(seen == kA);
}

static void test_wait_still() {
    Host h("wait-still 3; press A hold=1");
    h.sig = flat(10);
    CHECK(h.tick() == 0);  // reference
    CHECK(h.tick() == 0 && h.tick() == 0);
    h.sig = flat(200);     // a change resets the count
    CHECK(h.tick() == 0);
    CHECK(h.tick() == 0 && h.tick() == 0);
    CHECK(h.tick() == kA);
}

static void test_shot() {
    Host h("shot first; wait 1; shot second");
    h.tick();
    CHECK(h.script.take_shots() == std::vector<std::string>{"first"});
    h.tick();
    CHECK(h.script.take_shots() == std::vector<std::string>{"second"});
    CHECK(h.script.take_shots().empty());
}

static void test_trace_frame() {
    Host h("trace-frame; wait 1; trace-frame 3");
    h.tick();
    CHECK(h.script.take_traces() == std::vector<int>{1});
    h.tick();
    CHECK(h.script.take_traces() == std::vector<int>{3});
    Script s;
    std::string e;
    CHECK(!s.parse("trace-frame x", e) && !s.parse("trace-frame 1 2", e));
}

static void test_log() {
    Host h("press A; wait 1");
    h.tick();
    const auto log = h.script.take_log();
    CHECK(log.size() == 2 && log[0].find("press A") != std::string::npos);
    CHECK(h.script.take_log().empty());
}

static void test_stick() {
    Host h("stick up+left hold=3; wait 5; cstick 0 -50 hold=2; stick 10 -20 hold=1");
    h.tick();
    CHECK(h.script.sticks().x == -100 && h.script.sticks().y == 100 && h.script.sticks().sub_y == 0);
    h.tick();
    h.tick();
    h.tick();
    CHECK(h.script.sticks().x == 0 && h.script.sticks().y == 0);  // released after 3 frames
    h.tick();
    h.tick();  // frame 6: the wait is over, the C-stick and the stick start
    CHECK(h.script.sticks().sub_y == -50 && h.script.sticks().x == 10 && h.script.sticks().y == -20);
    h.tick();
    CHECK(h.script.sticks().sub_y == -50 && h.script.sticks().x == 0);
    Script s;
    std::string e;
    CHECK(!s.parse("stick", e) && !s.parse("stick 1", e) && !s.parse("stick 200 0", e) && !s.parse("stick sideways", e));
    CHECK(s.parse("stick neutral hold=4", e));
}

static void test_signature() {
    // Left half black, right half white.
    const int w = 64, h = 36;
    std::vector<uint8_t> rgb(size_t(w) * h * 3, 0);
    for (int y = 0; y < h; ++y)
        for (int x = w / 2; x < w; ++x)
            for (int c = 0; c < 3; ++c) rgb[(size_t(y) * w + x) * 3 + c] = 255;
    const FrameSignature s = frame_signature(rgb.data(), w, h);
    CHECK(s.size() == size_t(kSigW) * kSigH);
    CHECK(s[0] == 0 && s[kSigW - 1] >= 254);
    CHECK(signature_diff(s, s) == 0);
    CHECK(signature_diff(flat(0), flat(255)) == 1.0);
    const double half = signature_diff(s, flat(0));
    CHECK(half > 0.49 && half < 0.51);
    CHECK(signature_diff(s, FrameSignature(3, 0)) == 1.0);
    // Images smaller than the grid still produce a full signature.
    const uint8_t px[3] = {255, 255, 255};
    CHECK(frame_signature(px, 1, 1).size() == size_t(kSigW) * kSigH);
}

int main() {
    test_parse();
    test_press_and_wait();
    test_at_and_seconds();
    test_wait_file();
    test_press_until_file();
    test_wait_presented();
    test_wait_change();
    test_wait_still();
    test_shot();
    test_trace_frame();
    test_log();
    test_stick();
    test_signature();
    if (g_failures) std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    else std::puts("input script tests passed");
    return g_failures ? 1 : 0;
}
