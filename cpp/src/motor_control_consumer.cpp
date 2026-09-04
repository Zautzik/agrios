// Demo consumer: the "C++ motor control" side of the bridge. Creates (owns)
// the shared-memory segment, then polls it for poses written by a producer
// in another process -- normally cpp/python/perception_producer_demo.py.
//
// Usage: motor_control_consumer [count]
//   count   stop after consuming this many poses (default: run until a
//           sentinel pose with timestamp_ns < 0 arrives, or Ctrl+C)

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

#include "agrios/bridge_c_api.h"
#include "agrios/bridge_config.hpp"

namespace {

// Busy-polls with a short sleep rather than a hard spin: this is a demo
// meant to run happily alongside everything else on a dev machine, not a
// latency-critical control loop. A real motor-control consumer on dedicated
// hardware would spin without sleeping, or use a futex/eventfd wakeup
// instead of polling at all.
constexpr auto kPollInterval = std::chrono::microseconds(500);

// This process is the OWNER of /agrios_pose_bridge -- it's the one that
// created the segment and is responsible for shm_unlink-ing it. Without a
// signal handler, SIGINT (Ctrl-C) or SIGTERM (`kill`, or what any real
// process supervisor sends on shutdown) hits the default disposition:
// immediate termination, no destructors, agrios_bridge_close() never runs,
// the segment is orphaned in /dev/shm. This flag exists so the main loop can
// exit on its own terms and reach that cleanup instead.
std::atomic<bool> g_running{true};
static_assert(std::atomic<bool>::is_always_lock_free,
              "g_running must be lock-free to be touched from a signal handler");
void handle_shutdown_signal(int) { g_running.store(false, std::memory_order_relaxed); }

}  // namespace

int main(int argc, char** argv) {
    std::optional<long> count;
    if (argc > 1) {
        count = std::strtol(argv[1], nullptr, 10);
    }

    std::signal(SIGINT, handle_shutdown_signal);
    std::signal(SIGTERM, handle_shutdown_signal);

    AgriosBridgeHandle* handle = agrios_bridge_open(agrios::kDefaultShmName, /*is_owner=*/1);
    if (handle == nullptr) {
        std::cerr << "failed to create shared-memory segment " << agrios::kDefaultShmName << '\n';
        return 1;
    }

    std::cout << "motor_control_consumer: created " << agrios::kDefaultShmName
              << " (capacity " << agrios::kBridgeCapacity << "), waiting for poses...\n";

    long consumed = 0;
    AgriosPose6D pose{};
    while (g_running.load(std::memory_order_relaxed) && (!count.has_value() || consumed < *count)) {
        if (!agrios_bridge_pop(handle, &pose)) {
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }

        if (pose.timestamp_ns < 0) {
            std::cout << "motor_control_consumer: received shutdown sentinel, exiting\n";
            break;
        }

        ++consumed;
        std::cout << "[motor_control] target pose #" << consumed << ": "
                  << "tvec=(" << pose.tvec[0] << ", " << pose.tvec[1] << ", " << pose.tvec[2] << ")"
                  << " rvec=(" << pose.rvec[0] << ", " << pose.rvec[1] << ", " << pose.rvec[2] << ")"
                  << " t=" << pose.timestamp_ns << "ns\n";
    }

    std::cout << "motor_control_consumer: consumed " << consumed << " poses, shutting down\n";
    agrios_bridge_close(handle);
    return 0;
}
