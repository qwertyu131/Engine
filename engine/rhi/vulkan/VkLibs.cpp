// Single translation unit for the volk and VMA implementations (third-party, warnings off).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable : 4100 4127 4189 4324 4505)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#define VOLK_IMPLEMENTATION
#include <volk.h>

// VMA fetches every Vulkan function through vkGetInstanceProcAddr / vkGetDeviceProcAddr (volk).
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
