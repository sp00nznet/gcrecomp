// SPDX-License-Identifier: MIT
//
// Guest-thread scheduler, alarm and VI retrace interrupt tests. The guest threads are synthetic RecompiledFuncs that call the same
// replacement functions the recompiled SDK code reaches; only one guest thread runs at a time, so the
// event log needs no locking and its order is deterministic.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gcrecomp/hw/gc_hw.h"
#include "gcrecomp/hw/os_defs.h"
#include "gcrecomp/interrupts.h"
#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"
#include "gcrecomp/vi.h"

using namespace gcrecomp;
using namespace gcrecomp::os;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

constexpr uint32_t kMain = 0x80003800;
constexpr uint32_t kQ1 = 0x80020000, kQ2 = 0x80020010, kMutex = 0x80020100, kMutex2 = 0x80020120;
constexpr uint32_t kMq = 0x80020200, kMqArray = 0x80020240, kCond = 0x80020300, kAlarm = 0x80020400;
constexpr uint32_t kFlag = 0x80020500, kOut = 0x80020510, kAlarm2 = 0x80020420;

uint32_t thread_addr(int i) { return 0x80010000 + i * 0x400; }
uint32_t stack_top(int i) { return 0x80500000 + (i + 1) * 0x10000; }
uint32_t func_addr(int i) { return 0x80300000 + i * 4; }

std::vector<std::string> g_log;
void LOG(const std::string& s) { g_log.push_back(s); }
bool log_is(const std::vector<std::string>& expected) {
    if (g_log == expected) return true;
    std::fprintf(stderr, "  log:     ");
    for (auto& s : g_log) std::fprintf(stderr, "%s ", s.c_str());
    std::fprintf(stderr, "\n  expected:");
    for (auto& s : expected) std::fprintf(stderr, " %s", s.c_str());
    std::fprintf(stderr, "\n");
    return false;
}

PPCContext g_main_ctx;
int g_next_func = 0;

void setup() {
    sched::reset();
    if (!g_mem.ram) g_mem.init();
    std::memset(g_mem.ram, 0, 0x600000 + 0x100000);
    g_func_table.table.clear();
    g_next_func = 0;
    g_log.clear();
    g_mem.write32(hw::OS_CURRENT_THREAD, kMain);
    g_mem.write16(kMain + thread_off::STATE, OSThread::STATE_RUNNING);
    g_mem.write32(kMain + thread_off::PRIORITY, 16);
    g_mem.write32(kMain + thread_off::BASE_PRIORITY, 16);
    g_main_ctx.reset();
    g_main_ctx.r[1] = 0x80480000;
    // No VI/PI state, and no handler for the retrace interrupt, from an earlier test.
    std::memset(g_mem.hw_regs + (vi::kRegBase - Memory::HW_REG_BASE), 0, 0x100);
    std::memset(g_mem.hw_regs + (vi::kPiRegCause - Memory::HW_REG_BASE), 0, 0x10);
    g_interrupts.handlers[vi::kRetraceInterrupt] = 0;
}

uint32_t call(PPCContext* c, RecompiledFunc fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0,
              uint32_t a4 = 0, uint32_t a5 = 0, uint32_t a6 = 0) {
    c->r[3] = a0, c->r[4] = a1, c->r[5] = a2, c->r[6] = a3, c->r[7] = a4, c->r[8] = a5, c->r[9] = a6;
    fn(c, &g_mem);
    return c->r[3];
}

// OSCreateThread with a synthetic function; the thread is suspended until OSResumeThread.
uint32_t create(PPCContext* c, int slot, RecompiledFunc fn, uint32_t arg, int prio, uint32_t attr = 0) {
    uint32_t f = func_addr(g_next_func++);
    g_func_table.register_func(f, fn);
    CHECK(call(c, sched::os_create_thread, thread_addr(slot), f, arg, stack_top(slot), 0x8000, (uint32_t)prio, attr) == 1);
    return thread_addr(slot);
}

uint32_t state_of(uint32_t t) { return g_mem.read16(t + thread_off::STATE); }
uint32_t prio_of(uint32_t t) { return g_mem.read32(t + thread_off::PRIORITY); }

// ---- Sleep and wakeup hand the baton back and forth ---------------------------------------------------------

void worker_pingpong(PPCContext* c, Memory*) {
    LOG("W1");
    call(c, sched::os_wakeup_thread, kQ1);
    LOG("W2");
    call(c, sched::os_sleep_thread, kQ2);
    LOG("W3");
    c->r[3] = 7;
}

