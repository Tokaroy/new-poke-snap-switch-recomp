// SPDX-License-Identifier: MIT
// Matcher test for the driver quirk table (runtime/nvn/nvn_quirks.h): vendor, device range and driver range, per-vendor driver decoding.
//   clang++ -std=c++20 -I runtime/nvn tests/quirks/quirks_test.cpp runtime/nvn/nvn_quirks.cpp -o quirks_test.exe && quirks_test.exe
#include <cstdio>
#include <cstdlib>

#include "nvn_quirks.h"

using namespace snap_quirks;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static GpuId gpu(uint32_t v, uint32_t d, uint32_t major, uint32_t minor) { GpuId g; g.vendor = v; g.device = d; g.driver = (uint64_t)major << 32 | minor; return g; }

int main() {
  // NVIDIA packs 10/8/8/6 bits: 621.14 -> major 621, minor 14
  CHECK(Decode(0x10de, (621u << 22) | (14u << 14)) == ((uint64_t)621 << 32 | 14));
  // Intel on Windows: 101.5762
  CHECK(Decode(0x8086, (101u << 14) | 5762u) == ((uint64_t)101 << 32 | 5762));
  // standard packing: 24.3.4 -> 24.3
  CHECK(Decode(0x1002, (24u << 22) | (3u << 12) | 4u) == ((uint64_t)24 << 32 | 3));

  const Quirk any_nv  = {"a", 0x10de, 0, 0, 0, 0, 0, 0, {"present_wait"}, "t"};
  const Quirk dev_rng = {"b", 0x10de, 0x2680, 0x2690, 0, 0, 0, 0, {"subgroup_pin"}, "t"};
  const Quirk drv_rng = {"c", 0x1002, 0, 0, 24, 1, 25, 6, {"custom_border_color"}, "t"};
  const Quirk open_hi = {"d", 0x8086, 0, 0, 101, 5000, 0, 0, {"provoking_vertex"}, "t"};
  const Quirk any_gpu = {"e", 0, 0, 0, 0, 0, 0, 0, {"present_wait"}, "t"};

  CHECK(Matches(any_nv, gpu(0x10de, 0x2684, 621, 14)));
  CHECK(!Matches(any_nv, gpu(0x1002, 0x2684, 621, 14)));
  CHECK(Matches(dev_rng, gpu(0x10de, 0x2684, 1, 0)));        // 0x2684 is inside 0x2680..0x2690
  CHECK(Matches(dev_rng, gpu(0x10de, 0x2680, 1, 0)));        // both ends inclusive
  CHECK(Matches(dev_rng, gpu(0x10de, 0x2690, 1, 0)));
  CHECK(!Matches(dev_rng, gpu(0x10de, 0x267f, 1, 0)));
  CHECK(!Matches(dev_rng, gpu(0x10de, 0x2691, 1, 0)));
  CHECK(!Matches(drv_rng, gpu(0x1002, 1, 24, 0)));           // below the low end
  CHECK(Matches(drv_rng, gpu(0x1002, 1, 24, 1)));            // low end inclusive
  CHECK(Matches(drv_rng, gpu(0x1002, 1, 25, 6)));            // high end inclusive
  CHECK(!Matches(drv_rng, gpu(0x1002, 1, 25, 7)));
  CHECK(Matches(drv_rng, gpu(0x1002, 1, 24, 900)));          // 24.900 is between 24.1 and 25.6
  CHECK(!Matches(open_hi, gpu(0x8086, 1, 101, 4999)));
  CHECK(Matches(open_hi, gpu(0x8086, 1, 101, 5000)));
  CHECK(Matches(open_hi, gpu(0x8086, 1, 999, 0)));           // no high end: open
  CHECK(Matches(any_gpu, gpu(0x1234, 0x5678, 0, 0)));

  // the real table (nvn_quirks.cpp) has no entry for these features: Off() stays false for them (it turns off "gpl" only)
  Init(0x10de, 0x2684, (621u << 22) | (14u << 14), "Test GPU");
#ifndef SNAP_QUIRKS_TEST_TABLE
  CHECK(!Off("present_wait"));
  CHECK(!Off("provoking_vertex"));
#endif
  printf(fails ? "quirks_test: %d FAILED\n" : "quirks_test: all passed\n", fails);
  return fails ? 1 : 0;
}
