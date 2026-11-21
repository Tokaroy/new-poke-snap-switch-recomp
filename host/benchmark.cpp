// SPDX-License-Identifier: MIT
// Built-in ride benchmark (see benchmark.h). The game runs as a second process so its settings, save profile and logging switches are plain
// environment variables:
//   * a scratch save profile (<data_root>\benchmark\profiles\<id>) holding the packed 'unlock_all' test save (host/benchmark_data.h, made by
//     scripts/bench_pack.py) and a copy of the player's settings.cfg, so the run uses the player's picture settings and never touches a real save;
//   * SNAP_INPUT_SCRIPT = the Florio ride route (host/routes/bench_florio.txt, embedded), SNAP_PERF + SNAP_GPU_PASSES = the frame and per-pass logs
//     (runtime/os/perf.cpp, runtime/nvn/nvn_perf.cpp), vsync and the frame limit off, sound off.
// When the game exits this process reads the two present numbers the route logged ("wait 'ride' met" / "wait 'rail>=110' met"), cuts the frame
// log to that span, computes the numbers, writes JSON and PNG and shows the results window (Dear ImGui on a D3D11 window, like the launcher's).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "benchmark.h"

#include <windows.h>
#include <winioctl.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "hle/hle_config.h"
#include "hle/json_lite.h"
#include "hle/profiles.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "ui_font.h"

#include "benchmark_data.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern "C" void snap_perf_overlay_info(double* gpu_ms, double* cpu_ms, int* render_w, int* render_h, double* pace_hz, char* aa, int aa_len);   // nvn_perf.cpp
extern "C" void snap_perf_device_info(char* name, int name_len, char* driver, int driver_len);                                            // nvn_perf.cpp

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")

namespace fs = std::filesystem;

namespace snap_benchmark {
namespace {

// ---------------------------------------------------------------------------------------------------------------- small helpers
std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string U8(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}
fs::path P(const std::string& s) { return fs::path(W(s)); }
std::string env(const char* n) { const char* e = std::getenv(n); return e && *e ? e : ""; }

bool read_text(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
bool write_text(const fs::path& p, const std::string& t) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(t.data(), (std::streamsize)t.size());
    if (!f.good()) return false;
  }
  fs::rename(tmp, p, ec);
  if (ec) { fs::remove(p, ec); ec.clear(); fs::rename(tmp, p, ec); }
  return !ec;
}

std::string fmt(const char* f, ...) {
  char b[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(b, sizeof b, f, ap);
  va_end(ap);
  return b;
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c == '\n') o += "\\n";
    else if (c < 0x20) o += ' ';
    else o += (char)c;
  }
  return o + "\"";
}

std::map<std::string, std::string> read_cfg(const fs::path& p) {   // settings.cfg: key=value lines, # comments
  std::map<std::string, std::string> m;
  std::string t;
  if (!read_text(p, t)) return m;
  std::istringstream is(t);
  std::string line;
  while (std::getline(is, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
  }
  return m;
}

// ---------------------------------------------------------------------------------------------------------------- results
struct Pass { std::string name; double ms = 0; };
struct Result {
  std::string date;           // "2026-11-15 18:30"
  std::string stamp;          // file name stem: benchmark-20261115-183012
  size_t frames = 0;
  double duration_s = 0, avg_fps = 0, p1_fps = 0, p01_fps = 0, min_fps = 0, max_fps = 0, avg_ms = 0, p99_ms = 0, max_ms = 0, gpu_busy_ms = 0;
  std::vector<Pass> passes;   // top 5
  std::vector<std::pair<std::string, std::string>> settings;   // label, value (the settings summary)
  std::string settings_key;   // same key = same settings (for the comparison with the previous run)
  std::vector<float> frame_ms;   // every frame of the ride
  double prev_avg_fps = 0, prev_p1_fps = 0;   // previous saved run with the same settings (0 = none)
  std::string prev_note;
  std::string json_path, png_path;
  std::string error;
};

double mean_of_worst(std::vector<float> v, double fraction) {   // v sorted descending inside
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end(), std::greater<float>());
  size_t k = (size_t)std::ceil((double)v.size() * fraction);
  k = std::max<size_t>(1, std::min(k, v.size()));
  double s = 0;
  for (size_t i = 0; i < k; i++) s += v[i];
  return s / (double)k;
}

void compute_stats(Result& r) {
  const std::vector<float>& v = r.frame_ms;
  r.frames = v.size();
  if (v.empty()) return;
  double sum = 0, mx = 0;
  for (float x : v) { sum += x; mx = std::max<double>(mx, x); }
  r.duration_s = sum / 1000.0;
  r.avg_ms = sum / (double)v.size();
  r.avg_fps = 1000.0 / r.avg_ms;
  r.max_ms = mx;
  r.min_fps = mx > 0 ? 1000.0 / mx : 0;
  const double w1 = mean_of_worst(v, 0.01), w01 = mean_of_worst(v, 0.001);
  r.p1_fps = w1 > 0 ? 1000.0 / w1 : 0;
  r.p01_fps = w01 > 0 ? 1000.0 / w01 : 0;
  std::vector<float> s = v;
  std::sort(s.begin(), s.end());
  r.p99_ms = s[std::min(s.size() - 1, (size_t)((double)s.size() * 0.99))];
  r.max_fps = s.front() > 0 ? 1000.0 / s.front() : 0;
}

std::string dur_text(double s) {
  const int t = (int)(s + 0.5);
  return t >= 60 ? fmt("%d min %02d s", t / 60, t % 60) : fmt("%d s", t);
}

std::string summary_text(const Result& r) {
  std::ostringstream o;
  o << "Snap Recomp ride benchmark (Florio Nature Park, " << dur_text(r.duration_s) << ", " << r.frames << " frames)\n";
  o << fmt("Average %.1f fps | 1%% low %.1f | 0.1%% low %.1f | lowest %.1f\n", r.avg_fps, r.p1_fps, r.p01_fps, r.min_fps);
  o << fmt("Frame time: average %.2f ms, 99th percentile %.2f ms, worst %.1f ms", r.avg_ms, r.p99_ms, r.max_ms);
  if (r.gpu_busy_ms > 0) o << fmt(", GPU busy %.2f ms", r.gpu_busy_ms);
  o << "\n";
  if (!r.passes.empty()) {
    o << "GPU per pass (ms per frame): ";
    for (size_t i = 0; i < r.passes.size(); i++) o << (i ? ", " : "") << r.passes[i].name << " " << fmt("%.2f", r.passes[i].ms);
    o << "\n";
  }
  for (auto& kv : r.settings) o << kv.first << ": " << kv.second << "\n";
  return o.str();
}

