// SPDX-License-Identifier: MIT
// Driver quirk table, see nvn_quirks.h and docs/driver-quirks.md. Project code, ours.
#include "nvn_quirks.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace snap_quirks {
namespace {

const char* const kFeatures[] = {"provoking_vertex", "custom_border_color", "subgroup_pin", "present_wait", nullptr};

// The table. An entry exists only for a problem that was actually seen on that GPU and driver (the `evidence` field names the run, report
// or note). As of 2026-11-15 no such problem is on record: every run so far was on one NVIDIA RTX 4090 and none of the logs or notes
// shows a driver or GPU bug (the AMD and Intel paths are untested, see the "no AMD GPU" note). So the table is empty; the machinery is
// here, and SNAP_QUIRK=<feature> turns a feature off by hand for a bug report or a test. Shape of an entry:
//   {"name", vendor, dev_lo, dev_hi, drv_lo_major, drv_lo_minor, drv_hi_major, drv_hi_minor, {"feature", nullptr}, "run or report"},
const Quirk kTable[] = {
#ifdef SNAP_QUIRKS_TEST_TABLE   // tests/quirks only: one entry for the RTX 4090 range on driver 600 and up
    {"test_entry", 0x10de, 0x2680, 0x2690, 600, 0, 0, 0, {"present_wait", "provoking_vertex"}, "unit test"},
#endif
    {nullptr, 0, 0, 0, 0, 0, 0, 0, {nullptr}, nullptr},   // end marker (C++ has no empty arrays)
};

GpuId g_gpu;
char g_gpuLine[320] = "";
char g_appliedLine[256] = "";
const char* g_off[16];
int g_nOff = 0;
bool g_inited = false;

bool IsFeature(const char* s, size_t n) {
  for (const char* const* f = kFeatures; *f; f++) if (strlen(*f) == n && !strncmp(*f, s, n)) return true;
  return false;
}
void AddOff(const char* feature) {
  for (int i = 0; i < g_nOff; i++) if (!strcmp(g_off[i], feature)) return;
  if (g_nOff < 16) g_off[g_nOff++] = feature;
}
const char* VendorName(uint32_t v) {
  switch (v) {
    case 0x10de: return "NVIDIA";
    case 0x1002: return "AMD";
    case 0x8086: return "Intel";
    default: return "vendor";
  }
}

}  // namespace

void Init(uint32_t vendor, uint32_t device, uint32_t driver_raw, const char* name) {
  if (g_inited) return;
  g_inited = true;
  g_gpu.vendor = vendor; g_gpu.device = device; g_gpu.driver = Decode(vendor, driver_raw);
  snprintf(g_gpuLine, sizeof g_gpuLine, "%s, vendor 0x%04x (%s) device 0x%04x, driver %u.%u (raw 0x%08x)", name ? name : "?", vendor, VendorName(vendor), device,
           (unsigned)(g_gpu.driver >> 32), (unsigned)(g_gpu.driver & 0xffffffffu), driver_raw);

  const char* tbl = getenv("SNAP_QUIRKS");
  const bool tableOn = !(tbl && tbl[0] == '0');
  int matched = 0, entries = 0;
  for (const Quirk* q = kTable; q->name; q++) {
    entries++;
    char var[96];
    snprintf(var, sizeof var, "SNAP_QUIRK_%s", q->name);
    for (char* p = var; *p; p++) *p = (char)toupper((unsigned char)*p);
    const char* ov = getenv(var);
    const bool forceOn = ov && ov[0] == '1', forceOff = ov && ov[0] == '0';
    const bool hit = forceOn || (tableOn && !forceOff && Matches(*q, g_gpu));
    if (!hit) { if (forceOff && Matches(*q, g_gpu)) fprintf(stderr, "[quirk] %s matches this GPU but %s=0: not applied\n", q->name, var); continue; }
    matched++;
    fprintf(stderr, "[quirk] applied %s%s: turns off", q->name, forceOn ? " (forced by env)" : "");
    for (int i = 0; i < 4 && q->off[i]; i++) { AddOff(q->off[i]); fprintf(stderr, " %s", q->off[i]); }
    fprintf(stderr, " (seen: %s)\n", q->evidence ? q->evidence : "?");
  }

  if (const char* f = getenv("SNAP_QUIRK"); f && *f) {   // forced by hand: SNAP_QUIRK=present_wait,subgroup_pin
    while (*f) {
      while (*f == ',' || *f == ' ') f++;
      const char* e = f;
      while (*e && *e != ',' && *e != ' ') e++;
      if (e > f) {
        if (IsFeature(f, (size_t)(e - f))) {
          for (const char* const* k = kFeatures; *k; k++) if (strlen(*k) == (size_t)(e - f) && !strncmp(*k, f, (size_t)(e - f))) { AddOff(*k); fprintf(stderr, "[quirk] SNAP_QUIRK: %s off\n", *k); }
        } else {
          fprintf(stderr, "[quirk] SNAP_QUIRK: unknown feature '%.*s' ignored (known:", (int)(e - f), f);
          for (const char* const* k = kFeatures; *k; k++) fprintf(stderr, " %s", *k);
          fprintf(stderr, ")\n");
        }
      }
      f = e;
    }
  }

  size_t at = 0;
  g_appliedLine[0] = 0;
  for (int i = 0; i < g_nOff; i++) at += (size_t)snprintf(g_appliedLine + at, sizeof g_appliedLine - at, "%s%s", i ? ", " : "", g_off[i]);
  fprintf(stderr, "[quirk] GPU %s: %d of %d table entries applied%s%s%s\n", g_gpuLine, matched, entries, tableOn ? "" : " (SNAP_QUIRKS=0: table ignored)",
          g_nOff ? "; features off: " : "", g_appliedLine);
}

bool Off(const char* feature) {
  for (int i = 0; i < g_nOff; i++) if (!strcmp(g_off[i], feature)) return true;
  return false;
}

}  // namespace snap_quirks

extern "C" const char* snap_quirks_gpu_line() { return snap_quirks::g_gpuLine; }
extern "C" const char* snap_quirks_applied_line() { return snap_quirks::g_appliedLine; }
