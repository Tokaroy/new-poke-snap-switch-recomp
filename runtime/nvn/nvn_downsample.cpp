// SPDX-License-Identifier: MIT
// Present area downscale (project code, ours).
//
// Enhanced supersampling renders the window texture larger than the window (5760x3240 for a 3840x2160 window, a 1.5x step). The present
// used to shrink it with one bilinear vkCmdBlitImage, which reads 4 texels per output pixel and skips the rest, so thin edges shimmer.
// This pass runs a compute box filter (shaders/area.comp: every covered texel, weighted by covered area) into a window-size RGBA16F image;
// the present then blits that 1:1 (NEAREST). Same-size presents keep the NEAREST blit and upscales keep LINEAR (the caller decides).
// The framegen image (transfer-only) and formats without linear filtering fall back to the blit.
//
// Lifetime: the source view is the texture's own cached sampled view (TextureImpl::View, freed with the texture). Descriptor sets are a
// ring rewritten every present; a slot is reused only after the queue timeline passed the present that last used it. The intermediate
// image is recreated on a size change and the old one goes to the graveyard (RetireFn).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nvn_downsample.h"
#include "nvn_area_spv.h"
#include "nvn_internal.h"

namespace nvn {
namespace {
constexpr int kRing = 8;
constexpr VkFormat kMidFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
struct Mid { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; int w = 0, h = 0; };
struct State {
  bool tried = false, ok = false;
  VkSampler samp = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout pl = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set[kRing] = {};
  uint64_t setValue[kRing] = {};   // timeline value of the present that last used each set
  int next = 0;
  Mid mid;
  VkFormat lastBad = VK_FORMAT_UNDEFINED, lastGood = VK_FORMAT_UNDEFINED;
} S;

bool Setup(Host* h) {
  VkDevice d = h->device;
  VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(h->phys, kMidFmt, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
  if ((fp.optimalTilingFeatures & need) != need) return false;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(d, &si, nullptr, &S.samp) != VK_SUCCESS) return false;
  VkDescriptorSetLayoutBinding bn[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount = 2; dl.pBindings = bn;
  if (vkCreateDescriptorSetLayout(d, &dl, nullptr, &S.dsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1; pli.pSetLayouts = &S.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
  if (vkCreatePipelineLayout(d, &pli, nullptr, &S.pl) != VK_SUCCESS) return false;
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize = sizeof kAreaCompSpv; mi.pCode = kAreaCompSpv;
  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(d, &mi, nullptr, &mod) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
  ci.layout = S.pl;
  const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &S.pipe);
  vkDestroyShaderModule(d, mod, nullptr);
  if (r != VK_SUCCESS) return false;
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kRing}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kRing}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets = kRing; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  if (vkCreateDescriptorPool(d, &dp, nullptr, &S.pool) != VK_SUCCESS) return false;
  VkDescriptorSetLayout layouts[kRing];
  for (auto& l : layouts) l = S.dsl;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = S.pool; ai.descriptorSetCount = kRing; ai.pSetLayouts = layouts;
  return vkAllocateDescriptorSets(d, &ai, S.set) == VK_SUCCESS;
}

void FreeMid(Host* h, Mid& m) {
  if (m.view) vkDestroyImageView(h->device, m.view, nullptr);
  if (m.img) vkDestroyImage(h->device, m.img, nullptr);
  if (m.mem) vkFreeMemory(h->device, m.mem, nullptr);
  m = Mid{};
}

bool EnsureMid(Host* h, int w, int hh) {
  if (S.mid.img && S.mid.w == w && S.mid.h == hh) return true;
  if (S.mid.img) { Mid old = S.mid; RetireFn([h, old]() mutable { FreeMid(h, old); }); S.mid = Mid{}; }   // earlier presents may still read it
  Mid m; m.w = w; m.h = hh;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = kMidFmt; ci.extent = {(uint32_t)w, (uint32_t)hh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateImage(h->device, &ci, nullptr, &m.img) != VK_SUCCESS) return false;
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, m.img, &mr);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(h->device, &ai, nullptr, &m.mem) != VK_SUCCESS || vkBindImageMemory(h->device, m.img, m.mem, 0) != VK_SUCCESS) { FreeMid(h, m); return false; }
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = m.img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = kMidFmt; vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(h->device, &vc, nullptr, &m.view) != VK_SUCCESS) { FreeMid(h, m); return false; }
  S.mid = m;
  return true;
}

