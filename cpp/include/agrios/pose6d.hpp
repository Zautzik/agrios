#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace agrios {

// A 6-DoF pose plus a monotonic capture timestamp -- the payload passed from
// perception to motor control across the shared-memory bridge.
//
// Field order is deliberate: six doubles (each naturally 8-byte-aligned)
// followed by one int64_t, so the struct needs no compiler-inserted padding
// and has the same layout under any compiler that follows the Itanium C++
// ABI (GCC/Clang on Linux, the targets this is meant to run on). Getting
// this right matters here specifically because Pose6D crosses a language
// boundary: shared_ring_buffer.py parses these same 56 bytes with a fixed
// struct.Struct format string, not by re-deriving the layout from anywhere.
struct Pose6D {
    double x;
    double y;
    double z;
    double pitch;
    double yaw;
    double roll;
    std::int64_t timestamp_ns;  // steady_clock::now().time_since_epoch(), nanoseconds
};

static_assert(std::is_trivially_copyable_v<Pose6D>,
              "Pose6D crosses a process boundary through shared memory -- it "
              "must be safe to copy as raw bytes");
static_assert(sizeof(Pose6D) == 56,
              "Pose6D's layout changed -- update the struct.Struct format "
              "string in cpp/python/shared_ring_buffer.py to match, or this "
              "silently desyncs the two sides of the bridge");

}  // namespace agrios
