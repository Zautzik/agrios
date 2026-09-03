// Demo consumer: the "C++ motor control" side of the bridge. Creates (owns)
// the shared-memory segment, then polls it for poses written by a producer
// in another process -- normally cpp/python/perception_producer_demo.py.
//
// Usage: motor_control_consumer [count]
//   count   stop after consuming this many poses (default: run until a
//           sentinel pose with timestamp_ns < 0 arrives, or Ctrl+C)

#include <chrono>
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

}  // namespace

int main(int argc, char** argv) {
    std::optional<long> count;
    if (argc > 1) {
        count = std::strtol(argv[1], nullptr, 10);
    }

    AgriosBridgeHandle* handle = agrios_bridge_open(agrios::kDefaultShmName, /*is_owner=*/1);
    if (handle == nullptr) {
        std::cerr << "failed to create shared-memory segment " << agrios::kDefaultShmName << '\n';
        return 1;
    }

    std::cout << "motor_control_consumer: created " << agrios::kDefaultShmName
              << " (capacity " << agrios::kBridgeCapacity << "), waiting for poses...\n";

    long consumed = 0;
    AgriosPose6D pose{};
    while (!count.has_value() || consumed < *count) {
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
                  << "x=" << pose.x << " y=" << pose.y << " z=" << pose.z
                  << " pitch=" << pose.pitch << " yaw=" << pose.yaw << " roll=" << pose.roll
                  << " t=" << pose.timestamp_ns << "ns\n";
    }

    std::cout << "motor_control_consumer: consumed " << consumed << " poses, shutting down\n";
    agrios_bridge_close(handle);
    return 0;
}
