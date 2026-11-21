// SPDX-License-Identifier: MIT
// Shader preparation: see shaderprep.h. A first-run sweep that plays one scripted ride per course in a hidden copy of the game so the pipeline
// service records and compiles every pipeline into this PC's shader cache. Runs on the benchmark's scratch save profile, never on a real save.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "shaderprep.h"

#include <windows.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "benchmark.h"
#include "hle/hle_config.h"
#include "hle/json_lite.h"
#include "hle/profiles.h"
#include "prep_routes_data.h"

namespace fs = std::filesystem;

namespace snap_shaderprep {
namespace {

std::wstring W(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
fs::path P(const std::string& s) { return fs::path(W(s)); }
std::string env(const char* k) { const char* v = std::getenv(k); return v ? v : ""; }
bool read_text(const fs::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}
bool write_text(const fs::path& p, const std::string& t) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << t;
  return f.good();
}

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

constexpr int kCourses = (int)(sizeof kPrepCourses / sizeof kPrepCourses[0]);

std::string cache_dir_now() {
  const std::string e = env("SNAP_SHADERCACHE_DIR");
  return !e.empty() ? e : hle::config().data_root + "\\shadercache";
}

struct Result { std::string key; unsigned long exit_code = 0; double seconds = 0; long new_pipelines = 0; };

unsigned known_pipelines(const std::string& dir) {
  std::string t, e;
  hle::Json j;
  if (!read_text(P(dir) / L"status.json", t) || !hle::json_parse(t, j, e)) return 0;
  const hle::Json* k = j.get("known");
  return k ? (unsigned)k->num() : 0;
}

// Which settings.cfg rows change the pipeline keys or switch whole passes (and so their pipelines) on or off. A cache prepared with other values of
// these leaves the new ones to compile on first use. Everything else (sound, window, controls, speeds, strengths of live effects) is left out.
bool pipeline_row(const std::string& k) {
  static const char* const skip[] = {"gfx_ui_font", "gfx_hdr_peak", "gfx_hdr_paper", "gfx_sharpen", "gfx_fov", "gfx_draw_dist", "gfx_lod_dist", "gfx_lod_bias",
                                     "gfx_lod_force0", "gfx_full_rate_anim", "gfx_aniso", "gfx_render_scale", "gfx_upscale_quality", "gfx_smaa_quality"};
  for (const char* x : skip) if (k == x) return false;
  static const char* const pre[] = {"gfx_", "rt_", "atmos_", "water_", "ssao_", "foliage_", "leaf_", "wind_"};
  for (const char* x : pre) if (k.rfind(x, 0) == 0) return true;
  return k == "soft_particles" || k == "contact_shadows" || k == "look" || k == "frame_generation";
}

std::string g_settings;   // settings_hash() of the player's settings, taken when this preparation started (written to prepare.json)
std::string g_driver;     // driver_stamp(), the same moment

void write_progress(const std::string& dir, const char* state, int index, int total, const std::string& course, int ok, int failed, long long started,
                    const std::vector<Result>& res) {
  const long long now = (long long)std::time(nullptr);
  std::string t = "{\"format\": 1, \"state\": \"" + std::string(state) + "\", \"index\": " + std::to_string(index) + ", \"total\": " + std::to_string(total) +
                  ", \"course\": \"" + course + "\", \"ok\": " + std::to_string(ok) + ", \"failed\": " + std::to_string(failed) +
                  ", \"started\": " + std::to_string(started) + ", \"updated\": " + std::to_string(now) + ", \"pipelines\": " + std::to_string(known_pipelines(dir)) +
                  ", \"seconds\": " + std::to_string(now - started) + ", \"settings\": \"" + g_settings + "\", \"driver\": \"" + g_driver + "\", \"results\": [";
  for (size_t i = 0; i < res.size(); i++) {
    char b[200];
    std::snprintf(b, sizeof b, "%s{\"key\": \"%s\", \"exit\": %lu, \"seconds\": %.0f, \"new_pipelines\": %ld}", i ? ", " : "", res[i].key.c_str(), res[i].exit_code, res[i].seconds, res[i].new_pipelines);
    t += b;
  }
  t += "]}\n";
  const fs::path p = P(dir) / L"prepare.json";
  write_text(P(dir) / L"prepare.json.tmp", t);
  std::error_code ec;
  fs::rename(P(dir) / L"prepare.json.tmp", p, ec);
}

std::string exe_path() {
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  const int n = WideCharToMultiByte(CP_UTF8, 0, mp, -1, nullptr, 0, nullptr, nullptr);
  std::string o((size_t)std::max(n - 1, 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, mp, -1, o.data(), n, nullptr, nullptr);
  return o;
}

}  // namespace

bool wanted(int argc, char** argv) {
  for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--prepare-shaders")) return true;
  return false;
}

std::string settings_hash(const std::string& data_root) {
  // the row source is the one the scratch profile copies (host/benchmark.cpp make_scratch): SNAP_PROFILES_DIR or <data root>\profiles, the profile the game starts with
  const std::string root = !env("SNAP_PROFILES_DIR").empty() ? env("SNAP_PROFILES_DIR") : data_root + "\\profiles";
  std::map<std::string, std::string> rows;
  if (env("SNAP_SAVE_DIR").empty() || !env("SNAP_PROFILES_DIR").empty()) {
    hle::profiles::Store st(root);
    st.load();
    const std::string want = env("SNAP_PROFILE");
    const hle::profiles::Profile* pf = want.empty() ? st.find(st.last_used()) : st.find(want);
    if (!pf && !st.list().empty() && want.empty()) pf = &st.list().front();
    std::string t;
    if (pf && read_text(P(root) / P(pf->id) / L"settings.cfg", t)) {
      std::istringstream is(t);
      std::string line;
      while (std::getline(is, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        if (pipeline_row(k)) rows[k] = line.substr(eq + 1);
      }
    }
  }
  unsigned long long h = 1469598103934665603ull;   // FNV-1a over the sorted "key=value" rows
  auto mix = [&](const std::string& x) { for (unsigned char c : x) { h ^= c; h *= 1099511628211ull; } h ^= 0x0a; h *= 1099511628211ull; };
  for (const auto& kv : rows) mix(kv.first + "=" + kv.second);
  char b[24];
  std::snprintf(b, sizeof b, "%016llx", h);
  return b;
}

std::string driver_stamp() {
  static const std::string s = []() -> std::string {
    std::string out;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)) || !f) return out;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i) {
      DXGI_ADAPTER_DESC1 d{};
      a->GetDesc1(&d);
      if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a->Release(); continue; }
      LARGE_INTEGER umd{};
      a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);   // the user-mode driver version; stays 0 when the system will not say
      char b[96];
      std::snprintf(b, sizeof b, "%04x:%04x:%llx", d.VendorId, d.DeviceId, (unsigned long long)umd.QuadPart);
      out = b;
      a->Release();
      break;
    }
    f->Release();
    return out;
  }();
  return s;
}