void test_sleep_wakeup_handoff() {
    setup();
    PPCContext* c = &g_main_ctx;
    uint32_t w = create(c, 1, worker_pingpong, 0, 16);
    CHECK(state_of(w) == OSThread::STATE_READY);
    CHECK(g_mem.read32(w + thread_off::SUSPEND_COUNT) == 1);
    CHECK(g_mem.read32(w + thread_off::STACK_BASE) == stack_top(1));
    CHECK(g_mem.read32(w + thread_off::STACK_END) == stack_top(1) - 0x8000);
    CHECK(call(c, sched::os_is_thread_suspended, w) == 1);
    CHECK(call(c, sched::os_resume_thread, w) == 1);  // previous suspend count
    LOG("M1");  // same priority: the resume did not preempt
    call(c, sched::os_sleep_thread, kQ1);
    // W ran W1, woke us (no preemption at equal priority), ran W2 and went to sleep on Q2.
    LOG("M2");
    CHECK(state_of(w) == OSThread::STATE_WAITING);
    CHECK(g_mem.read32(w + thread_off::QUEUE) == kQ2 && g_mem.read32(kQ2) == w);
    CHECK(g_mem.read32(hw::OS_CURRENT_THREAD) == kMain);
    CHECK(state_of(kMain) == OSThread::STATE_RUNNING);
    call(c, sched::os_wakeup_thread, kQ2);
    LOG("M3");
    CHECK(call(c, sched::os_join_thread, w, kOut) == 1);
    LOG("M4");
    CHECK(g_mem.read32(kOut) == 7);
    CHECK(log_is({"M1", "W1", "W2", "M2", "M3", "W3", "M4"}));
}

// ---- Priority --------------------------------------------------------------------------------------------

void fn_log_h(PPCContext* c, Memory*) { LOG("H1"); c->r[3] = 0; }
void fn_log_l(PPCContext* c, Memory*) {
    LOG("L1");
    call(c, sched::os_wakeup_thread, kQ1);
    LOG("L2");
    c->r[3] = 0;
}

void test_priority_preemption_on_resume() {
    setup();
    PPCContext* c = &g_main_ctx;
    uint32_t h = create(c, 1, fn_log_h, 0, 4);
    uint32_t l = create(c, 2, fn_log_l, 0, 20);
    LOG("M1");
    call(c, sched::os_resume_thread, h);  // higher priority: runs to its end before we continue
    LOG("M2");
    CHECK(state_of(h) == OSThread::STATE_MORIBUND);
    call(c, sched::os_resume_thread, l);  // lower priority: does not run
    LOG("M3");
    call(c, sched::os_yield_thread);  // nobody at our priority or above: returns at once
    LOG("M4");
    call(c, sched::os_sleep_thread, kQ1);  // L runs; waking us preempts it at once
    LOG("M5");
    CHECK(state_of(l) == OSThread::STATE_READY);
    CHECK(call(c, sched::os_join_thread, l, 0) == 1);
    LOG("M6");
    CHECK(log_is({"M1", "H1", "M2", "M3", "M4", "L1", "M5", "L2", "M6"}));
}

void fn_log_p(PPCContext* c, Memory*) { LOG("P1"); c->r[3] = 0; }

void test_set_priority_and_scheduler_disable() {
    setup();
    PPCContext* c = &g_main_ctx;
    uint32_t p = create(c, 1, fn_log_p, 0, 30);
    call(c, sched::os_resume_thread, p);
    LOG("M1");  // P is below us
    CHECK(call(c, sched::os_set_thread_priority, p, 32) == 0);  // out of range
    CHECK(call(c, sched::os_set_thread_priority, p, 2) == 1);   // raising it above us runs it at once
    LOG("M2");
    CHECK(g_mem.read32(p + thread_off::BASE_PRIORITY) == 2);
    CHECK(log_is({"M1", "P1", "M2"}));

    setup();
    c = &g_main_ctx;
    p = create(c, 1, fn_log_p, 0, 2);
    CHECK(call(c, sched::os_disable_scheduler) == 0);
    CHECK(call(c, sched::os_disable_scheduler) == 1);
    call(c, sched::os_resume_thread, p);
    LOG("M1");  // no switch while the scheduler is disabled
    CHECK(call(c, sched::os_enable_scheduler) == 2);
    LOG("M2");  // still disabled once
    CHECK(call(c, sched::os_enable_scheduler) == 1);
    LOG("M3");  // enabled: P runs inside that call, before M3
    CHECK(log_is({"M1", "M2", "P1", "M3"}));
}

// ---- Mutexes ---------------------------------------------------------------------------------------------

void fn_mutex_contender(PPCContext* c, Memory*) {
    std::string n = "T" + std::to_string(c->r[3]);
    if (c->r[3] == 1) {
        CHECK(call(c, sched::os_try_lock_mutex, kMutex) == 0);  // held by main
        LOG(n + "-try0");
    }
    LOG(n + "-lock");
    call(c, sched::os_lock_mutex, kMutex);
    LOG(n + "-got");
    CHECK(g_mem.read32(kMutex + mutex_off::THREAD) == g_mem.read32(hw::OS_CURRENT_THREAD));
    call(c, sched::os_unlock_mutex, kMutex);
    LOG(n + "-done");
    c->r[3] = 0;
}

