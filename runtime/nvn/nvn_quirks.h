// SPDX-License-Identifier: MIT
// Driver quirk table (project code, ours, no game data). A small table keyed by GPU vendor, device ID and driver version range; a matching
// entry turns off a risky Vulkan feature or path. The renderer asks Off("<feature>") where it would otherwise enable that feature.
//
// Features that can be turned off (each is also a plain env switch of its own; the quirk is the per-GPU way to do the same):
//   provoking_vertex      VK_EXT_provoking_vertex (flat varyings from the last vertex)        env SNAP_VK_PROVOKING=0
//   custom_border_color   VK_EXT_custom_border_color (arbitrary RGBA sampler borders)          env SNAP_VK_BORDER=0
//   subgroup_pin          pinning shaders that use subgroup ops to 32 lanes (subgroupSizeControl)  env SNAP_VK_SGPIN=0
//   gpl                   VK_EXT_graphics_pipeline_library fast linking (setting gfx_gpl = auto only; on / off override)  env SNAP_GPL=off
//   present_wait          VK_KHR_present_id + present_wait (queue depth cap in the pacer)       env SNAP_PACE_WAIT=0
//
// Overrides (read once, at device creation):
//   SNAP_QUIRK=<feature>[,<feature>...]   force these features off on any GPU, with or without a table entry
//   SNAP_QUIRK_<ENTRY>=0|1                0 = never apply that table entry, 1 = apply it on any GPU (ENTRY = the entry's name, upper case)
//   SNAP_QUIRKS=0                          ignore the whole table (SNAP_QUIRK still works)
// The log says which entries applied: "[quirk] ..." lines at start (also in problem reports and _crash.txt). docs/driver-quirks.md.
//
// RULE for adding an entry: it needs evidence from a real log or note (the run id or the report in the `evidence` field). No guessed entries.
#pragma once
#include <cstdint>

namespace snap_quirks {

struct GpuId {
  uint32_t vendor = 0, device = 0;
  uint64_t driver = 0;   // major << 32 | minor, from Decode()
};

// VkPhysicalDeviceProperties::driverVersion is packed per vendor. NVIDIA: 10/8/8/6 bits ("621.14" = major 621, minor 14). Intel on Windows:
// major = raw >> 14, minor = raw & 0x3fff ("101.5762"). Everyone else: the standard Vulkan packing (major.minor).
inline uint64_t Decode(uint32_t vendor, uint32_t raw) {
  uint32_t major, minor;
  if (vendor == 0x10de) { major = raw >> 22; minor = (raw >> 14) & 0xff; }
  else if (vendor == 0x8086) { major = raw >> 14; minor = raw & 0x3fff; }
  else { major = raw >> 22; minor = (raw >> 12) & 0x3ff; }
  return (uint64_t)major << 32 | minor;
}

struct Quirk {
  const char* name;                // SNAP_QUIRK_<NAME> overrides this entry
  uint32_t vendor;                 // 0 = any vendor
  uint32_t dev_lo, dev_hi;         // device ID range, inclusive; both 0 = any device
  uint32_t drv_lo_major, drv_lo_minor;   // driver range, inclusive; the high end is open when drv_hi_major is 0
  uint32_t drv_hi_major, drv_hi_minor;
  const char* off[4];              // features this entry turns off (null terminated list)
  const char* evidence;            // where the problem was seen: run id, report, note
};

inline bool Matches(const Quirk& q, const GpuId& g) {
  if (q.vendor && q.vendor != g.vendor) return false;
  if ((q.dev_lo || q.dev_hi) && (g.device < q.dev_lo || g.device > q.dev_hi)) return false;
  const uint64_t lo = (uint64_t)q.drv_lo_major << 32 | q.drv_lo_minor;
  const uint64_t hi = (uint64_t)q.drv_hi_major << 32 | q.drv_hi_minor;
  if (g.driver < lo) return false;
  if (q.drv_hi_major && g.driver > hi) return false;
  return true;
}

// Once, after the physical device is picked and before any optional feature is enabled. Logs the GPU and every entry that applied.
void Init(uint32_t vendor, uint32_t device, uint32_t driver_raw, const char* name);
// True when a table entry or SNAP_QUIRK turns `feature` off. Cheap after Init (a handful of string compares); false before Init.
bool Off(const char* feature);

}  // namespace snap_quirks

// "NVIDIA GeForce RTX 4090, vendor 0x10de device 0x2684, driver 621.14 (raw 0x...)" for crash reports; empty until Init ran.
extern "C" const char* snap_quirks_gpu_line();
// "provoking_vertex, present_wait" or "" (what is switched off right now), for crash reports.
extern "C" const char* snap_quirks_applied_line();
