// SPDX-License-Identifier: MIT
// Fault injection for the host buffer helper (runtime/nvn/nvn_hostbuf.h, behind CreateHostBuffer): each Vulkan step fails by itself and the helper must
// return false with null outputs and everything it made destroyed; a bind failure must not reach vkMapMemory; the success path hands out a mapped pointer.
// No device needed: the steps are a fake. Build: tests\nvn\build_hostbuf.bat
#include <cstdio>
#include <cstring>

#include "nvn_hostbuf.h"

namespace {

int g_failed = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failed; std::printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct FakeOps {
  VkResult create = VK_SUCCESS, alloc = VK_SUCCESS, bind = VK_SUCCESS, map = VK_SUCCESS;
  uint32_t memType = 0;                      // UINT32_MAX = no host-visible type
  int created = 0, destroyed = 0, allocated = 0, freed = 0, bound = 0, mapped = 0, errors = 0;
  const char* lastError = nullptr;
  alignas(16) unsigned char backing[64] = {};
  VkResult CreateBuffer(const VkBufferCreateInfo*, VkBuffer* b) { if (create != VK_SUCCESS) return create; ++created; *b = (VkBuffer)(uintptr_t)0x10; return VK_SUCCESS; }
  void MemReqs(VkBuffer, VkMemoryRequirements* r) { r->size = 64; r->memoryTypeBits = 1; }
  uint32_t MemType(uint32_t) { return memType; }
  VkResult Allocate(const VkMemoryAllocateInfo*, VkDeviceMemory* m) { if (alloc != VK_SUCCESS) return alloc; ++allocated; *m = (VkDeviceMemory)(uintptr_t)0x20; return VK_SUCCESS; }
  VkResult Bind(VkBuffer, VkDeviceMemory) { ++bound; return bind; }
  VkResult Map(VkDeviceMemory, void** p) { ++mapped; if (map != VK_SUCCESS) return map; *p = backing; return VK_SUCCESS; }
  void DestroyBuffer(VkBuffer) { ++destroyed; }
  void Free(VkDeviceMemory) { ++freed; }
  void Error(const char* step, VkResult) { ++errors; lastError = step; }
};

struct Out { VkBuffer b = (VkBuffer)(uintptr_t)0x99; VkDeviceMemory m = (VkDeviceMemory)(uintptr_t)0x99; void* p = (void*)0x99; };

bool run(FakeOps& o, Out& out) { return snapnvn::CreateHostBufferWith(o, 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &out.b, &out.m, &out.p); }
void expect_clean_failure(const char* name, FakeOps& o, bool ok, const Out& out, const char* step) {
  std::printf("%s\n", name);
  CHECK(!ok);
  CHECK(out.b == VK_NULL_HANDLE && out.m == VK_NULL_HANDLE && out.p == nullptr);   // outputs cleared even though the caller passed junk in
  CHECK(o.created == o.destroyed);                                                 // every buffer made was destroyed
  CHECK(o.allocated == o.freed);                                                   // every allocation made was freed
  CHECK(o.errors == 1 && o.lastError && !std::strcmp(o.lastError, step));          // exactly one error, naming the failing step
}

}  // namespace

int main() {
  { FakeOps o; Out r; bool ok = run(o, r);
    std::printf("success\n");
    CHECK(ok && r.b && r.m && r.p == o.backing && o.bound == 1 && o.mapped == 1 && o.errors == 0 && o.destroyed == 0 && o.freed == 0);
    std::memset(r.p, 0xAB, 64); CHECK(o.backing[63] == 0xAB); }               // the pointer is usable
  { FakeOps o; o.create = VK_ERROR_OUT_OF_HOST_MEMORY; Out r; bool ok = run(o, r);
    expect_clean_failure("create fails", o, ok, r, "vkCreateBuffer"); CHECK(o.allocated == 0 && o.bound == 0 && o.mapped == 0); }
  { FakeOps o; o.memType = UINT32_MAX; Out r; bool ok = run(o, r);
    expect_clean_failure("no host-visible memory type", o, ok, r, "no host-visible memory type"); CHECK(o.created == 1 && o.allocated == 0 && o.bound == 0); }
  { FakeOps o; o.alloc = VK_ERROR_OUT_OF_DEVICE_MEMORY; Out r; bool ok = run(o, r);
    expect_clean_failure("allocate fails", o, ok, r, "vkAllocateMemory"); CHECK(o.created == 1 && o.destroyed == 1 && o.bound == 0 && o.mapped == 0); }
  { FakeOps o; o.bind = VK_ERROR_OUT_OF_DEVICE_MEMORY; Out r; bool ok = run(o, r);
    expect_clean_failure("bind fails", o, ok, r, "vkBindBufferMemory"); CHECK(o.bound == 1 && o.mapped == 0 && o.freed == 1); }   // a bind failure never maps
  { FakeOps o; o.map = VK_ERROR_MEMORY_MAP_FAILED; Out r; bool ok = run(o, r);
    expect_clean_failure("map fails", o, ok, r, "vkMapMemory"); CHECK(o.bound == 1 && o.mapped == 1 && o.destroyed == 1 && o.freed == 1); }
  std::printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
