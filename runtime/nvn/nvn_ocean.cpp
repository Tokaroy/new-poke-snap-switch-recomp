// SPDX-License-Identifier: GPL-3.0-only
// Additional linking permission: see runtime/fx/ocean_fft/LICENSE.
// Glue between the FFT ocean module (runtime/fx/ocean_fft/ocean_gpu.cpp) and the water pass (nvn_ssr.cpp). Created on first use, never for photo frames (the
// water pass does not run there), time = presents / 60 like the rest of the water pass. Visual only: the maps are sampled by water_fx.frag at the world position
// of each water pixel; the game's sea mesh and everything the game reads back are untouched.
#include <algorithm>
#include <cstdio>
#include <string>

#include "fx/ocean_fft/ocean_gpu.h"
#include "nvn_internal.h"
#include "nvn_ocean.h"
#include "nvn_ocean_spv.h"

namespace nvn {
namespace {
struct O {
  bool failed = false;
  ocean_fft::OceanGpu* gpu = nullptr;
  ocean_fft::Outputs maps;
  int lastPresent = -1;
  double lastClock = 0.0;
  double lastTime = 0;
  // 1x1x3 RGBA16F dummies for the two array bindings when the ocean is not in use
  VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; bool dummyTried = false;
} o;

ocean_fft::Spirv Sp(const uint32_t* w, size_t bytes) { return {w, bytes / sizeof(uint32_t)}; }

void Fail(const char* why) { fprintf(stderr, "ocean: disabled (%s)\n", why); o.failed = true; }
}  // namespace

void OceanDummyViews(ReplayCtx& cx, VkImageView* normal, VkImageView* foam) {
  *normal = *foam = VK_NULL_HANDLE;
  Host* h = cx.h;
  if (!o.view && !o.dummyTried) {
    o.dummyTried = true;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R16G16B16A16_SFLOAT; ci.extent = {1, 1, 1}; ci.mipLevels = 1; ci.arrayLayers = 3; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(h->device, &ci, nullptr, &o.img) == VK_SUCCESS) {
      VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, o.img, &mr);
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vc.image = o.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vc.format = ci.format; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 3};
      if (vkAllocateMemory(h->device, &ai, nullptr, &o.mem) == VK_SUCCESS && vkBindImageMemory(h->device, o.img, o.mem, 0) == VK_SUCCESS &&
          vkCreateImageView(h->device, &vc, nullptr, &o.view) == VK_SUCCESS) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = o.img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 3};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cx.cb, &di);
        const VkClearColorValue zero{};
        vkCmdClearColorImage(cx.cb, o.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &b.subresourceRange);
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier2(cx.cb, &di);
      } else { o.view = VK_NULL_HANDLE; }
    }
  }
  *normal = *foam = o.view;
}

bool OceanMaps(ReplayCtx& cx, VkImageView* normal, VkImageView* foam) {
  if (!o.failed) {
    if (!o.gpu) {
      Host* h = cx.h;
      ocean_fft::CreateInfo ci;
      ci.params.choppiness = 1.5f; ci.params.foam_threshold = 0.95f; ci.params.foam_gain = 3.f; ci.params.foam_decay = 1.2f;   // defaults make almost no foam (CPU stats: whitecaps on about 12% / 5% / 0% of the three cascades at these values)
      ci.physical = h->phys; ci.device = h->device;
      ci.storage_image_extended_formats_enabled = h->features.shaderStorageImageExtendedFormats == VK_TRUE;
      ci.init = Sp(kOceanInit, sizeof kOceanInit); ci.evolve = Sp(kOceanEvolve, sizeof kOceanEvolve); ci.ifft = Sp(kOceanIfft, sizeof kOceanIfft); ci.output = Sp(kOceanOutput, sizeof kOceanOutput);
      std::string err;
      if (!ci.storage_image_extended_formats_enabled) Fail("the GPU lacks shaderStorageImageExtendedFormats");
      else if (!(o.gpu = ocean_fft::create(ci, &err))) Fail(err.c_str());
      else fprintf(stderr, "ocean: FFT ocean created (%u cascades, %ux%u)\n", ci.params.cascades, ci.params.resolution, ci.params.resolution);
    }
    if (o.gpu) {
      // one update per present: a second pass in the same present reuses the maps. Time is the wall-clock WaterClock (frame-rate independent, per-frame dt clamped).
      const double dt = o.lastPresent < 0 ? 0.0 : std::max(0.0, WaterClock() - o.lastClock);
      if (o.lastPresent < 0 || g_presentCount != o.lastPresent) {
        std::string err;
        if (!ocean_fft::update(o.gpu, cx.cb, dt, &o.maps, &err)) Fail(err.c_str());   // never destroyed: a command buffer in flight may still read it
        else { o.lastPresent = g_presentCount; o.lastClock = WaterClock(); }
      }
      if (!o.failed && o.maps.normal && o.maps.foam_jacobian) { *normal = o.maps.normal; *foam = o.maps.foam_jacobian; return true; }
    }
  }
  OceanDummyViews(cx, normal, foam);
  return false;
}
}  // namespace nvn