// The source format must allow linear filtering (what the bilinear blit needed) and sampling; cached for the last good and bad format.
bool FormatOk(Host* h, VkFormat f) {
  if (f == S.lastGood) return true;
  if (f == S.lastBad) return false;
  VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(h->phys, f, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  const bool ok = (fp.optimalTilingFeatures & need) == need;
  (ok ? S.lastGood : S.lastBad) = f;
  return ok;
}

void Bar(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
  b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cb, &di);
}

// ---- SNAP_PRESENT_AREA_PAIR=<dir>: test aid (comparison shots). On every present that takes a frame of a route `shot=<n>` burst (the
// presents where nvn_vk.cpp's NVN_FRAME_DUMP burst check fires), it also blits the same source rect the old way (bilinear) into a second
// RGBA16F image, reads back both and writes <dir>/pair_p<present>_area.bmp and ..._bilinear.bmp (24-bit, value * 255 rounded, which is
// what the 1:1 blit stores into a UNORM swapchain). Same frame, same pose; the files are written once the GPU finished that present.
struct Pair { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void* map = nullptr; Mid lin; uint64_t value = 0; int w = 0, h = 0, present = 0; };
Pair P;

float Half(uint16_t v) {
  const uint32_t s = (v >> 15) & 1, e = (v >> 10) & 31, m = v & 1023;
  float f = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? 65504.0f : std::ldexp((float)(m | 1024), (int)e - 25);
  return s ? -f : f;
}

void WriteBmp(const std::string& path, const uint16_t* px, int w, int h) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return;
  const int row = (w * 3 + 3) & ~3;
  const uint32_t size = 54 + (uint32_t)row * h;
  uint8_t hd[54] = {'B', 'M'};
  auto put = [&](int o, uint32_t v) { for (int i = 0; i < 4; i++) hd[o + i] = (uint8_t)(v >> (8 * i)); };
  put(2, size); put(10, 54); put(14, 40); put(18, (uint32_t)w); put(22, (uint32_t)h); hd[26] = 1; hd[28] = 24; put(34, (uint32_t)row * h);
  fwrite(hd, 1, 54, f);
  std::vector<uint8_t> line(row, 0);
  for (int y = h - 1; y >= 0; y--) {
    for (int x = 0; x < w; x++) {
      const uint16_t* p = px + ((size_t)y * w + x) * 4;
      for (int c = 0; c < 3; c++) line[x * 3 + c] = (uint8_t)std::lround(std::clamp(Half(p[2 - c]), 0.0f, 1.0f) * 255.0f);
    }
    fwrite(line.data(), 1, row, f);
  }
  fclose(f);
}

void PairFinish(Host* h, QueueImpl* q, bool wait) {
  if (!P.buf) return;
  uint64_t done = 0; vkGetSemaphoreCounterValue(h->device, q->timeline, &done);
  if (done < P.value) {
    if (!wait) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; wi.semaphoreCount = 1; wi.pSemaphores = &q->timeline; wi.pValues = &P.value;
    vkWaitSemaphores(h->device, &wi, UINT64_MAX);
  }
  const char* dir = getenv("SNAP_PRESENT_AREA_PAIR");
  const uint16_t* px = (const uint16_t*)P.map;
  const std::string base = std::string(dir) + "/pair_p" + std::to_string(P.present);
  WriteBmp(base + "_area.bmp", px, P.w, P.h);
  WriteBmp(base + "_bilinear.bmp", px + (size_t)P.w * P.h * 4, P.w, P.h);
  fprintf(stderr, "present: area pair written %s_{area,bilinear}.bmp (%dx%d)\n", base.c_str(), P.w, P.h);
  vkDestroyBuffer(h->device, P.buf, nullptr); vkFreeMemory(h->device, P.mem, nullptr);
  FreeMid(h, P.lin);
  P = Pair{};
}

