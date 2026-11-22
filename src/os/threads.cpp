// SPDX-License-Identifier: MIT
//
// Guest threads: see gcrecomp/threads.h for the model. Semantics follow the Dolphin SDK as documented
// in its public headers and as observed through libogc's and Dolphin's behaviour; no SDK code is used.
#include "gcrecomp/threads.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#else
#include <pthread.h>
#endif

#include "gcrecomp/hw/gc_hw.h"
#include "gcrecomp/hw/os_defs.h"
#include "gcrecomp/interrupts.h"
#include "gcrecomp/vi.h"

namespace gcrecomp::sched {

namespace {

using namespace os;
using Clock = std::chrono::steady_clock;
using Lock = std::unique_lock<std::mutex>;

constexpr uint32_t kStackSize = 64u << 20;  // host stack of a guest thread (virtual, committed on use)
constexpr uint32_t kAlarmStackGap = 0x300;  // distance below the interrupted r1 for an alarm handler

// Unwinds the host stack of a guest thread that is cancelled (or dropped by reset()).
struct Killed {};
// Unwinds the host stack of a guest thread that called OSExitThread (or whose function returned).
struct Exited {};

struct GThread {
    uint32_t addr = 0;           // guest address of the OSThread
    std::condition_variable cv;  // the thread waits here for the baton
    PPCContext* ctx = nullptr;   // context of the guest code that calls into the scheduler
    uint64_t seq = 0;            // FIFO order among threads of equal priority
    bool waiting = false;        // on a thread queue
    bool exited = false;         // moribund
    bool killed = false;         // the host thread must unwind
    bool is_main = false;
    // Start-up data (threads created by OSCreateThread).
    uint32_t func = 0, arg = 0, sp = 0, r2 = 0, r13 = 0;
};

struct AlarmRec {
    uint32_t addr;
    uint64_t fire;
    uint64_t period;  // 0: one shot
    uint32_t handler;
};

struct State {
    std::mutex m;
    Memory* mem = nullptr;
    std::map<uint32_t, std::shared_ptr<GThread>> threads;  // by guest address
    GThread* cur = nullptr;
    GThread* main = nullptr;
    uint64_t next_seq = 1;
    int scheduler_disabled = 0;
    int irq_depth = 0;  // > 0 while an interrupt handler (alarm, VI retrace) runs
    bool retrace_armed = false;  // the guest registered a handler for the VI interrupt
    std::vector<AlarmRec> alarms;
    int live_hosts = 0;
    std::condition_variable hosts_cv;
    bool deadlock_reported = false;
};

// Never destroyed: blocked host threads may outlive static destruction.
State& S() {
    static State* s = new State;
    return *s;
}

// Interrupt state of the running guest thread (each guest thread is a host thread).
thread_local bool t_int_enabled = true;
// The guest thread this host thread runs (null for host threads that are not guest threads), and how many
// scheduler operations it is inside of: the periodic poll in Memory::translate must not re-enter those.
thread_local GThread* t_guest = nullptr;
thread_local int t_busy = 0;

// Earliest moment an interrupt source (an alarm, the VI retrace) comes due, in Clock nanoseconds; read without
// the lock by poll(). Updated by update_deadline() whenever alarms or the retrace source change.
constexpr int64_t kNoDeadline = INT64_MAX;
std::atomic<int64_t> g_deadline{kNoDeadline};

int64_t now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
int64_t to_ns(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count(); }

// ---- Guest memory lists --------------------------------------------------------------------------------
// Queues are {head, tail} pairs; nodes carry next/prev pointers at the given offsets.

void list_append(Memory* m, uint32_t q, uint32_t n, uint32_t o_next, uint32_t o_prev) {
    uint32_t tail = m->read32(q + 4);
    m->write32(n + o_next, 0);
    m->write32(n + o_prev, tail);
    if (tail) m->write32(tail + o_next, n);
    else m->write32(q, n);
    m->write32(q + 4, n);
}

void list_remove(Memory* m, uint32_t q, uint32_t n, uint32_t o_next, uint32_t o_prev) {
    uint32_t next = m->read32(n + o_next), prev = m->read32(n + o_prev);
    if (next) m->write32(next + o_prev, prev);
    else m->write32(q + 4, prev);
    if (prev) m->write32(prev + o_next, next);
    else m->write32(q, next);
    m->write32(n + o_next, 0);
    m->write32(n + o_prev, 0);
}

void tq_enqueue(Memory* m, uint32_t q, uint32_t t) {
    list_append(m, q, t, thread_off::LINK_NEXT, thread_off::LINK_PREV);
    m->write32(t + thread_off::QUEUE, q);
}

void tq_remove(Memory* m, uint32_t q, uint32_t t) {
    list_remove(m, q, t, thread_off::LINK_NEXT, thread_off::LINK_PREV);
    m->write32(t + thread_off::QUEUE, 0);
}

// ---- Time ----------------------------------------------------------------------------------------------

Clock::time_point deadline_for(uint64_t fire_ticks) {
    uint64_t now = timebase_now();
    if (fire_ticks <= now) return Clock::now();
    uint64_t d = std::min<uint64_t>(fire_ticks - now, uint64_t(hw::TIMEBASE_FREQ_HZ) * 3600);
    uint64_t sec = d / hw::TIMEBASE_FREQ_HZ, rem = d % hw::TIMEBASE_FREQ_HZ;
    return Clock::now() + std::chrono::nanoseconds(sec * 1000000000ull + rem * 1000000000ull / hw::TIMEBASE_FREQ_HZ);
}

// ---- Thread state --------------------------------------------------------------------------------------

GThread* find(uint32_t addr) {
    auto& ts = S().threads;
    auto it = ts.find(addr);
    return it == ts.end() ? nullptr : it->second.get();
}

int32_t suspend_count(const GThread& t) { return (int32_t)S().mem->read32(t.addr + thread_off::SUSPEND_COUNT); }
int32_t priority(const GThread& t) { return (int32_t)S().mem->read32(t.addr + thread_off::PRIORITY); }
bool detached(const GThread& t) { return S().mem->read16(t.addr + thread_off::ATTR) & thread_off::ATTR_DETACH; }
bool runnable(const GThread& t) { return !t.waiting && !t.exited && suspend_count(t) <= 0; }

// Mirrors the scheduler's view into the guest's state field.
void sync_all() {
    State& s = S();
    for (auto& [addr, t] : s.threads) {
        if (addr == 0) continue;
        uint16_t st;
        if (t->exited) st = OSThread::STATE_MORIBUND;
        else if (t->waiting) st = OSThread::STATE_WAITING;
        else if (t.get() == s.cur && runnable(*t)) st = OSThread::STATE_RUNNING;
        else st = OSThread::STATE_READY;
        s.mem->write16(addr + thread_off::STATE, st);
    }
}

// Highest-priority runnable thread; lowest sequence number breaks ties.
GThread* pick() {
    GThread* best = nullptr;
    int32_t best_prio = 0;
    for (auto& [addr, t] : S().threads) {
        if (!runnable(*t)) continue;
        int32_t p = priority(*t);
        if (!best || p < best_prio || (p == best_prio && t->seq < best->seq)) {
            best = t.get();
            best_prio = p;
        }
    }
    return best;
}

void make_ready(GThread& t) {
    t.waiting = false;
    if (runnable(t)) t.seq = S().next_seq++;
}

void switch_to(GThread* next) {
    State& s = S();
    s.cur = next;
    s.mem->write32(hw::OS_CURRENT_THREAD, next->addr);
    sync_all();
    next->cv.notify_one();
}

// ---- Interrupts: alarms (the decrementer) and the VI retrace -------------------------------------------------

// Recomputes g_deadline: the earliest alarm, or the next retrace if the guest registered its handler.
void update_deadline() {
    State& s = S();
    int64_t d = kNoDeadline;
    for (auto& a : s.alarms) d = std::min(d, to_ns(deadline_for(a.fire)));
    if (s.retrace_armed && vi::retrace_timer().started()) d = std::min(d, to_ns(vi::retrace_timer().next()));
    g_deadline.store(d, std::memory_order_relaxed);
}

// Runs an interrupt handler on the running guest thread, as the exception entry would: with a copy of the
// interrupted registers, a stack frame below the interrupted one, interrupts masked and no thread switch until the
// handler returns (threads it makes ready run afterwards, when the interrupted thread reschedules).
// `r3` and `r4` are the handler's arguments; `before` and `after` run in the same interrupt context.
template <class Before, class After>
void run_interrupt(Lock& lk, uint32_t handler, uint32_t r3, uint32_t r4, Before before, After after) {
    State& s = S();
    GThread* self = s.cur;
    PPCContext c;
    if (self->ctx) c = *self->ctx;
    else c.reset();
    c.r[1] = (c.r[1] - kAlarmStackGap) & ~0xFu;
    c.r[3] = r3;
    c.r[4] = r4;
    c.lr = 0;
    Memory* mem = s.mem;
    ++s.irq_depth;
    bool saved = t_int_enabled;
    t_int_enabled = false;
    lk.unlock();
    before();
    if (handler) g_func_table.call(handler, &c, mem);
    after(&c);
    lk.lock();
    t_int_enabled = saved;
    --s.irq_depth;
}

void run_alarm_handler(Lock& lk, const AlarmRec& a) {
    // r4: the interrupted context (the context is at offset 0 of the thread).
    run_interrupt(lk, a.handler, a.addr, S().cur->addr, [] {}, [](PPCContext*) {});
}

void fire_alarms(Lock& lk) {
    State& s = S();
    for (;;) {
        uint64_t now = timebase_now();
        size_t idx = s.alarms.size();
        for (size_t i = 0; i < s.alarms.size(); ++i)
            if (s.alarms[i].fire <= now && (idx == s.alarms.size() || s.alarms[i].fire < s.alarms[idx].fire)) idx = i;
        if (idx == s.alarms.size()) break;
        AlarmRec a = s.alarms[idx];
        if (a.period) {
            s.alarms[idx].fire = a.fire + a.period * ((now - a.fire) / a.period + 1);
            s.mem->write64(a.addr + alarm_off::FIRE, s.alarms[idx].fire);
        } else {
            s.alarms.erase(s.alarms.begin() + idx);
            s.mem->write32(a.addr + alarm_off::HANDLER, 0);
        }
        run_alarm_handler(lk, a);
    }
    update_deadline();
}

uint32_t be32_at(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// The VI retrace: when a field has elapsed, the enabled display interrupts become pending; if the guest has
// registered its handler and unmasked PI_VI, the handler runs (the SDK's __VIRetraceHandler clears the status
// bits, calls the retrace callbacks, counts the retrace and wakes VIWaitForRetrace).
void fire_retrace(Lock& lk) {
    State& s = S();
    if (!s.retrace_armed || s.irq_depth) return;
    vi::RetraceTimer& timer = vi::retrace_timer();
    if (!timer.take(Clock::now())) return;
    Memory* mem = s.mem;
    uint8_t* vi_regs = mem->hw_regs + (vi::kRegBase - Memory::HW_REG_BASE);
    uint8_t* pi_cause = mem->hw_regs + (vi::kPiRegCause - Memory::HW_REG_BASE);
    const uint8_t* pi_mask = mem->hw_regs + (vi::kPiRegMask - Memory::HW_REG_BASE);
    uint32_t handler = g_interrupts.handlers[vi::kRetraceInterrupt];
    bool raised = vi::raise_display_interrupts(vi_regs);
    if (raised) put_be32(pi_cause, be32_at(pi_cause) | vi::kPiCauseVi);
    bool deliver = raised && handler && (be32_at(pi_mask) & vi::kPiCauseVi);
    run_interrupt(
        lk, deliver ? handler : 0, vi::kRetraceInterrupt, s.cur->addr,
        [] {
            if (g_retrace_hook) g_retrace_hook();  // the host's window events
        },
        [&](PPCContext* c) {
            // Asynchronous memory card operations and disc reads complete from an interrupt as well.
            if (deliver) {
                card_run_pending_callbacks(c, mem);
                dvd_run_pending_callbacks(c, mem);
            }
        });
    if (!vi::display_interrupt_pending(vi_regs)) put_be32(pi_cause, be32_at(pi_cause) & ~vi::kPiCauseVi);
    update_deadline();
}

// Every thread is blocked: wait for the next alarm or retrace (the idle thread of the real OS).
void idle_wait(Lock& lk, GThread* self) {
    State& s = S();
    int64_t d = g_deadline.load(std::memory_order_relaxed);
    Clock::time_point until;
    if (d == kNoDeadline) {
        if (!s.deadlock_reported) {
            s.deadlock_reported = true;
            std::fprintf(stderr, "[sched] every guest thread is blocked and no alarm is pending\n");
        }
        until = Clock::now() + std::chrono::milliseconds(50);
    } else {
        until = Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(d)));
    }
    self->cv.wait_until(lk, until, [&] { return self->killed; });
    if (self->killed) throw Killed{};
}

// ---- The scheduler -----------------------------------------------------------------------------------------

// Gives the baton to the best thread. Returns when the calling thread holds it again (immediately if it is
// still the best one). A thread that has exited (`self->exited`) returns as soon as it handed the baton on.
void reschedule(Lock& lk) {
    State& s = S();
    GThread* self = s.cur;
    if (s.irq_depth) return;  // an interrupt handler: the interrupted thread is rescheduled afterwards
    for (;;) {
        if (self->killed) throw Killed{};
        if (!runnable(*self) || t_int_enabled) {
            fire_retrace(lk);
            fire_alarms(lk);
            if (self->killed) throw Killed{};
        }
        GThread* best = pick();
        if (best && best != self && runnable(*self) && s.scheduler_disabled > 0) best = self;
        if (!best) {
            idle_wait(lk, self);
            continue;
        }
        if (best == self) {
            sync_all();
            return;
        }
        switch_to(best);
        if (self->exited) return;
        self->cv.wait(lk, [&] { return s.cur == self || self->killed; });
        if (self->killed) throw Killed{};
        return;
    }
}

void sleep_on(Lock& lk, uint32_t queue) {
    State& s = S();
    s.cur->waiting = true;
    tq_enqueue(s.mem, queue, s.cur->addr);
    reschedule(lk);
}

// Makes every thread of the queue ready (in queue order). Does not reschedule.
void wake_all(uint32_t queue) {
    State& s = S();
    while (uint32_t a = s.mem->read32(queue)) {
        tq_remove(s.mem, queue, a);
        if (GThread* t = find(a)) make_ready(*t);
    }
}

// Priority inheritance: a thread runs at the best of its own priority and the priorities of the threads
// waiting for the mutexes it owns; a thread that is itself waiting for a mutex passes it on to the owner.
void recompute_priority(uint32_t t_addr, int depth = 0) {
    Memory* m = S().mem;
    int32_t eff = (int32_t)m->read32(t_addr + thread_off::BASE_PRIORITY);
    for (uint32_t mx = m->read32(t_addr + thread_off::MUTEX_QUEUE); mx; mx = m->read32(mx + mutex_off::LINK_NEXT))
        for (uint32_t w = m->read32(mx + mutex_off::QUEUE); w; w = m->read32(w + thread_off::LINK_NEXT))
            eff = std::min(eff, (int32_t)m->read32(w + thread_off::PRIORITY));
    if (eff == (int32_t)m->read32(t_addr + thread_off::PRIORITY)) return;
    m->write32(t_addr + thread_off::PRIORITY, (uint32_t)eff);
    uint32_t waiting_for = m->read32(t_addr + thread_off::MUTEX);
    if (waiting_for && depth < 32) {
        if (uint32_t owner = m->read32(waiting_for + mutex_off::THREAD)) recompute_priority(owner, depth + 1);
    }
}

void unlock_all_mutexes(uint32_t t_addr) {
    Memory* m = S().mem;
    uint32_t q = t_addr + thread_off::MUTEX_QUEUE;
    while (uint32_t mx = m->read32(q)) {
        list_remove(m, q, mx, mutex_off::LINK_NEXT, mutex_off::LINK_PREV);
        m->write32(mx + mutex_off::THREAD, 0);
        m->write32(mx + mutex_off::COUNT, 0);
        wake_all(mx + mutex_off::QUEUE);
    }
}

// ---- Entry of an API call --------------------------------------------------------------------------------

void init_main(Memory* mem) {
#if defined(_WIN32)
    // The default Windows timer tick is ~15.6 ms, far too coarse for the 1 ms alarms and retrace waits the
    // scheduler sleeps on; request 1 ms for the lifetime of the process.
    static const bool timer_resolution = timeBeginPeriod(1) == TIMERR_NOERROR;
    (void)timer_resolution;
#endif
    State& s = S();
    s.mem = mem;
    auto t = std::make_shared<GThread>();
    t->addr = mem->read32(hw::OS_CURRENT_THREAD);
    t->is_main = true;
    t->seq = s.next_seq++;
    s.cur = s.main = t.get();
    s.threads[t->addr] = t;
    t_guest = t.get();
}

// Holds the scheduler lock for the duration of one API call and keeps the guest's thread states in sync.
struct Op {
    Lock lk;
    Op(PPCContext* ctx, Memory* mem) : lk(S().m) {
        State& s = S();
        if (!s.cur) init_main(mem);
        s.mem = mem;
        if (s.irq_depth == 0) s.cur->ctx = ctx;
        ++t_busy;
    }
    ~Op() {
        --t_busy;
        if (!std::uncaught_exceptions()) sync_all();
    }
};

// ---- Host threads ----------------------------------------------------------------------------------------

[[noreturn]] void exit_current(Lock& lk, uint32_t value) {
    State& s = S();
    GThread* self = s.cur;
    Memory* m = s.mem;
    m->write32(self->addr + thread_off::EXIT_VALUE, value);
    unlock_all_mutexes(self->addr);
    self->exited = true;
    wake_all(self->addr + thread_off::JOIN_QUEUE);
    std::shared_ptr<GThread> keep;
    if (detached(*self)) {
        m->write16(self->addr + thread_off::STATE, 0);
        auto it = s.threads.find(self->addr);
        if (it != s.threads.end() && it->second.get() == self) {
            keep = it->second;
            s.threads.erase(it);
        }
    }
    reschedule(lk);
    if (self->is_main) {
        // The host's own thread cannot end here: it stays parked.
        self->cv.wait(lk, [&] { return self->killed; });
        throw Killed{};
    }
    throw Exited{};
}

void thread_main(std::shared_ptr<GThread> t) {
    State& s = S();
    try {
        PPCContext c;
        c.reset();
        Memory* mem;
        {
            Lock lk(s.m);
            t->cv.wait(lk, [&] { return s.cur == t.get() || t->killed; });
            if (t->killed) throw Killed{};
            mem = s.mem;
            t_guest = t.get();
            c.r[1] = t->sp;
            c.r[2] = t->r2;
            c.r[13] = t->r13;
            c.r[3] = t->arg;
            t->ctx = &c;
        }
        g_func_table.call(t->func, &c, mem);
        Lock lk(s.m);
        t->ctx = &c;
        exit_current(lk, c.r[3]);
    } catch (const Killed&) {
    } catch (const Exited&) {
    }
    Lock lk(s.m);
    --s.live_hosts;
    s.hosts_cv.notify_all();
}

#if defined(_WIN32)
void spawn(std::shared_ptr<GThread> t) {
    std::thread([t] { thread_main(t); }).detach();
}
#else
void* host_entry(void* p) {
    std::unique_ptr<std::shared_ptr<GThread>> t(static_cast<std::shared_ptr<GThread>*>(p));
    thread_main(*t);
    return nullptr;
}

void spawn(std::shared_ptr<GThread> t) {
    auto* arg = new std::shared_ptr<GThread>(std::move(t));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kStackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    int rc = pthread_create(&th, &attr, host_entry, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::fprintf(stderr, "[sched] cannot create a host thread (%d)\n", rc);
        std::abort();
    }
}
#endif

void retire(GThread& t) {
    t.exited = true;
    t.killed = true;
    t.cv.notify_one();
}

// Mutex internals (shared by OSLockMutex and OSWaitCond).
void mutex_lock(Lock& lk, uint32_t mx) {
    State& s = S();
    Memory* m = s.mem;
    for (;;) {
        uint32_t cur = s.cur->addr;
        uint32_t owner = m->read32(mx + mutex_off::THREAD);
        if (owner == 0) {
            m->write32(mx + mutex_off::THREAD, cur);
            m->write32(mx + mutex_off::COUNT, 1);
            list_append(m, cur + thread_off::MUTEX_QUEUE, mx, mutex_off::LINK_NEXT, mutex_off::LINK_PREV);
            return;
        }
        if (owner == cur) {
            m->write32(mx + mutex_off::COUNT, m->read32(mx + mutex_off::COUNT) + 1);
            return;
        }
        m->write32(cur + thread_off::MUTEX, mx);
        s.cur->waiting = true;
        tq_enqueue(m, mx + mutex_off::QUEUE, cur);
        recompute_priority(owner);  // the owner inherits our priority while we wait
        reschedule(lk);
        m->write32(cur + thread_off::MUTEX, 0);
    }
}

// Fully releases the mutex if the calling thread owns it. Returns false otherwise.
bool mutex_release(uint32_t mx, uint32_t cur) {
    Memory* m = S().mem;
    if (m->read32(mx + mutex_off::THREAD) != cur) return false;
    m->write32(mx + mutex_off::COUNT, 0);
    list_remove(m, cur + thread_off::MUTEX_QUEUE, mx, mutex_off::LINK_NEXT, mutex_off::LINK_PREV);
    m->write32(mx + mutex_off::THREAD, 0);
    recompute_priority(cur);
    wake_all(mx + mutex_off::QUEUE);
    return true;
}

}  // namespace

