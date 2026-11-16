// SPDX-License-Identifier: MIT
// Problem report: see problem_report.h. Project code, ours. Reads only small text files and the system's own facts; never touches game files, keys or saves.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "problem_report.h"
#include "install_check.h"
#include "../../runtime/hle/hle_config.h"
#include "../../runtime/hle/cheats.h"
#include "../../runtime/hle/settings.h"

namespace fs = std::filesystem;

namespace snap_report {
namespace {

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
std::string env(const char* name) {
  wchar_t buf[2048];
  const DWORD n = GetEnvironmentVariableW(W(name).c_str(), buf, 2048);
  return n && n < 2048 ? U8(std::wstring(buf, n)) : std::string();
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// ---- scrubbing -------------------------------------------------------------------------------------------------------
struct Identity {
  std::string profile;   // C:\Users\<name>
  std::vector<std::pair<std::string, std::string>> words;   // needle (lower case) -> replacement; matched as whole words, case-insensitive
};
const Identity& identity() {
  static const Identity id = [] {
    Identity r;
    r.profile = env("USERPROFILE");
    auto add = [&](std::string n, const char* rep) {
      if (n.size() < 3) return;   // too short to match as a word without damaging other text
      r.words.push_back({lower(n), rep});
    };
    wchar_t un[256]; DWORD n = 256;
    if (GetUserNameW(un, &n)) add(U8(un), "<user>");
    wchar_t cn[256]; n = 256;
    if (GetComputerNameW(cn, &n)) add(U8(cn), "<pc>");
    add(env("USERNAME"), "<user>");
    add(env("COMPUTERNAME"), "<pc>");
    add(env("USERDOMAIN"), "<pc>");
    if (HMODULE s = LoadLibraryW(L"secur32.dll")) {   // the account's display name (a full name) when Windows has one
      using Fn = BOOLEAN(WINAPI*)(int, LPWSTR, PULONG);
      if (auto f = (Fn)GetProcAddress(s, "GetUserNameExW")) {
        wchar_t dn[256]; ULONG dl = 256;
        if (f(3 /*NameDisplay*/, dn, &dl)) { std::string d = U8(dn); add(d, "<user>"); size_t sp = d.find(' '); if (sp != std::string::npos) { add(d.substr(0, sp), "<user>"); add(d.substr(sp + 1), "<user>"); } }
      }
      FreeLibrary(s);
    }
    return r;
  }();
  return id;
}

bool is_word_char(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// Replaces every case-insensitive occurrence of `needle_lower` in `s` that is not inside a longer word.
std::string replace_word(const std::string& s, const std::string& needle_lower, const std::string& rep, bool whole_word) {
  if (needle_lower.empty()) return s;
  const std::string ls = lower(s);
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    const size_t p = ls.find(needle_lower, i);
    if (p == std::string::npos) break;
    const size_t e = p + needle_lower.size();
    const bool ok = !whole_word || ((p == 0 || !is_word_char((unsigned char)s[p - 1])) && (e >= s.size() || !is_word_char((unsigned char)s[e])));
    out.append(s, i, p - i);
    if (ok) out += rep; else out.append(s, p, needle_lower.size());
    i = e;
  }
  out.append(s, i, std::string::npos);
  return out;
}

// "<drive>:\Users\<anything up to the next separator>" in the forms a log or JSON file writes it (\, \\, /) -> "<drive>:\Users\<user>" (same separators).
std::string scrub_user_paths(const std::string& s) {
  std::string out;
  size_t i = 0;
  const std::string ls = lower(s);
  while (i < s.size()) {
    // look for ":" + separator(s) + "users" + separator(s)
    size_t p = ls.find("users", i);
    if (p == std::string::npos) break;
    size_t b = p;                          // separators before "users"
    while (b > i && (s[b - 1] == '\\' || s[b - 1] == '/')) --b;
    const bool had_sep = b < p;
    size_t e = p + 5;                      // separators after
    size_t sep2 = e;
    while (sep2 < s.size() && (s[sep2] == '\\' || s[sep2] == '/')) ++sep2;
    const bool drive_before = b >= 2 && s[b - 1] == ':' && std::isalpha((unsigned char)s[b - 2]);
    const bool root_before = b >= 1 && (s[b - 1] == ':') ;   // "\\?\C:" style handled by the drive test
    (void)root_before;
    if (had_sep && sep2 > e && (drive_before || (b == 0 || s[b - 1] == '"' || s[b - 1] == '\'' || s[b - 1] == ' ' || s[b - 1] == '=' )) ) {
      size_t n = sep2;                     // the account folder name
      while (n < s.size() && s[n] != '\\' && s[n] != '/' && s[n] != '"' && s[n] != '\'' && s[n] != ' ' && s[n] != '\r' && s[n] != '\n' && s[n] != ':' && s[n] != '*' && s[n] != '?' && s[n] != '<' && s[n] != '>' && s[n] != '|') ++n;
      if (n > sep2) {
        out.append(s, i, sep2 - i);
        out += "<user>";
        i = n;
        continue;
      }
    }
    out.append(s, i, p + 5 - i);
    i = p + 5;
  }
  out.append(s, i, std::string::npos);
  return out;
}

}  // namespace

std::string scrub(const std::string& text) {
  const Identity& id = identity();
  std::string t = text;
  if (!id.profile.empty()) {   // the exact profile folder in its common spellings first (also catches a folder name that is not a plain word)
    std::string bs = id.profile, dbs, fwd = id.profile;
    for (char c : id.profile) { if (c == '\\') dbs += "\\\\"; else dbs += c; }
    for (char& c : fwd) if (c == '\\') c = '/';
    t = replace_word(t, lower(dbs), "%USERPROFILE%", false);
    t = replace_word(t, lower(bs), "%USERPROFILE%", false);
    t = replace_word(t, lower(fwd), "%USERPROFILE%", false);
  }
  t = scrub_user_paths(t);                               // any other C:\Users\<name>
  for (auto& w : id.words) t = replace_word(t, w.first, w.second, true);
  return t;
}

namespace {

// ---- zip (stored entries, UTF-8 names) -------------------------------------------------------------------------------
uint32_t crc32(const std::string& d) {
  static uint32_t t[256];
  static bool init = false;
  if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : d) c = t[(c ^ ch) & 255] ^ (c >> 8);
  return ~c;
}
void put16(std::string& o, uint32_t v) { o += (char)(v & 255); o += (char)((v >> 8) & 255); }
void put32(std::string& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }

struct Zip {
  std::string buf;
  struct E { std::string name; uint32_t crc, size, off; };
  std::vector<E> es;
  uint16_t dtime = 0, ddate = 0;
  Zip() {
    time_t now = time(nullptr);
    tm t{};
    localtime_s(&t, &now);
    dtime = (uint16_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
    ddate = (uint16_t)(((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);
  }
  void add(const std::string& name, const std::string& data) {
    E e{name, crc32(data), (uint32_t)data.size(), (uint32_t)buf.size()};
    put32(buf, 0x04034b50); put16(buf, 20); put16(buf, 0x0800); put16(buf, 0); put16(buf, dtime); put16(buf, ddate);
    put32(buf, e.crc); put32(buf, e.size); put32(buf, e.size); put16(buf, (uint32_t)name.size()); put16(buf, 0);
    buf += name;
    buf += data;
    es.push_back(e);
  }
  std::string finish() {
    const uint32_t cd = (uint32_t)buf.size();
    for (auto& e : es) {
      put32(buf, 0x02014b50); put16(buf, 20); put16(buf, 20); put16(buf, 0x0800); put16(buf, 0); put16(buf, dtime); put16(buf, ddate);
      put32(buf, e.crc); put32(buf, e.size); put32(buf, e.size); put16(buf, (uint32_t)e.name.size());
      put16(buf, 0); put16(buf, 0); put16(buf, 0); put16(buf, 0); put32(buf, 0); put32(buf, e.off);
      buf += e.name;
    }
    const uint32_t cdn = (uint32_t)buf.size() - cd;
    put32(buf, 0x06054b50); put16(buf, 0); put16(buf, 0); put16(buf, (uint32_t)es.size()); put16(buf, (uint32_t)es.size());
    put32(buf, cdn); put32(buf, cd); put16(buf, 0);
    return buf;
  }
};

// Reads a file that may be open for writing by this process (a live log). Last `max_bytes` only, cut to a line start.
bool read_tail(const fs::path& p, size_t max_bytes, std::string* out) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  const long long total = sz.QuadPart;
  long long start = total > (long long)max_bytes ? total - (long long)max_bytes : 0;
  LARGE_INTEGER pos; pos.QuadPart = start;
  SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
  std::string d((size_t)(total - start), '\0');
  DWORD got = 0, off = 0;
  while (off < d.size() && ReadFile(h, d.data() + off, (DWORD)std::min<size_t>(d.size() - off, 1u << 20), &got, nullptr) && got) off += got;
  CloseHandle(h);
  d.resize(off);
  if (start > 0) { const size_t nl = d.find('\n'); if (nl != std::string::npos) d.erase(0, nl + 1); d = "[report: earlier lines cut, only the last " + std::to_string(max_bytes >> 10) + " KB are kept]\n" + d; }
  *out = std::move(d);
  return true;
}

// ---- system info -----------------------------------------------------------------------------------------------------
std::string reg_str(HKEY root, const wchar_t* path, const wchar_t* name) {
  HKEY k;
  if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return "";
  wchar_t buf[512]; DWORD n = sizeof buf, type = 0;
  std::string r;
  if (RegQueryValueExW(k, name, nullptr, &type, (BYTE*)buf, &n) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) r = U8(buf);
  else if (type == REG_DWORD) { DWORD v = 0; n = 4; if (RegQueryValueExW(k, name, nullptr, nullptr, (BYTE*)&v, &n) == ERROR_SUCCESS) r = std::to_string(v); }
  RegCloseKey(k);
  return r;
}

struct GUID_ { unsigned long a; unsigned short b, c; unsigned char d[8]; };
std::string gpu_list() {
  std::ostringstream o;
  HMODULE dx = LoadLibraryW(L"dxgi.dll");
  if (!dx) return "dxgi.dll not available\n";
  using Create = HRESULT(WINAPI*)(REFIID, void**);
  auto create = (Create)GetProcAddress(dx, "CreateDXGIFactory1");
  static const GUID kFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
  static const GUID kDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
  // Minimal vtable access: IDXGIFactory1::EnumAdapters1 (slot 12) returns IDXGIAdapter1; IDXGIAdapter1::GetDesc1 (slot 10); CheckInterfaceSupport (slot 7).
  struct Unk { void** vt; };
  void* fac = nullptr;
  if (!create || FAILED(create(kFactory1, &fac)) || !fac) { FreeLibrary(dx); return "no DXGI factory\n"; }
  auto call = [](void* obj, int slot) { return (*(void***)obj)[slot]; };
  for (UINT i = 0; i < 8; ++i) {
    void* ad = nullptr;
    using EnumFn = HRESULT(WINAPI*)(void*, UINT, void**);
    if (FAILED(((EnumFn)call(fac, 12))(fac, i, &ad)) || !ad) break;
    struct Desc1 { WCHAR Description[128]; UINT VendorId, DeviceId, SubSysId, Revision; SIZE_T Dedicated, DedicatedSys, Shared; LUID Luid; UINT Flags; } d{};
    using DescFn = HRESULT(WINAPI*)(void*, Desc1*);
    if (SUCCEEDED(((DescFn)call(ad, 10))(ad, &d))) {
      LARGE_INTEGER umd{};
      using CheckFn = HRESULT(WINAPI*)(void*, REFGUID, LARGE_INTEGER*);
      const bool have = SUCCEEDED(((CheckFn)call(ad, 7))(ad, kDevice, &umd));
      char line[512];
      std::snprintf(line, sizeof line, "GPU %u: %s | vendor 0x%04X device 0x%04X | video memory %llu MB (shared %llu MB)%s", i, U8(d.Description).c_str(), d.VendorId, d.DeviceId,
                    (unsigned long long)(d.Dedicated >> 20), (unsigned long long)(d.Shared >> 20), (d.Flags & 2) ? " | software adapter" : "");
      o << line;
      if (have) {
        const unsigned a = (unsigned)(umd.HighPart >> 16), b = (unsigned)(umd.HighPart & 0xFFFF), c = (unsigned)(umd.LowPart >> 16), e = (unsigned)(umd.LowPart & 0xFFFF);
        char dv[160];
        std::snprintf(dv, sizeof dv, " | driver %u.%u.%u.%u", a, b, c, e);
        o << dv;
        if (d.VendorId == 0x10DE) { std::snprintf(dv, sizeof dv, " (NVIDIA %u.%02u)", (c % 10) * 100 + e / 100, e % 100); o << dv; }
      }
      o << "\n";
    }
    ((ULONG(WINAPI*)(void*))call(ad, 2))(ad);   // Release
  }
  ((ULONG(WINAPI*)(void*))call(fac, 2))(fac);
  FreeLibrary(dx);
  return o.str();
}

BOOL CALLBACK mon_cb(HMONITOR m, HDC, LPRECT, LPARAM lp) {
  auto* o = (std::ostringstream*)lp;
  MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
  if (!GetMonitorInfoW(m, &mi)) return TRUE;
  DEVMODEW dm{}; dm.dmSize = sizeof dm;
  EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
  *o << "Display " << U8(mi.szDevice) << ": " << (mi.rcMonitor.right - mi.rcMonitor.left) << "x" << (mi.rcMonitor.bottom - mi.rcMonitor.top) << " at " << dm.dmDisplayFrequency << " Hz"
     << ((mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "") << "\n";
  return TRUE;
}

std::string system_info(const char* who) {
  std::ostringstream o;
  o << "Snap Recomp problem report (" << who << ")\n";
  { time_t now = time(nullptr); tm t{}; localtime_s(&t, &now); char b[64]; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &t); o << "Made: " << b << " (local time)\n"; }
  // Windows
  using RtlFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof v;
  if (auto f = (RtlFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
  const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  o << "Windows: " << reg_str(HKEY_LOCAL_MACHINE, cv, L"ProductName") << " " << reg_str(HKEY_LOCAL_MACHINE, cv, L"DisplayVersion") << ", build " << v.dwMajorVersion << "." << v.dwMinorVersion << "." << v.dwBuildNumber
    << "." << reg_str(HKEY_LOCAL_MACHINE, cv, L"UBR") << "\n";
  o << "Windows display language: " << hle::windows_ui_language_tag() << " (game language chosen by Auto: " << hle::resolve_game_language() << ")\n";
  // CPU, memory
  SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
  o << "CPU: " << reg_str(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString") << " | " << si.dwNumberOfProcessors << " logical processors\n";
  MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
  if (GlobalMemoryStatusEx(&ms)) o << "Memory: " << (ms.ullTotalPhys >> 20) << " MB total, " << (ms.ullAvailPhys >> 20) << " MB free; page file " << (ms.ullTotalPageFile >> 20) << " MB\n";
  o << gpu_list();
  {   // display drivers as Windows lists them (the DXGI driver-version query fails on recent drivers)
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
      for (DWORD i = 0; i < 16; ++i) {
        wchar_t sub[64]; DWORD n = 64;
        if (RegEnumKeyExW(k, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (sub[0] < L'0' || sub[0] > L'9') continue;
        const std::wstring p = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\" + std::wstring(sub);
        const std::string desc = reg_str(HKEY_LOCAL_MACHINE, p.c_str(), L"DriverDesc"), ver = reg_str(HKEY_LOCAL_MACHINE, p.c_str(), L"DriverVersion"), dt = reg_str(HKEY_LOCAL_MACHINE, p.c_str(), L"DriverDate");
        if (!desc.empty()) o << "Display driver: " << desc << " | version " << ver << " | " << dt << "\n";
      }
      RegCloseKey(k);
    }
  }
  EnumDisplayMonitors(nullptr, nullptr, mon_cb, (LPARAM)&o);
  // the running program
  wchar_t mp[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mp, MAX_PATH);
  o << "Program: " << U8(fs::path(mp).filename().wstring()) << "\n";
  o << "Cheats active: " << (hle::cheats::active() ? "yes" : "no") << "\n";   // runtime/hle/cheats.h: the cheat switch is on and a cheat or an always-on section is enabled
  // settings that are in effect
  o << "\nSNAP_* environment variables set for this process:\n";
  LPWCH block = GetEnvironmentStringsW();
  std::vector<std::string> vars;
  for (LPWCH p = block; p && *p; p += wcslen(p) + 1) { std::string s = U8(p); if (s.rfind("SNAP_", 0) == 0) vars.push_back(s); }
  if (block) FreeEnvironmentStringsW(block);
  std::sort(vars.begin(), vars.end());
  for (auto& s : vars) o << "  " << s << "\n";
  if (vars.empty()) o << "  (none)\n";
  return o.str();
}

}  // namespace

bool make(const std::string& dir_in, const char* who, std::string* path, std::string* why) {
  auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
  // ---- where
  fs::path dir;
  if (!dir_in.empty()) dir = fs::path(W(dir_in));
  else if (!env("SNAP_REPORT_DIR").empty()) dir = fs::path(W(env("SNAP_REPORT_DIR")));
  else {
    PWSTR dp = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &dp)) && dp) { dir = dp; CoTaskMemFree(dp); }
    else return fail("could not find the Desktop folder");
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  // ---- gather
  Zip z;
  std::vector<std::string> listing;
  auto add_text = [&](const std::string& name, const std::string& text) { const std::string t = scrub(text); z.add(name, t); listing.push_back(name + " (" + std::to_string(t.size()) + " bytes)"); };
  auto add_file = [&](const std::string& name, const fs::path& p, size_t cap) {
    std::string d;
    std::error_code e;
    if (p.empty() || !fs::is_regular_file(p, e) || !read_tail(p, cap, &d)) return;
    add_text(name, d);
  };
  const std::string data_root = hle::default_data_root();
  const fs::path droot = fs::path(W(data_root));
  const fs::path cfgdir = !env("SNAP_CONFIG_DIR").empty() ? fs::path(W(env("SNAP_CONFIG_DIR"))) : droot;
  // logs
  fs::path logs;
  if (!env("LOCALAPPDATA").empty()) logs = fs::path(W(env("LOCALAPPDATA"))) / L"SnapRecomp" / L"logs";
  add_file("logs/stderr.log", logs / L"stderr.log", 3u << 20);
  const std::string bt = env("SNAP_BOOT_TRACE");
  add_file("logs/trace.log", bt.empty() ? logs / L"trace.log" : fs::path(W(bt)), 3u << 20);
  // the run before (kept at start by host/mods/crashlog.cpp) and the fatal-error note; any of the three may be absent
  add_file("logs/stderr.log.prev", logs / L"stderr.log.prev", 3u << 20);
  add_file("logs/trace.log.prev", logs / L"trace.log.prev", 3u << 20);
  add_file("logs/_crash.txt", logs / L"_crash.txt", 1u << 20);
  // settings
  const std::string live = hle::settings::file_path();
  add_file("settings/settings.cfg", !live.empty() ? fs::path(W(live)) : droot / L"settings.cfg", 1u << 20);
  { int n = 0; fs::path pr = droot / L"profiles";
    if (fs::is_directory(pr, ec)) for (auto& e : fs::directory_iterator(pr, ec)) {
      if (n >= 8) break;
      const fs::path sc = e.path() / L"settings.cfg";
      if (fs::is_regular_file(sc, ec) && (live.empty() || fs::path(W(live)) != sc)) add_file("settings/profile" + std::to_string(++n) + "_settings.cfg", sc, 1u << 20);
    } }
  add_file("settings/input.json", fs::path(W(hle::default_input_path())), 1u << 20);
  add_file("settings/audio.json", cfgdir / L"audio.json", 1u << 18);
  add_file("settings/window.json", cfgdir / L"window.json", 1u << 18);
  add_file("settings/hle.json", droot / L"config" / L"hle.json", 1u << 18);
  { wchar_t mp[MAX_PATH] = {}; GetModuleFileNameW(nullptr, mp, MAX_PATH); add_file("settings/config.json", fs::path(mp).parent_path() / L"config.json", 1u << 18); }
  add_file("settings/mods.json", droot / L"mods" / L"mods.json", 1u << 18);
  { std::string ic(16384, '\0'); ic.resize((size_t)snap_install_check_text(ic.data(), (int)ic.size())); if (!ic.empty()) add_text("install_check.txt", ic); }   // host/mods/install_check.cpp: words only, no hashes
  // shader cache: stats, a size listing, and the pipeline key list when it is small
  {
    std::string sd = env("SNAP_SHADERCACHE_DIR");
    fs::path scd = !sd.empty() ? fs::path(W(sd)) : (!env("SNAP_CONFIG_DIR").empty() ? fs::path(W(env("SNAP_CONFIG_DIR"))) / L"shadercache" : droot / L"shadercache");
    if (fs::is_directory(scd, ec)) {
      add_file("shadercache/stats.txt", scd / L"stats.txt", 1u << 16);
      add_file("shadercache/mode.txt", scd / L"mode.txt", 1u << 16);
      std::ostringstream ls;
      for (auto& e : fs::directory_iterator(scd, ec)) { std::error_code e2; if (e.is_regular_file(e2)) ls << U8(e.path().filename().wstring()) << "\t" << e.file_size(e2) << " bytes\n"; }
      add_text("shadercache/listing.txt", ls.str());
      const fs::path kp = scd / L"pipeline_keys.bin";
      if (fs::is_regular_file(kp, ec) && fs::file_size(kp, ec) <= (4u << 20)) {
        std::ifstream f(kp, std::ios::binary);
        std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (scrub(d) == d) { z.add("shadercache/pipeline_keys.bin", d); listing.push_back("shadercache/pipeline_keys.bin (" + std::to_string(d.size()) + " bytes)"); }   // binary: kept only when nothing in it needed scrubbing
      }
    }
  }
  add_text("system.txt", system_info(who ? who : "game"));
  std::string readme =
      "Snap Recomp problem report\r\n\r\n"
      "This zip was made on your PC by the button in the launcher or the F1 menu. Nothing was uploaded; attach it to your bug report yourself.\r\n\r\n"
      "What is inside: the program's two log files, your settings files, the shader cache statistics and pipeline key list, and system.txt (Windows, CPU, memory, graphics card and driver, displays).\r\n"
      "What is not inside: game files, keys, save files, photos. Your Windows user name, the computer name and user folder paths were replaced with <user>, <pc> and %USERPROFILE%.\r\n"
      "Open the files in any text editor to check them before you send the zip.\r\n\r\nFiles:\r\n";
  for (auto& l : listing) readme += "  " + l + "\r\n";
  z.add("README.txt", readme);
  const std::string zip = z.finish();
  // ---- write
  char stamp[40];
  { time_t now = time(nullptr); tm t{}; localtime_s(&t, &now); std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &t); }
  const fs::path out = dir / (std::wstring(L"SnapRecomp-problem-report-") + W(stamp) + L".zip");
  {
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) return fail("could not write " + U8(out.filename().wstring()) + " in " + U8(dir.wstring()));
    f.write(zip.data(), (std::streamsize)zip.size());
    if (!f) return fail("could not finish writing the zip (disk full?)");
  }
  if (path) *path = U8(out.wstring());
  return true;
}

}  // namespace snap_report

#include <thread>
// Test switch: SNAP_REPORT_AT=<seconds> makes one report that many seconds after start (into SNAP_REPORT_DIR) and logs its path, so a hidden run can check the zip.
static const int g_report_at = [] {
  const char* e = std::getenv("SNAP_REPORT_AT");
  if (e && *e) std::thread([s = std::atoi(e)] {
    Sleep((DWORD)s * 1000);
    std::string p, w;
    if (snap_report::make("", "test", &p, &w)) std::fprintf(stderr, "problem report: %s\n", p.c_str());
    else std::fprintf(stderr, "problem report failed: %s\n", w.c_str());
  }).detach();
  return 0;
}();

extern "C" int snap_problem_report(const char* who, char* path_out, int path_cap, char* why_out, int why_cap) {
  std::string path, why;
  const bool ok = snap_report::make("", who, &path, &why);
  const std::string& s = ok ? path : why;
  char* dst = ok ? path_out : why_out;
  const int cap = ok ? path_cap : why_cap;
  if (dst && cap > 0) { std::strncpy(dst, s.c_str(), (size_t)cap - 1); dst[cap - 1] = 0; }
  return ok ? 1 : 0;
}
