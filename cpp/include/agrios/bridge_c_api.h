#ifndef AGRIOS_BRIDGE_C_API_H
#define AGRIOS_BRIDGE_C_API_H

/* Plain-C ABI over SharedRingBuffer<Pose6D, kBridgeCapacity>.
 *
 * Why this exists instead of letting Python poke the shared-memory bytes
 * directly: push()/pop()'s correctness depends on real acquire/release
 * memory fences, not just on reading/writing the right bytes at the right
 * offsets. Python's struct/ctypes modules have no memory-order-aware atomic
 * primitives, and a naive load/store would only happen to work by accident
 * on x86-64 (whose strong memory model papers over the missing fences) --
 * it would be a real, silent bug on a weakly-ordered target like the
 * ARM64 Jetson/Raspberry Pi boards this bridge is meant to eventually run
 * motor control on. Routing Python through this compiled shim means both
 * languages execute the exact same std::atomic operations, so there's one
 * implementation of the protocol, not two that have to be kept in sync by
 * hand.
 *
 * Capacity is fixed at compile time (kBridgeCapacity, in bridge_c_api.cpp)
 * because a C ABI can't carry a template parameter; every process using this
 * library links against the same build and therefore agrees on it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AgriosPose6D {
    double x;
    double y;
    double z;
    double pitch;
    double yaw;
    double roll;
    int64_t timestamp_ns; /* monotonic clock, nanoseconds */
} AgriosPose6D;

typedef struct AgriosBridgeHandle AgriosBridgeHandle; /* opaque */

/* Creates or attaches to the named POSIX shared-memory segment.
 * is_owner = 1: this process creates/resets the segment (call exactly once,
 *   from whichever process starts first -- it clears any stale segment from
 *   a crashed prior run and zero-initializes the ring buffer).
 * is_owner = 0: this process attaches to an already-created segment.
 * Returns NULL on failure. */
AgriosBridgeHandle* agrios_bridge_open(const char* shm_name, int is_owner);

/* Unmaps and closes the handle. If it was opened with is_owner = 1, also
 * unlinks the shared-memory segment. */
void agrios_bridge_close(AgriosBridgeHandle* handle);

/* Returns 1 on success, 0 if the ring buffer is full. */
int agrios_bridge_push(AgriosBridgeHandle* handle, const AgriosPose6D* pose);

/* Returns 1 on success (out is filled in), 0 if the ring buffer is empty. */
int agrios_bridge_pop(AgriosBridgeHandle* handle, AgriosPose6D* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AGRIOS_BRIDGE_C_API_H */