bool read_progress(const std::string& dir, Progress& p) {
  std::string t, e;
  hle::Json j;
  if (!read_text(P(dir) / L"prepare.json", t) || !hle::json_parse(t, j, e) || j.type != hle::Json::Obj) return false;
  const std::string st = j.get("state") ? j.get("state")->str() : "";
  p.state = st == "running" ? 1 : st == "done" ? 2 : st == "stopped" || st == "partial" ? 3 : st == "failed" ? 4 : 0;
  p.index = j.get("index") ? (int)j.get("index")->num() : 0;
  p.total = j.get("total") ? (int)j.get("total")->num() : 0;
  p.ok = j.get("ok") ? (int)j.get("ok")->num() : 0;
  p.failed = j.get("failed") ? (int)j.get("failed")->num() : 0;
  p.course = j.get("course") ? j.get("course")->str() : "";
  p.pipelines = j.get("pipelines") ? (unsigned)j.get("pipelines")->num() : 0;
  p.updated = j.get("updated") ? (long long)j.get("updated")->num() : 0;
  p.settings = j.get("settings") ? j.get("settings")->str() : "";
  p.driver = j.get("driver") ? j.get("driver")->str() : "";
  if (p.state == 1 && (long long)std::time(nullptr) - p.updated > 20 * 60) p.state = 3;   // the process died without writing a last state
  hle::Json s;
  if (read_text(P(dir) / L"status.json", t) && hle::json_parse(t, s, e)) {
    if (s.get("ready")) p.ready = (unsigned)s.get("ready")->num();
    if (s.get("known")) p.pipelines = (unsigned)s.get("known")->num();
  }
  return true;
}