// ---- Public helpers ----------------------------------------------------------------------------------------

bool interrupts_exchange(bool enabled) {
    bool prev = t_int_enabled;
    t_int_enabled = enabled;
    return prev;
}

bool interrupts_enabled() { return t_int_enabled; }

void retrace_arm() {
    Lock lk(S().m);
    S().retrace_armed = true;
    vi::retrace_timer().start(Clock::now());
    update_deadline();
}

uint64_t retrace_count() { return vi::retrace_timer().count(); }

void poll(PPCContext* ctx, Memory* mem) {
    // The decrementer and the VI retrace are timers that interrupt whatever runs. Interrupts cannot preempt
    // recompiled code, so they are taken at the calls a spinning loop makes (and every 256K memory operations,
    // see Memory::translate): when the earliest deadline has passed and the thread has interrupts enabled.
    if (g_deadline.load(std::memory_order_relaxed) == kNoDeadline) return;
    if (now_ns() < g_deadline.load(std::memory_order_relaxed)) return;
    if (!t_int_enabled || t_busy) return;
    if (!ctx) {
        if (!t_guest) return;
        ctx = t_guest->ctx;
        if (!ctx) return;
    }
    Op op(ctx, mem);
    if (S().irq_depth) return;
    reschedule(op.lk);
}

