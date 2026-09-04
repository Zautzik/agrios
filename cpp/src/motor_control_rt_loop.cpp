// A PREEMPT_RT / ROS 2 real-time-guideline-compliant 1 kHz motor-control
// loop. Reads target poses from the same shared-memory bridge as
// motor_control_consumer.cpp (see that file for the simple, non-real-time
// version used to verify the bridge itself); this file is about the loop's
// *timing and scheduling* discipline, not new bridge functionality.
//
// Real-time requirements this satisfies, and how:
//   - mlockall(MCL_CURRENT | MCL_FUTURE) + stack pre-fault, first thing in
//     the RT thread's body -- see agrios::rt::configure_current_thread_realtime.
//   - SCHED_FIFO, priority 99 -- see the same function. (See cpp/README.md
//     for a real warning about actually using priority 99.)
//   - pthread_setaffinity_np() pinning to an isolated core -- same function;
//     see cpp/README.md for the isolcpus/nohz_full/rcu_nocbs prerequisite
//     this code cannot itself set up or verify.
//   - Zero dynamic allocation, and no std::cout, inside while(true): every
//     value the hot loop touches is a stack value or a pre-allocated buffer
//     (the shared-memory ring buffer, the telemetry ring buffer below); the
//     hot loop's only "output" is a non-blocking push() into an in-process
//     lock-free ring buffer, drained by a separate, ordinary-priority
//     monitor thread that does the actual printing. This is the same
//     pattern ros2_control's RealtimeBuffer/RealtimePublisher classes use to
//     get state out of an RT update() loop without letting a logger's I/O
//     jitter leak into the control loop's timing.
//
// Also publishes synthetic joint-angle telemetry each iteration over the
// separate joint-telemetry shared-memory channel (see joint_state.hpp),
// consumed by e.g. a ReadJointStates tool on the perception/monitoring side.
// There's no real hardware behind this yet, so the "joint angles" are a
// fixed, clearly-synthetic waveform -- a stand-in for real encoder feedback,
// same honesty as the control_output stand-in below. What's real is the
// non-blocking push() pattern it's exercising, not the numbers.
//
// Usage: motor_control_rt_loop [--core N] [--priority P] [--iterations N]
// Requires CAP_SYS_NICE and CAP_IPC_LOCK (or root) -- e.g. under Docker:
//   docker run --cap-add=SYS_NICE --cap-add=IPC_LOCK ...

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

#include "agrios/bridge_c_api.h"
#include "agrios/bridge_config.hpp"
#include "agrios/rt_thread.hpp"
#include "agrios/spsc_ring_buffer.hpp"