void test_mutex_contention() {
    setup();
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_init_mutex, kMutex);
    CHECK(call(c, sched::os_try_lock_mutex, kMutex) == 1);
    call(c, sched::os_lock_mutex, kMutex);  // recursive
    CHECK(g_mem.read32(kMutex + mutex_off::COUNT) == 2);
    call(c, sched::os_unlock_mutex, kMutex);
    CHECK(g_mem.read32(kMutex + mutex_off::THREAD) == kMain);  // still ours
    uint32_t t1 = create(c, 1, fn_mutex_contender, 1, 10);
    uint32_t t2 = create(c, 2, fn_mutex_contender, 2, 8);
    call(c, sched::os_resume_thread, t1);  // runs at once (higher priority), blocks on the mutex
    LOG("M-after-t1");
    CHECK(state_of(t1) == OSThread::STATE_WAITING);
    CHECK(g_mem.read32(t1 + thread_off::MUTEX) == kMutex);
    CHECK(prio_of(kMain) == 10);  // priority inheritance
    CHECK(g_mem.read32(kMain + thread_off::BASE_PRIORITY) == 16);
    call(c, sched::os_resume_thread, t2);  // priority 8: above our inherited 10, runs at once and blocks too
    LOG("M-after-t2");
    CHECK(prio_of(kMain) == 8);
    call(c, sched::os_unlock_mutex, kMutex);  // both wake; T2 (priority 8) takes the mutex first
    LOG("M-end");
    CHECK(prio_of(kMain) == 16);
    CHECK(g_mem.read32(kMutex + mutex_off::THREAD) == 0);
    CHECK(log_is({"T1-try0", "T1-lock", "M-after-t1", "T2-lock", "M-after-t2", "T2-got", "T2-done", "T1-got", "T1-done",
                  "M-end"}));
}

// A thread that holds a mutex when it is cancelled releases it.
void fn_hold_and_sleep(PPCContext* c, Memory*) {
    call(c, sched::os_lock_mutex, kMutex2);
    LOG("holding");
    call(c, sched::os_sleep_thread, kQ2);
    LOG("never");
}

void test_cancel_releases_mutex_and_join() {
    setup();
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_init_mutex, kMutex2);
    uint32_t t = create(c, 1, fn_hold_and_sleep, 0, 8);
    call(c, sched::os_resume_thread, t);
    CHECK(g_mem.read32(kMutex2 + mutex_off::THREAD) == t);
    call(c, sched::os_cancel_thread, t);
    CHECK(g_mem.read32(kMutex2 + mutex_off::THREAD) == 0);
    CHECK(state_of(t) == OSThread::STATE_MORIBUND);
    CHECK(g_mem.read32(kQ2) == 0);  // removed from the queue
    CHECK(call(c, sched::os_join_thread, t, kOut) == 1);
    CHECK(g_mem.read32(kOut) == 0xFFFFFFFFu);
    CHECK(log_is({"holding"}));
}

// ---- Condition variables ---------------------------------------------------------------------------------

void fn_cond_waiter(PPCContext* c, Memory*) {
    call(c, sched::os_lock_mutex, kMutex);
    call(c, sched::os_lock_mutex, kMutex);  // twice: the count survives the wait
    LOG("W-wait");
    call(c, sched::os_wait_cond, kCond, kMutex);
    LOG("W-woke");
    CHECK(g_mem.read32(kMutex + mutex_off::THREAD) == g_mem.read32(hw::OS_CURRENT_THREAD));
    CHECK(g_mem.read32(kMutex + mutex_off::COUNT) == 2);
    call(c, sched::os_unlock_mutex, kMutex);
    call(c, sched::os_unlock_mutex, kMutex);
    c->r[3] = 0;
}

void test_condition_variable() {
    setup();
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_init_mutex, kMutex);
    call(c, sched::os_init_cond, kCond);
    uint32_t w = create(c, 1, fn_cond_waiter, 0, 16);
    call(c, sched::os_resume_thread, w);
    call(c, sched::os_yield_thread);  // W runs, waits on the condition (releasing the mutex)
    call(c, sched::os_lock_mutex, kMutex);
    call(c, sched::os_signal_cond, kCond);
    LOG("M-signal");
    call(c, sched::os_unlock_mutex, kMutex);
    CHECK(call(c, sched::os_join_thread, w, 0) == 1);
    CHECK(log_is({"W-wait", "M-signal", "W-woke"}));
}

// ---- Message queues --------------------------------------------------------------------------------------

void fn_producer(PPCContext* c, Memory*) {
    for (uint32_t i = 1; i <= 5; ++i) {
        CHECK(call(c, sched::os_send_message, kMq, i, 1) == 1);  // OS_MESSAGE_BLOCK
        LOG("sent" + std::to_string(i));
    }
    c->r[3] = 0;
}