std::string result_json(const Result& r) {
  std::ostringstream o;
  o << "{\n  \"version\": 1,\n  \"app\": \"Snap Recomp ride benchmark\",\n  \"route\": \"Florio Nature Park (Day), full ride\",\n";
  o << "  \"date\": " << json_str(r.date) << ",\n  \"frames\": " << r.frames << ",\n";
  o << fmt("  \"duration_s\": %.3f,\n  \"avg_fps\": %.3f,\n  \"low_1pct_fps\": %.3f,\n  \"low_0_1pct_fps\": %.3f,\n  \"min_fps\": %.3f,\n  \"max_fps\": %.3f,\n",
           r.duration_s, r.avg_fps, r.p1_fps, r.p01_fps, r.min_fps, r.max_fps);
  o << fmt("  \"frame_ms_avg\": %.4f,\n  \"frame_ms_p99\": %.4f,\n  \"frame_ms_max\": %.4f,\n  \"gpu_busy_ms_avg\": %.4f,\n", r.avg_ms, r.p99_ms, r.max_ms, r.gpu_busy_ms);
  o << "  \"gpu_passes\": [";
  for (size_t i = 0; i < r.passes.size(); i++) o << (i ? ", " : "") << "{\"name\": " << json_str(r.passes[i].name) << fmt(", \"ms_per_frame\": %.4f}", r.passes[i].ms);
  o << "],\n  \"settings\": {";
  for (size_t i = 0; i < r.settings.size(); i++) o << (i ? ", " : "") << json_str(r.settings[i].first) << ": " << json_str(r.settings[i].second);
  o << "},\n  \"settings_key\": " << json_str(r.settings_key) << ",\n  \"frame_ms\": [";
  for (size_t i = 0; i < r.frame_ms.size(); i++) o << (i ? "," : "") << fmt("%.2f", r.frame_ms[i]);
  o << "]\n}\n";
  return o.str();
}

bool load_result(const fs::path& p, Result& r) {
  std::string t, err;
  hle::Json j;
  if (!read_text(p, t) || !hle::json_parse(t, j, err) || j.type != hle::Json::Obj) return false;
  auto num = [&](const char* k) { const hle::Json* x = j.get(k); return x ? x->num() : 0.0; };
  r.date = j.get("date") ? j.get("date")->str() : "";
  r.frames = (size_t)num("frames");
  r.duration_s = num("duration_s"); r.avg_fps = num("avg_fps"); r.p1_fps = num("low_1pct_fps"); r.p01_fps = num("low_0_1pct_fps");
  r.min_fps = num("min_fps"); r.max_fps = num("max_fps"); r.avg_ms = num("frame_ms_avg"); r.p99_ms = num("frame_ms_p99"); r.max_ms = num("frame_ms_max");
  r.gpu_busy_ms = num("gpu_busy_ms_avg");
  r.settings_key = j.get("settings_key") ? j.get("settings_key")->str() : "";
  if (const hle::Json* a = j.get("gpu_passes")) for (auto& x : a->a) r.passes.push_back({x.get("name") ? x.get("name")->str() : "", x.get("ms_per_frame") ? x.get("ms_per_frame")->num() : 0});
  if (const hle::Json* s = j.get("settings")) for (auto& kv : s->o) r.settings.emplace_back(kv.first, kv.second.str());
  if (const hle::Json* a = j.get("frame_ms")) for (auto& x : a->a) r.frame_ms.push_back((float)x.num());
  return r.frames > 0 && !r.frame_ms.empty();
}

std::string results_dir() {
  if (!env("SNAP_BENCH_DIR").empty()) return env("SNAP_BENCH_DIR");
  if (env("SNAP_PORTABLE") == "1" && !env("APPDATA").empty()) return env("APPDATA") + "\\SnapRecomp\\benchmarks";   // portable or --data-dir (host/portable.h); runtime/ui/ui_bench.cpp reads the same folder
  PWSTR docs = nullptr;
  std::string d;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) { d = U8(docs); CoTaskMemFree(docs); }
  if (d.empty()) d = hle::config().data_root;
  return d + "\\Snap Recomp\\benchmarks";
}

// ---------------------------------------------------------------------------------------------------------------- log analysis
// frames.csv: frame,t_s,dt_ms,tid,cpu_mcycles,gpu_busy_ms,... one row per present. Returns the dt of frames (from, to], their mean GPU busy time.
bool read_frames(const fs::path& csv, uint64_t from, uint64_t to, std::vector<float>& dt, double& gpu_busy) {
  std::ifstream f(csv);
  if (!f) return false;
  std::string line;
  std::getline(f, line);
  double gsum = 0;
  size_t gn = 0;
  while (std::getline(f, line)) {
    const char* p = line.c_str();
    char* e = nullptr;
    const uint64_t fr = std::strtoull(p, &e, 10);
    if (fr <= from || fr > to) continue;
    // columns: 0 frame, 1 t_s, 2 dt_ms, 3 tid, 4 cpu_mcycles, 5 gpu_busy_ms
    const char* c[6] = {};
    int col = 0;
    c[0] = p;
    for (const char* q = p; *q && col < 5; ++q) if (*q == ',') c[++col] = q + 1;
    if (col < 5) continue;
    const double d = std::atof(c[2]), g = std::atof(c[5]);
    if (d <= 0) continue;
    dt.push_back((float)d);
    if (g >= 0) { gsum += g; gn++; }
  }
  gpu_busy = gn ? gsum / (double)gn : 0;
  return !dt.empty();
}

// The GPU pass labels of nvn_perf.cpp ("rp 1920x1080 c4 f1a z t3f2", "cs p123 64x45x1", "aa pass 1920x1080"...) as names a player can read; passes that
// differ only in a target id or a pipeline id become one row.
std::string friendly_pass(const std::string& l) {
  auto starts = [&](const char* p) { return l.rfind(p, 0) == 0; };
  if (l == "cb-start") return "Command buffer start";
  if (starts("aa pass")) return "Anti-aliasing";
  if (starts("fx pass")) return "Post effects";
  if (starts("upscale")) return "Upscaler";
  if (l == "present clear+blit") return "Present: blit";
  if (l == "present look+ui") return "Present: look and menus";
  unsigned w = 0, h = 0;
  int c = 0;
  if (starts("rp ") && std::sscanf(l.c_str() + 3, "%ux%u c%d", &w, &h, &c) == 3)
    return fmt("Render %ux%u, %d target%s%s", w, h, c, c == 1 ? "" : "s", l.find(" z") != std::string::npos ? ", depth" : "");
  int id = 0, x = 0, y = 0, z = 0;
  if (starts("cs ") && std::sscanf(l.c_str() + 3, "p%d %dx%dx%d", &id, &x, &y, &z) == 4) return fmt("Compute %dx%dx%d", x, y, z);
  return l;
}

// passes.csv: present,slot,idx,ms,draws,label. Per label: total ms of the frames (from, to] divided by the number of frames.
void read_passes(const fs::path& csv, uint64_t from, uint64_t to, size_t frames, std::vector<Pass>& top) {
  std::ifstream f(csv);
  if (!f || !frames) return;
  std::map<std::string, double> sum;
  std::string line;
  std::getline(f, line);
  while (std::getline(f, line)) {
    size_t p[5], pos = 0;
    int k = 0;
    while (k < 5 && (pos = line.find(',', pos)) != std::string::npos) p[k++] = pos++;
    if (k < 5) continue;
    const uint64_t fr = std::strtoull(line.c_str(), nullptr, 10);
    if (fr <= from || fr > to) continue;
    std::string label = line.substr(p[4] + 1);
    while (!label.empty() && (label.back() == '\r' || label.back() == '\n')) label.pop_back();
    label = friendly_pass(label);
    sum[label] += std::atof(line.c_str() + p[2] + 1);
  }
  std::vector<Pass> all;
  for (auto& kv : sum) all.push_back({kv.first, kv.second / (double)frames});
  std::sort(all.begin(), all.end(), [](const Pass& a, const Pass& b) { return a.ms > b.ms; });
  if (all.size() > 5) all.resize(5);
  top = all;
}

