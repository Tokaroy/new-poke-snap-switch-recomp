// SPDX-License-Identifier: MIT
// Present area downscale (nvn_downsample.cpp, project code, ours).
#pragma once
#include <vulkan/vulkan.h>

namespace nvn {
struct Host; struct QueueImpl; struct TextureImpl;
// When the window texture is larger than the window (supersampling), PresentWindowOnce calls this instead of shrinking with a bilinear
// vkCmdBlitImage. It box-filters the source rect (sx, sy, sw, sh) of src (TRANSFER_SRC_OPTIMAL, left that way) into a dw x dh image and
// returns it in TRANSFER_SRC_OPTIMAL for a 1:1 NEAREST blit at (0, 0, dw, dh). VK_NULL_HANDLE: not applicable or setup failed; the caller
// blits as before. q is the present queue (queueMutex held); its next timeline value is this present's. SNAP_PRESENT_AREA=0 turns it off.
VkImage AreaDownsample(Host* h, QueueImpl* q, VkCommandBuffer cb, TextureImpl* src, int sx, int sy, int sw, int sh, int dw, int dh);
}  // namespace nvn
