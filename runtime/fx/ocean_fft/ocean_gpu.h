// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see LICENSE.
#pragma once
#include "ocean_types.h"
#include <cstddef>
#include <vulkan/vulkan.h>

namespace ocean_fft {
struct Spirv { const uint32_t* words = nullptr; size_t count = 0; };
struct CreateInfo {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    // The caller must have enabled this Vulkan device feature when creating device.
    bool storage_image_extended_formats_enabled = false;
    Params params;
    Spirv init, evolve, ifft, output;
};
struct Outputs {
    VkImageView displacement = VK_NULL_HANDLE; // RGBA32F array: horizontal X, height, horizontal Z, unused; feet
    VkImageView normal = VK_NULL_HANDLE;       // RGBA32F array: unit normal XYZ, unused
    VkImageView foam_jacobian = VK_NULL_HANDLE;// RG32F array: persistent foam [0,1], horizontal Jacobian determinant
    uint32_t resolution = 0, cascades = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
};
struct OceanGpu;
OceanGpu* create(const CreateInfo& info, std::string* error = nullptr);
// Record into a begun primary command buffer outside a render pass. Calls and
// submissions must be serialized, in update order, on a single queue family.
bool update(OceanGpu* ocean, VkCommandBuffer commands, double dt, Outputs* outputs, std::string* error = nullptr);
// Caller must finish every GPU command that references this object before destroy.
void destroy(OceanGpu* ocean);
}  // namespace ocean_fft
