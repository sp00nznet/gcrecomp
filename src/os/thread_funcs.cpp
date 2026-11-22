// SPDX-License-Identifier: MIT
//
// Binds the SDK's thread, mutex, condition, message-queue and alarm functions to the guest-thread
// scheduler in threads.cpp. The SDK's own scheduler (SelectThread and friends) waits for interrupts that
// do not exist here, so everything that reaches it is replaced.
#include <cstring>

#include "gcrecomp/runtime.h"
#include "gcrecomp/threads.h"

namespace gcrecomp {

namespace {

struct Entry {
    const char* name;
    RecompiledFunc func;
};

const Entry kTable[] = {
    {"OSCreateThread", sched::os_create_thread},
    {"OSExitThread", sched::os_exit_thread},
    {"OSCancelThread", sched::os_cancel_thread},
    {"OSDetachThread", sched::os_detach_thread},
    {"OSJoinThread", sched::os_join_thread},
    {"OSResumeThread", sched::os_resume_thread},
    {"OSSuspendThread", sched::os_suspend_thread},
    {"OSSleepThread", sched::os_sleep_thread},
    {"OSWakeupThread", sched::os_wakeup_thread},
    {"OSYieldThread", sched::os_yield_thread},
    {"OSSetThreadPriority", sched::os_set_thread_priority},
    {"OSGetThreadPriority", sched::os_get_thread_priority},
    {"OSGetCurrentThread", sched::os_get_current_thread},
    {"OSIsThreadSuspended", sched::os_is_thread_suspended},
    {"OSIsThreadTerminated", sched::os_is_thread_terminated},
    {"OSDisableScheduler", sched::os_disable_scheduler},
    {"OSEnableScheduler", sched::os_enable_scheduler},
    {"OSInitThreadQueue", sched::os_init_thread_queue},
    {"OSInitMutex", sched::os_init_mutex},
    {"OSLockMutex", sched::os_lock_mutex},
    {"OSTryLockMutex", sched::os_try_lock_mutex},
    {"OSUnlockMutex", sched::os_unlock_mutex},
    {"OSInitCond", sched::os_init_cond},
    {"OSWaitCond", sched::os_wait_cond},
    {"OSSignalCond", sched::os_signal_cond},
    {"OSInitMessageQueue", sched::os_init_message_queue},
    {"OSSendMessage", sched::os_send_message},
    {"OSJamMessage", sched::os_jam_message},
    {"OSReceiveMessage", sched::os_receive_message},
    {"OSCreateAlarm", sched::os_create_alarm},
    {"OSSetAlarm", sched::os_set_alarm},
    {"OSSetPeriodicAlarm", sched::os_set_periodic_alarm},
    {"OSCancelAlarm", sched::os_cancel_alarm},
    {nullptr, nullptr},
};

}  // namespace

RecompiledFunc lookup_thread_func(const char* name) {
    for (const Entry* e = kTable; e->name; ++e) {
        if (std::strcmp(e->name, name) == 0) return e->func;
    }
    return nullptr;
}

}  // namespace gcrecomp
