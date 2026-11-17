// SPDX-License-Identifier: MIT
// The host-visible mapped buffer helper behind CreateHostBuffer (nvn_shader.cpp), written over a small "ops" object so tests/nvn/hostbuf_test.cpp
// can fail each Vulkan step by itself. Header-only on purpose: it needs only <vulkan/vulkan.h> types, no device.
//   Ops: VkResult CreateBuffer(const VkBufferCreateInfo*, VkBuffer*)   MemReqs(VkBuffer, VkMemoryRequirements*)   uint32_t MemType(uint32_t bits)
//        VkResult Allocate(const VkMemoryAllocateInfo*, VkDeviceMemory*)   VkResult Bind(VkBuffer, VkDeviceMemory)   VkResult Map(VkDeviceMemory, void**)
//        void DestroyBuffer(VkBuffer)   void Free(VkDeviceMemory)   void Error(const char* step, VkResult r)
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace snapnvn {

// True only when the buffer is created, bound to its memory and mapped. On any failure the outputs are null, whatever was made is destroyed,
// and Ops::Error names the failing step: no caller can end up with a buffer that is unbound or a mapped pointer that was never set.
template <class Ops>
bool CreateHostBufferWith(Ops& o, uint64_t size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, void** mapped) {
  *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *mapped = nullptr;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = usage;
  VkResult r = o.CreateBuffer(&bi, buf);
  if (r != VK_SUCCESS) { *buf = VK_NULL_HANDLE; o.Error("vkCreateBuffer", r); return false; }
  VkMemoryRequirements mr{};
  o.MemReqs(*buf, &mr);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = o.MemType(mr.memoryTypeBits);
  if (mai.memoryTypeIndex == UINT32_MAX) { o.Error("no host-visible memory type", VK_ERROR_FEATURE_NOT_PRESENT); o.DestroyBuffer(*buf); *buf = VK_NULL_HANDLE; return false; }
  r = o.Allocate(&mai, mem);
  if (r != VK_SUCCESS) { o.Error("vkAllocateMemory", r); o.DestroyBuffer(*buf); *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; return false; }
  const char* failed = nullptr;
  if ((r = o.Bind(*buf, *mem)) != VK_SUCCESS) failed = "vkBindBufferMemory";
  else if ((r = o.Map(*mem, mapped)) != VK_SUCCESS) failed = "vkMapMemory";
  if (failed) {
    o.Error(failed, r);
    o.DestroyBuffer(*buf);
    o.Free(*mem);
    *buf = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE; *mapped = nullptr;
    return false;
  }
  return true;
}

}  // namespace snapnvn