void reset() {
    State& s = S();
    Lock lk(s.m);
    for (auto& [addr, t] : s.threads) {
        if (t.get() == s.main) continue;
        t->killed = true;
        t->cv.notify_one();
    }
    s.hosts_cv.wait(lk, [&] { return s.live_hosts == 0; });
    s.threads.clear();
    s.alarms.clear();
    s.retrace_armed = false;
    vi::retrace_timer().reset();
    update_deadline();
    s.cur = s.main = nullptr;
    t_guest = nullptr;
    t_busy = 0;
    s.scheduler_disabled = 0;
    s.irq_depth = 0;
    s.next_seq = 1;
    s.deadlock_reported = false;
    t_int_enabled = true;
}

size_t thread_count() {
    Lock lk(S().m);
    return S().threads.size();
}

// ---- Threads ---------------------------------------------------------------------------------------------

// OSCreateThread(thread, func, arg, stack_top, stack_size, priority, attr)
void os_create_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    State& s = S();
    uint32_t addr = ctx->r[3], stack = ctx->r[6], size = ctx->r[7];
    int32_t prio = (int32_t)ctx->r[8];
    if (prio < 0 || prio > 31) {
        ctx->r[3] = 0;
        return;
    }
    if (auto it = s.threads.find(addr); it != s.threads.end()) {  // the address is being reused
        if (it->second.get() == s.cur) {
            ctx->r[3] = 0;
            return;
        }
        retire(*it->second);
        s.threads.erase(it);
    }
    if (uint8_t* p = mem->translate(addr)) std::memset(p, 0, OSThread::SIZE);
    mem->write16(addr + thread_off::STATE, OSThread::STATE_READY);
    mem->write16(addr + thread_off::ATTR, (uint16_t)(ctx->r[9] & thread_off::ATTR_DETACH));
    mem->write32(addr + thread_off::SUSPEND_COUNT, 1);  // created suspended
    mem->write32(addr + thread_off::PRIORITY, (uint32_t)prio);
    mem->write32(addr + thread_off::BASE_PRIORITY, (uint32_t)prio);
    mem->write32(addr + thread_off::EXIT_VALUE, 0xFFFFFFFFu);
    mem->write32(addr + thread_off::STACK_BASE, stack);
    mem->write32(addr + thread_off::STACK_END, stack - size);
    if (size >= 4) mem->write32(stack - size, 0xDEADBABE);  // the SDK's stack overflow marker

    auto t = std::make_shared<GThread>();
    t->addr = addr;
    t->func = ctx->r[4];
    t->arg = ctx->r[5];
    t->sp = stack - 8;
    mem->write32(t->sp, 0);
    mem->write32(t->sp + 4, 0);
    t->r2 = ctx->r[2];
    t->r13 = ctx->r[13];
    t->seq = s.next_seq++;
    s.threads[addr] = t;
    ++s.live_hosts;
    spawn(t);
    ctx->r[3] = 1;
}

