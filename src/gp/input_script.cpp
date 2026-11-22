// SPDX-License-Identifier: MIT
#include "input_script.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "input.h"

namespace gcrecomp::gp::input {

FrameSignature frame_signature(const uint8_t* rgb, int w, int h) {
    FrameSignature sig(size_t(kSigW) * kSigH, 0);
    if (!rgb || w <= 0 || h <= 0) return sig;
    for (int cy = 0; cy < kSigH; ++cy) {
        const int y0 = cy * h / kSigH, y1 = std::max(y0 + 1, (cy + 1) * h / kSigH);
        for (int cx = 0; cx < kSigW; ++cx) {
            const int x0 = cx * w / kSigW, x1 = std::max(x0 + 1, (cx + 1) * w / kSigW);
            uint64_t sum = 0, n = 0;
            for (int y = y0; y < y1 && y < h; ++y)
                for (int x = x0; x < x1 && x < w; ++x) {
                    const uint8_t* p = rgb + (size_t(y) * w + x) * 3;
                    sum += (p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8;  // Rec. 601 luma
                    ++n;
                }
            sig[size_t(cy) * kSigW + cx] = n ? uint8_t(sum / n) : 0;
        }
    }
    return sig;
}

double signature_diff(const FrameSignature& a, const FrameSignature& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    uint64_t sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += uint64_t(std::abs(int(a[i]) - int(b[i])));
    return double(sum) / (255.0 * double(a.size()));
}

namespace {

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

// One button: a letter (A B X Y S U D L R Z; L and R are the D-pad as in the first script format)
// or a name (start, up, down, left, right, z, trigger-l, trigger-r). 0 when unknown.
uint16_t button_mask(const std::string& name) {
    if (name.size() == 1) {
        switch (std::toupper((unsigned char)name[0])) {
            case 'A': return kA;
            case 'B': return kB;
            case 'X': return kX;
            case 'Y': return kY;
            case 'S': return kStart;
            case 'U': return kUp;
            case 'D': return kDown;
            case 'L': return kLeft;
            case 'R': return kRight;
            case 'Z': return kZ;
            default: return 0;
        }
    }
    const std::string n = lower(name);
    if (n == "start") return kStart;
    if (n == "up") return kUp;
    if (n == "down") return kDown;
    if (n == "left") return kLeft;
    if (n == "right") return kRight;
    if (n == "trigger-l") return kL;
    if (n == "trigger-r") return kR;
    return 0;
}

bool parse_number(const std::string& s, double& out) {
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == 0 && !s.empty() && out >= 0;
}

// Parses one command (already split into words); returns false with `error` set.
bool parse_command(const std::vector<std::string>& w, const std::string& src, std::vector<Command>& out, std::string& error) {
    auto fail = [&](const std::string& why) {
        error = "'" + src + "': " + why;
        return false;
    };
    // Legacy form "FRAME:BUTTON": at FRAME; press BUTTON.
    if (w.size() == 1 && std::isdigit((unsigned char)w[0][0]) && w[0].find(':') != std::string::npos) {
        const size_t colon = w[0].find(':');
        Command at;
        at.kind = CmdKind::At;
        at.count = std::atoi(w[0].c_str());
        at.source = src;
        Command press;
        press.kind = CmdKind::Press;
        press.mask = button_mask(w[0].substr(colon + 1));  // an unknown button holds nothing
        press.count = 5;
        press.source = src;
        out.push_back(at);
        out.push_back(press);
        return true;
    }
    Command c;
    c.source = src;
    // Options common to the waits: timeout=SECONDS, threshold=FRACTION; and hold=FRAMES for press.
    std::vector<std::string> args;
    bool have_hold = false;
    for (size_t i = 1; i < w.size(); ++i) {
        const size_t eq = w[i].find('=');
        if (eq == std::string::npos) {
            args.push_back(w[i]);
            continue;
        }
        const std::string key = lower(w[i].substr(0, eq));
        double v;
        if (!parse_number(w[i].substr(eq + 1), v)) return fail("bad number in '" + w[i] + "'");
        if (key == "timeout") c.timeout = v;
        else if (key == "threshold") c.threshold = v;
        else if (key == "hold") { c.count = int(v); have_hold = true; }
        else if (key == "every") c.every = int(v);
        else return fail("unknown option '" + key + "'");
    }
    const std::string verb = lower(w[0]);
    auto need_args = [&](size_t n) { return args.size() == n; };
    auto number_arg = [&](double& v) { return args.size() >= 1 && parse_number(args[0], v); };
    double v = 0;
    if (verb == "press") {
        c.kind = CmdKind::Press;
        if (!need_args(1)) return fail("press takes buttons joined by '+' (press A, press S+A hold=10)");
        std::string rest = args[0];
        for (size_t pos = 0; pos <= rest.size();) {
            size_t plus = rest.find('+', pos);
            if (plus == std::string::npos) plus = rest.size();
            const std::string name = rest.substr(pos, plus - pos);
            const uint16_t m = button_mask(name);
            if (!m) return fail("unknown button '" + name + "'");
            c.mask |= m;
            pos = plus + 1;
        }
        if (!have_hold) c.count = 5;
        if (c.count < 1) return fail("hold must be at least 1 frame");
    } else if (verb == "press-until-file") {
        c.kind = CmdKind::PressUntilFile;
        if (args.size() < 2) return fail("press-until-file takes a path substring and buttons, used in turn (press-until-file mov/s01 D A every=60)");
        c.text = lower(args[0]);
        for (size_t i = 1; i < args.size(); ++i) {
            uint16_t mask = 0;
            for (size_t pos = 0; pos <= args[i].size();) {
                size_t plus = args[i].find('+', pos);
                if (plus == std::string::npos) plus = args[i].size();
                const uint16_t m = button_mask(args[i].substr(pos, plus - pos));
                if (!m) return fail("unknown button in '" + args[i] + "'");
                mask |= m;
                pos = plus + 1;
            }
            c.cycle.push_back(mask);
        }
        if (!have_hold) c.count = 10;
        if (c.count < 1 || c.every < 1) return fail("hold and every must be at least 1 frame");
    } else if (verb == "at") {
        c.kind = CmdKind::At;
        if (!need_args(1) || !number_arg(v)) return fail("at takes a frame number");
        c.count = int(v);
    } else if (verb == "wait") {
        c.kind = CmdKind::Wait;
        if (!need_args(1) || !number_arg(v)) return fail("wait takes a number of frames");
        c.count = int(v);
    } else if (verb == "wait-seconds") {
        c.kind = CmdKind::WaitSeconds;
        if (!need_args(1) || !number_arg(v)) return fail("wait-seconds takes a duration");
        c.seconds = v;
    } else if (verb == "wait-presented") {
        c.kind = CmdKind::WaitPresented;
        if (!need_args(1) || !number_arg(v)) return fail("wait-presented takes a number of presented frames");
        c.count = int(v);
    } else if (verb == "wait-file") {
        c.kind = CmdKind::WaitFile;
        if (!need_args(1)) return fail("wait-file takes a path substring (no spaces), matched ignoring case");
        c.text = lower(args[0]);
    } else if (verb == "wait-change") {
        c.kind = CmdKind::WaitChange;
        if (args.size() > 1) return fail("wait-change takes at most a threshold");
        if (args.size() == 1) {
            if (!parse_number(args[0], c.threshold)) return fail("bad threshold");
        }
    } else if (verb == "wait-still") {
        c.kind = CmdKind::WaitStill;
        if (args.empty() || args.size() > 2 || !number_arg(v)) return fail("wait-still takes a number of frames and an optional threshold");
        c.count = int(v);
        if (c.count < 1) return fail("wait-still needs at least 1 frame");
        if (args.size() == 2 && !parse_number(args[1], c.threshold)) return fail("bad threshold");
    } else if (verb == "trace-frame") {
        c.kind = CmdKind::TraceFrame;
        c.count = 1;
        if (args.size() > 1 || (args.size() == 1 && !number_arg(v))) return fail("trace-frame takes an optional frame offset (1 = the frame in progress)");
        if (args.size() == 1) c.count = std::max(1, int(v));
    } else if (verb == "shot") {
        c.kind = CmdKind::Shot;
        if (!need_args(1)) return fail("shot takes a file name (written as NAME.ppm in the screenshot directory)");
        c.text = args[0];
    } else {
        return fail("unknown command '" + w[0] + "'");
    }
    out.push_back(c);
    return true;
}

}  // namespace

bool Script::parse(const char* text, std::string& error) {
    cmds_.clear();
    pc_ = 0;
    started_ = false;
    holds_.clear();
    if (!text) return true;
    std::string current;
    std::vector<std::string> pieces;
    bool comment = false;
    for (const char* p = text;; ++p) {
        const char ch = *p;
        if (ch == 0 || ch == ';' || ch == ',' || ch == '\n') {
            pieces.push_back(current);
            current.clear();
            if (ch == '\n') comment = false;
            if (ch == 0) break;
            continue;
        }
        if (ch == '#') comment = true;
        if (!comment) current.push_back(ch == '\r' || ch == '\t' ? ' ' : ch);
    }
    for (const std::string& piece : pieces) {
        const std::vector<std::string> w = words(piece);
        if (w.empty()) continue;
        std::string src;
        for (const std::string& s : w) src += (src.empty() ? "" : " ") + s;
        if (!parse_command(w, src, cmds_, error)) {
            cmds_.clear();
            return false;
        }
    }
    return true;
}

bool Script::needs_signature() const {
    if (finished()) return false;
    const CmdKind k = cmds_[pc_].kind;
    return k == CmdKind::WaitChange || k == CmdKind::WaitStill;
}

std::vector<std::string> Script::take_shots() {
    std::vector<std::string> out;
    out.swap(shots_);
    return out;
}

std::vector<int> Script::take_traces() {
    std::vector<int> out;
    out.swap(traces_);
    return out;
}

std::vector<std::string> Script::take_log() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

uint16_t Script::step(const Observation& o) {
    file_cursor_ = 0;
    while (pc_ < cmds_.size()) {
        const Command& c = cmds_[pc_];
        if (!started_) {
            started_ = true;
            start_frame_ = o.frame;
            start_seconds_ = o.seconds;
            start_presented_ = o.presented;
            ref_.clear();
            // The picture on screen when the wait begins is the reference, if the host has it.
            if (o.sig && o.sig_at == o.presented) ref_ = *o.sig;
            last_sig_presented_ = o.sig_at;
            still_count_ = 0;
            next_press_ = o.frame;
            cycle_pos_ = 0;
            log_.push_back("frame " + std::to_string(o.frame) + ": " + c.source);
        }
        bool done = true;
        switch (c.kind) {
            case CmdKind::At: done = o.frame >= c.count; break;
            case CmdKind::Wait: done = o.frame - start_frame_ >= c.count; break;
            case CmdKind::WaitSeconds: done = o.seconds - start_seconds_ >= c.seconds; break;
            case CmdKind::WaitPresented: done = o.presented - start_presented_ >= uint64_t(c.count); break;
            case CmdKind::PressUntilFile:
            case CmdKind::WaitFile: {
                done = false;
                for (; file_cursor_ < o.files.size(); ++file_cursor_)
                    if (lower(o.files[file_cursor_]).find(c.text) != std::string::npos) {
                        ++file_cursor_;
                        done = true;
                        break;
                    }
                if (!done && c.kind == CmdKind::PressUntilFile && o.frame >= next_press_) {
                    holds_.push_back({c.cycle[cycle_pos_], o.frame + c.count});
                    cycle_pos_ = (cycle_pos_ + 1) % c.cycle.size();
                    next_press_ = o.frame + c.every;
                }
                break;
            }
            case CmdKind::WaitChange:
            case CmdKind::WaitStill: {
                done = false;
                if (o.sig && o.sig_at != last_sig_presented_) {  // a new picture
                    last_sig_presented_ = o.sig_at;
                    if (ref_.empty()) {
                        ref_ = *o.sig;
                    } else if (c.kind == CmdKind::WaitChange) {
                        done = signature_diff(ref_, *o.sig) > c.threshold;
                    } else {
                        if (signature_diff(ref_, *o.sig) <= c.threshold) ++still_count_;
                        else still_count_ = 0;
                        ref_ = *o.sig;
                        done = still_count_ >= c.count;
                    }
                }
                break;
            }
            case CmdKind::Press:
            case CmdKind::TraceFrame:
            case CmdKind::Shot: break;
        }
        if (!done && c.timeout > 0 && o.seconds - start_seconds_ >= c.timeout) {
            log_.push_back("frame " + std::to_string(o.frame) + ": '" + c.source + "' timed out after " + std::to_string(c.timeout) + " s");
            done = true;
        }
        if (!done) break;
        if (c.kind == CmdKind::Press) holds_.push_back({c.mask, o.frame + c.count});
        if (c.kind == CmdKind::Shot) shots_.push_back(c.text);
        if (c.kind == CmdKind::TraceFrame) traces_.push_back(c.count);
        ++pc_;
        started_ = false;
    }
    uint16_t buttons = 0;
    for (size_t i = 0; i < holds_.size();) {
        if (o.frame < holds_[i].until) {
            buttons |= holds_[i].mask;
            ++i;
        } else {
            holds_.erase(holds_.begin() + long(i));
        }
    }
    return buttons;
}

}  // namespace gcrecomp::gp::input
