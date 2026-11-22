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
// OSSetThreadPriority, OSUnlockMutex, OSSendMessage, ...), OSYieldThread, OSExitThread, and
// VIWaitForRetrace. Nothing preempts a thread that never reaches one of them (there is no decrementer
// interrupt): guest code that busy-waits on a flag set by another thread needs a yield hook.
//
// Alarms (OSSetAlarm, ...) are driven by the timebase and their handlers run at a switch point, or while
// every thread is blocked. A running thread with interrupts disabled does not get them delivered.
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

// VIWaitForRetrace's wait: the calling guest thread sleeps `microseconds` while the others run.
void retrace_wait(PPCContext* ctx, Memory* mem, uint32_t microseconds);

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
