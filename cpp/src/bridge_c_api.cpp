#include "agrios/bridge_c_api.h"

#include <cstring>
#include <exception>
#include <iostream>

#include "agrios/bridge_config.hpp"
#include "agrios/pose6d.hpp"
#include "agrios/shared_ring_buffer.hpp"

namespace {

using Bridge = agrios::SharedRingBuffer<agrios::Pose6D, agrios::kBridgeCapacity>;

static_assert(sizeof(AgriosPose6D) == sizeof(agrios::Pose6D),
              "AgriosPose6D (C ABI) and agrios::Pose6D (C++ implementation) "
              "have drifted apart -- keep their fields identical in order "
              "and type");

}  // namespace

struct AgriosBridgeHandle {
    Bridge bridge;
};

extern "C" {

AgriosBridgeHandle* agrios_bridge_open(const char* shm_name, int is_owner) {
    if (shm_name == nullptr) {
        return nullptr;
    }
    try {
        return new AgriosBridgeHandle{Bridge(shm_name, is_owner != 0)};
    } catch (const std::exception& e) {
        std::cerr << "agrios_bridge_open failed: " << e.what() << '\n';
        return nullptr;
    }
}

void agrios_bridge_close(AgriosBridgeHandle* handle) { delete handle; }

int agrios_bridge_push(AgriosBridgeHandle* handle, const AgriosPose6D* pose) {
    if (handle == nullptr || pose == nullptr) {
        return 0;
    }
    // AgriosPose6D and agrios::Pose6D are field-for-field identical (checked
    // above); this is a same-layout reinterpretation, not a type pun across
    // unrelated types.
    const auto& typed_pose = *reinterpret_cast<const agrios::Pose6D*>(pose);
    return handle->bridge.get().push(typed_pose) ? 1 : 0;
}

int agrios_bridge_pop(AgriosBridgeHandle* handle, AgriosPose6D* out) {
    if (handle == nullptr || out == nullptr) {
        return 0;
    }
    auto& typed_out = *reinterpret_cast<agrios::Pose6D*>(out);
    return handle->bridge.get().pop(typed_out) ? 1 : 0;
}

}  // extern "C"