namespace {

constexpr std::int64_t kPeriodNs = 1'000'000;         // 1 kHz
constexpr std::int64_t kOverrunThresholdNs = 200'000;  // 200 us of lateness counts as a miss

struct TelemetrySample {
    std::uint64_t iteration;
    std::int64_t jitter_ns;  // actual wake time minus the requested deadline
    bool overrun;
    bool pose_received;
    AgriosPose6D pose;  // meaningful only if pose_received
};
static_assert(std::is_trivially_copyable_v<TelemetrySample>);

// In-process only (not shared memory) -- one RT producer, one ordinary-
// priority consumer (the monitor thread below). Reusing SPSCRingBuffer here
// is exactly the point: it's already proven correct (see
// tests/spsc_ring_buffer_test.cpp) for the shared-memory pose bridge, and
// the algorithm doesn't care whether the two sides are two processes or two
// threads in one process.
using TelemetryRing = agrios::SPSCRingBuffer<TelemetrySample, 256>;

std::atomic<bool> g_running{true};
std::atomic<bool> g_rt_thread_failed{false};

// SIGINT and SIGTERM both need to route through here, not just SIGINT --
// SIGTERM is what `kill` sends by default, and what every real process
// supervisor (systemd, Docker, k8s) sends on shutdown before escalating to
// SIGKILL. Handling only SIGINT means every non-interactive shutdown path
// bypasses cleanup and orphans this process's two owned shared-memory
// segments. std::atomic<bool>::store with a lock-free atomic is one of the
// few operations the C++ standard actually permits inside a signal handler
// ([support.signal]) -- nothing else happens here on purpose.
static_assert(std::atomic<bool>::is_always_lock_free,
              "g_running must be lock-free to be touched from a signal handler");
void handle_shutdown_signal(int) { g_running.store(false, std::memory_order_relaxed); }

void add_ns(timespec& t, std::int64_t ns) {
    constexpr std::int64_t kNsPerSec = 1'000'000'000;
    t.tv_nsec += ns;
    while (t.tv_nsec >= kNsPerSec) {
        t.tv_nsec -= kNsPerSec;
        t.tv_sec += 1;
    }
}

std::int64_t diff_ns(const timespec& a, const timespec& b) {
    return (static_cast<std::int64_t>(a.tv_sec) - b.tv_sec) * 1'000'000'000 +
           (static_cast<std::int64_t>(a.tv_nsec) - b.tv_nsec);
}

struct RtLoopArgs {
    agrios::rt::RtThreadConfig rt_config;
    AgriosBridgeHandle* bridge = nullptr;
    AgriosJointBridgeHandle* joint_bridge = nullptr;
    TelemetryRing* telemetry = nullptr;
    std::optional<std::uint64_t> max_iterations;
};

// The RT thread's entry point. Everything from configure_current_thread_
// realtime() onward down to the loop is the real-time-critical section;
// nothing past this function's setup prologue allocates, locks, does I/O,
// or can throw.
void* rt_thread_main(void* arg_ptr) {
    auto& args = *static_cast<RtLoopArgs*>(arg_ptr);

    // Requirement 1-3: mlockall + stack pre-fault, SCHED_FIFO, CPU affinity.
    // "At the very beginning of the thread lifecycle" -- this is the first
    // thing this thread does, full stop.
    const agrios::rt::RtSetupResult setup = agrios::rt::configure_current_thread_realtime(args.rt_config);
    if (!setup.ok) {
        // Setup, not the hot loop -- std::cerr here is fine. Failing loudly
        // and refusing to run un-real-time is the correct behavior for code
        // that specifically exists to make real-time guarantees; see the
        // comment on configure_current_thread_realtime().
        std::cerr << "rt_thread_main: real-time setup failed: " << setup.error << '\n';
        g_rt_thread_failed.store(true, std::memory_order_relaxed);
        return nullptr;
    }

    timespec deadline{};
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    std::uint64_t iteration = 0;
    AgriosPose6D last_pose{};
    bool have_pose = false;

    // ---- Hot loop: no allocation, no std::cout, no locks, no exceptions. ----
    while (g_running.load(std::memory_order_relaxed)) {
        add_ns(deadline, kPeriodNs);

        // Absolute-deadline sleep, not a relative sleep_for(period): a
        // relative sleep re-measured from "now" each iteration accumulates
        // this iteration's own execution time as drift on every single pass
        // -- over a long-running loop that drift is unbounded. Sleeping to a
        // deadline that advances by a fixed period regardless of how long
        // the previous iteration took is what actually keeps this at 1 kHz
        // rather than "a bit under 1 kHz, worsening over time."
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const std::int64_t jitter_ns = diff_ns(now, deadline);

        // Non-blocking, allocation-free pose read -- see
        // tests/spsc_ring_buffer_test.cpp and NOTES.md for how this was
        // verified correct under real concurrent load.
        AgriosPose6D pose{};
        if (agrios_bridge_pop(args.bridge, &pose)) {
            last_pose = pose;
            have_pose = true;
        }

        // Stand-in for real control-law math (PID / trajectory tracking /
        // whatever this project's motor hardware eventually needs): a fixed,
        // bounded amount of work operating only on stack values, same as
        // the real thing would need to be to stay allocation-free here.
        volatile double control_output =
            have_pose ? (last_pose.tvec[0] + last_pose.tvec[1] + last_pose.tvec[2]) : 0.0;
        (void)control_output;

        // Telemetry out: push() is O(1), wait-free, and never allocates or
        // blocks -- if the monitor thread is behind, this sample is silently
        // dropped rather than the control loop waiting on it. Losing a
        // telemetry sample is fine; missing a control deadline to protect
        // one would not be.
        TelemetrySample sample{iteration, jitter_ns, jitter_ns > kOverrunThresholdNs, have_pose, last_pose};
        args.telemetry->push(sample);

        // Synthetic joint-angle telemetry, published to a separate reader
        // process over shared memory (see the file-level comment for why
        // these numbers aren't real). std::sin is bounded-time and
        // allocation-free -- fine to call here, unlike the things this loop
        // actually bans (malloc/new/push_back/std::cout).
        const double phase = static_cast<double>(iteration) * 0.01;
        AgriosJointState6 joint_state{};
        for (int j = 0; j < 6; ++j) {
            joint_state.joint_angles_rad[j] = 0.3 * std::sin(phase + j * 0.5);
        }
        joint_state.timestamp_ns = now.tv_sec * 1'000'000'000LL + now.tv_nsec;
        agrios_joint_bridge_push(args.joint_bridge, &joint_state);

        ++iteration;
        if (args.max_iterations.has_value() && iteration >= *args.max_iterations) {
            break;
        }
    }
    // ---- End hot loop. ----

    g_running.store(false, std::memory_order_relaxed);
    return nullptr;
}

// Ordinary SCHED_OTHER priority. Drains telemetry and does all the I/O the
// RT thread above is not allowed to do. Its own timing is not guaranteed and
// doesn't need to be -- if it falls behind, the ring buffer just fills and
// the RT thread's push()es (which never block) start being dropped.
void monitor_thread_main(TelemetryRing& telemetry) {
    std::uint64_t received = 0;
    std::uint64_t overruns = 0;
    std::int64_t max_jitter_ns = 0;
    std::int64_t sum_jitter_ns = 0;

    TelemetrySample sample{};
    while (g_running.load(std::memory_order_relaxed) || !telemetry.empty()) {
        if (!telemetry.pop(sample)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        ++received;
        sum_jitter_ns += sample.jitter_ns;
        if (sample.jitter_ns > max_jitter_ns) {
            max_jitter_ns = sample.jitter_ns;
        }
        if (sample.overrun) {
            ++overruns;
        }
        if (sample.iteration % 200 == 0) {
            std::cout << "[monitor] iter=" << sample.iteration << " jitter=" << sample.jitter_ns
                      << "ns pose=" << (sample.pose_received ? "yes" : "no") << '\n';
        }
    }

    std::cout << "[monitor] done: " << received << " samples, " << overruns << " overruns (>"
              << kOverrunThresholdNs << "ns late), max_jitter=" << max_jitter_ns << "ns, avg_jitter="
              << (received ? sum_jitter_ns / static_cast<std::int64_t>(received) : 0) << "ns\n";
}

}  // namespace

int main(int argc, char** argv) {
    agrios::rt::RtThreadConfig rt_config;
    std::optional<std::uint64_t> max_iterations;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--core" && i + 1 < argc) {
            rt_config.cpu_core = std::atoi(argv[++i]);
        } else if (arg == "--priority" && i + 1 < argc) {
            rt_config.priority = std::atoi(argv[++i]);
        } else if (arg == "--iterations" && i + 1 < argc) {
            max_iterations = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        }
    }

    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);

    AgriosBridgeHandle* bridge = agrios_bridge_open(agrios::kDefaultShmName, /*is_owner=*/1);
    if (bridge == nullptr) {
        std::cerr << "failed to open pose shared-memory bridge\n";
        return 1;
    }

    // This process is the owner of the joint-telemetry channel too: it's the
    // producer, and the one process guaranteed to be running whenever there's
    // telemetry to report. Readers (e.g. a ReadJointStates tool) attach as
    // non-owners.
    AgriosJointBridgeHandle* joint_bridge =
        agrios_joint_bridge_open(agrios::kDefaultJointTelemetryShmName, /*is_owner=*/1);
    if (joint_bridge == nullptr) {
        std::cerr << "failed to open joint-telemetry shared-memory bridge\n";
        agrios_bridge_close(bridge);
        return 1;
    }

    auto telemetry = std::make_unique<TelemetryRing>();  // allocated once, before the RT thread starts

    RtLoopArgs args;
    args.rt_config = rt_config;
    args.bridge = bridge;
    args.joint_bridge = joint_bridge;
    args.telemetry = telemetry.get();
    args.max_iterations = max_iterations;

    std::cout << "motor_control_rt_loop: starting monitor thread\n";
    std::thread monitor(monitor_thread_main, std::ref(*telemetry));

    std::cout << "motor_control_rt_loop: starting RT thread (priority=" << rt_config.priority
              << ", core=" << rt_config.cpu_core << ")\n";
    pthread_attr_t attr = agrios::rt::make_realtime_thread_attr(rt_config);
    pthread_t rt_thread{};
    if (pthread_create(&rt_thread, &attr, rt_thread_main, &args) != 0) {
        std::cerr << "pthread_create failed: " << std::strerror(errno) << '\n';
        g_running.store(false, std::memory_order_relaxed);
        monitor.join();
        agrios_joint_bridge_close(joint_bridge);
        agrios_bridge_close(bridge);
        return 1;
    }
    pthread_attr_destroy(&attr);

    pthread_join(rt_thread, nullptr);
    g_running.store(false, std::memory_order_relaxed);
    monitor.join();

    agrios_joint_bridge_close(joint_bridge);
    agrios_bridge_close(bridge);
    return g_rt_thread_failed.load(std::memory_order_relaxed) ? 1 : 0;
}
