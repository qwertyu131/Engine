#pragma once
// What a device really supports, queried from the backend (spec §5.3, §6.1). Plan F2 Task 2 fills the
// identity fields; Task 7 adds the full capability table (limits, formats, features) and --dump-caps.
#include "Types.h"

#include <cstdint>
#include <string>

namespace eng::rhi {

struct DeviceCaps {
    Backend backend = Backend::Vulkan;
    int tier = 0;            // spec §6.1: 0 = T0, 1 = T1, 2 = T2
    std::string apiVersion;  // "1.4.329" (Vulkan instance/device API) / "11_1" (D3D11 feature level)
    std::string adapterName; // UTF-8
    uint32_t vendorId = 0;
    bool software = false;   // WARP / CPU device
};

} // namespace eng::rhi