// "input script: wait 'ride' met after 12.3 s at present 4567" -> 4567 (the last such line)
bool find_wait_present(const std::string& log, const char* cond, uint64_t& present) {
  const std::string key = std::string("wait '") + cond + "' met after ";
  size_t pos = log.rfind(key);
  if (pos == std::string::npos) return false;
  const size_t at = log.find(" at present ", pos);
  const size_t eol = log.find('\n', pos);
  if (at == std::string::npos || (eol != std::string::npos && at > eol)) return false;
  present = std::strtoull(log.c_str() + at + 12, nullptr, 10);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- scratch profile
// Packed save (scripts/bench_pack.py): u32 files, then per file: u16 path length, path, u64 size, u32 runs, per run: u64 offset, u32 length, bytes.
bool write_save(const fs::path& save_dir, const std::string& uid_dir, std::string& err) {
  const unsigned char* p = kBenchSave;
  const unsigned char* end = kBenchSave + sizeof kBenchSave;
  auto rd = [&](void* d, size_t n) { if ((size_t)(end - p) < n) return false; std::memcpy(d, p, n); p += n; return true; };
  uint32_t nfiles = 0;
  if (!rd(&nfiles, 4)) return false;
  for (uint32_t i = 0; i < nfiles; i++) {
    uint16_t pl = 0; uint64_t size = 0; uint32_t runs = 0;
    if (!rd(&pl, 2) || (size_t)(end - p) < pl) return false;
    std::string rel((const char*)p, pl);
    p += pl;
    if (!rd(&size, 8) || !rd(&runs, 4)) return false;
    const size_t slash = rel.find('/');
    if (slash != std::string::npos) rel = uid_dir + rel.substr(slash);   // the first part is the user folder: use this PC's
    const fs::path path = save_dir / P(rel);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create " + U8(path.wstring()); return false; }
    DWORD got = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &got, nullptr);   // the zero parts take no disk space
    LARGE_INTEGER sz; sz.QuadPart = (LONGLONG)size;
    SetFilePointerEx(h, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    for (uint32_t k = 0; k < runs; k++) {
      uint64_t off = 0; uint32_t len = 0;
      if (!rd(&off, 8) || !rd(&len, 4) || (size_t)(end - p) < len) { CloseHandle(h); return false; }
      LARGE_INTEGER o; o.QuadPart = (LONGLONG)off;
      SetFilePointerEx(h, o, nullptr, FILE_BEGIN);
      DWORD wr = 0;
      if (!WriteFile(h, p, len, &wr, nullptr) || wr != len) { CloseHandle(h); err = "cannot write " + U8(path.wstring()); return false; }
      p += len;
    }
    CloseHandle(h);
  }
  return true;
}

struct Scratch {
  std::string root, id, settings_src;   // profiles root (SNAP_PROFILES_DIR for the game), the profile id, the player's settings.cfg that was copied
};

bool make_scratch(const std::string& bench_dir, Scratch& sc, std::string& err) {
  const hle::Config& cfg = hle::config();
  // the player's settings: the profile the game would start with (SNAP_PROFILE, else the last used one); a run with a fixed save folder has none
  const std::string real_root = !env("SNAP_PROFILES_DIR").empty() ? env("SNAP_PROFILES_DIR") : cfg.data_root + "\\profiles";
  const bool explicit_save = !env("SNAP_SAVE_DIR").empty();
  fs::path src_cfg;
  if (!explicit_save || !env("SNAP_PROFILES_DIR").empty()) {
    hle::profiles::Store st(real_root);
    st.load();
    const std::string want = env("SNAP_PROFILE");
    const hle::profiles::Profile* pf = want.empty() ? st.find(st.last_used()) : st.find(want);
    if (!pf && !st.list().empty() && want.empty()) pf = &st.list().front();
    if (pf) src_cfg = P(real_root) / P(pf->id) / "settings.cfg";
  }
  sc.settings_src = U8(src_cfg.wstring());
  sc.root = bench_dir + "\\profiles";
  std::error_code ec;
  if (fs::path(P(sc.root)).filename() != L"profiles") { err = "unexpected scratch folder"; return false; }
  fs::remove_all(P(sc.root), ec);   // only ever this tool's own scratch folder
  hle::profiles::Store s(sc.root);
  s.load();
  std::string e2;
  sc.id = s.create("Benchmark", &e2);
  if (sc.id.empty()) { err = "cannot create the scratch profile: " + e2; return false; }
  s.set_last_used(sc.id);
  s.save();
  if (!write_save(P(s.save_dir(sc.id)), hle::uid_dir_name(cfg.uid), err)) { if (err.empty()) err = "cannot write the scratch save"; return false; }
  // settings: the player's, minus the rows that would change the ride itself or write files
  std::string text = "# scratch copy for the benchmark (the player's settings; rows that change the ride are left out)\n";
  if (!src_cfg.empty() && fs::exists(src_cfg, ec)) {
    std::string t;
    read_text(src_cfg, t);
    std::istringstream is(t);
    std::string line;
    static const char* skip[] = {"neo_speed", "film", "fps_overlay", "fps_overlay_corner", "photo_export", "photo_dir", "shiny", "player_name"};
    while (std::getline(is, line)) {
      while (!line.empty() && (line.back() == '\r')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      const std::string k = line.substr(0, line.find('='));
      bool drop = false;
      for (const char* s2 : skip) if (k == s2) drop = true;
      if (!drop) text += line + "\n";
    }
  }
  if (!write_text(P(sc.root) / P(sc.id) / "settings.cfg", text)) { err = "cannot write the scratch settings"; return false; }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- child process
struct EnvBlock {
  std::vector<std::pair<std::wstring, std::wstring>> v;
  EnvBlock() {
    wchar_t* b = GetEnvironmentStringsW();
    for (const wchar_t* p = b; p && *p; p += wcslen(p) + 1) {
      const wchar_t* eq = wcschr(p + 1, L'=');
      if (eq) v.emplace_back(std::wstring(p, eq), std::wstring(eq + 1));
    }
    if (b) FreeEnvironmentStringsW(b);
  }
  void set(const char* k, const std::string& val) {
    const std::wstring wk = W(k);
    for (auto& kv : v) if (_wcsicmp(kv.first.c_str(), wk.c_str()) == 0) { kv.second = W(val); return; }
    v.emplace_back(wk, W(val));
  }
  void erase(const char* k) {
    const std::wstring wk = W(k);
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto& kv) { return _wcsicmp(kv.first.c_str(), wk.c_str()) == 0; }), v.end());
  }
  std::wstring block() const {
    std::wstring o;
    for (auto& kv : v) { o += kv.first; o += L'='; o += kv.second; o += L'\0'; }
    o += L'\0';
    return o;
  }
};

