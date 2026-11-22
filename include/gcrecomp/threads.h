#pragma once
// SPDX-License-Identifier: MIT
// =============================================================================
// Guest threads: a cooperative scheduler with one host thread per guest thread.
//
// Every guest thread (OSCreateThread) owns a host thread and its own PPCContext, but only one of them
// runs at any time: the one that holds the baton (`current`). The baton moves only at the switch points
// below, so guest memory needs no locking. Priority scheduling follows the Dolphin SDK: lower number is
// higher priority, a thread that becomes ready preempts a lower-priority running thread at once, equal
// priorities run in FIFO order, and a preempted thread keeps its place ahead of its peers.
//
// Switch points: the blocking calls (OSSleepThread, OSSuspendThread, OSJoinThread, OSLockMutex,
// OSReceiveMessage, ...), the calls that make another thread ready (OSResumeThread, OSWakeupThread,
// OSSetThreadPriority, OSUnlockMutex, OSSendMessage, ...), OSYieldThread and OSExitThread. There is no time
// slicing (the real OS has none either): a running thread keeps the baton until it reaches one of them.
//
// Interrupts: two timer sources interrupt the running thread when they come due, provided it has interrupts
// enabled: the alarms (OSSetAlarm, ..., the decrementer) and the VI retrace, a 59.94 Hz wall-clock source that
// raises the enabled display interrupts and runs the handler the game registered for OS interrupt 24 (see vi.h).
// They are taken at poll() points (the calls a spinning loop makes, and every 256K memory operations), at the
// scheduler's switch points, and while every thread is blocked (the idle thread of the real OS). A handler runs
// without thread switches; the threads it makes ready run when the interrupted thread reschedules.
//
// The thread, mutex, condition and message-queue structures stay in guest memory in the SDK's layout
// (see hw/os_defs.h); the recompiled SDK code that is not replaced can read and link them.
// =============================================================================

#include <cstdint>

#include "gcrecomp/runtime.h"

namespace gcrecomp::sched {

// Timebase ticks as OSGetTime reports them (40.5 MHz since the runtime started).
uint64_t timebase_now();

// OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts. The interrupt state belongs to the
// guest thread. Sets the state and returns the previous one.
bool interrupts_exchange(bool enabled);

// The current guest thread's interrupt state, without changing it.
bool interrupts_enabled();

// Starts delivering the VI retrace interrupt: called when the guest registers its handler for OS interrupt 24
// (__OSSetInterruptHandler). The field clock starts here, unless a read of the beam position started it.
void retrace_arm();

// Retraces taken so far (the clock ticks; whether a handler ran depends on the guest's interrupt masks).
uint64_t retrace_count();

// A safe point: takes the interrupts that have come due (runs the alarm handlers and the retrace handler, then
// lets a higher-priority thread they made ready run). Cheap when nothing is due: one clock read. Does nothing
// while the calling thread has interrupts disabled, or inside a scheduler call or interrupt handler.
// `ctx` is the caller's context; null uses the running guest thread's own (for Memory::translate, which has none).
void poll(PPCContext* ctx, Memory* mem);

// Forget every guest thread, mutex waiter and alarm (cancelling the host threads). The calling host
// thread becomes the main guest thread again, found through OS_CURRENT_THREAD at the next call. For
// tests; not meant to be called while guest threads are running.
void reset();

// Number of guest threads currently known to the scheduler (the main thread included).
size_t thread_count();

// ---- Replacements for the SDK functions (arguments in registers, as for any RecompiledFunc) ----------
// Threads
void os_create_thread(PPCContext*, Memory*);
void os_exit_thread(PPCContext*, Memory*);
void os_cancel_thread(PPCContext*, Memory*);
void os_detach_thread(PPCContext*, Memory*);
void os_join_thread(PPCContext*, Memory*);
void os_resume_thread(PPCContext*, Memory*);
void os_suspend_thread(PPCContext*, Memory*);
void os_sleep_thread(PPCContext*, Memory*);
void os_wakeup_thread(PPCContext*, Memory*);
void os_yield_thread(PPCContext*, Memory*);
void os_set_thread_priority(PPCContext*, Memory*);
void os_get_thread_priority(PPCContext*, Memory*);
void os_get_current_thread(PPCContext*, Memory*);
void os_is_thread_suspended(PPCContext*, Memory*);
void os_is_thread_terminated(PPCContext*, Memory*);
void os_disable_scheduler(PPCContext*, Memory*);
void os_enable_scheduler(PPCContext*, Memory*);
void os_init_thread_queue(PPCContext*, Memory*);
// Mutexes and condition variables
void os_init_mutex(PPCContext*, Memory*);
void os_lock_mutex(PPCContext*, Memory*);
void os_try_lock_mutex(PPCContext*, Memory*);
void os_unlock_mutex(PPCContext*, Memory*);
void os_init_cond(PPCContext*, Memory*);
void os_wait_cond(PPCContext*, Memory*);
void os_signal_cond(PPCContext*, Memory*);
// Message queues
void os_init_message_queue(PPCContext*, Memory*);
void os_send_message(PPCContext*, Memory*);
void os_jam_message(PPCContext*, Memory*);
void os_receive_message(PPCContext*, Memory*);
// Alarms
void os_create_alarm(PPCContext*, Memory*);
void os_set_alarm(PPCContext*, Memory*);
void os_set_periodic_alarm(PPCContext*, Memory*);
void os_cancel_alarm(PPCContext*, Memory*);

}  // namespace gcrecomp::sched
