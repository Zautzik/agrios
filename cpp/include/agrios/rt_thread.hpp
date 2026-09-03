#pragma once

// Utilities for turning a pthread into a PREEMPT_RT / ROS 2 real-time-
// guideline-compliant thread: memory locking, stack pre-faulting, SCHED_FIFO
// scheduling, and CPU affinity. See motor_control_rt_loop.cpp for the actual
// control loop this exists for, and cpp/README.md for the full writeup of
// what each step buys you and what it doesn't.

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>

namespace agrios::rt {

struct RtThreadConfig {
    int priority = 99;       // SCHED_FIFO priority, 1-99. See the warning on
                              // 99 specifically in cpp/README.md before using
                              // it on a real box.
    int cpu_core = -1;       // -1 = don't set affinity. Pinning alone does not
                              // isolate a core -- see cpp/README.md.
    std::size_t prefault_stack_bytes = 8 * 1024 * 1024;
};

// Touches every page of a large stack buffer once, forcing the kernel to back
// it with real physical memory immediately rather than on first write deeper
// inside the RT loop (a signal handler, an unusually deep call chain).
// mlockall(MCL_FUTURE) keeps future mappings from being swapped back out, but
// does not by itself guarantee a page is populated before it's first
// touched -- that first touch is still a (minor) page fault. This function is
// the standard belt-and-suspenders fix for that gap (this exact pattern is
// the one in the PREEMPT_RT community's own reference real-time-programming
// HOWTO). It is only safe to call from a thread whose stack was explicitly
// sized to be larger than prefault_stack_bytes -- see
// make_realtime_thread_attr() below, which exists specifically to guarantee
// that before this is ever called.
inline void prefault_stack(std::size_t bytes) {
    volatile unsigned char dummy[65536];  // one page-sized-plus chunk per pass
    std::size_t touched = 0;
    while (touched < bytes) {
        std::memset(const_cast<unsigned char*>(dummy), 0, sizeof(dummy));
        touched += sizeof(dummy);
    }
    // Silence "value never read" -- the memset side effect is the point.
    (void)dummy[0];
}

// pthread_create() with a plain default attr gives you whatever the platform
// default stack size is (commonly 8 MiB, but it's not guaranteed and isn't
// something this code should assume) -- and prefault_stack() above needs
// prefault_stack_bytes of *actual* stack headroom to touch, on top of
// whatever the loop itself uses. Build the thread with an explicit,
// sufficiently large stack via this attr rather than relying on a platform
// default lining up with what prefault_stack() is about to do.
inline pthread_attr_t make_realtime_thread_attr(const RtThreadConfig& cfg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, cfg.prefault_stack_bytes + (2 * 1024 * 1024));
    return attr;
}

struct RtSetupResult {
    bool ok = false;
    std::string error;  // populated iff !ok
};

// Call once, at the very start of the real-time thread's body (before it
// touches the hot loop) -- never from any other thread on this thread's
// behalf; several of these calls (SCHED_FIFO, mlockall) are explicitly
// documented as acting on "the calling thread"/"the calling process".
//
// Every step here can legitimately fail on an unprivileged process:
// SCHED_FIFO and mlockall both require CAP_SYS_NICE/CAP_IPC_LOCK (or root),
// which most containers -- including the one this was developed and tested
// in -- do not grant by default. Silently continuing at normal scheduling
// and unlocked memory after asking for real-time guarantees and not getting
// them is worse than refusing to run: a control loop that *thinks* it's
// real-time-safe and isn't will fail exactly when it's under the load it was
// built to survive. That's why this returns a hard failure with a specific
// reason instead of a bool best-effort flag, and motor_control_rt_loop.cpp
// exits rather than proceeding when this fails.
inline RtSetupResult configure_current_thread_realtime(const RtThreadConfig& cfg) {
    // 1. Lock all current and future memory mappings of this process into
    // RAM -- no page of this process's address space can be swapped out from
    // under the RT thread once this succeeds.
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return {false, std::string("mlockall failed: ") + std::strerror(errno) +
                            " (needs CAP_IPC_LOCK or root)"};
    }

    // 2. Pre-fault this thread's stack so the pages prefault_stack() touches
    // are already resident -- see prefault_stack()'s comment for why
    // mlockall alone doesn't cover this.
    prefault_stack(cfg.prefault_stack_bytes);

    // 3. SCHED_FIFO: fixed-priority, no time-slicing -- this thread runs
    // until it blocks or a higher/equal-priority SCHED_FIFO/SCHED_RR thread
    // preempts it, unlike SCHED_OTHER's fair time-sliced scheduling. This is
    // what actually makes "real-time" mean something here: without it, the
    // kernel's completely fair scheduler is free to delay this thread behind
    // other CPU-bound work for an unbounded amount of time.
    sched_param sch{};
    sch.sched_priority = cfg.priority;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sch) != 0) {
        return {false, std::string("pthread_setschedparam(SCHED_FIFO, ") +
                            std::to_string(cfg.priority) +
                            ") failed: " + std::strerror(errno) + " (needs CAP_SYS_NICE or root)"};
    }

    // 4. Pin to a specific core, if requested. This alone does not isolate
    // the core -- see cpp/README.md for the isolcpus/nohz_full/rcu_nocbs
    // kernel-boot-parameter prerequisite this code cannot set up or verify
    // from inside a single process.
    if (cfg.cpu_core >= 0) {
        cpu_set_t cpu_set;
        CPU_ZERO(&cpu_set);
        CPU_SET(cfg.cpu_core, &cpu_set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set) != 0) {
            return {false, std::string("pthread_setaffinity_np(core ") +
                                std::to_string(cfg.cpu_core) + ") failed: " + std::strerror(errno)};
        }
    }

    return {true, {}};
}

}  // namespace agrios::rt
