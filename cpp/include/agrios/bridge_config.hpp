#pragma once

#include <cstddef>

namespace agrios {

// Shared between bridge_c_api.cpp and any C++ code (e.g. motor_control_demo)
// that attaches to the same segment directly through SharedRingBuffer rather
// than through the C API. Every attaching process -- C++ or Python, via
// bridge_c_api -- must agree on this value; it's baked into the shared
// library at build time, so changing it means rebuilding everything that
// links against it.
inline constexpr std::size_t kBridgeCapacity = 1024;

// perception -> motor control: target poses (Pose6D).
inline constexpr const char* kDefaultShmName = "/agrios_pose_bridge";

// motor control -> monitoring/perception: joint-state telemetry (JointState6).
// A separate segment, not a second producer on the pose segment -- see
// joint_state.hpp for why this has to be its own channel.
inline constexpr std::size_t kJointTelemetryCapacity = 1024;
inline constexpr const char* kDefaultJointTelemetryShmName = "/agrios_joint_telemetry";

}  // namespace agrios