void test_message_queue() {
    setup();
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_init_message_queue, kMq, kMqArray, 2);
    CHECK(call(c, sched::os_receive_message, kMq, kOut, 0) == 0);  // empty, non-blocking
    uint32_t p = create(c, 1, fn_producer, 0, 16);
    call(c, sched::os_resume_thread, p);
    std::vector<uint32_t> got;
    for (int i = 0; i < 5; ++i) {
        CHECK(call(c, sched::os_receive_message, kMq, kOut, 1) == 1);  // blocks while the queue is empty
        got.push_back(g_mem.read32(kOut));
    }
    CHECK((got == std::vector<uint32_t>{1, 2, 3, 4, 5}));
    CHECK(call(c, sched::os_join_thread, p, 0) == 1);
    // The capacity of 2 bounded the producer: it was blocked until the consumer made room.
    CHECK(log_is({"sent1", "sent2", "sent3", "sent4", "sent5"}));

    setup();
    c = &g_main_ctx;
    call(c, sched::os_init_message_queue, kMq, kMqArray, 3);
    CHECK(call(c, sched::os_send_message, kMq, 1, 0) == 1);
    CHECK(call(c, sched::os_send_message, kMq, 2, 0) == 1);
    CHECK(call(c, sched::os_jam_message, kMq, 9, 0) == 1);   // goes in front
    CHECK(call(c, sched::os_send_message, kMq, 3, 0) == 0);  // full, non-blocking
    for (uint32_t want : {9u, 1u, 2u}) {
        CHECK(call(c, sched::os_receive_message, kMq, kOut, 0) == 1);
        CHECK(g_mem.read32(kOut) == want);
    }
    CHECK(call(c, sched::os_receive_message, kMq, kOut, 0) == 0);
}

// ---- Alarms ----------------------------------------------------------------------------------------------

int g_alarm_calls = 0;

void alarm_wake_q1(PPCContext* c, Memory*) {
    ++g_alarm_calls;
    CHECK(c->r[3] == kAlarm);
    LOG("alarm");
    call(c, sched::os_wakeup_thread, kQ1);
}

void alarm_count(PPCContext*, Memory*) { ++g_alarm_calls; }

uint32_t reg_func(RecompiledFunc fn) {
    uint32_t f = func_addr(g_next_func++);
    g_func_table.register_func(f, fn);
    return f;
}

void alarm_wake_q2(PPCContext* c, Memory*) { call(c, sched::os_wakeup_thread, kQ2); }

// The calling thread sleeps for `ms` (an alarm wakes it) while the other threads and alarms run.
void sleep_ms(PPCContext* c, int ms) {
    c->r[3] = kAlarm2, c->r[4] = 0, c->r[5] = uint32_t(ms * (hw::TIMEBASE_FREQ_HZ / 1000)), c->r[6] = reg_func(alarm_wake_q2);
    sched::os_set_alarm(c, &g_mem);
    call(c, sched::os_sleep_thread, kQ2);
}

void test_alarm_wakes_sleeper() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_create_alarm, kAlarm);
    uint64_t ticks = 2 * (hw::TIMEBASE_FREQ_HZ / 1000);  // 2 ms
    auto t0 = std::chrono::steady_clock::now();
    c->r[3] = kAlarm, c->r[4] = (uint32_t)(ticks >> 32), c->r[5] = (uint32_t)ticks, c->r[6] = reg_func(alarm_wake_q1);
    sched::os_set_alarm(c, &g_mem);
    CHECK(g_mem.read32(kAlarm + alarm_off::HANDLER) == c->r[6]);
    LOG("M1");
    call(c, sched::os_sleep_thread, kQ1);  // every thread is blocked: the alarm is what wakes us
    auto elapsed = std::chrono::steady_clock::now() - t0;
    LOG("M2");
    CHECK(elapsed >= std::chrono::microseconds(1900));
    CHECK(g_alarm_calls == 1);
    CHECK(g_mem.read32(kAlarm + alarm_off::HANDLER) == 0);  // one-shot: disarmed after firing
    CHECK(log_is({"M1", "alarm", "M2"}));
}

void fn_sleeper_q1(PPCContext* c, Memory*) {
    LOG("S1");
    call(c, sched::os_sleep_thread, kQ1);
    LOG("S2");
    call(c, sched::os_wakeup_thread, kQ2);
    c->r[3] = 0;
}

void test_alarm_wakes_thread_which_wakes_main() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    uint32_t s = create(c, 1, fn_sleeper_q1, 0, 10);
    call(c, sched::os_resume_thread, s);  // higher priority: runs and sleeps
    uint64_t ticks = hw::TIMEBASE_FREQ_HZ / 1000;
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = (uint32_t)ticks, c->r[6] = reg_func(alarm_wake_q1);
    sched::os_set_alarm(c, &g_mem);
    call(c, sched::os_sleep_thread, kQ2);
    LOG("M1");
    CHECK(log_is({"S1", "alarm", "S2", "M1"}));
}