std::string tail_lines(const std::string& t, int n) {
  size_t pos = t.size();
  while (n-- > 0 && pos > 0) {
    const size_t p = t.rfind('\n', pos > 1 ? pos - 2 : 0);
    if (p == std::string::npos) { pos = 0; break; }
    pos = p + 1;
  }
  std::string s = t.substr(pos);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

// ---------------------------------------------------------------------------------------------------------------- results window
struct Ui {
  HWND hwnd = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain* sc = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  UINT rw = 1280, rh = 720;
  bool resize = false, done = false;
  ImFont* font = nullptr;
  float s = 1, ox = 0, oy = 0;
  const Result* r = nullptr;
  std::string toast;
  double toast_until = 0;
  bool open_folder = false;
} U;

ImVec2 V(float x, float y) { return ImVec2(U.ox + x * U.s, U.oy + y * U.s); }
ImU32 C(unsigned rgb, int a = 255) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a); }
const unsigned kPanel = 0x6e7180, kCream = 0xf4f2f0, kWhite = 0xfcfcfc, kText = 0x4f3d29, kGrey = 0x8b8a8b, kOrange = 0xda743a, kOrLight = 0xf8ab79, kBlue = 0x4e6a8f, kLine = 0xdedbd6, kGreen = 0x6fa04a;

float text_w(float sz, const char* t) { return U.font->CalcTextSizeA(sz * U.s, FLT_MAX, 0.0f, t).x / U.s; }
void text(float x, float y, float sz, ImU32 col, const char* t, int al = 0) {   // y = vertical centre; al 0 left, 1 centre, 2 right
  const ImVec2 ts = U.font->CalcTextSizeA(sz * U.s, FLT_MAX, 0.0f, t);
  ImVec2 p = V(x, y);
  p.y -= ts.y * 0.5f;
  if (al == 1) p.x -= ts.x * 0.5f; else if (al == 2) p.x -= ts.x;
  ImGui::GetForegroundDrawList()->AddText(U.font, sz * U.s, p, col, t);
}
void text_clip(float x, float y, float sz, ImU32 col, std::string t, float maxw, int al = 0) {
  if (text_w(sz, t.c_str()) > maxw) {
    while (t.size() > 3 && text_w(sz, (t + "..").c_str()) > maxw) t.pop_back();
    t += "..";
  }
  text(x, y, sz, col, t.c_str(), al);
}
void card(float x, float y, float w, float h) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (int i = 0; i < 4; i++) {
    const float g = 2.0f + i * 3.0f;
    dl->AddRectFilled(V(x - g, y - g + 5), V(x + w + g, y + h + g + 5), IM_COL32(60, 45, 30, 6), (14 + g) * U.s);
  }
  dl->AddRectFilled(V(x, y), V(x + w, y + h), C(kWhite), 14 * U.s);
}
bool hot(float x, float y, float w, float h) {
  const ImVec2 m = ImGui::GetIO().MousePos;
  const ImVec2 a = V(x, y), b = V(x + w, y + h);
  return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
}
bool button(float x, float y, float w, float h, const char* label, const char* key, bool primary) {
  const bool over = hot(x, y, w, h);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const unsigned base = primary ? kOrange : kPanel;
  dl->AddRectFilled(V(x, y), V(x + w, y + h), C(base, over ? 255 : 235), h * 0.5f * U.s);
  if (over) dl->AddRect(V(x, y), V(x + w, y + h), C(kOrLight), h * 0.5f * U.s, 0, 3 * U.s);
  text(x + (key ? (w - 56) / 2 : w / 2), y + h / 2, 24, C(0xffffff), label, 1);
  if (key) {
    const float kx = x + w - 38;
    dl->AddRectFilled(V(kx - 14, y + h / 2 - 15), V(kx + 14, y + h / 2 + 15), IM_COL32(255, 255, 255, 60), 8 * U.s);
    text(kx, y + h / 2, 20, C(0xffffff), key, 1);
  }
  return over && ImGui::IsMouseClicked(0);
}

void draw_graph(const Result& r, float x, float y, float w, float h) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const std::vector<float>& v = r.frame_ms;
  double top = std::max({r.p99_ms * 1.5, r.avg_ms * 2.2, 8.0});
  top = std::min(top, std::max(r.max_ms, 8.0));
  top = std::ceil(top / 2.0) * 2.0;
  const int nb = (int)std::min<size_t>(v.size(), (size_t)(w / 2.0f));
  for (int g = 0; g <= 3; g++) {
    const float gy = y + h - h * (float)g / 3.0f;
    dl->AddLine(V(x, gy), V(x + w, gy), C(kLine), 1 * U.s);
    text(x - 8, gy, 16, C(kGrey), fmt("%.0f", top * g / 3.0).c_str(), 2);
  }
  std::vector<ImVec2> pts;
  for (int b = 0; b < nb; b++) {
    const size_t i0 = v.size() * (size_t)b / (size_t)nb, i1 = std::max(i0 + 1, v.size() * (size_t)(b + 1) / (size_t)nb);
    float m = 0;
    for (size_t i = i0; i < i1 && i < v.size(); i++) m = std::max(m, v[i]);
    const double c = std::min<double>(m, top);
    pts.push_back(V(x + w * (float)b / (float)std::max(1, nb - 1), y + h - h * (float)(c / top)));
  }
  if (pts.size() > 1) dl->AddPolyline(pts.data(), (int)pts.size(), C(kBlue), 0, 1.6f * U.s);
  const float ay = y + h - h * (float)(r.avg_ms / top);
  dl->AddLine(V(x, ay), V(x + w, ay), C(kOrange), 2 * U.s);
  text(x, y + h + 16, 16, C(kGrey), "0 s", 0);
  text(x + w / 2, y + h + 16, 16, C(kGrey), dur_text(r.duration_s / 2).c_str(), 1);
  text(x + w, y + h + 16, 16, C(kGrey), dur_text(r.duration_s).c_str(), 2);
}

