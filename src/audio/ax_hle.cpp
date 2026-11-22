// SPDX-License-Identifier: MIT
//
// AX microcode HLE: see gcrecomp/audio/ax.h. Ported from the author's Rust implementation
// (fepor-recomp, crates/runtime/src/ax.rs).
#include "gcrecomp/audio/ax.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace gcrecomp::audio {

namespace {

constexpr size_t kFrame = AxUcode::kFrame;
constexpr size_t kPbWords = 122;
using PbWords = uint16_t[kPbWords];

constexpr uint32_t kTaskMailMask = 0xFFFF0000;
constexpr uint32_t kTaskMailToDsp = 0xCDD10000;
constexpr uint32_t kMailResume = 0xCDD10000;
constexpr uint32_t kMailNewUcode = 0xCDD10001;
constexpr uint32_t kMailReset = 0xCDD10002;
constexpr uint32_t kMailContinue = 0xCDD10003;

// buffer indices
constexpr size_t kMainL = 0;
constexpr size_t kMainR = 1;
constexpr size_t kMainS = 2;
constexpr size_t kAuxAL = 3;
constexpr size_t kAuxBL = 6;

// mixer control bits (AXMixControl)
constexpr uint32_t kMixMainL = 0x000001;
constexpr uint32_t kMixMainLRamp = 0x000002;
constexpr uint32_t kMixMainR = 0x000004;
constexpr uint32_t kMixMainRRamp = 0x000008;
constexpr uint32_t kMixMainS = 0x000010;
constexpr uint32_t kMixMainSRamp = 0x000020;
constexpr uint32_t kMixAuxAL = 0x000040;
constexpr uint32_t kMixAuxALRamp = 0x000080;
constexpr uint32_t kMixAuxAR = 0x000100;
constexpr uint32_t kMixAuxARRamp = 0x000200;
constexpr uint32_t kMixAuxAS = 0x000400;
constexpr uint32_t kMixAuxASRamp = 0x000800;
constexpr uint32_t kMixAuxBL = 0x001000;
constexpr uint32_t kMixAuxBLRamp = 0x002000;
constexpr uint32_t kMixAuxBR = 0x004000;
constexpr uint32_t kMixAuxBRRamp = 0x008000;
constexpr uint32_t kMixAuxBS = 0x010000;
constexpr uint32_t kMixAuxBSRamp = 0x020000;

uint32_t hilo(uint16_t hi, uint16_t lo) { return uint32_t(hi) << 16 | lo; }

int16_t clamp16(int64_t v) { return int16_t(std::clamp<int64_t>(v, -0x8000, 0x7FFF)); }

// Main memory, big-endian, physical addresses wrapped to 26 bits; out-of-range reads give 0 and
// out-of-range writes are dropped.
uint16_t r16(const AxMemory& m, uint32_t addr) {
    const size_t a = addr & 0x03FFFFFF;
    if (a + 2 > m.ram_size) return 0;
    return uint16_t(m.ram[a] << 8 | m.ram[a + 1]);
}
uint32_t r32(const AxMemory& m, uint32_t addr) { return hilo(r16(m, addr), r16(m, addr + 2)); }
void w16(AxMemory& m, uint32_t addr, uint16_t v) {
    const size_t a = addr & 0x03FFFFFF;
    if (a + 2 > m.ram_size) return;
    m.ram[a] = uint8_t(v >> 8);
    m.ram[a + 1] = uint8_t(v);
}
void w32(AxMemory& m, uint32_t addr, uint32_t v) {
    w16(m, addr, uint16_t(v >> 16));
    w16(m, addr + 2, uint16_t(v));
}
uint8_t aram8(const AxMemory& m, uint32_t addr) {
    if (!m.aram || !m.aram_size) return 0;
    return m.aram[addr & (m.aram_size - 1)];
}

/// The DSP accelerator: reads and decodes voice samples from ARAM.
struct Accel {
    uint32_t start, end, cur;
    uint16_t format;
    int16_t gain, yn1, yn2;
    uint16_t pred_scale;
    bool reads_stopped;

    uint16_t size() const { return format & 3; }
    uint16_t decode() const { return format >> 2 & 3; }
    uint16_t gain_scale() const { return format >> 4 & 3; }