void test_alarm_cancel_and_periodic() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = hw::TIMEBASE_FREQ_HZ / 1000, c->r[6] = reg_func(alarm_count);
    sched::os_set_alarm(c, &g_mem);
    call(c, sched::os_cancel_alarm, kAlarm);
    sleep_ms(c, 5);
    CHECK(g_alarm_calls == 0);

    uint64_t now = sched::timebase_now(), period = hw::TIMEBASE_FREQ_HZ / 1000;  // every 1 ms
    c->r[3] = kAlarm, c->r[4] = (uint32_t)(now >> 32), c->r[5] = (uint32_t)now;
    c->r[6] = 0, c->r[7] = (uint32_t)period, c->r[8] = reg_func(alarm_count);
    sched::os_set_periodic_alarm(c, &g_mem);
    sleep_ms(c, 20);
    int calls = g_alarm_calls;
    CHECK(calls >= 5 && calls <= 21);
    call(c, sched::os_cancel_alarm, kAlarm);
    sleep_ms(c, 3);
    CHECK(g_alarm_calls == calls);
}

// An alarm is an interrupt: it is not delivered to a running thread that has disabled interrupts.
void test_alarm_waits_for_interrupts() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = 1, c->r[6] = reg_func(alarm_count);
    sched::os_set_alarm(c, &g_mem);
    CHECK(sched::interrupts_exchange(false) == true);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    call(c, sched::os_yield_thread);
    CHECK(g_alarm_calls == 0);
    CHECK(sched::interrupts_exchange(true) == false);
    call(c, sched::os_yield_thread);
    CHECK(g_alarm_calls == 1);
}

// ---- Misc ------------------------------------------------------------------------------------------------

// ---- Equal priorities are not time-sliced ------------------------------------------------------------------

void fn_log_e(PPCContext* c, Memory*) { LOG("E"); c->r[3] = 0; }

// The real OS has no time slicing: a ready thread of the same priority runs only once the running one blocks,
// however long the running one spins (and however many interrupts it takes meanwhile).
void test_no_time_slicing() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    uint32_t e = create(c, 1, fn_log_e, 0, 16);  // same priority as main
    call(c, sched::os_resume_thread, e);
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = hw::TIMEBASE_FREQ_HZ / 1000, c->r[6] = reg_func(alarm_count);
    sched::os_set_alarm(c, &g_mem);
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) sched::poll(c, &g_mem);
    CHECK(g_alarm_calls == 1);  // the interrupt was taken...
    CHECK(g_log.empty());       // ...and did not hand the baton to the peer
    sleep_ms(c, 1);
    CHECK(log_is({"E"}));
}

// ---- Alarms and sleepers that come due while a thread spins -------------------------------------------------

void fn_sleeper_h(PPCContext* c, Memory*) {
    LOG("H1");
    call(c, sched::os_sleep_thread, kQ1);
    LOG("H2");
    c->r[3] = 0;
}

// A thread that spins (the ADX wait loop) takes the alarm interrupt at its next poll point, and the
// higher-priority thread the handler wakes runs at once.
void test_alarm_interrupts_spinning_thread() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    uint32_t h = create(c, 1, fn_sleeper_h, 0, 5);
    call(c, sched::os_resume_thread, h);  // H1, then it sleeps
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = 2 * (hw::TIMEBASE_FREQ_HZ / 1000), c->r[6] = reg_func(alarm_wake_q1);
    sched::os_set_alarm(c, &g_mem);
    auto t0 = std::chrono::steady_clock::now();
    while (g_log.size() < 3 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) sched::poll(c, &g_mem);
    CHECK(log_is({"H1", "alarm", "H2"}));

    // Not with interrupts disabled.
    g_log.clear();
    uint32_t h2 = create(c, 2, fn_sleeper_h, 0, 5);
    call(c, sched::os_resume_thread, h2);
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = hw::TIMEBASE_FREQ_HZ / 1000, c->r[6] = reg_func(alarm_wake_q1);
    sched::os_set_alarm(c, &g_mem);
    sched::interrupts_exchange(false);
    t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) sched::poll(c, &g_mem);
    CHECK(log_is({"H1"}));
    sched::interrupts_exchange(true);
}