void draw_results(const Result& r) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(V(0, 0), V(1280, 84), C(kPanel));
  text(40, 42, 36, C(0xffffff), "Ride benchmark");
  text(1240, 42, 21, C(0xe9eaee), fmt("Florio Nature Park  |  %s  |  %zu frames", dur_text(r.duration_s).c_str(), r.frames).c_str(), 2);

  card(40, 108, 380, 484);
  text(66, 142, 20, C(kGrey), "AVERAGE");
  const std::string big = fmt("%.1f", r.avg_fps);
  text(62, 214, 104, C(kOrange), big.c_str());
  text(66 + text_w(104, big.c_str()) + 6, 238, 28, C(kGrey), "fps");
  struct Row { const char* label; std::string val; };
  const Row rows[] = {{"1% low", fmt("%.1f fps", r.p1_fps)},       {"0.1% low", fmt("%.1f fps", r.p01_fps)},
                      {"Lowest frame", fmt("%.1f fps", r.min_fps)}, {"Average frame", fmt("%.2f ms", r.avg_ms)},
                      {"99th percentile", fmt("%.2f ms", r.p99_ms)}, {"GPU busy", r.gpu_busy_ms > 0 ? fmt("%.2f ms", r.gpu_busy_ms) : "n/a"}};
  float y = 300;
  for (const Row& row : rows) {
    dl->AddLine(V(66, y - 22), V(394, y - 22), C(kLine), 1 * U.s);
    text(66, y, 21, C(kGrey), row.label);
    text(394, y, 28, C(kText), row.val.c_str(), 2);
    y += 44;
  }
  if (r.prev_avg_fps > 0) {
    const double d = (r.avg_fps - r.prev_avg_fps) / r.prev_avg_fps * 100.0;
    text(66, 570, 17, C(fabs(d) <= 5.0 ? kGreen : kOrange), fmt("Last run: %.1f fps (%+.1f%%)", r.prev_avg_fps, d).c_str());
  } else {
    text(66, 570, 17, C(kGrey), r.prev_note.empty() ? "No earlier run on these settings." : r.prev_note.c_str());
  }

  card(444, 108, 796, 218);
  text(470, 134, 22, C(kText), "Frame time over the ride (ms)");
  text(1214, 134, 18, C(kOrange), fmt("average %.2f ms", r.avg_ms).c_str(), 2);
  draw_graph(r, 520, 168, 690, 106);

  card(444, 344, 420, 248);
  text(470, 372, 22, C(kText), "GPU time per pass");
  text(838, 372, 15, C(kGrey), "ms per frame", 2);
  double mx = 0.001;
  for (auto& p : r.passes) mx = std::max(mx, p.ms);
  float py = 416;
  for (auto& p : r.passes) {
    text_clip(470, py, 18, C(kText), p.name, 215);
    const float bw = (float)(p.ms / mx) * 90.0f;
    dl->AddRectFilled(V(698, py - 9), V(698 + std::max(2.0f, bw), py + 9), C(kBlue), 4 * U.s);
    text(838, py, 18, C(kText), fmt("%.2f", p.ms).c_str(), 2);
    py += 39;
  }
  if (r.passes.empty()) text(470, 420, 18, C(kGrey), "No pass timing was recorded.");

  card(880, 344, 360, 248);
  text(902, 372, 22, C(kText), "Settings");
  float sy = 400;
  for (auto& kv : r.settings) {
    text(902, sy, 16, C(kGrey), kv.first.c_str());
    text_clip(1218, sy, 16, C(kText), kv.second, 190, 2);
    sy += 19.0f;
  }

  const bool c1 = button(40, 616, 280, 56, "Copy summary", "C", true);
  const bool c2 = button(336, 616, 340, 56, "Open results folder", "O", false);
  const bool c3 = button(990, 616, 250, 56, "Close", "Esc", false);
  text(40, 696, 15, C(kGrey), "Saved as JSON and PNG in Documents\\Snap Recomp\\benchmarks. Average = frames / time; 1% and 0.1% low = the slowest 1% / 0.1% of frames, as fps.");
  const double now = ImGui::GetTime();
  if (c1 || ImGui::IsKeyPressed(ImGuiKey_C, false)) { ImGui::SetClipboardText(summary_text(r).c_str()); U.toast = "Summary copied to the clipboard"; U.toast_until = now + 2.5; }
  if (c2 || ImGui::IsKeyPressed(ImGuiKey_O, false)) U.open_folder = true;
  if (c3 || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) U.done = true;
  if (now < U.toast_until) text(640, 596, 20, C(kGreen), U.toast.c_str(), 1);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return TRUE;
  switch (m) {
    case WM_SIZE: if (U.dev && w != SIZE_MINIMIZED) { U.rw = LOWORD(l); U.rh = HIWORD(l); U.resize = true; } return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)l; mm->ptMinTrackSize.x = 800; mm->ptMinTrackSize.y = 450; return 0; }
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_CLOSE: U.done = true; return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

bool create_target() {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(U.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  const HRESULT hr = U.dev->CreateRenderTargetView(bb, nullptr, &U.rtv);
  bb->Release();
  return SUCCEEDED(hr);
}
void release_target() { if (U.rtv) { U.rtv->Release(); U.rtv = nullptr; } }
bool create_device() {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = U.hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL fl[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  for (D3D_DRIVER_TYPE t : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, t, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &sd, &U.sc, &U.dev, &got, &U.ctx))) return create_target();
  return false;
}
void destroy_device() {
  release_target();
  if (U.sc) { U.sc->Release(); U.sc = nullptr; }
  if (U.ctx) { U.ctx->Release(); U.ctx = nullptr; }
  if (U.dev) { U.dev->Release(); U.dev = nullptr; }
}