void PairRecord(Host* h, QueueImpl* q, VkCommandBuffer cb, TextureImpl* src, int sx, int sy, int sw, int sh, int dw, int dh) {
  static const char* dir = getenv("SNAP_PRESENT_AREA_PAIR");
  if (!dir || !*dir) return;
  PairFinish(h, q, true);   // one pair in flight at a time
  if (!(g_fdBurst > 0 && g_fdBurst % 4 == 3)) return;   // nvn_vk.cpp took a burst frame this present (4 -> 3, 60 -> 59, ...)
  Pair p; p.w = dw; p.h = dh; p.present = g_presentCount; p.value = q->value + 1;
  const uint64_t bytes = (uint64_t)dw * dh * 8;
  if (!CreateHostBuffer(h, bytes * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &p.buf, &p.mem, &p.map)) return;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D; ci.format = kMidFmt; ci.extent = {(uint32_t)dw, (uint32_t)dh, 1}; ci.mipLevels = 1; ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  bool ok = vkCreateImage(h->device, &ci, nullptr, &p.lin.img) == VK_SUCCESS;
  if (ok) {
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(h->device, p.lin.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = h->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ok = vkAllocateMemory(h->device, &ai, nullptr, &p.lin.mem) == VK_SUCCESS && vkBindImageMemory(h->device, p.lin.img, p.lin.mem, 0) == VK_SUCCESS;
  }
  if (!ok) { vkDestroyBuffer(h->device, p.buf, nullptr); vkFreeMemory(h->device, p.mem, nullptr); FreeMid(h, p.lin); return; }
  Bar(cb, p.lin.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  VkImageBlit b{};
  b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  b.srcOffsets[0] = {sx, sy, 0}; b.srcOffsets[1] = {sx + sw, sy + sh, 1};
  b.dstOffsets[0] = {0, 0, 0}; b.dstOffsets[1] = {dw, dh, 1};
  vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.lin.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);   // the old present filter
  Bar(cb, p.lin.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferImageCopy bc{}; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {(uint32_t)dw, (uint32_t)dh, 1};
  vkCmdCopyImageToBuffer(cb, S.mid.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.buf, 1, &bc);
  bc.bufferOffset = bytes;
  vkCmdCopyImageToBuffer(cb, p.lin.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.buf, 1, &bc);
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; di.memoryBarrierCount = 1; di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cb, &di);
  P = p;
}
}  // namespace

VkImage AreaDownsample(Host* h, QueueImpl* q, VkCommandBuffer cb, TextureImpl* src, int sx, int sy, int sw, int sh, int dw, int dh) {
  static const bool kOn = [] { const char* e = getenv("SNAP_PRESENT_AREA"); return !(e && e[0] == '0'); }();
  if (!kOn || !h || !q || !src || !src->image || !src->fi || src->fi->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return VK_NULL_HANDLE;
  if (dw <= 0 || dh <= 0 || sw < dw || sh < dh || (sw == dw && sh == dh)) return VK_NULL_HANDLE;
  if (!S.tried) {
    S.tried = true; S.ok = Setup(h);
    fprintf(stderr, S.ok ? "present: area downscale on (SNAP_PRESENT_AREA=0 turns it off)\n" : "present: area downscale setup failed; bilinear blit\n");
  }
  if (!S.ok || !FormatOk(h, src->fi->vk)) return VK_NULL_HANDLE;
  const VkImageView sv = src->View(nullptr, true, 3);
  if (!sv || !EnsureMid(h, dw, dh)) return VK_NULL_HANDLE;
  // A ring slot is rewritten only after the present that last used it finished (the acquire throttle makes this wait rare).
  const int k = S.next; S.next = (S.next + 1) % kRing;
  if (S.setValue[k]) {
    uint64_t done = 0; vkGetSemaphoreCounterValue(h->device, q->timeline, &done);
    if (done < S.setValue[k]) {
      VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; wi.semaphoreCount = 1; wi.pSemaphores = &q->timeline; wi.pValues = &S.setValue[k];
      vkWaitSemaphores(h->device, &wi, UINT64_MAX);
    }
  }
  S.setValue[k] = q->value + 1;   // this present's submit signals the next value (queueMutex is held until it is submitted)
  VkDescriptorImageInfo ii[2] = {{S.samp, sv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, S.mid.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  for (int i = 0; i < 2; i++) {
    w[i].dstSet = S.set[k]; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1; w[i].pImageInfo = &ii[i];
    w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  }
  vkUpdateDescriptorSets(h->device, 2, w, 0, nullptr);
  // src: TRANSFER_SRC (frame dump copy, game writes before it) -> sampled; mid: previous present's blit read -> storage write
  Bar(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  Bar(cb, S.mid.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe);
  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pl, 0, 1, &S.set[k], 0, nullptr);
  const int32_t pc[6] = {sx, sy, sw, sh, dw, dh};
  vkCmdPushConstants(cb, S.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
  vkCmdDispatch(cb, (uint32_t)(dw + 15) / 16, (uint32_t)(dh + 15) / 16, 1);
  Bar(cb, src->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  Bar(cb, S.mid.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  PairRecord(h, q, cb, src, sx, sy, sw, sh, dw, dh);   // SNAP_PRESENT_AREA_PAIR test aid (off unless set)
  return S.mid.img;
}
}  // namespace nvn