void os_exit_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    exit_current(op.lk, ctx->r[3]);
}

void os_cancel_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    State& s = S();
    GThread* t = find(ctx->r[3]);
    if (!t || t->exited) return;
    if (t == s.cur) exit_current(op.lk, 0xFFFFFFFFu);
    if (uint32_t q = mem->read32(t->addr + thread_off::QUEUE)) tq_remove(mem, q, t->addr);
    mem->write32(t->addr + thread_off::MUTEX, 0);
    unlock_all_mutexes(t->addr);
    mem->write32(t->addr + thread_off::EXIT_VALUE, 0xFFFFFFFFu);
    std::shared_ptr<GThread> keep = s.threads[t->addr];
    retire(*t);
    wake_all(t->addr + thread_off::JOIN_QUEUE);
    if (detached(*t)) {
        mem->write16(t->addr + thread_off::STATE, 0);
        s.threads.erase(t->addr);
    }
    reschedule(op.lk);
}

void os_detach_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t a = ctx->r[3];
    mem->write16(a + thread_off::ATTR, mem->read16(a + thread_off::ATTR) | thread_off::ATTR_DETACH);
    GThread* t = find(a);
    if (t && t->exited) {
        mem->write16(a + thread_off::STATE, 0);
        S().threads.erase(a);
    }
    wake_all(a + thread_off::JOIN_QUEUE);
    reschedule(op.lk);
}