// The back buffer as a PNG (through the Windows imaging component).
bool save_png(const std::string& path) {
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(U.sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  D3D11_TEXTURE2D_DESC d{};
  bb->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  bool ok = false;
  if (SUCCEEDED(U.dev->CreateTexture2D(&d, nullptr, &st))) {
    U.ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (SUCCEEDED(U.ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp))) {
      std::vector<unsigned char> px((size_t)d.Width * d.Height * 4);
      for (UINT y = 0; y < d.Height; ++y) {
        const unsigned char* s = (const unsigned char*)mp.pData + (size_t)y * mp.RowPitch;
        unsigned char* o = px.data() + (size_t)y * d.Width * 4;
        for (UINT x = 0; x < d.Width; ++x) { o[x * 4 + 0] = s[x * 4 + 2]; o[x * 4 + 1] = s[x * 4 + 1]; o[x * 4 + 2] = s[x * 4 + 0]; o[x * 4 + 3] = 255; }
      }
      U.ctx->Unmap(st, 0);
      const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      IWICImagingFactory* f = nullptr;
      if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        IWICStream* stm = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
        const std::wstring tmp = P(path).wstring() + L".tmp";
        std::error_code ec;
        fs::create_directories(P(path).parent_path(), ec);
        if (SUCCEEDED(f->CreateStream(&stm)) && SUCCEEDED(stm->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stm, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(d.Width, d.Height))) {
          WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
          fr->SetPixelFormat(&pf);
          ok = SUCCEEDED(fr->WritePixels(d.Height, d.Width * 4, (UINT)px.size(), px.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        }
        if (pb) pb->Release();
        if (fr) fr->Release();
        if (enc) enc->Release();
        if (stm) stm->Release();
        f->Release();
        if (ok) ok = MoveFileExW(tmp.c_str(), P(path).wstring().c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        else DeleteFileW(tmp.c_str());
      }
      if (SUCCEEDED(co)) CoUninitialize();
    }
    st->Release();
  }
  bb->Release();
  return ok;
}

// Shows the window until closed; saves the PNG (when png_path is set) once the first full frames are drawn. Returns false when no window could be made.
bool show_results(Result& r, const std::string& png_path) {
  U = Ui();
  U.r = &r;
  WNDCLASSEXW wc{sizeof wc};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"SnapRecompBenchmarkUi";
  wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);
  const DWORD style = WS_OVERLAPPEDWINDOW;
  U.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Snap Recomp - Ride benchmark", style, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
  if (!U.hwnd) { UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }
  {
    using GetDpi = UINT(WINAPI*)(HWND);
    GetDpi gd = (GetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    const float sc = gd ? gd(U.hwnd) / 96.0f : 1.0f;
    RECT rc{0, 0, (LONG)(1280 * sc), (LONG)(720 * sc)};
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = std::min<int>(rc.right - rc.left, wa.right - wa.left), h = std::min<int>(rc.bottom - rc.top, wa.bottom - wa.top);
    SetWindowPos(U.hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  RECT cr{};
  GetClientRect(U.hwnd, &cr);
  U.rw = (UINT)(cr.right - cr.left); U.rh = (UINT)(cr.bottom - cr.top);
  if (!create_device()) { destroy_device(); DestroyWindow(U.hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  bool ok = ImGui_ImplWin32_Init(U.hwnd) && ImGui_ImplDX11_Init(U.dev, U.ctx);
  std::vector<unsigned char> ttf;   // the game's own UI font, decoded from the player's files; else a system font
  if (ok) {
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    if (snap::ui::game_font_ttf(ttf) && !ttf.empty()) U.font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), 22.0f, &fc);
    if (!U.font) {
      char sysdir[MAX_PATH] = {};
      GetWindowsDirectoryA(sysdir, MAX_PATH);
      for (const char* f : {"\\Fonts\\segoeuib.ttf", "\\Fonts\\segoeui.ttf"}) {
        const std::string p = std::string(sysdir) + f;
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) { U.font = io.Fonts->AddFontFromFileTTF(p.c_str(), 22.0f); if (U.font) break; }
      }
    }
    if (!U.font) U.font = io.Fonts->AddFontDefaultVector();
  }
  if (!ok || !U.font) {
    if (ok) { ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); }
    ImGui::DestroyContext();
    destroy_device();
    DestroyWindow(U.hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return false;
  }
  ShowWindow(U.hwnd, SW_SHOW);
  SetForegroundWindow(U.hwnd);
  const double autoclose = env("SNAP_BENCH_AUTOCLOSE").empty() ? 0.0 : std::atof(env("SNAP_BENCH_AUTOCLOSE").c_str());
  int frames = 0;
  bool shot_done = png_path.empty();
  const auto t0 = std::chrono::steady_clock::now();
  while (!U.done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) U.done = true;
    }
    if (U.done) break;
    if (IsIconic(U.hwnd)) { Sleep(40); continue; }
    if (U.resize) {
      U.resize = false;
      release_target();
      U.sc->ResizeBuffers(0, U.rw, U.rh, DXGI_FORMAT_UNKNOWN, 0);
      create_target();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    U.s = std::min(ds.x / 1280.0f, ds.y / 720.0f);
    U.ox = (ds.x - 1280 * U.s) / 2;
    U.oy = (ds.y - 720 * U.s) / 2;
    draw_results(r);
    ImGui::Render();
    const float clear[4] = {0.96f, 0.95f, 0.94f, 1.0f};
    U.ctx->OMSetRenderTargets(1, &U.rtv, nullptr);
    U.ctx->ClearRenderTargetView(U.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ++frames;
    if (!shot_done && frames >= 6) {   // the first stable frame, before any click or toast
      shot_done = true;
      const bool saved = save_png(png_path);
      std::fprintf(stderr, "benchmark: results picture %s %s\n", png_path.c_str(), saved ? "saved" : "FAILED");
    }
    U.sc->Present(1, 0);
    if (U.open_folder) {
      U.open_folder = false;
      std::error_code ec;
      fs::create_directories(P(results_dir()), ec);
      ShellExecuteW(nullptr, L"explore", P(results_dir()).wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (autoclose > 0 && shot_done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > autoclose) U.done = true;
  }
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  destroy_device();
  DestroyWindow(U.hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return true;
}

// ---------------------------------------------------------------------------------------------------------------- the run
void fail(const std::string& msg, bool quiet) {
  std::fprintf(stderr, "benchmark: %s\n", msg.c_str());
  if (!quiet) MessageBoxW(nullptr, W(msg).c_str(), L"Snap Recomp - Ride benchmark", MB_OK | MB_ICONWARNING);
}

std::string nice_value(const std::string& v, const char* def) { return v.empty() ? def : v; }

// the settings summary from what the player has set (the scratch copy of settings.cfg) and what the game reported (info json)
void build_settings(Result& r, const std::map<std::string, std::string>& cfg, const hle::Json& info, bool uncapped) {
  auto istr = [&](const char* k) { const hle::Json* x = info.get(k); return x ? x->str() : std::string(); };
  auto inum = [&](const char* k) { const hle::Json* x = info.get(k); return x ? x->num() : 0.0; };
  auto get = [&](const char* k) { auto it = cfg.find(k); return it == cfg.end() ? std::string() : it->second; };
  const int ww = (int)inum("window_w"), wh = (int)inum("window_h"), rw = (int)inum("render_w"), rh = (int)inum("render_h");
  r.settings.emplace_back("Output", ww > 0 ? fmt("%d x %d", ww, wh) : "unknown");
  std::string render = rw > 0 ? fmt("%d x %d", rw, rh) : "unknown";
  r.settings.emplace_back("Internal resolution", render);
  r.settings.emplace_back("Look", get("gfx_look") == "original" ? "Original" : "Enhanced");
  std::string up = get("gfx_upscaler");
  std::string upq = get("gfx_upscale_quality");
  r.settings.emplace_back("Upscaler", up.empty() || up == "off" ? "None" : up + (upq.empty() ? "" : " (" + upq + ")"));
  r.settings.emplace_back("Anti-aliasing", istr("aa").empty() ? nice_value(get("gfx_aa"), "game's own") : istr("aa"));
  r.settings.emplace_back("Lighting", nice_value(get("gfx_lighting"), "default"));
  // HD textures: any enabled mod pack whose name starts with "hd"
  int hd = 0;
  {
    std::string mods = env("SNAP_MODS_DIR").empty() ? hle::config().data_root + "\\mods" : env("SNAP_MODS_DIR");
    std::string t, err;
    hle::Json j;
    if (read_text(P(mods) / "mods.json", t) && hle::json_parse(t, j, err)) {
      if (const hle::Json* a = j.get("packs")) for (auto& p : a->a) {
        std::string id = p.get("id") ? p.get("id")->str() : "";
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (id.rfind("hd", 0) == 0 && p.get("enabled") && p.get("enabled")->boolean()) hd++;
      }
    }
  }
  r.settings.emplace_back("HD textures", hd ? fmt("On (%d packs)", hd) : "Off");
  std::string fps = get("fps_target");
  if (!env("SNAP_FPS").empty()) fps = env("SNAP_FPS");   // a hand-set variable wins over the saved row, as in the game
  r.settings.emplace_back("Frame rate target", (fps.empty() || fps == "default" ? "60" : fps) + (uncapped ? " target, limiter off" : " target"));
  r.settings.emplace_back("GPU", nice_value(istr("gpu"), "unknown"));
  r.settings.emplace_back("Driver", nice_value(istr("driver"), "unknown"));
  r.settings_key.clear();
  for (auto& kv : r.settings) if (kv.first != "Driver") r.settings_key += kv.second + "|";
}

std::string now_text() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[64];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
  return b;
}
std::string now_stamp() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char b[64];
  std::strftime(b, sizeof b, "benchmark-%Y%m%d-%H%M%S", &tm);
  return b;
}

// the newest earlier saved run with the same settings
void find_previous(Result& r, const std::string& dir) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(P(dir), ec))
    if (e.path().extension() == L".json" && e.path().filename().wstring().rfind(L"benchmark-", 0) == 0 && U8(e.path().stem().wstring()) != r.stamp) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (size_t i = files.size(); i-- > 0;) {
    std::string t, err;
    hle::Json j;
    if (!read_text(files[i], t)) continue;
    // only the header fields are needed: avoid parsing the whole frame list of a big file twice
    const size_t cut = t.find("\"frame_ms\"");
    std::string head = (cut == std::string::npos ? t : t.substr(0, cut)) ;
    if (cut != std::string::npos) { while (!head.empty() && (head.back() == ' ' || head.back() == '\n' || head.back() == ',')) head.pop_back(); head += "}"; }
    if (!hle::json_parse(head, j, err)) continue;
    if (!j.get("settings_key") || j.get("settings_key")->str() != r.settings_key) { if (r.prev_note.empty()) r.prev_note = "Earlier runs used other settings."; continue; }
    r.prev_avg_fps = j.get("avg_fps") ? j.get("avg_fps")->num() : 0;
    r.prev_p1_fps = j.get("low_1pct_fps") ? j.get("low_1pct_fps")->num() : 0;
    r.prev_note.clear();
    return;
  }
}

struct Options { bool uncapped = true, quiet = false; std::vector<std::string> pass, sets; };   // sets: "--set key=value" values for the child; they leave pass empty so launcher mode still counts

bool execute(const Options& op, Result& out) {
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  const fs::path exe = mp, exe_dir = exe.parent_path();
  const fs::path cfg_path = exe_dir / L"config.json";
  std::error_code ec;
  const bool launcher_mode = fs::exists(cfg_path, ec) && op.pass.empty();
  hle::load_config(launcher_mode ? U8(cfg_path.wstring()) : std::string());
  const std::string data_root = hle::config().data_root;
  const std::string bench_root = data_root + "\\benchmark";
  const std::string bench_dir = bench_root + "\\" + std::to_string(GetCurrentProcessId());   // one folder per run: two benchmarks at once cannot collide
  fs::create_directories(P(bench_dir), ec);
  {   // folders left by a run that crashed (named like a process id, older than a day)
    const auto cut = fs::file_time_type::clock::now() - std::chrono::hours(24);
    for (auto& e : fs::directory_iterator(P(bench_root), ec)) {
      const std::wstring n = e.path().filename().wstring();
      if (!e.is_directory(ec) || n.empty() || n.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
      if (e.last_write_time(ec) < cut) fs::remove_all(e.path(), ec);
    }
  }
  const std::string base = bench_dir + "\\run";
  for (const char* suf : {"_frames.csv", "_threads.csv", "_events.csv", "_waits.csv", "_passes.csv", "_info.json", "_stderr.log"}) fs::remove(P(base + suf), ec);

  Scratch sc;
  std::string err;
  if (!make_scratch(bench_dir, sc, err)) { out.error = err; return false; }
  const std::string route_path = bench_dir + "\\bench_florio.txt";
  if (!write_text(P(route_path), std::string((const char*)kBenchRoute))) { out.error = "cannot write " + route_path; return false; }

  EnvBlock eb;
  for (const char* k : {"SNAP_SAVE_DIR", "SNAP_SAVE_ROOT", "SNAP_INPUT_SCRIPT", "SNAP_NEO_SPEED", "SNAP_FILM", "SNAP_PHOTO_EXPORT", "SNAP_FPS_OVERLAY", "SNAP_UI_TEST", "SNAP_LEGACY_SAVE_DIR"}) eb.erase(k);
  eb.set("SNAP_PROFILES_DIR", sc.root);
  eb.set("SNAP_PROFILE", sc.id);
  eb.set("SNAP_PROFILE_PICKER", "0");
  eb.set("SNAP_INPUT_SCRIPT", route_path);
  eb.set("SNAP_PERF", base);
  eb.set("SNAP_GPU_PASSES", base + "_passes.csv");
  eb.set("SNAP_BENCH_INFO", base + "_info.json");
  eb.set("SNAP_VSYNC", "off");
  eb.set("SNAP_WINDOW_FRAME_CAP", "0");
  eb.set("SNAP_SHINY", "0");
  eb.set("SNAP_VOL_MASTER", "0");
  eb.set("SNAP_SAVE_BACKUP_KEEP", "0");   // the game copies the whole save folder at launch (1.6 GB when not sparse, minutes); the scratch save needs no backup
  if (op.uncapped) eb.set("SNAP_FPS_UNCAPPED", "1"); else eb.erase("SNAP_FPS_UNCAPPED");
  if (launcher_mode) eb.set("SNAP_LAUNCHER_AUTOPLAY", "1");
  std::string stderr_path = base + "_stderr.log";
  if (launcher_mode) {   // launcher mode sends the game's stderr to its own log file
    std::string la = env("LOCALAPPDATA");
    if (la.empty()) { PWSTR p = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) { la = U8(p); CoTaskMemFree(p); } }
    stderr_path = la + "\\SnapRecomp\\logs\\stderr.log";
  }

  uint64_t from = 0, to = 0;
  double rail_end = 0;   // rail progress when the ride ended (the whole course is about 115)
  std::string log;
  auto run_once = [&]() -> bool {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const std::string& a : op.pass) cmd += L" \"" + W(a) + L"\"";
    for (const std::string& a : op.sets) cmd += L" --set \"" + W(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE herr = INVALID_HANDLE_VALUE;
    if (!launcher_mode) {
      herr = CreateFileW(P(stderr_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (herr != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = herr;
        si.hStdError = herr;
      }
    }
    PROCESS_INFORMATION pi{};
    // the game is started from the folder it is normally started from: the exe folder in launcher mode, else this process's own (a development run uses relative paths)
    std::wstring cwd = exe_dir.wstring();
    if (!launcher_mode) { wchar_t cb[MAX_PATH * 2] = {}; if (GetCurrentDirectoryW((DWORD)(sizeof cb / sizeof cb[0]), cb)) cwd = cb; }
    const std::wstring envb = eb.block();
    std::fprintf(stderr, "benchmark: starting the game (scratch profile %s, route %s)\n", sc.id.c_str(), route_path.c_str());
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, herr != INVALID_HANDLE_VALUE, CREATE_UNICODE_ENVIRONMENT, (LPVOID)envb.data(), cwd.c_str(), &si, &pi);
    if (herr != INVALID_HANDLE_VALUE) CloseHandle(herr);
    if (!started) { out.error = "The game could not be started (error " + std::to_string(GetLastError()) + ")."; return false; }
    const DWORD wr = WaitForSingleObject(pi.hProcess, 25 * 60 * 1000);
    DWORD code = 0;
    if (wr == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 124); code = 124; } else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    log.clear();
    read_text(P(stderr_path), log);
    from = 0; to = 0;
    const bool have = find_wait_present(log, "ride", from) && find_wait_present(log, "rail>=110", to) && to > from;
    if (!have) {
      out.error = "The ride did not finish (the game exited with code " + std::to_string(code) + ").";
      const std::string t = tail_lines(log, 3);
      if (!t.empty()) out.error += "\n\nLast lines of the game's log:\n" + t;
      return false;
    }
    { const std::string key = "rail progress "; const size_t q = log.find(key, log.rfind("wait 'rail>=110' met")); rail_end = q == std::string::npos ? 0.0 : std::atof(log.c_str() + q + key.size()); }
    return true;
  };
  if (!run_once()) return false;
  if (rail_end < 100.0) {   // the rail stalled (first run: shader compiling freezes the game clock): the cache is warm now, so go again once
    std::fprintf(stderr, "benchmark: the ride ended at rail %.1f of about 115 (stalls); running it once more\n", rail_end);
    if (!run_once()) return false;
    if (rail_end < 100.0) { out.error = "The ride stopped early twice (at rail " + fmt("%.0f", rail_end) + " of about 115). The game stalled; try again."; return false; }
  }
  double gpu_busy = 0;
  Result r;
  if (!read_frames(P(base + "_frames.csv"), from, to, r.frame_ms, gpu_busy) || r.frame_ms.size() < 300) {
    out.error = "The frame log has no frames for the ride (" + base + "_frames.csv).";
    return false;
  }
  r.gpu_busy_ms = gpu_busy;
  compute_stats(r);
  read_passes(P(base + "_passes.csv"), from, to, r.frame_ms.size(), r.passes);
  hle::Json info;
  {
    std::string t, e2;
    if (read_text(P(base + "_info.json"), t)) hle::json_parse(t, info, e2);
  }
  build_settings(r, read_cfg(P(sc.root) / P(sc.id) / "settings.cfg"), info, op.uncapped);
  r.date = now_text();
  r.stamp = now_stamp();
  const std::string dir = results_dir();
  find_previous(r, dir);
  r.json_path = dir + "\\" + r.stamp + ".json";
  r.png_path = dir + "\\" + r.stamp + ".png";
  if (!write_text(P(r.json_path), result_json(r))) std::fprintf(stderr, "benchmark: cannot write %s\n", r.json_path.c_str());
  std::fprintf(stderr, "benchmark: %zu frames, %.1f fps average, 1%% low %.1f, 0.1%% low %.1f -> %s\n", r.frames, r.avg_fps, r.p1_fps, r.p01_fps, r.json_path.c_str());
  out = r;
  if (env("SNAP_BENCH_KEEP").empty()) fs::remove_all(P(bench_dir), ec);   // the scratch save and the logs; SNAP_BENCH_KEEP=1 keeps them for a test
  return true;
}

}  // namespace

bool wanted(int argc, char** argv) {
  for (int i = 1; i < argc; i++)
    if (!std::strcmp(argv[i], "--benchmark") || !std::strcmp(argv[i], "--benchmark-uncapped") || !std::strcmp(argv[i], "--benchmark-capped") || !std::strcmp(argv[i], "--benchmark-view")) return true;
  return false;
}

int run_cli(const std::vector<std::string>& args) {
  Options op;
  std::string view;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--benchmark") continue;
    if (args[i] == "--benchmark-uncapped") op.uncapped = true;
    else if (args[i] == "--benchmark-capped") op.uncapped = false;
    else if (args[i] == "--benchmark-view" && i + 1 < args.size()) view = args[++i];
    else if (args[i] == "--set" && i + 1 < args.size()) op.sets.push_back(args[++i]);
    else op.pass.push_back(args[i]);
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Result r;
  if (!view.empty()) {
    hle::load_config("");
    if (!load_result(P(view), r)) { fail("Cannot read the benchmark file " + view, false); return 2; }
    r.stamp = U8(P(view).stem().wstring());
    find_previous(r, U8(P(view).parent_path().wstring()));
    show_results(r, env("SNAP_BENCH_SHOT"));
    return 0;
  }
  if (!execute(op, r)) { fail(r.error.empty() ? "The benchmark failed." : r.error, !env("SNAP_BENCH_AUTOCLOSE").empty()); return 2; }
  if (!show_results(r, r.png_path)) {
    std::fputs(summary_text(r).c_str(), stdout);
    fail("The results window could not be opened; the numbers were saved to " + r.json_path, false);
  }
  return 0;
}

bool run_from_launcher(bool uncapped) {
  Options op;
  op.uncapped = uncapped;
  Result r;
  if (!execute(op, r)) { fail(r.error.empty() ? "The benchmark failed." : r.error, false); return false; }
  if (!show_results(r, r.png_path)) fail("The results window could not be opened; the numbers were saved to " + r.json_path, false);
  return true;
}

// The benchmark's throw-away profile (packed unlock_all save plus a copy of the player's settings) for other tools that play scripted routes
// (host/shaderprep.cpp). `dir` is a scratch folder of the caller's; its "profiles" subfolder is replaced. hle::load_config must have run.
bool make_scratch_profile(const std::string& dir, std::string& profiles_root, std::string& profile_id, std::string& err) {
  Scratch sc;
  if (!make_scratch(dir, sc, err)) return false;
  profiles_root = sc.root;
  profile_id = sc.id;
  return true;
}

}  // namespace snap_benchmark