void request_stop(const std::string& dir) { write_text(P(dir) / L"prepare.stop", "stop\n"); }

bool start_detached(std::string& err) {
  const std::wstring exe = W(exe_path());
  std::wstring cmd = L"\"" + exe + L"\" --prepare-shaders --force";
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const DWORD fl = DETACHED_PROCESS | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
  BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, fl | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
  if (!ok) ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, fl, nullptr, nullptr, &si, &pi);
  if (!ok) { err = "could not start the shader preparation (error " + std::to_string(GetLastError()) + ")"; return false; }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

int run_cli(const std::vector<std::string>& args) {
  std::vector<std::string> pass, only, sets;   // sets: "--set key=value" values for the child (kept out of pass so launcher mode still counts)
  bool force = false, visible = false, list = false, stop = false;
  for (size_t i = 0; i < args.size(); i++) {
    const std::string& a = args[i];
    if (a == "--prepare-shaders") continue;
    if (a == "--force") force = true;
    else if (a == "--visible") visible = true;
    else if (a == "--list") list = true;
    else if (a == "--stop") stop = true;
    else if (a == "--courses" && i + 1 < args.size()) {
      std::stringstream ss(args[++i]);
      std::string k;
      while (std::getline(ss, k, ',')) if (!k.empty()) only.push_back(k);
    } else if (a == "--set" && i + 1 < args.size()) sets.push_back(args[++i]);
    else pass.push_back(a);
  }
  if (list) {
    for (int i = 0; i < kCourses; i++) std::printf("%s\t%s\n", kPrepCourses[i].key, kPrepCourses[i].name);
    return 0;
  }
  const fs::path exe = P(exe_path());
  std::error_code ec;
  const bool launcher_mode = fs::exists(exe.parent_path() / L"config.json", ec) && pass.empty();
  hle::load_config(launcher_mode ? exe_path().substr(0, exe_path().find_last_of('\\')) + "\\config.json" : std::string());
  const std::string dir = cache_dir_now();
  fs::create_directories(P(dir), ec);
  if (stop) { request_stop(dir); std::printf("shaderprep: stop requested\n"); return 0; }

  HANDLE mx = CreateMutexW(nullptr, FALSE, L"Local\\SnapRecompShaderPrep");
  if (mx && GetLastError() == ERROR_ALREADY_EXISTS) { std::fprintf(stderr, "shaderprep: another preparation is already running\n"); return 3; }
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);

  Progress was;
  g_settings = settings_hash(hle::config().data_root);
  g_driver = driver_stamp();
  if (!force && read_progress(dir, was) && was.state == 2 && only.empty() && (was.settings.empty() || was.settings == g_settings) && (was.driver.empty() || g_driver.empty() || was.driver == g_driver)) {
    std::printf("shaderprep: the shader cache is already prepared (%u pipelines); use --force to run it again\n", was.pipelines);
    return 0;
  }
  std::vector<int> pick;
  for (int i = 0; i < kCourses; i++) {
    if (only.empty()) { pick.push_back(i); continue; }
    for (const std::string& k : only) if (k == kPrepCourses[i].key) pick.push_back(i);
  }
  if (pick.empty()) { std::fprintf(stderr, "shaderprep: no such course (see --prepare-shaders --list)\n"); return 2; }

  const std::string work = hle::config().data_root + "\\shaderprep\\" + std::to_string(GetCurrentProcessId());
  fs::create_directories(P(work), ec);
  std::string profiles, profile, err;
  if (!snap_benchmark::make_scratch_profile(work, profiles, profile, err)) { std::fprintf(stderr, "shaderprep: %s\n", err.c_str()); return 2; }
  fs::create_directories(P(work) / L"no_mods", ec);
  fs::remove(P(dir) / L"prepare.stop", ec);

  const std::string speed = !env("SNAP_PREP_SPEED").empty() ? env("SNAP_PREP_SPEED") : "3";
  const int limit_s = !env("SNAP_PREP_TIMEOUT").empty() ? std::atoi(env("SNAP_PREP_TIMEOUT").c_str()) : 900;
  const long long started = (long long)std::time(nullptr);
  std::vector<Result> res;
  int ok = 0, failed = 0;
  bool stopped = false;
  HDESK desk = nullptr;
  const std::wstring desk_name = L"SnapRecompPrep" + std::to_wstring(GetCurrentProcessId());
  if (!visible) desk = CreateDesktopW(desk_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);   // a never-shown desktop: nothing reaches the player's screen or focus
  if (!visible && !desk) std::fprintf(stderr, "shaderprep: no hidden desktop (error %lu); the game windows will show\n", GetLastError());
  HANDLE job = CreateJobObjectW(nullptr, nullptr);   // the game dies with this process
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
  }

  for (size_t n = 0; n < pick.size() && !stopped; n++) {
    const PrepCourse& c = kPrepCourses[pick[n]];
    write_progress(dir, "running", (int)n + 1, (int)pick.size(), c.name, ok, failed, started, res);
    std::printf("shaderprep: [%zu/%zu] %s\n", n + 1, pick.size(), c.name);
    std::fflush(stdout);
    const std::string route = work + "\\route_" + c.key + ".txt";
    write_text(P(route), c.route);
    EnvBlock eb;
    for (const char* k : {"SNAP_SAVE_DIR", "SNAP_SAVE_ROOT", "SNAP_INPUT_SCRIPT", "SNAP_NEO_SPEED", "SNAP_FILM", "SNAP_PHOTO_EXPORT", "SNAP_FPS_OVERLAY", "SNAP_UI_TEST",
                          "SNAP_LEGACY_SAVE_DIR", "SNAP_FPS", "SNAP_FPS_UNCAPPED", "SNAP_PERF", "SNAP_PERF_SHARED", "SNAP_SHADER_PRECOMPILE"})
      eb.erase(k);
    eb.set("SNAP_PROFILES_DIR", profiles);
    eb.set("SNAP_PROFILE", profile);
    eb.set("SNAP_PROFILE_PICKER", "0");
    eb.set("SNAP_INPUT_SCRIPT", route);
    eb.set("SNAP_INPUT_BACKGROUND", "1");
    eb.set("SNAP_NEO_SPEED", speed);
    eb.set("SNAP_AUDIO_BACKEND", "null");
    eb.set("SNAP_VOL_MASTER", "0");
    eb.set("SNAP_VSYNC", "off");
    eb.set("SNAP_WINDOW_FRAME_CAP", "0");
    eb.set("SNAP_WINDOW_PAUSE", "0");
    eb.set("SNAP_SHINY", "0");
    eb.set("SNAP_MODS_DIR", work + "\\no_mods");
    eb.set("SNAP_SHADERCACHE_DIR", dir);
    eb.set("SNAP_SHADER_PRECOMPILE", "playing");   // never hold the game on the progress screen
    eb.set("SNAP_PIPELINE_SEED", "0");
    if (env("SNAP_WINDOW_W").empty()) { eb.set("SNAP_WINDOW_W", "1920"); eb.set("SNAP_WINDOW_H", "1080"); }
    if (launcher_mode) eb.set("SNAP_LAUNCHER_AUTOPLAY", "1");

    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    for (const std::string& a : pass) cmd += L" \"" + W(a) + L"\"";
    for (const std::string& a : sets) cmd += L" --set \"" + W(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    std::wstring dname = desk_name;
    if (desk) si.lpDesktop = dname.data();
    HANDLE herr = INVALID_HANDLE_VALUE;
    if (!launcher_mode) {   // a development run: the game writes to stderr, keep it per course
      SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
      herr = CreateFileW(P(work + "\\prep_" + c.key + ".log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (herr != INVALID_HANDLE_VALUE) { si.dwFlags |= STARTF_USESTDHANDLES; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = herr; si.hStdError = herr; }
    }
    std::wstring cwd = exe.parent_path().wstring();
    if (!launcher_mode) { wchar_t cb[MAX_PATH * 2] = {}; if (GetCurrentDirectoryW((DWORD)(sizeof cb / sizeof cb[0]), cb)) cwd = cb; }
    const std::wstring envb = eb.block();
    const unsigned before = known_pipelines(dir);
    const auto t0 = std::chrono::steady_clock::now();
    PROCESS_INFORMATION pi{};
    const BOOL started_ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, herr != INVALID_HANDLE_VALUE,
                                           CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, (LPVOID)envb.data(), cwd.c_str(), &si, &pi);
    if (herr != INVALID_HANDLE_VALUE) CloseHandle(herr);
    Result r;
    r.key = c.key;
    if (!started_ok) {
      r.exit_code = GetLastError();
      std::fprintf(stderr, "shaderprep: could not start the game (error %lu)\n", r.exit_code);
      failed++;
      res.push_back(r);
      continue;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    for (;;) {
      const DWORD w = WaitForSingleObject(pi.hProcess, 500);
      if (w == WAIT_OBJECT_0) { GetExitCodeProcess(pi.hProcess, (DWORD*)&r.exit_code); break; }
      const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::error_code e2;
      if (fs::exists(P(dir) / L"prepare.stop", e2)) { TerminateProcess(pi.hProcess, 125); r.exit_code = 125; stopped = true; break; }
      if (el > limit_s) { TerminateProcess(pi.hProcess, 124); r.exit_code = 124; break; }
    }
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.new_pipelines = (long)known_pipelines(dir) - (long)before;
    if (r.exit_code == 0) ok++; else if (!stopped) failed++;
    std::printf("shaderprep:   exit %lu after %.0f s, %ld new pipelines (%u known)\n", r.exit_code, r.seconds, r.new_pipelines, known_pipelines(dir));
    std::fflush(stdout);
    res.push_back(r);
  }
  const bool clean = !stopped && failed == 0 && ok == (int)pick.size();
  const bool all = clean && only.empty();   // "done" only when every course ran; a --courses subset is "partial"
  write_progress(dir, stopped ? "stopped" : all ? "done" : clean ? "partial" : "failed", (int)pick.size(), (int)pick.size(), "", ok, failed, started, res);
  fs::remove(P(dir) / L"prepare.stop", ec);
  // every pipeline is now recorded and in the driver cache, so replaying them at startup takes seconds: make that the default unless the player already chose
  if (all && !fs::exists(P(dir) / L"mode.txt", ec)) write_text(P(dir) / L"mode.txt", "now\n");
  if (desk) CloseDesktop(desk);
  if (job) CloseHandle(job);
  if (env("SNAP_PREP_KEEP").empty()) fs::remove_all(P(work), ec);   // the scratch save and the per-course logs
  std::printf("shaderprep: %s, %d of %zu courses ran, %u pipelines known\n", stopped ? "stopped" : clean ? "done" : "finished with problems", ok, pick.size(), known_pipelines(dir));
  return stopped ? 1 : failed ? 2 : 0;
}

}  // namespace snap_shaderprep