// OSJoinThread(thread, &value): TRUE once the thread has finished.
void os_join_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    State& s = S();
    uint32_t a = ctx->r[3], out = ctx->r[4];
    auto it = s.threads.find(a);
    if (it == s.threads.end() || detached(*it->second)) {
        ctx->r[3] = 0;
        return;
    }
    std::shared_ptr<GThread> t = it->second;
    if (!t->exited) sleep_on(op.lk, a + thread_off::JOIN_QUEUE);
    if (t->exited) {
        if (out) mem->write32(out, mem->read32(a + thread_off::EXIT_VALUE));
        auto cur = s.threads.find(a);
        if (cur != s.threads.end() && cur->second == t) s.threads.erase(cur);
        ctx->r[3] = 1;
    } else {
        ctx->r[3] = 0;
    }
}

// OSResumeThread(thread): returns the previous suspend count.
void os_resume_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t a = ctx->r[3];
    int32_t old = (int32_t)mem->read32(a + thread_off::SUSPEND_COUNT);
    GThread* t = find(a);
    if (old > 0) {
        mem->write32(a + thread_off::SUSPEND_COUNT, (uint32_t)(old - 1));
        if (t && old == 1 && !t->waiting && !t->exited) t->seq = S().next_seq++;
    }
    ctx->r[3] = (uint32_t)old;
    reschedule(op.lk);
}