// The poll point that has no context of its own: Memory::translate takes the interrupts every 256K memory
// operations, so a loop that only reads memory is interrupted too.
void test_alarm_interrupts_memory_loop() {
    setup();
    g_alarm_calls = 0;
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_get_current_thread);  // the first call makes the main thread a guest thread
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = hw::TIMEBASE_FREQ_HZ / 1000, c->r[6] = reg_func(alarm_count);
    sched::os_set_alarm(c, &g_mem);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    uint32_t sink = 0;
    for (int i = 0; i < (1 << 20) && g_alarm_calls == 0; ++i) sink += g_mem.read32(kFlag);
    CHECK(g_alarm_calls == 1);
    CHECK(sink == 0);
    // With interrupts disabled the same loop is not interrupted.
    g_alarm_calls = 0;
    c->r[3] = kAlarm, c->r[4] = 0, c->r[5] = hw::TIMEBASE_FREQ_HZ / 1000, c->r[6] = reg_func(alarm_count);
    sched::os_set_alarm(c, &g_mem);
    sched::interrupts_exchange(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    for (int i = 0; i < (1 << 20); ++i) sink += g_mem.read32(kFlag);
    CHECK(g_alarm_calls == 0);
    sched::interrupts_exchange(true);
    call(c, sched::os_cancel_alarm, kAlarm);
}

// ---- VI retrace --------------------------------------------------------------------------------------------

constexpr uint32_t kViDispInt0 = vi::kRegBase + vi::kRegDispInt0;
int g_retraces = 0;
uint32_t g_retrace_arg = 0, g_retrace_status = 0;

// A stand-in for __VIRetraceHandler: reads DISP_INT0, clears its status bit, counts, wakes the threads on kQ1.
void retrace_handler(PPCContext* c, Memory*) {
    ++g_retraces;
    g_retrace_arg = c->r[3];
    uint32_t v = g_mem.read32(kViDispInt0);
    g_retrace_status = v;
    if (v & vi::kDispIntInt) g_mem.write32(kViDispInt0, v & ~vi::kDispIntInt);
    call(c, sched::os_wakeup_thread, kQ1);
}

// What the game's VIInit leaves behind: DISP_INT0 enabled, handler 24 registered, PI_VI unmasked.
void setup_retrace(bool unmask = true) {
    setup();
    g_retraces = 0;
    g_retrace_arg = g_retrace_status = 0;
    g_mem.write32(kViDispInt0, vi::kDispIntEnb);
    g_mem.write32(vi::kPiRegMask, unmask ? vi::kPiCauseVi : 0);
    g_interrupts.handlers[vi::kRetraceInterrupt] = reg_func(retrace_handler);
    sched::retrace_arm();
}

// VIWaitForRetrace: the thread sleeps on the retrace queue and the retrace handler wakes it, one field later.
void test_retrace_interrupt_wakes_waiter() {
    setup_retrace();
    PPCContext* c = &g_main_ctx;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) call(c, sched::os_sleep_thread, kQ1);  // every thread blocked: the idle wait takes it
    auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(g_retraces == 3);
    CHECK(g_retrace_arg == vi::kRetraceInterrupt);
    CHECK(g_retrace_status & vi::kDispIntInt);  // the handler saw a pending interrupt...
    CHECK(g_retrace_status & vi::kDispIntEnb);
    CHECK(!(g_mem.read32(kViDispInt0) & vi::kDispIntInt));  // ...and cleared it
    CHECK(!(g_mem.read32(vi::kPiRegCause) & vi::kPiCauseVi));
    CHECK(sched::retrace_count() == 3);
    // Three fields at 59.94 Hz: 50 ms, give or take the host's scheduling.
    CHECK(elapsed >= std::chrono::milliseconds(48));
    CHECK(elapsed < std::chrono::milliseconds(150));
    CHECK(sched::interrupts_enabled());  // the handler's masked state does not leak out
}

// No handler registered, or PI_VI masked: the retrace clock runs, nothing is delivered, and the status stays pending.
void test_retrace_masked_or_unregistered() {
    setup_retrace(false);
    PPCContext* c = &g_main_ctx;
    sleep_ms(c, 40);
    CHECK(g_retraces == 0);
    CHECK(sched::retrace_count() >= 2);
    CHECK(g_mem.read32(kViDispInt0) & vi::kDispIntInt);
    g_mem.write32(vi::kPiRegMask, vi::kPiCauseVi);  // __OSUnmaskInterrupts(PI_VI)
    sleep_ms(c, 20);
    CHECK(g_retraces >= 1);
    CHECK(!(g_mem.read32(kViDispInt0) & vi::kDispIntInt));

    setup();  // a display interrupt that is not enabled raises nothing
    g_interrupts.handlers[vi::kRetraceInterrupt] = reg_func(retrace_handler);
    g_retraces = 0;
    g_mem.write32(vi::kPiRegMask, vi::kPiCauseVi);
    sched::retrace_arm();
    sleep_ms(c, 40);
    CHECK(g_retraces == 0);
}

void fn_retrace_waiter(PPCContext* c, Memory*) {
    LOG("W1");
    call(c, sched::os_sleep_thread, kQ1);
    LOG("W2");
    c->r[3] = 0;
}