    uint16_t current_sample(const AxMemory& mem) const {
        switch (size()) {
        case 0: {
            const uint16_t v = aram8(mem, cur >> 1);
            return (cur & 1) ? (v & 0xF) : (v >> 4);
        }
        case 1: return aram8(mem, cur);
        case 2: return uint16_t(aram8(mem, cur * 2) << 8 | aram8(mem, cur * 2 + 1));
        default: return 0;
        }
    }

    // One sample; may hit the end of the voice (flags are handled through `pb`).
    int16_t read_sample(PbWords pb, const AxMemory& mem) {
        if (reads_stopped) return 0;
        const int16_t raw = int16_t(current_sample(mem));
        const size_t coef_idx = (pred_scale >> 4) & 7;
        const int32_t coef1 = int16_t(pb[63 + coef_idx * 2]);
        const int32_t coef2 = int16_t(pb[63 + coef_idx * 2 + 1]);
        int16_t val;
        uint32_t step;
        if (decode() == 0) {
            int32_t r = raw & 0xF;
            const int32_t scale = 1 << (pred_scale & 0xF);
            if (r >= 8) r -= 16;
            const int32_t v32 = scale * r + ((0x400 + coef1 * yn1 + coef2 * yn2) >> 11);
            val = int16_t(std::clamp(v32, -0x8000, 0x7FFF));
            step = 2;
            yn2 = yn1;
            yn1 = val;
            ++cur;
            if ((end & 0xF) == 0 && cur == end) {
                cur = start + 1;
            } else if ((end & 0xF) == 1 && cur == end - 1) {
                cur = start;
            } else if ((cur & 15) == 0) {
                pred_scale = aram8(mem, (cur & ~15u) >> 1);
                cur += 2;
                return finish(val, step + 2, pb);
            }
        } else {
            int shift;
            switch (gain_scale()) {
            case 0: shift = 11; break;
            case 2: shift = 16; break;
            default: shift = 0; break;
            }
            const int32_t v32 = ((int32_t(gain) * raw) >> shift) + (((coef1 * yn1) >> shift) + ((coef2 * yn2) >> shift));
            val = int16_t(v32);
            yn2 = yn1;
            yn1 = val;
            step = 2;
            if (decode() != 1) ++cur;
        }
        return finish(val, step, pb);
    }

    int16_t finish(int16_t val, uint32_t step, PbWords pb) {
        if (cur == end + step - 1) {
            cur = start;
            reads_stopped = true;
            end_exception(pb);
        }
        return val;
    }