// OSSuspendThread(thread): returns the previous suspend count.
void os_suspend_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t a = ctx->r[3];
    int32_t old = (int32_t)mem->read32(a + thread_off::SUSPEND_COUNT);
    mem->write32(a + thread_off::SUSPEND_COUNT, (uint32_t)(old + 1));
    ctx->r[3] = (uint32_t)old;
    reschedule(op.lk);
}

// OSSleepThread(queue)
void os_sleep_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    sleep_on(op.lk, ctx->r[3]);
}

// OSWakeupThread(queue): every sleeper of the queue becomes ready.
void os_wakeup_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    wake_all(ctx->r[3]);
    reschedule(op.lk);
}

void os_yield_thread(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    S().cur->seq = S().next_seq++;
    reschedule(op.lk);
}

// OSSetThreadPriority(thread, priority): FALSE if the priority is out of range.
void os_set_thread_priority(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t a = ctx->r[3];
    int32_t prio = (int32_t)ctx->r[4];
    if (prio < 0 || prio > 31) {
        ctx->r[3] = 0;
        return;
    }
    mem->write32(a + thread_off::BASE_PRIORITY, (uint32_t)prio);
    mem->write32(a + thread_off::PRIORITY, (uint32_t)prio);
    recompute_priority(a);
    // A thread that waits for a mutex passes its new priority on to the owner.
    if (uint32_t mx = mem->read32(a + thread_off::MUTEX))
        if (uint32_t owner = mem->read32(mx + mutex_off::THREAD)) recompute_priority(owner);
    ctx->r[3] = 1;
    reschedule(op.lk);
}