// A thread spinning in a loop that only reads memory takes the retrace interrupt, and the higher-priority thread
// blocked in VIWaitForRetrace (the handler wakes it) runs without the spinner reaching any OS call.
void test_retrace_interrupts_spinning_thread() {
    setup_retrace();
    PPCContext* c = &g_main_ctx;
    call(c, sched::os_get_current_thread);
    uint32_t w = create(c, 1, fn_retrace_waiter, 0, 5);
    call(c, sched::os_resume_thread, w);  // W1, then it sleeps
    auto t0 = std::chrono::steady_clock::now();
    uint32_t sink = 0;
    while (g_log.size() < 2 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2))
        for (int i = 0; i < 4096; ++i) sink += g_mem.read32(kFlag);
    CHECK(log_is({"W1", "W2"}));
    CHECK(g_retraces == 1);
    CHECK(sink == 0);

    // With interrupts disabled the retrace stays pending.
    g_log.clear();
    g_retraces = 0;
    uint32_t w2 = create(c, 2, fn_retrace_waiter, 0, 5);
    call(c, sched::os_resume_thread, w2);
    sched::interrupts_exchange(false);
    t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(40))
        for (int i = 0; i < 4096; ++i) sink += g_mem.read32(kFlag);
    CHECK(log_is({"W1"}));
    CHECK(g_retraces == 0);
    sched::interrupts_exchange(true);
    sleep_ms(c, 30);  // the next retrace wakes it
    CHECK(log_is({"W1", "W2"}));
}

// ---- The retrace clock and the display interrupt registers (no scheduler, no display) --------------------------

void test_retrace_timer_model() {
    using Clock = vi::RetraceTimer::Clock;
    const auto p = vi::kFieldPeriod;
    CHECK(std::chrono::duration<double>(p).count() > 1.0 / 59.95 && std::chrono::duration<double>(p).count() < 1.0 / 59.93);
    vi::RetraceTimer t;
    auto t0 = Clock::now();
    CHECK(!t.started() && !t.due(t0 + 10 * p) && !t.take(t0 + 10 * p));
    t.start(t0);
    t.start(t0 + 3 * p);  // idempotent
    CHECK(t.next() == t0 + p);
    CHECK(!t.take(t0 + p - std::chrono::nanoseconds(1)));
    CHECK(t.take(t0 + p));
    CHECK(t.next() == t0 + 2 * p && t.count() == 1);
    // A host that fell behind loses the retraces it missed: one is taken, the field restarts on the boundary.
    CHECK(t.take(t0 + 5 * p + p / 2));
    CHECK(!t.take(t0 + 5 * p + p / 2));
    CHECK(t.next() == t0 + 6 * p && t.count() == 2);
    // The beam sweeps the field: line 1 at the retrace, the last line just before the next one.
    auto b0 = t.beam(t0 + 5 * p);
    auto bm = t.beam(t0 + 5 * p + p / 2);
    auto be = t.beam(t0 + 6 * p - std::chrono::nanoseconds(1000));
    CHECK(b0.vcount == 1 && b0.hcount == 1);
    CHECK(bm.vcount >= vi::kLinesPerField / 2 - 1 && bm.vcount <= vi::kLinesPerField / 2 + 2);
    CHECK(be.vcount == vi::kLinesPerField);
    CHECK(be.hcount >= 1 && be.hcount <= vi::kHalfLinesPerLine);
    t.reset();
    CHECK(!t.started() && t.count() == 0);
}

void test_display_interrupt_registers() {
    uint8_t regs[0x100] = {};
    auto rd = [&](uint32_t off) {
        return (uint32_t(regs[off]) << 24) | (uint32_t(regs[off + 1]) << 16) | (uint32_t(regs[off + 2]) << 8) | regs[off + 3];
    };
    auto wr = [&](uint32_t off, uint32_t v) {
        regs[off] = uint8_t(v >> 24), regs[off + 1] = uint8_t(v >> 16), regs[off + 2] = uint8_t(v >> 8), regs[off + 3] = uint8_t(v);
    };
    CHECK(!vi::raise_display_interrupts(regs));  // nothing enabled
    CHECK(rd(vi::kRegDispInt0) == 0);
    wr(vi::kRegDispInt0, vi::kDispIntEnb | 0x00E0012C);  // enabled, with a position
    wr(vi::kRegDispInt0 + 4, 0x00F00000);                 // DI1 not enabled
    CHECK(!vi::display_interrupt_pending(regs));
    CHECK(vi::raise_display_interrupts(regs));
    CHECK(rd(vi::kRegDispInt0) == (vi::kDispIntInt | vi::kDispIntEnb | 0x00E0012C));  // position untouched
    CHECK(rd(vi::kRegDispInt0 + 4) == 0x00F00000);
    CHECK((regs[vi::kRegDispInt0] & 0x80) != 0);  // the SDK reads the high half-word: bit 15 is the status
    wr(vi::kRegDispInt0, rd(vi::kRegDispInt0) & ~vi::kDispIntInt);  // what the handler does
    CHECK(!vi::display_interrupt_pending(regs));
    // Registers 2 and 3 (light pen) are never raised by the retrace.
    wr(vi::kRegDispInt0, 0);
    wr(vi::kRegDispInt0 + 8, vi::kDispIntEnb);
    CHECK(!vi::raise_display_interrupts(regs));
    CHECK(rd(vi::kRegDispInt0 + 8) == vi::kDispIntEnb);
    vi::write_beam(regs, {0x0123, 0x0345});
    CHECK(rd(vi::kRegVertCount) == 0x01230345);
}

