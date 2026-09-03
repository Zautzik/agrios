#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace agrios {

// A 6-DoF target pose plus a monotonic capture timestamp -- the payload
// passed from perception to motor control across the shared-memory bridge.
//
// Translation vector + Rodrigues rotation vector (tvec/rvec), not Euler
// angles: this is the representation cv2.solvePnP, ArUco pose estimation,
// and FoundationPose all emit natively, so a perception pipeline can push
// its output here with zero conversion. It's also lossless and free of
// gimbal lock, unlike Euler angles -- there is no "safe" fixed axis order to
// pick that avoids a degenerate orientation. rvec's direction is the
// rotation axis and its magnitude is the rotation angle in radians
// (Rodrigues' rotation formula); recovering a rotation matrix from it is
// cv2.Rodrigues(rvec) on the Python/OpenCV side.
//
// Field order is deliberate: six doubles (each naturally 8-byte-aligned)
// followed by one int64_t, so the struct needs no compiler-inserted padding
// and has the same layout under any compiler that follows the Itanium C++
// ABI (GCC/Clang on Linux, the targets this is meant to run on). Getting
// this right matters here specifically because Pose6D crosses a language
// boundary: shared_ring_buffer.py parses these same 56 bytes with a fixed
// ctypes.Structure layout, not by re-deriving it from anywhere.
struct Pose6D {
    double tvec[3];  // translation vector [tx, ty, tz], meters
    double rvec[3];  // Rodrigues rotation vector [rx, ry, rz], radians
    std::int64_t timestamp_ns;  // steady_clock::now().time_since_epoch(), nanoseconds
};

static_assert(std::is_trivially_copyable_v<Pose6D>,
              "Pose6D crosses a process boundary through shared memory -- it "
              "must be safe to copy as raw bytes");
static_assert(sizeof(Pose6D) == 56,
              "Pose6D's layout changed -- update the ctypes.Structure in "
              "cpp/python/shared_ring_buffer.py to match, or this silently "
              "desyncs the two sides of the bridge");

}  // namespace agrios