void os_get_thread_priority(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = mem->read32(ctx->r[3] + thread_off::BASE_PRIORITY);
}

void os_get_current_thread(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = mem->read32(hw::OS_CURRENT_THREAD);
}

void os_is_thread_suspended(PPCContext* ctx, Memory* mem) {
    ctx->r[3] = (int32_t)mem->read32(ctx->r[3] + thread_off::SUSPEND_COUNT) > 0 ? 1 : 0;
}

void os_is_thread_terminated(PPCContext* ctx, Memory* mem) {
    uint16_t st = mem->read16(ctx->r[3] + thread_off::STATE);
    ctx->r[3] = (st == OSThread::STATE_MORIBUND || st == 0) ? 1 : 0;
}

// Returns the previous disable count.
void os_disable_scheduler(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    ctx->r[3] = (uint32_t)S().scheduler_disabled++;
}

void os_enable_scheduler(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    State& s = S();
    ctx->r[3] = (uint32_t)s.scheduler_disabled;
    if (s.scheduler_disabled > 0 && --s.scheduler_disabled == 0) reschedule(op.lk);
}

void os_init_thread_queue(PPCContext* ctx, Memory* mem) {
    mem->write32(ctx->r[3], 0);
    mem->write32(ctx->r[3] + 4, 0);
}

// ---- Mutexes and condition variables ---------------------------------------------------------------------

void os_init_mutex(PPCContext* ctx, Memory* mem) {
    if (uint8_t* p = mem->translate(ctx->r[3])) std::memset(p, 0, OSMutex::SIZE);
}

void os_lock_mutex(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    mutex_lock(op.lk, ctx->r[3]);
}

void os_try_lock_mutex(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t mx = ctx->r[3], cur = S().cur->addr;
    uint32_t owner = mem->read32(mx + mutex_off::THREAD);
    if (owner == 0 || owner == cur) {
        mutex_lock(op.lk, mx);  // cannot block
        ctx->r[3] = 1;
    } else {
        ctx->r[3] = 0;
    }
}

void os_unlock_mutex(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t mx = ctx->r[3], cur = S().cur->addr;
    if (mem->read32(mx + mutex_off::THREAD) != cur) return;
    uint32_t count = mem->read32(mx + mutex_off::COUNT);
    if (count > 1) {
        mem->write32(mx + mutex_off::COUNT, count - 1);
        return;
    }
    mutex_release(mx, cur);
    reschedule(op.lk);
}

void os_init_cond(PPCContext* ctx, Memory* mem) {
    mem->write32(ctx->r[3], 0);
    mem->write32(ctx->r[3] + 4, 0);
}

// OSWaitCond(cond, mutex): releases the mutex, sleeps, takes the mutex back with its old count.
void os_wait_cond(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t cond = ctx->r[3], mx = ctx->r[4], cur = S().cur->addr;
    if (mem->read32(mx + mutex_off::THREAD) != cur) return;
    uint32_t count = mem->read32(mx + mutex_off::COUNT);
    mutex_release(mx, cur);
    sleep_on(op.lk, cond);
    mutex_lock(op.lk, mx);
    mem->write32(mx + mutex_off::COUNT, count);
}

void os_signal_cond(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    wake_all(ctx->r[3]);
    reschedule(op.lk);
}

// ---- Message queues --------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kMessageBlock = 1;  // OS_MESSAGE_BLOCK
}

// OSInitMessageQueue(queue, array, count)
void os_init_message_queue(PPCContext* ctx, Memory* mem) {
    uint32_t q = ctx->r[3];
    if (uint8_t* p = mem->translate(q)) std::memset(p, 0, OSMessageQueue::SIZE);
    mem->write32(q + mq_off::MSG_ARRAY, ctx->r[4]);
    mem->write32(q + mq_off::MSG_COUNT, ctx->r[5]);
}