void fn_int_state(PPCContext* c, Memory*) {
    LOG(sched::interrupts_exchange(false) ? "T-enabled" : "T-disabled");  // a new thread starts enabled
    c->r[3] = 0;
}

void test_interrupt_state_is_per_thread() {
    setup();
    PPCContext* c = &g_main_ctx;
    CHECK(sched::interrupts_exchange(false) == true);
    uint32_t t = create(c, 1, fn_int_state, 0, 10);
    call(c, sched::os_resume_thread, t);  // runs to its end
    CHECK(sched::interrupts_exchange(true) == false);  // ours is still disabled
    CHECK(log_is({"T-enabled"}));
}

void fn_detached(PPCContext* c, Memory*) { LOG("D"); call(c, sched::os_exit_thread, 5); LOG("never"); }

void test_detached_exit() {
    setup();
    PPCContext* c = &g_main_ctx;
    uint32_t t = create(c, 1, fn_detached, 0, 10, 1);
    call(c, sched::os_resume_thread, t);
    LOG("M");
    CHECK(state_of(t) == 0);
    CHECK(call(c, sched::os_join_thread, t, 0) == 0);  // detached: cannot be joined
    CHECK(sched::thread_count() == 1);
    CHECK(log_is({"D", "M"}));
}

void fn_suspend_target(PPCContext* c, Memory*) {
    LOG("T1");
    call(c, sched::os_sleep_thread, kQ1);
    LOG("T2");
    c->r[3] = 0;
}

void test_suspend_keeps_woken_thread_off() {
    setup();
    PPCContext* c = &g_main_ctx;
    uint32_t t = create(c, 1, fn_suspend_target, 0, 16);
    call(c, sched::os_resume_thread, t);
    call(c, sched::os_yield_thread);  // T1, then T sleeps
    CHECK(state_of(t) == OSThread::STATE_WAITING);
    CHECK(call(c, sched::os_suspend_thread, t) == 0);
    call(c, sched::os_wakeup_thread, kQ1);
    call(c, sched::os_yield_thread);
    LOG("M1");  // T is awake but suspended: it must not have run
    CHECK(state_of(t) == OSThread::STATE_READY);
    CHECK(call(c, sched::os_is_thread_suspended, t) == 1);
    CHECK(call(c, sched::os_resume_thread, t) == 1);
    CHECK(call(c, sched::os_join_thread, t, 0) == 1);
    CHECK(log_is({"T1", "M1", "T2"}));
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    } tests[] = {
        {"sleep_wakeup_handoff", test_sleep_wakeup_handoff},
        {"priority_preemption_on_resume", test_priority_preemption_on_resume},
        {"set_priority_and_scheduler_disable", test_set_priority_and_scheduler_disable},
        {"mutex_contention", test_mutex_contention},
        {"cancel_releases_mutex_and_join", test_cancel_releases_mutex_and_join},
        {"condition_variable", test_condition_variable},
        {"message_queue", test_message_queue},
        {"alarm_wakes_sleeper", test_alarm_wakes_sleeper},
        {"alarm_wakes_thread_which_wakes_main", test_alarm_wakes_thread_which_wakes_main},
        {"alarm_cancel_and_periodic", test_alarm_cancel_and_periodic},
        {"alarm_waits_for_interrupts", test_alarm_waits_for_interrupts},
        {"no_time_slicing", test_no_time_slicing},
        {"alarm_interrupts_spinning_thread", test_alarm_interrupts_spinning_thread},
        {"alarm_interrupts_memory_loop", test_alarm_interrupts_memory_loop},
        {"retrace_timer_model", test_retrace_timer_model},
        {"display_interrupt_registers", test_display_interrupt_registers},
        {"retrace_interrupt_wakes_waiter", test_retrace_interrupt_wakes_waiter},
        {"retrace_masked_or_unregistered", test_retrace_masked_or_unregistered},
        {"retrace_interrupts_spinning_thread", test_retrace_interrupts_spinning_thread},
        {"interrupt_state_is_per_thread", test_interrupt_state_is_per_thread},
        {"detached_exit", test_detached_exit},
        {"suspend_keeps_woken_thread_off", test_suspend_keeps_woken_thread_off},
    };
    for (auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::printf("%-40s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    sched::reset();
    return g_failures ? 1 : 0;
}
