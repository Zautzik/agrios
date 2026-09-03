#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace agrios {

// Current physical joint angles plus a monotonic sample timestamp -- the
// telemetry payload flowing motor control -> monitoring/perception, the
// opposite direction from Pose6D. A separate channel (separate shared-memory
// segment, separate SPSCRingBuffer instance) rather than reusing the pose
// bridge: these are two independent producer/consumer pairs with different
// producers and different consumers, and SPSCRingBuffer is single-producer/
// single-consumer by design -- multiplexing two unrelated data flows onto
// one ring buffer would violate that on the very first mixed read.
//
// Six joints, matching this project's 6-DoF convention elsewhere (Pose6D,
// the RT loop). A different arm would need a different N; this isn't
// generalized to a runtime-sized joint count because the whole point of this
// bridge is a fixed-layout, allocation-free struct that can live in shared
// memory -- a variable-length payload would need dynamic sizing on both
// sides of a language boundary, which is exactly what this design avoids
// everywhere else.
struct JointState6 {
    double joint_angles_rad[6];
    std::int64_t timestamp_ns;
};

static_assert(std::is_trivially_copyable_v<JointState6>,
              "JointState6 crosses a process boundary through shared memory "
              "-- it must be safe to copy as raw bytes");
static_assert(sizeof(JointState6) == 56,
              "JointState6's layout changed -- update the ctypes.Structure "
              "in cpp/python/shared_ring_buffer.py to match, or this "
              "silently desyncs the two sides of the bridge");

}  // namespace agrios