    void end_exception(PbWords pb) {
        if (pb[55] != 0) {
            // looping: continue from the loop point
            pred_scale = pb[90] & 0x7F;
            if (pb[8] != 1) {
                yn1 = int16_t(pb[91]);
                yn2 = int16_t(pb[92]);
                reads_stopped = false;
            } else {
                // streams refresh yn1/yn2 (which resumes reads) and count loops
                reads_stopped = false;
                pb[97] = uint16_t(pb[97] + 1);
            }
        } else {
            pb[7] = 0;  // a one-shot voice reached its end
        }
    }
};

// The microcode's polyphase resampler has one set of 128 four-tap filters per cutoff (0.5, 0.8 and
// 1.0 of the output rate). The hardware table is a DSP ROM dump and cannot be shipped; every set is
// a Hamming-windowed sinc, regenerated here (parameters fitted to the table: max deviation < 1 %).
struct PolyTable {
    int16_t t[3][128][4];
    PolyTable() {
        // (cutoff, window offset `a` in a + (1 - a) cos(pi x / 2), overall gain)
        static constexpr double kSets[3][3] = {
            {0.510, 0.55, 0.9998},
            {0.785, 0.48, 1.0002},
            {1.005, 0.56, 0.9837},
        };
        const double pi = 3.14159265358979323846;
        for (int s = 0; s < 3; ++s) {
            const double fc = kSets[s][0], a = kSets[s][1], gain = kSets[s][2];
            for (int p = 0; p < 128; ++p) {
                const double phase = p / 128.0;
                double h[4];
                double sum = 0;
                for (int k = 0; k < 4; ++k) {
                    const double x = k - 1.0 - phase;
                    const double sinc = x == 0.0 ? 1.0 : std::sin(pi * fc * x) / (pi * fc * x);
                    const double window = std::fabs(x) <= 2.0 ? a + (1.0 - a) * std::cos(pi * x / 2.0) : 0.0;
                    h[k] = fc * sinc * window;
                    sum += h[k];
                }
                for (int k = 0; k < 4; ++k) t[s][p][k] = int16_t(std::lround(h[k] / sum * gain * 32768.0));
            }
        }
    }
};

const PolyTable& poly_table() {
    static const PolyTable table;
    return table;
}

// Which filter set a voice's resampling ratio uses (the smallest cutoff that covers it); -1 above 1.0.
int poly_set(uint32_t ratio) {
    if (ratio <= 0x8000) return 0;
    if (ratio <= 0xCCCC) return 1;
    if (ratio <= 0x10000) return 2;
    return -1;
}

// Resamples `count` samples (`input` yields the source samples); returns the new fractional position.
template <class Input>
uint32_t resample(Input&& input, int16_t* out, size_t count, int16_t last[4], uint32_t pos, uint32_t ratio,
                  uint16_t srctype) {
    if (srctype == 0 || srctype == 1) {
        int16_t temp[4];
        size_t idx = 0;
        for (int k = 0; k < 4; ++k) temp[idx++ & 3] = last[k];
        const int set = srctype == 0 ? poly_set(ratio) : -1;
        for (size_t n = 0; n < count; ++n) {
            pos += ratio;
            while (pos >= 0x10000) {
                temp[idx++ & 3] = input();
                pos -= 0x10000;
            }
            const uint16_t frac = uint16_t(pos & 0xFFFF);
            const uint16_t inv = uint16_t(-frac);
            int16_t sample;
            if (set >= 0) {
                // 4-tap polyphase: the window is the last four source samples, centred between the
                // middle two; 128 phases per set
                const int16_t* taps = poly_table().t[set][frac >> 9];
                int32_t acc = 0;
                for (size_t k = 0; k < 4; ++k) acc += int32_t(taps[k]) * temp[(idx + k) & 3];
                idx += 4;
                sample = int16_t(std::clamp(acc >> 15, -0x8000, 0x7FFF));
            } else if (frac != 0) {
                const int32_t s0 = temp[idx & 3];
                ++idx;
                const int32_t s1 = temp[idx & 3];
                ++idx;
                idx += 2;
                sample = int16_t(((s0 * int32_t(inv)) + (s1 * int32_t(frac))) >> 16);
            } else {
                sample = temp[idx & 3];
                ++idx;
                idx += 3;
            }
            out[n] = sample;
        }
        for (int k = 3; k >= 0; --k) {
            --idx;
            last[k] = temp[idx & 3];
        }
    } else {
        for (size_t n = 0; n < count; ++n) out[n] = input();
        const size_t n = std::max<size_t>(count, 4);
        std::memcpy(last, out + n - 4, 4 * sizeof(int16_t));
    }
    return pos;
}

void mix_add(int32_t* out, const int16_t* input, size_t count, PbWords pb, size_t vol_at, size_t dpop_at, bool ramp) {
    uint16_t volume = pb[vol_at];
    const uint16_t delta = ramp ? pb[vol_at + 1] : 0;
    for (size_t i = 0; i < count; ++i) {
        const int64_t s = (int64_t(input[i]) * volume) >> 15;
        const int16_t s16 = clamp16(int32_t(s));
        out[i] += s16;
        volume = uint16_t(volume + delta);
        pb[dpop_at] = uint16_t(s16);
    }
    pb[vol_at] = volume;
}

uint32_t convert_mixer_control(uint32_t m) {
    uint32_t r = 0;
    auto set = [&](bool cond, uint32_t bits) {
        if (cond) r |= bits;
    };
    set(m & 0x0001, kMixMainL);
    set(m & 0x0002, kMixMainR);
    set(m & 0x0004, kMixMainS);
    set(m & 0x0008, kMixMainLRamp | kMixMainRRamp | kMixMainSRamp);
    set(m & 0x0010, kMixAuxAL);
    set(m & 0x0020, kMixAuxAR);
    set(m & 0x0040, kMixAuxALRamp | kMixAuxARRamp);
    set(m & 0x0080, kMixAuxAS);
    set(m & 0x0100, kMixAuxASRamp);
    set(m & 0x0200, kMixAuxBL);
    set(m & 0x0400, kMixAuxBR);
    set(m & 0x0800, kMixAuxBLRamp | kMixAuxBRRamp);
    set(m & 0x1000, kMixAuxBS);
    set(m & 0x2000, kMixAuxBSRamp);
    return r;
}

}  // namespace

AxUcode::AxUcode() = default;