namespace {
// Shared by OSSendMessage (append) and OSJamMessage (put in front).
void put_message(PPCContext* ctx, Memory* mem, bool jam) {
    Op op(ctx, mem);
    uint32_t q = ctx->r[3], msg = ctx->r[4], flags = ctx->r[5];
    uint32_t count = mem->read32(q + mq_off::MSG_COUNT);
    while (mem->read32(q + mq_off::USED_COUNT) >= count) {
        if (!(flags & kMessageBlock)) {
            ctx->r[3] = 0;
            return;
        }
        sleep_on(op.lk, q + mq_off::SEND_QUEUE);
    }
    uint32_t array = mem->read32(q + mq_off::MSG_ARRAY);
    uint32_t first = mem->read32(q + mq_off::FIRST_INDEX), used = mem->read32(q + mq_off::USED_COUNT);
    if (jam) {
        first = (first + count - 1) % count;
        mem->write32(q + mq_off::FIRST_INDEX, first);
        mem->write32(array + 4 * first, msg);
    } else {
        mem->write32(array + 4 * ((first + used) % count), msg);
    }
    mem->write32(q + mq_off::USED_COUNT, used + 1);
    wake_all(q + mq_off::RECEIVE_QUEUE);
    ctx->r[3] = 1;
    reschedule(op.lk);
}
}  // namespace

// OSSendMessage(queue, message, flags)
void os_send_message(PPCContext* ctx, Memory* mem) { put_message(ctx, mem, false); }
void os_jam_message(PPCContext* ctx, Memory* mem) { put_message(ctx, mem, true); }

// OSReceiveMessage(queue, &message, flags)
void os_receive_message(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint32_t q = ctx->r[3], out = ctx->r[4], flags = ctx->r[5];
    while (mem->read32(q + mq_off::USED_COUNT) == 0) {
        if (!(flags & kMessageBlock)) {
            ctx->r[3] = 0;
            return;
        }
        sleep_on(op.lk, q + mq_off::RECEIVE_QUEUE);
    }
    uint32_t count = mem->read32(q + mq_off::MSG_COUNT), array = mem->read32(q + mq_off::MSG_ARRAY);
    uint32_t first = mem->read32(q + mq_off::FIRST_INDEX);
    if (out) mem->write32(out, mem->read32(array + 4 * first));
    mem->write32(q + mq_off::FIRST_INDEX, (first + 1) % count);
    mem->write32(q + mq_off::USED_COUNT, mem->read32(q + mq_off::USED_COUNT) - 1);
    wake_all(q + mq_off::SEND_QUEUE);
    ctx->r[3] = 1;
    reschedule(op.lk);
}

// ---- Alarms ----------------------------------------------------------------------------------------------

namespace {
uint64_t reg64(const PPCContext* ctx, int hi) { return ((uint64_t)ctx->r[hi] << 32) | ctx->r[hi + 1]; }

void arm_alarm(Memory* mem, uint32_t alarm, uint64_t fire, uint64_t period, uint64_t start, uint32_t handler) {
    State& s = S();
    s.alarms.erase(std::remove_if(s.alarms.begin(), s.alarms.end(), [&](const AlarmRec& a) { return a.addr == alarm; }),
                   s.alarms.end());
    s.alarms.push_back({alarm, fire, period, handler});
    update_deadline();
    mem->write32(alarm + alarm_off::HANDLER, handler);
    mem->write64(alarm + alarm_off::FIRE, fire);
    mem->write64(alarm + alarm_off::PERIOD, period);
    mem->write64(alarm + alarm_off::START, start);
}
}  // namespace

void os_create_alarm(PPCContext* ctx, Memory* mem) {
    if (uint8_t* p = mem->translate(ctx->r[3])) std::memset(p, 0, OSAlarm::SIZE);
}

// OSSetAlarm(alarm, tick, handler): `tick` (r4:r5) is relative to now.
void os_set_alarm(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint64_t tick = reg64(ctx, 4);
    if ((int64_t)tick < 0) tick = 0;
    arm_alarm(mem, ctx->r[3], timebase_now() + tick, 0, 0, ctx->r[6]);
}

// OSSetPeriodicAlarm(alarm, start, period, handler): `start` (r4:r5) is an absolute time.
void os_set_periodic_alarm(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    uint64_t start = reg64(ctx, 4), period = reg64(ctx, 6), now = timebase_now();
    uint64_t fire = start;
    if (period && start < now) fire = start + period * ((now - start) / period + 1);
    arm_alarm(mem, ctx->r[3], fire, period, start, ctx->r[8]);
}

void os_cancel_alarm(PPCContext* ctx, Memory* mem) {
    Op op(ctx, mem);
    State& s = S();
    uint32_t a = ctx->r[3];
    s.alarms.erase(std::remove_if(s.alarms.begin(), s.alarms.end(), [&](const AlarmRec& r) { return r.addr == a; }),
                   s.alarms.end());
    update_deadline();
    mem->write32(a + alarm_off::HANDLER, 0);
}

}  // namespace gcrecomp::sched