// ---------------------------------------------------------------------------------------------------------------- inside the game (the child)
// Called from the present hook (host/boot.cpp). With SNAP_BENCH_INFO set, writes what only the running game knows: the graphics card, the
// driver, the window and 3D sizes and the anti-aliasing text, once, shortly after start-up.
extern "C" void snap_benchmark_on_present(uint64_t n) {
  static const char* path = std::getenv("SNAP_BENCH_INFO");
  if (!path || !*path || n != 900) return;
  char gpu[256] = "", drv[96] = "", aa[160] = "";
  snap_perf_device_info(gpu, sizeof gpu, drv, sizeof drv);
  int rw = 0, rh = 0;
  double gms = 0, cms = 0, hz = 0;
  snap_perf_overlay_info(&gms, &cms, &rw, &rh, &hz, aa, sizeof aa);
  int ww = 0, wh = 0;
  if (HWND h = FindWindowW(nullptr, L"New Pokemon Snap (native recompilation)")) {
    RECT cr{};
    if (GetClientRect(h, &cr)) { ww = cr.right - cr.left; wh = cr.bottom - cr.top; }
  }
  using snap_benchmark::json_str;
  std::string t = "{\"gpu\": " + json_str(gpu) + ", \"driver\": " + json_str(drv) + ", \"window_w\": " + std::to_string(ww) + ", \"window_h\": " + std::to_string(wh) +
                  ", \"render_w\": " + std::to_string(rw) + ", \"render_h\": " + std::to_string(rh) + ", \"aa\": " + json_str(aa) + ", \"pace_hz\": " + std::to_string(hz) + "}\n";
  std::ofstream f(std::filesystem::path(snap_benchmark::W(path)), std::ios::binary | std::ios::trunc);
  f << t;
}

// F1 menu: start this exe again as the benchmark and leave (the running game is closed the way the profile restart does it). The new copy gets
// the same command line plus --benchmark, so a development run (--main ... run) hands the same arguments on to the game it plays.
extern "C" void snap_fs_drain_for_exit(int ms);   // runtime/hle/fs.cpp (always linked in the host build; a weak declaration here collides in lld-link)
extern "C" bool snap_benchmark_launch_from_game() {
  wchar_t exe[MAX_PATH * 2];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH * 2)) return false;
  std::wstring cmd = GetCommandLineW();
  cmd += L" --benchmark";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
  if (!ok) ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  snap_fs_drain_for_exit(3000);   // let a save commit / photo export in flight finish (bounded), like the window-close path
  TerminateProcess(GetCurrentProcess(), 0);   // the save layer commits atomically
  return true;
}