std::optional<uint32_t> AxUcode::handle_mail(uint32_t mail, AxMemory& mem) {
    if (upload_mails_ > 0) {
        --upload_mails_;
        if (upload_mails_ == 0) return kDspResume;
        return std::nullopt;
    }
    switch (state_) {
    case MailState::WaitingForCmdListSize:
        if ((mail & 0xFFFF0000) == kMailCmdList) {
            cmd_size_ = mail & 0xFFFF;
            state_ = MailState::WaitingForCmdListAddress;
        }
        return std::nullopt;
    case MailState::WaitingForCmdListAddress:
        copy_cmdlist(mail, cmd_size_, mem);
        run_cmdlist(mem);
        ++frames_;
        state_ = MailState::WaitingForNextTask;
        return kDspYield;
    case MailState::WaitingForNextTask:
        if ((mail & kTaskMailMask) != kTaskMailToDsp) mail = kTaskMailToDsp | (mail & ~kTaskMailMask);
        switch (mail) {
        case kMailResume:
            state_ = MailState::WaitingForCmdListSize;
            return kDspResume;
        case kMailNewUcode:
            // ten mails describe the next microcode; the game only swaps to ones this model lacks
            upload_mails_ = 10;
            state_ = MailState::WaitingForCmdListSize;
            return std::nullopt;
        case kMailReset: reset_requested_ = true; return std::nullopt;
        case kMailContinue: state_ = MailState::WaitingForCmdListSize; return std::nullopt;
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}

void AxUcode::copy_cmdlist(uint32_t addr, size_t size, const AxMemory& mem) {
    if (size >= std::size(cmdlist_)) return;
    for (size_t i = 0; i < size; ++i) cmdlist_[i] = r16(mem, addr + 2 * uint32_t(i));
}

void AxUcode::run_cmdlist(AxMemory& mem) {
    size_t idx = 0;
    uint32_t pb_addr = 0;
    auto arg = [&]() { return cmdlist_[idx++ % 512]; };
    auto addr = [&]() {
        const uint16_t hi = arg();
        const uint16_t lo = arg();
        return hilo(hi, lo);
    };
    while (idx < std::size(cmdlist_)) {
        const uint16_t cmd = cmdlist_[idx++];
        if (cmd < 32) ++stats_.commands[cmd];
        switch (cmd) {
        case 0x00: setup(addr(), mem); break;
        case 0x01: {
            const uint32_t a = addr();
            const uint16_t vm = arg();
            const uint16_t va = arg();
            const uint16_t vb = arg();
            download_and_mix(a, vm, va, vb, mem);
            break;
        }
        case 0x02: pb_addr = addr(); break;
        case 0x03: process_pb_list(pb_addr, mem); break;
        case 0x04:
        case 0x05: {
            const uint32_t a = addr();
            const uint32_t b = addr();
            mix_aux(cmd - 4, a, b, mem);
            break;
        }
        case 0x06: upload_lrs(addr(), mem); break;
        case 0x07: set_main_lr(addr(), mem); break;
        case 0x08: idx += 10; break;
        case 0x09: mix_aux(1, 0, addr(), mem); break;
        case 0x0A:
        case 0x0B:
        case 0x0C: break;
        case 0x0D: {
            const uint32_t a = addr();
            const size_t size = arg();
            copy_cmdlist(a, size, mem);
            idx = 0;
            break;
        }
        case 0x0E: {
            const uint32_t surround = addr();
            const uint32_t lr = addr();
            output(lr, surround, mem);
            break;
        }
        case 0x0F: return;
        case 0x10: {
            const uint32_t a = addr();
            const uint32_t b = addr();
            mix_auxb_lr(a, b, mem);
            break;
        }
        case 0x11: set_opposite_lr(addr(), mem); break;
        case 0x12: {
            const uint16_t threshold = arg();
            const uint16_t frames = arg();
            compressor(threshold, frames, addr(), mem);
            break;
        }
        case 0x13: {
            uint32_t v[6];
            for (uint32_t& x : v) x = addr();
            send_aux_and_mix(v, mem);
            break;
        }
        default: ++unknown_cmds_; return;
        }
    }
}

void AxUcode::setup(uint32_t init_addr, const AxMemory& mem) {
    for (uint32_t b = 0; b < 9; ++b) {
        const uint16_t w0 = r16(mem, init_addr + 6 * b);
        const uint16_t w1 = r16(mem, init_addr + 6 * b + 2);
        const int32_t delta = int16_t(r16(mem, init_addr + 6 * b + 4));
        const int32_t value = int32_t(hilo(w0, w1));
        for (size_t j = 0; j < kFrame; ++j)
            bufs_[b][j] = value == 0 ? 0 : int32_t(uint32_t(value) + uint32_t(int32_t(j) * delta));
    }
}

void AxUcode::download_and_mix(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb,
                               const AxMemory& mem) {
    const uint16_t vols[3] = {vol_main, vol_auxa, vol_auxb};
    uint32_t a = addr;
    for (size_t group = 0; group < 3; ++group) {
        for (size_t ch = 0; ch < 3; ++ch) {
            for (size_t k = 0; k < kFrame; ++k) {
                const int64_t s = int32_t(r32(mem, a));
                a += 4;
                int32_t& dst = bufs_[group * 3 + ch][k];
                dst = int32_t(uint32_t(dst) + uint32_t((s * vols[group]) >> 15));
            }
        }
    }
}

void AxUcode::process_pb_list(uint32_t pb_addr, AxMemory& mem) {
    for (int guard = 0; pb_addr != 0 && guard < 256; ++guard) {
        PbWords pb;
        for (size_t i = 0; i < kPbWords; ++i) pb[i] = r16(mem, pb_addr + 2 * uint32_t(i));
        cur_pb_addr_ = pb_addr;
        // per-millisecond parameter updates
        uint16_t updates[32][2];
        const uint32_t ua = hilo(pb[39], pb[40]);
        for (uint32_t i = 0; i < 32; ++i) {
            updates[i][0] = r16(mem, ua + 4 * i);
            updates[i][1] = r16(mem, ua + 4 * i + 2);
        }
        for (size_t ms = 0; ms < 5; ++ms) {
            size_t start = 0;
            for (size_t i = 0; i < ms; ++i) start += pb[34 + i];
            if (start < 32) {
                const size_t count = pb[34 + ms];
                if (count <= 32 - start) {
                    for (size_t k = start; k < start + count; ++k)
                        if (updates[k][0] < kPbWords) pb[updates[k][0]] = updates[k][1];
                }
            }
            process_voice(pb, ms, mem);
        }
        for (size_t i = 0; i < kPbWords; ++i) w16(mem, pb_addr + 2 * uint32_t(i), pb[i]);
        pb_addr = hilo(pb[0], pb[1]);
    }
}

// One millisecond (32 samples) of one voice.
void AxUcode::process_voice(Pb pb, size_t ms, const AxMemory& mem) {
    constexpr size_t kCount = 32;
    if (pb[7] != 1) return;
    if (ms == 0 && voice_observer_) voice_observer_(cur_pb_addr_, pb);
    int16_t samples[kCount];
    Accel acc{hilo(pb[57], pb[58]) & 0x3FFFFFFF,
              hilo(pb[59], pb[60]) & 0x3FFFFFFF,
              hilo(pb[61], pb[62]) & 0xBFFFFFFF,
              pb[56],
              int16_t(pb[79]),
              int16_t(pb[81]),
              int16_t(pb[82]),
              uint16_t(pb[80] & 0x7F),
              false};
    const uint32_t ratio = hilo(pb[83], pb[84]);
    int16_t last[4] = {int16_t(pb[86]), int16_t(pb[87]), int16_t(pb[88]), int16_t(pb[89])};
    const uint32_t frac = pb[85];
    const uint16_t src_type = pb[4];
    PbWords pb_tmp;
    std::memcpy(pb_tmp, pb, sizeof pb_tmp);
    const uint32_t pos = resample([&]() { return acc.read_sample(pb_tmp, mem); }, samples, kCount, last, frac, ratio,
                                  src_type);
    std::memcpy(pb, pb_tmp, sizeof pb_tmp);
    pb[85] = uint16_t(pos & 0xFFFF);
    for (int k = 0; k < 4; ++k) pb[86 + k] = uint16_t(last[k]);
    pb[61] = uint16_t(acc.cur >> 16);
    pb[62] = uint16_t(acc.cur);
    pb[81] = uint16_t(acc.yn1);
    pb[82] = uint16_t(acc.yn2);
    pb[80] = acc.pred_scale;

    // volume envelope
    ++stats_.voice_ms;
    bool loud = false;
    for (int16_t& s : samples) {
        const int64_t v = (int32_t(s) * int32_t(int16_t(pb[50]))) >> 15;
        s = clamp16(v);
        pb[50] = uint16_t(pb[50] + pb[51]);
        if (s != 0) loud = true;
        if (std::abs(int(s)) > stats_.voice_peak) stats_.voice_peak = std::abs(int(s));
    }
    if (loud) ++stats_.voice_ms_loud;
    if (pb[93] != 0) {
        // low-pass filter
        const int32_t a0 = pb[95];
        const int32_t b0 = int16_t(pb[96]);
        int16_t yn1 = int16_t(pb[94]);
        for (int16_t& s : samples) {
            const int16_t v = clamp16((a0 * int32_t(s) + b0 * int32_t(yn1)) >> 15);
            yn1 = v;
            s = v;
        }
        pb[94] = uint16_t(yn1);
    }

    const uint32_t m = convert_mixer_control(pb[6]);
    struct Route {
        size_t buf, vol, dpop;
        uint32_t bit, ramp;
    };
    static constexpr Route kRoutes[] = {
        {0, 9, 41, kMixMainL, kMixMainLRamp}, {1, 11, 44, kMixMainR, kMixMainRRamp},
        {2, 23, 47, kMixMainS, kMixMainSRamp}, {3, 13, 42, kMixAuxAL, kMixAuxALRamp},
        {4, 15, 45, kMixAuxAR, kMixAuxARRamp}, {5, 25, 48, kMixAuxAS, kMixAuxASRamp},
        {6, 17, 43, kMixAuxBL, kMixAuxBLRamp}, {7, 19, 46, kMixAuxBR, kMixAuxBRRamp},
        {8, 21, 49, kMixAuxBS, kMixAuxBSRamp},
    };
    for (const Route& r : kRoutes) {
        if (m & r.bit) mix_add(&bufs_[r.buf][ms * kCount], samples, kCount, pb, r.vol, r.dpop, (m & r.ramp) != 0);
    }
}

void AxUcode::mix_aux(size_t aux_id, uint32_t write_addr, uint32_t read_addr, AxMemory& mem) {
    const size_t base = aux_id == 0 ? kAuxAL : kAuxBL;
    if (write_addr != 0) {
        uint32_t a = write_addr;
        for (size_t ch = 0; ch < 3; ++ch)
            for (size_t k = 0; k < kFrame; ++k) {
                w32(mem, a, uint32_t(bufs_[base + ch][k]));
                a += 4;
            }
    }
    uint32_t a = read_addr;
    for (size_t ch = 0; ch < 3; ++ch)
        for (size_t k = 0; k < kFrame; ++k) {
            bufs_[kMainL + ch][k] = int32_t(uint32_t(bufs_[kMainL + ch][k]) + r32(mem, a));
            a += 4;
        }
}

void AxUcode::tap(int which, const int32_t* samples) {
    double e = 0;
    for (size_t k = 0; k < kFrame; ++k) e += double(samples[k]) * double(samples[k]);
    stats_.tap_energy[which] += e;
    stats_.tap_samples[which] += kFrame;
}

void AxUcode::upload_lrs(uint32_t dst, AxMemory& mem) {
    for (int ch = 0; ch < 3; ++ch) tap(Stats::kUploadL + ch, bufs_[kMainL + ch]);
    for (size_t ch = 0; ch < 3; ++ch)
        for (size_t k = 0; k < kFrame; ++k) {
            w32(mem, dst, uint32_t(bufs_[kMainL + ch][k]));
            dst += 4;
        }
}

void AxUcode::set_main_lr(uint32_t src, const AxMemory& mem) {
    for (size_t k = 0; k < kFrame; ++k) {
        const int32_t s = int32_t(r32(mem, src + 4 * uint32_t(k)));
        bufs_[kMainL][k] = s;
        bufs_[kMainR][k] = s;
        bufs_[kMainS][k] = 0;
    }
}

void AxUcode::set_opposite_lr(uint32_t src, const AxMemory& mem) {
    {
        int32_t in[kFrame];
        for (size_t k = 0; k < kFrame; ++k) in[k] = int32_t(r32(mem, src + 4 * uint32_t(k)));
        tap(Stats::kOppositeIn, in);
    }
    for (size_t k = 0; k < kFrame; ++k) {
        const int32_t s = int32_t(r32(mem, src + 4 * uint32_t(k)));
        bufs_[kMainL][k] = int32_t(0u - uint32_t(s));
        bufs_[kMainR][k] = s;
        bufs_[kMainS][k] = 0;
    }
}

void AxUcode::compressor(uint16_t threshold, uint16_t release_frames, uint32_t table, const AxMemory& mem) {
    constexpr size_t kMillis = 5;
    constexpr size_t n = 32 * kMillis;
    bool triggered = false;
    for (size_t i = 0; i < n && !triggered; ++i) {
        auto mag = [](int32_t v) { return v < 0 ? 0u - uint32_t(v) : uint32_t(v); };
        triggered = mag(bufs_[kMainL][i]) > threshold || mag(bufs_[kMainR][i]) > threshold;
    }
    constexpr uint32_t frame_bytes = 32 * kMillis * 2;
    uint32_t offset;
    if (triggered) {
        offset = compressor_pos_ * frame_bytes;
        compressor_pos_ = release_frames;
    } else if (compressor_pos_ != 0) {
        --compressor_pos_;
        offset = (11 + compressor_pos_) * frame_bytes;
    } else {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const int64_t coef = r16(mem, table + offset + 2 * uint32_t(i));
        bufs_[kMainL][i] = int32_t((bufs_[kMainL][i] * coef) >> 15);
        bufs_[kMainR][i] = int32_t((bufs_[kMainR][i] * coef) >> 15);
    }
}

void AxUcode::output(uint32_t lr, uint32_t surround, AxMemory& mem) {
    for (int ch = 0; ch < 3; ++ch) tap(Stats::kOutL + ch, bufs_[kMainL + ch]);
    for (size_t k = 0; k < kFrame; ++k) w32(mem, surround + 4 * uint32_t(k), uint32_t(bufs_[kMainS][k]));
    for (size_t k = 0; k < kFrame; ++k) {
        const int16_t l = clamp16(bufs_[kMainL][k]);
        const int16_t r = clamp16(bufs_[kMainR][k]);
        // interleaved R, L as the microcode writes them (the AI plays word 1 as the left channel)
        w16(mem, lr + 4 * uint32_t(k), uint16_t(r));
        w16(mem, lr + 4 * uint32_t(k) + 2, uint16_t(l));
    }
}

void AxUcode::mix_auxb_lr(uint32_t ul, uint32_t dl, AxMemory& mem) {
    for (size_t k = 0; k < kFrame; ++k) {
        w32(mem, ul + 4 * uint32_t(k), uint32_t(bufs_[kAuxBL][k]));
        w32(mem, ul + 4 * uint32_t(kFrame + k), uint32_t(bufs_[kAuxBL + 1][k]));
    }
    for (size_t k = 0; k < kFrame; ++k) {
        const int32_t s = int32_t(r32(mem, dl + 4 * uint32_t(k)));
        bufs_[kAuxBL][k] = s;
        bufs_[kMainL][k] = int32_t(uint32_t(bufs_[kMainL][k]) + uint32_t(s));
    }
    for (size_t k = 0; k < kFrame; ++k) {
        const int32_t s = int32_t(r32(mem, dl + 4 * uint32_t(kFrame + k)));
        bufs_[kAuxBL + 1][k] = s;
        bufs_[kMainR][k] = int32_t(uint32_t(bufs_[kMainR][k]) - uint32_t(s));
    }
}

void AxUcode::send_aux_and_mix(const uint32_t a[6], AxMemory& mem) {
    const uint32_t auxa_up = a[0], auxb_s_up = a[1];
    for (size_t ch = 0; ch < 3; ++ch)
        for (size_t k = 0; k < kFrame; ++k)
            w32(mem, auxa_up + 4 * uint32_t(ch * kFrame + k), uint32_t(bufs_[kAuxAL + ch][k]));
    for (size_t k = 0; k < kFrame; ++k) w32(mem, auxb_s_up + 4 * uint32_t(k), uint32_t(bufs_[kAuxBL + 2][k]));
    const struct {
        size_t dst;
        uint32_t addr;
    } downloads[] = {{kMainL, a[2]}, {kMainR, a[3]}, {kAuxBL, a[4]}, {kAuxBL + 1, a[5]}};
    for (const auto& d : downloads)
        for (size_t k = 0; k < kFrame; ++k)
            bufs_[d.dst][k] = int32_t(uint32_t(bufs_[d.dst][k]) + r32(mem, d.addr + 4 * uint32_t(k)));
}

}  // namespace gcrecomp::audio
