// SPDX-License-Identifier: MIT
// Log hygiene and crash reports (project code, ours, no game data; docs/crash-logs.md).
//   snap_crashlog_rotate(logs_dir)   at start: last run's stderr.log and trace.log become stderr.log.prev and trace.log.prev (one generation)
//   snap_crashlog_install()          at start: abort() and std::terminate also write a crash report (the SEH filter in host/boot.cpp calls
//                                    snap_crashlog_seh, the GPU-loss and loader exits call snap_crashlog_fatal)
//   _crash.txt                       written on a fatal error next to the log (SNAP_CRASH_DIR overrides): what happened and where, the
//                                    faulting address, build, OS, GPU and driver, quirks in effect, and the last 60 log lines
// Nothing here allocates on the crash path (static buffers, Win32 file calls), because the crashing thread may hold the heap lock.
// The Options > Problem report zip (host/mods/problem_report.cpp) picks the three files up from the same folder.
#include <windows.h>

#include <io.h>
#include <process.h>
#include <signal.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>

extern "C" const char* snap_quirks_gpu_line();       // runtime/nvn/nvn_quirks.cpp
extern "C" const char* snap_quirks_applied_line();
extern "C" void snap_audio_log_checks(void);          // runtime/audio/audio_out.cpp: the "[audio] checks:" counter line
extern "C" void snap_fs_drain_for_exit(int ms);   // runtime/hle/fs.cpp: waits for a save commit in flight, then removes the session marker
extern "C" int snap_route_cond(const char* c, char* why, int n);   // runtime/hle/input_core.cpp: a route "wait" condition (SNAP_CRASH_TEST_AT=ride:<n>)
extern "C" __attribute__((weak)) void snap_fs_log_io_failures(void);   // runtime/hle/fs_core.cpp: one line when a save flush failed

namespace {

constexpr int kTailLines = 60;
constexpr DWORD kTailBytes = 24 * 1024;
char g_dirOverride[MAX_PATH * 2];            // set by snap_crashlog_set_dir (the launcher's logs folder)
ULONGLONG g_startTick = 0;
volatile LONG g_owner = 0;                   // thread id that is writing (or wrote) the report; other threads' fatal paths return at once
char g_hashCache[24];
LPTOP_LEVEL_EXCEPTION_FILTER g_myFilter = nullptr;   // the host's crash filter (host/boot.cpp), which the vectored handler below calls

void append(char* out, size_t cap, size_t& at, const char* fmt, ...) {
  if (at + 1 >= cap) return;
  va_list ap; va_start(ap, fmt);
  const int n = _vsnprintf_s(out + at, cap - at, _TRUNCATE, fmt, ap);
  va_end(ap);
  at += n < 0 ? strlen(out + at) : (size_t)n;
}

// where the module containing `addr` is and the offset in it ("snap_boot.exe+0x1a2b3c", "nvoglv64.dll+0x4b1c2")
void module_of(uint64_t addr, char* out, size_t cap) {
  HMODULE m = nullptr;
  if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)addr, &m) && m) {
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    _snprintf_s(out, cap, _TRUNCATE, "%s+0x%llx", base ? base + 1 : path, (unsigned long long)(addr - (uint64_t)(uintptr_t)m));
  } else {
    _snprintf_s(out, cap, _TRUNCATE, "0x%llx (no module)", (unsigned long long)addr);
  }
}

// FNV-1a 64 of the running exe (read in 1 MiB pieces). Only on the final write; ~0.1 s for a 70 MB exe. Cached.
const char* exe_hash() {
  if (g_hashCache[0]) return g_hashCache;
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) { strcpy_s(g_hashCache, "unreadable"); return g_hashCache; }
  LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
  if (sz.QuadPart > (512ll << 20)) { CloseHandle(f); strcpy_s(g_hashCache, "too large"); return g_hashCache; }
  static unsigned char buf[1 << 20];
  uint64_t h = 1469598103934665603ull;
  DWORD n = 0;
  while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) for (DWORD i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
  CloseHandle(f);
  _snprintf_s(g_hashCache, sizeof g_hashCache, _TRUNCATE, "%016llx", (unsigned long long)h);
  return g_hashCache;
}

// the file the CRT's stderr writes to (the player's logs\stderr.log, or a test run's boot_stderr.log); false for a console or a pipe
bool stderr_path(wchar_t* out, DWORD cap) {
  const intptr_t h = _get_osfhandle(2);
  if (h == -1) return false;
  if (GetFileType((HANDLE)h) != FILE_TYPE_DISK) return false;
  const DWORD n = GetFinalPathNameByHandleW((HANDLE)h, out, cap, FILE_NAME_NORMALIZED);
  if (!n || n >= cap) return false;
  if (!wcsncmp(out, L"\\\\?\\", 4)) memmove(out, out + 4, (wcslen(out) - 3) * sizeof(wchar_t));
  return true;
}

// the report's folder: SNAP_CRASH_DIR, the launcher's logs folder, the folder of the stderr file, else %LOCALAPPDATA%\SnapRecomp\logs
bool crash_path(wchar_t* out, size_t cap) {
  wchar_t dir[MAX_PATH * 2] = {};
  if (GetEnvironmentVariableW(L"SNAP_CRASH_DIR", dir, MAX_PATH * 2) && dir[0]) {}
  else if (g_dirOverride[0]) MultiByteToWideChar(CP_UTF8, 0, g_dirOverride, -1, dir, MAX_PATH * 2);
  else {
    wchar_t sp[MAX_PATH * 2] = {};
    if (stderr_path(sp, MAX_PATH * 2)) { wchar_t* s = wcsrchr(sp, L'\\'); if (s) { *s = 0; wcscpy_s(dir, sp); } }
    if (!dir[0]) {
      wchar_t la[MAX_PATH] = {};
      if (!GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH) || !la[0]) return false;
      _snwprintf_s(dir, MAX_PATH * 2, _TRUNCATE, L"%s\\SnapRecomp\\logs", la);
      CreateDirectoryW(la, nullptr);
      wchar_t base[MAX_PATH * 2]; _snwprintf_s(base, MAX_PATH * 2, _TRUNCATE, L"%s\\SnapRecomp", la);
      CreateDirectoryW(base, nullptr);
    }
  }
  CreateDirectoryW(dir, nullptr);
  _snwprintf_s(out, cap, _TRUNCATE, L"%s\\_crash.txt", dir);
  return true;
}

// the last kTailLines lines of the log file
void log_tail(char* out, size_t cap, size_t& at) {
  wchar_t sp[MAX_PATH * 2] = {};
  if (!stderr_path(sp, MAX_PATH * 2)) { append(out, cap, at, "(stderr is not a file in this run: no log tail)\r\n"); return; }
  HANDLE f = CreateFileW(sp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) { append(out, cap, at, "(could not open the log file)\r\n"); return; }
  static char buf[kTailBytes + 1];
  LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
  LARGE_INTEGER pos{}; pos.QuadPart = sz.QuadPart > (LONGLONG)kTailBytes ? sz.QuadPart - kTailBytes : 0;
  SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
  DWORD n = 0;
  if (!ReadFile(f, buf, kTailBytes, &n, nullptr)) n = 0;
  CloseHandle(f);
  buf[n] = 0;
  char* start = buf;
  if (pos.QuadPart) { char* nl = strchr(buf, '\n'); start = nl ? nl + 1 : buf; }   // the first line is cut: drop it
  int lines = 0;
  for (char* p = buf + n; p > start; p--) if (p[-1] == '\n' && p != buf + n && ++lines >= kTailLines) { start = p; break; }
  for (char* p = start; *p;) {   // one line at a time, CRLF
    char* e = strchr(p, '\n');
    const size_t len = e ? (size_t)(e - p) : strlen(p);
    size_t l2 = len; while (l2 && (p[l2 - 1] == '\r')) l2--;
    append(out, cap, at, "%.*s\r\n", (int)l2, p);
    if (!e) break;
    p = e + 1;
  }
}

void os_line(char* out, size_t cap, size_t& at) {
  using RtlFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof v;
  if (auto f = (RtlFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) f(&v);
  SYSTEM_INFO si{}; GetNativeSystemInfo(&si);
  MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
  append(out, cap, at, "OS: Windows %lu.%lu build %lu, %lu logical processors, %llu MB memory (%llu MB free)\r\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
         si.dwNumberOfProcessors, (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20));
}

void build_line(char* out, size_t cap, size_t& at, bool full) {
  wchar_t p[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  const wchar_t* name = wcsrchr(p, L'\\'); name = name ? name + 1 : p;
  HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  LARGE_INTEGER sz{}; FILETIME wt{};
  if (f != INVALID_HANDLE_VALUE) { GetFileSizeEx(f, &sz); GetFileTime(f, nullptr, nullptr, &wt); CloseHandle(f); }
  SYSTEMTIME st{}; FileTimeToSystemTime(&wt, &st);
  const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
  const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
  append(out, cap, at, "Build: %ls, %lld bytes, written %04u-%02u-%02u %02u:%02u UTC, PE timestamp 0x%08lx, hash (FNV-1a 64 of the exe) %s\r\n", name, (long long)sz.QuadPart, st.wYear,
         st.wMonth, st.wDay, st.wHour, st.wMinute, (unsigned long)nt->FileHeader.TimeDateStamp, full ? exe_hash() : "(first write: see the final one)");
#ifdef SNAP_BUILD_REV
  append(out, cap, at, "Build revision: %s\r\n", SNAP_BUILD_REV);
#endif
}

// the report. `fault` is a host address (0 = none, a stack list is given instead when `stack` is set).
void write_report(const char* kind, const char* detail, uint64_t fault, uint64_t accessed, const char* guest, bool stack, bool full) {
  static char out[40000];
  size_t at = 0;
  wchar_t path[MAX_PATH * 2];
  if (!crash_path(path, MAX_PATH * 2)) return;
  SYSTEMTIME lt{}; GetLocalTime(&lt);
  append(out, sizeof out, at, "Snap Recomp crash report\r\n");
  append(out, sizeof out, at, "Time: %04u-%02u-%02u %02u:%02u:%02u (local), pid %lu, thread %lu, up %llu s\r\n", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond,
         GetCurrentProcessId(), GetCurrentThreadId(), (unsigned long long)((GetTickCount64() - g_startTick) / 1000));
  append(out, sizeof out, at, "What: %s%s%s\r\n", kind, detail && *detail ? ": " : "", detail ? detail : "");
  if (fault) {
    char m[300]; module_of(fault, m, sizeof m);
    append(out, sizeof out, at, "Faulting address: %s (0x%llx)%s", m, (unsigned long long)fault, accessed ? "" : "\r\n");
    if (accessed) append(out, sizeof out, at, ", accessed 0x%llx\r\n", (unsigned long long)accessed);
  } else {
    append(out, sizeof out, at, "Faulting address: none (not a processor exception)\r\n");
  }
  if (stack) {
    void* fr[24]; const USHORT n = RtlCaptureStackBackTrace(1, 24, fr, nullptr);
    append(out, sizeof out, at, "Host stack:");
    for (USHORT i = 0; i < n; i++) { char m[300]; module_of((uint64_t)(uintptr_t)fr[i], m, sizeof m); append(out, sizeof out, at, "%s%s", i ? " <- " : " ", m); }
    append(out, sizeof out, at, "\r\n");
  }
  if (guest && *guest) append(out, sizeof out, at, "Guest: %s\r\n", guest);
  build_line(out, sizeof out, at, full);
  os_line(out, sizeof out, at);
  const char* gpu = snap_quirks_gpu_line();
  append(out, sizeof out, at, "GPU: %s\r\n", gpu && *gpu ? gpu : "(not created yet)");
  const char* q = snap_quirks_applied_line();
  append(out, sizeof out, at, "Driver quirks off: %s\r\n", q && *q ? q : "none");
  append(out, sizeof out, at, "\r\nLast %d log lines:\r\n", kTailLines);
  if (full) snap_audio_log_checks();   // the audio counters (rejected calls, underruns) land in the log and so in the tail below; once, on the full write
  if (full && snap_fs_log_io_failures) snap_fs_log_io_failures();   // same: lands in the tail below
  fflush(stderr);
  log_tail(out, sizeof out, at);
  HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(f, out, (DWORD)at, &w, nullptr);
  CloseHandle(f);
}

// only the first crashing thread writes; a second thread falling over while the first is writing must not scribble over it
bool claim() {
  const LONG me = (LONG)GetCurrentThreadId();
  const LONG prev = InterlockedCompareExchange(&g_owner, me, 0);
  return prev == 0 || prev == me;
}

void on_abort(int) {
  if (claim()) write_report("abort()", "the program called abort (a guest call it could not run, or an internal check)", 0, 0, nullptr, true, true);
  _exit(3);
}
void on_terminate() {
  if (claim()) write_report("std::terminate", "an uncaught C++ exception", 0, 0, nullptr, true, true);
  _exit(3);
}

}  // namespace

extern "C" {

void snap_crashlog_set_dir(const char* utf8_dir) {
  if (utf8_dir) strncpy_s(g_dirOverride, utf8_dir, _TRUNCATE);
}

// Last run's logs become <name>.prev (replacing the one before). A log another process still holds open stays where it is.
void snap_crashlog_rotate(const wchar_t* logs_dir) {
  if (!logs_dir || !*logs_dir) return;
  for (const wchar_t* n : {L"stderr.log", L"trace.log"}) {
    wchar_t a[MAX_PATH * 2], b[MAX_PATH * 2];
    _snwprintf_s(a, MAX_PATH * 2, _TRUNCATE, L"%s\\%s", logs_dir, n);
    _snwprintf_s(b, MAX_PATH * 2, _TRUNCATE, L"%s\\%s.prev", logs_dir, n);
    if (GetFileAttributesW(a) == INVALID_FILE_ATTRIBUTES) continue;
    MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
  }
}

// Why a vectored handler: the guest's recompiled functions carry no unwind data, so when one of them (or anything it called) faults, Windows may be
// unable to walk the stack up to the thread's top-level handler; it then never calls the unhandled-exception filter and ends the process with the
// exception code (found 2026-11-15: a forced access violation on the present thread left no report at all). A vectored handler runs before that
// walk. It repeats the walk; if no frame with a __try/__except can take the exception and the walk does not reach the thread's start (kernel32 or
// ntdll), Windows will not reach the filter, so the host's filter runs from here. SNAP_CRASH_TEST=<kind> logs the walk.
static bool handler_possible(const CONTEXT* src) {
  static const bool trace = getenv("SNAP_CRASH_TEST") != nullptr;
  static const PEXCEPTION_ROUTINE seh = (PEXCEPTION_ROUTINE)GetProcAddress(GetModuleHandleA("ntdll.dll"), "__C_specific_handler");
  CONTEXT c = *src;
  const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
  for (int i = 0; i < 160; i++) {
    if (!c.Rip) { if (trace) fprintf(stderr, "[crash] walk %d: null rip, chain broken\n", i); return false; }
    char m[300]; if (trace) module_of(c.Rip, m, sizeof m);
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)c.Rip, &mod);
    char mp[MAX_PATH] = ""; if (mod) GetModuleFileNameA(mod, mp, MAX_PATH);
    const char* mb = strrchr(mp, '\\'); mb = mb ? mb + 1 : mp;
    if (!_stricmp(mb, "kernel32.dll") || !_stricmp(mb, "ntdll.dll") || !_stricmp(mb, "kernelbase.dll")) { if (trace) fprintf(stderr, "[crash] walk %d: reached %s, chain intact\n", i, m); return true; }
    DWORD64 base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
    if (!fe) {   // no unwind data: treated as a leaf frame, the return address sits at [rsp], as Windows does
      if (c.Rsp < (DWORD64)(uintptr_t)tib->StackLimit || c.Rsp + 8 > (DWORD64)(uintptr_t)tib->StackBase) { if (trace) fprintf(stderr, "[crash] walk %d: %s no unwind data, rsp out of the stack\n", i, m); return false; }
      if (trace) fprintf(stderr, "[crash] walk %d: %s no unwind data\n", i, m);
      c.Rip = *(const DWORD64*)(uintptr_t)c.Rsp; c.Rsp += 8;
      continue;
    }
    PVOID hdata = nullptr; DWORD64 est = 0;
    PEXCEPTION_ROUTINE h = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, c.Rip, fe, &c, &hdata, &est, nullptr);
    if (trace) fprintf(stderr, "[crash] walk %d: %s handler %p%s\n", i, m, (void*)h, h && h == seh ? " (__try/__except)" : "");
    if (h && h == seh) return true;
    if (c.Rsp < (DWORD64)(uintptr_t)tib->StackLimit || c.Rsp >= (DWORD64)(uintptr_t)tib->StackBase) { if (trace) fprintf(stderr, "[crash] walk %d: rsp left the stack\n", i); return false; }
  }
  return true;   // too deep to tell: leave it to the normal path
}

LONG CALLBACK fatal_veh(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
      code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
  static volatile LONG busy = 0;
  if (!g_myFilter || InterlockedCompareExchange(&busy, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
  if (handler_possible(ep->ContextRecord)) { InterlockedExchange(&busy, 0); return EXCEPTION_CONTINUE_SEARCH; }
  g_myFilter(ep);   // writes the report and the stack to the log; the process then ends with the exception code, as before
  return EXCEPTION_CONTINUE_SEARCH;
}

void snap_crashlog_install() {
  static bool done = false;
  if (done) return;
  done = true;
  g_startTick = GetTickCount64();
  g_myFilter = SetUnhandledExceptionFilter(nullptr);   // host/boot.cpp installed its filter just before this call: remember it
  SetUnhandledExceptionFilter(g_myFilter);
  AddVectoredExceptionHandler(0, fatal_veh);   // last in the list: handlers added later with first=1 (watch pages, mods) look first
  signal(SIGABRT, on_abort);
  std::set_terminate(on_terminate);
}

// the SEH filter (host/boot.cpp): `stage` 1 = first look (no hash, so it is quick), 2 = after the stack was printed (full)
void snap_crashlog_seh(EXCEPTION_POINTERS* ep, const char* guest, int stage) {
  if (!claim()) return;
  char what[64];
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  const char* nm = code == EXCEPTION_ACCESS_VIOLATION ? " (access violation)" : code == EXCEPTION_STACK_OVERFLOW ? " (stack overflow)" :
                   code == EXCEPTION_ILLEGAL_INSTRUCTION ? " (illegal instruction)" : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? " (integer divide by zero)" :
                   code == EXCEPTION_IN_PAGE_ERROR ? " (page-in error)" : "";
  _snprintf_s(what, sizeof what, _TRUNCATE, "exception 0x%08lx%s", (unsigned long)code, nm);
  uint64_t accessed = 0;
  if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) && ep->ExceptionRecord->NumberParameters > 1) accessed = ep->ExceptionRecord->ExceptionInformation[1];
  write_report("host crash", what, (uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, accessed, guest, false, stage >= 2);
}

// a deliberate fatal exit (GPU lost, the game files are unusable): `kind` a short label, `detail` the reason
void snap_crashlog_fatal(const char* kind, const char* detail) {
  if (!claim()) return;
  write_report(kind, detail, 0, 0, nullptr, true, true);
}

// ---- launch marker (docs/safe-mode.md): <crash dir>\launch.marker is written when a start begins and removed when the title screen opens or the run
// ends cleanly. A marker left behind with a dead owner means the last start never got to the title screen: the launcher then offers safe mode.
static bool marker_path(wchar_t* out, size_t cap, wchar_t* crash_file, size_t crash_cap) {
  wchar_t p[MAX_PATH * 2];
  if (!crash_path(p, MAX_PATH * 2)) return false;
  wchar_t* s = wcsrchr(p, L'\\');
  if (!s) return false;
  if (crash_file) _snwprintf_s(crash_file, crash_cap, _TRUNCATE, L"%s", p);
  *s = 0;
  _snwprintf_s(out, cap, _TRUNCATE, L"%s\\launch.marker", p);
  return true;
}
static unsigned long long proc_start(DWORD pid) {   // creation time of a running process (FILETIME as a number); 0 when it is gone or cannot be opened
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return 0;
  FILETIME c{}, e{}, k{}, u{};
  unsigned long long r = 0;
  DWORD code = 0;
  if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE && GetProcessTimes(h, &c, &e, &k, &u)) r = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
  CloseHandle(h);
  return r;
}
static unsigned long long file_time(const wchar_t* p) {
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return 0;
  return ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

// A start begins (`safe` = it is a safe-mode start).
void snap_launchmark_begin(int safe) {
  wchar_t m[MAX_PATH * 2];
  if (!marker_path(m, MAX_PATH * 2, nullptr, 0)) return;
  char t[160];
  const int n = _snprintf_s(t, sizeof t, _TRUNCATE, "pid=%lu\r\nstart=%llu\r\nsafe=%d\r\n", GetCurrentProcessId(), proc_start(GetCurrentProcessId()), safe ? 1 : 0);
  HANDLE f = CreateFileW(m, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  WriteFile(f, t, (DWORD)(n > 0 ? n : 0), &w, nullptr);
  CloseHandle(f);
}

// The title screen opened, or the run ends cleanly. Only the process that wrote the marker removes it.
void snap_launchmark_clear() {
  wchar_t m[MAX_PATH * 2];
  if (!marker_path(m, MAX_PATH * 2, nullptr, 0)) return;
  char b[200] = {};
  HANDLE f = CreateFileW(m, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD n = 0;
  ReadFile(f, b, sizeof b - 1, &n, nullptr);
  CloseHandle(f);
  const char* p = strstr(b, "pid=");
  if (p && (DWORD)strtoul(p + 4, nullptr, 10) != GetCurrentProcessId()) return;   // another start's marker
  DeleteFileW(m);
}

// 0 = nothing to report; 1 = the last start never reached the title screen; 2 = the same, and that start was already a safe-mode one.
// `why` gets one plain sentence ("... crashed ..." when _crash.txt was written after the marker, else "... stopped ...").
int snap_launchmark_check(char* why, int cap) {
  if (why && cap > 0) why[0] = 0;
  wchar_t m[MAX_PATH * 2], cf[MAX_PATH * 2];
  if (!marker_path(m, MAX_PATH * 2, cf, MAX_PATH * 2)) return 0;
  char b[200] = {};
  HANDLE f = CreateFileW(m, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  DWORD n = 0;
  ReadFile(f, b, sizeof b - 1, &n, nullptr);
  CloseHandle(f);
  const char* pp = strstr(b, "pid=");
  const char* ps = strstr(b, "start=");
  const char* sf = strstr(b, "safe=");
  const DWORD pid = pp ? (DWORD)strtoul(pp + 4, nullptr, 10) : 0;
  const unsigned long long start = ps ? strtoull(ps + 6, nullptr, 10) : 0;
  if (pid == GetCurrentProcessId()) return 0;
  if (pid) { const unsigned long long cur = proc_start(pid); if (cur && cur == start) return 0; }   // that start is still running (a second copy): not a crash
  const bool crashed = file_time(cf) >= file_time(m);
  const bool was_safe = sf && sf[5] == '1';
  if (why && cap > 0)
    _snprintf_s(why, cap, _TRUNCATE, "%s %s before the title screen%s.", was_safe ? "The last start, already in safe mode," : "The last start", crashed ? "crashed" : "stopped",
                crashed ? " (the details are in logs\\_crash.txt)" : "");
  return was_safe ? 2 : 1;
}

// ---- session marker (docs/crash-resume.md): <crash dir>\session.marker exists from the title screen until a clean exit and names the last UI scene the game
// opened. A marker whose owner is gone means the last session ended without closing; the launcher then offers to start again and to skip the menus.
// Written through a temp file and a replace, so a crash never leaves half a marker. SNAP_CRASH_RESUME=0 or SNAP_CRASH_PROMPT=0 turns it off.
static bool session_enabled() {
  static const bool on = [] { const char* e = getenv("SNAP_CRASH_PROMPT"); if (e && *e) return *e != '0'; e = getenv("SNAP_CRASH_RESUME"); return !(e && e[0] == '0'); }();
  return on;
}
static bool session_path(wchar_t* out, size_t cap) {
  wchar_t m[MAX_PATH * 2];
  if (!marker_path(m, MAX_PATH * 2, nullptr, 0)) return false;
  wchar_t* s = wcsrchr(m, L'\\');
  if (!s) return false;
  *s = 0;
  _snwprintf_s(out, cap, _TRUNCATE, L"%s\\session.marker", m);
  return true;
}
static void session_write(const char* scene) {
  wchar_t p[MAX_PATH * 2], t[MAX_PATH * 2 + 8];
  if (!session_path(p, MAX_PATH * 2)) return;
  _snwprintf_s(t, MAX_PATH * 2 + 8, _TRUNCATE, L"%s.tmp", p);
  char b[200];
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  const long long unix_s = (long long)((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ull) - 11644473600ll;
  const int n = _snprintf_s(b, sizeof b, _TRUNCATE, "pid=%lu\r\nstart=%llu\r\nscene=%s\r\nwhen=%lld\r\n", GetCurrentProcessId(), proc_start(GetCurrentProcessId()), scene ? scene : "", unix_s);
  HANDLE f = CreateFileW(t, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0;
  const BOOL ok = WriteFile(f, b, (DWORD)(n > 0 ? n : 0), &w, nullptr);
  CloseHandle(f);
  if (!ok || !MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(t);
}
static char g_session_scene[48];
static bool g_session_on = false;
// the title screen (or the lab menu) opened: from here a crash is a crash during play
extern "C" void snap_session_begin() {
  if (g_session_on || !session_enabled() || getenv("SNAP_SAFE_MODE")) { return; }   // a safe-mode session is not offered a resume
  g_session_on = true;
  session_write("");
}
// a UI scene opened (host/mods/routes.cpp); only a change is written
extern "C" void snap_session_scene(const char* scene) {
  if (!g_session_on || !scene || !strcmp(scene, g_session_scene) || !strcmp(scene, "?")) return;
  _snprintf_s(g_session_scene, sizeof g_session_scene, _TRUNCATE, "%s", scene);
  session_write(g_session_scene);
}
// every clean exit: the window closed, the guest quit, the run ended. Only the owner removes it.
extern "C" void snap_session_end() {
  if (!g_session_on) return;
  g_session_on = false;
  wchar_t p[MAX_PATH * 2];
  if (session_path(p, MAX_PATH * 2)) DeleteFileW(p);
}
// The launcher: 0 = nothing to report, 1 = the last session ended without closing. `scene` gets its last UI scene, `when` the unix time of its last
// update, `crashed` whether _crash.txt was written after it. The marker is left in place: the next session replaces it, or the launcher calls snap_session_forget.
extern "C" int snap_session_check(char* scene, int cap, long long* when, int* crashed) {
  if (scene && cap > 0) scene[0] = 0;
  if (when) *when = 0;
  if (crashed) *crashed = 0;
  if (!session_enabled()) return 0;
  wchar_t p[MAX_PATH * 2], m[MAX_PATH * 2], cf[MAX_PATH * 2];
  if (!session_path(p, MAX_PATH * 2) || !marker_path(m, MAX_PATH * 2, cf, MAX_PATH * 2)) return 0;
  char b[300] = {};
  HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  DWORD n = 0;
  ReadFile(f, b, sizeof b - 1, &n, nullptr);
  CloseHandle(f);
  const char* pp = strstr(b, "pid=");
  const char* ps = strstr(b, "start=");
  const char* sc = strstr(b, "scene=");
  const char* wh = strstr(b, "when=");
  const DWORD pid = pp ? (DWORD)strtoul(pp + 4, nullptr, 10) : 0;
  const unsigned long long start = ps ? strtoull(ps + 6, nullptr, 10) : 0;
  if (pid == GetCurrentProcessId()) return 0;
  if (pid) { const unsigned long long cur = proc_start(pid); if (cur && cur == start) return 0; }   // still running (a second copy)
  if (scene && cap > 0 && sc) {
    sc += 6;
    int i = 0;
    while (sc[i] && sc[i] != '\r' && sc[i] != '\n' && i < cap - 1) { scene[i] = sc[i]; i++; }
    scene[i] = 0;
  }
  if (when && wh) *when = strtoll(wh + 5, nullptr, 10);
  if (crashed) *crashed = file_time(cf) >= file_time(p) ? 1 : 0;
  return 1;
}
extern "C" void snap_session_forget() {
  wchar_t p[MAX_PATH * 2];
  if (session_path(p, MAX_PATH * 2)) DeleteFileW(p);
}

// SNAP_CRASH_TEST=<kind> (test aid, off unless set): "av" dereferences null on the calling thread, "abort" calls abort(), "gpu" takes the GPU-loss
// exit, "terminate" throws out of a noexcept function. Called from the present hook after SNAP_CRASH_TEST_AT presents (default 120).
void snap_crashlog_test_tick(unsigned long long present) {
  static const char* kind = getenv("SNAP_CRASH_TEST");
  if (!kind || !*kind) return;
  // SNAP_CRASH_TEST_AT=ride:<n>: n presents after the ride starts (the route's "ride" condition), so a test can crash the game mid-course
  static const char* ate = getenv("SNAP_CRASH_TEST_AT");
  static long long ride_seen = -1;
  if (ate && !strncmp(ate, "ride:", 5)) {
    char why[8];
    if (ride_seen < 0 && snap_route_cond("ride", why, sizeof why)) ride_seen = (long long)present;
    if (ride_seen < 0 || (long long)present != ride_seen + atoll(ate + 5)) return;
  } else {
    static const unsigned long long at = [] { const char* e = getenv("SNAP_CRASH_TEST_AT"); return e && atoi(e) > 0 ? (unsigned long long)atoi(e) : 120ull; }();
    if (present != at) return;
  }
  fprintf(stderr, "[crash-test] SNAP_CRASH_TEST=%s at present %llu\n", kind, present);
  fflush(stderr);
  if (!strcmp(kind, "av")) { *(volatile int*)nullptr = 1; }
  else if (!strcmp(kind, "quit")) { snap_fs_drain_for_exit(2000); ExitProcess(0); }   // the clean exit path (a window close or the guest quitting): the session marker must be gone afterwards
  else if (!strcmp(kind, "kill")) TerminateProcess(GetCurrentProcess(), 0xDEAD);   // no handler, no cleanup: a power cut or a task kill
  else if (!strcmp(kind, "abort")) abort();
  else if (!strcmp(kind, "gpu")) { snap_crashlog_fatal("fatal GPU error", "test: vkQueuePresentKHR: VK_ERROR_DEVICE_LOST (SNAP_CRASH_TEST=gpu)"); ExitProcess(1); }
  else if (!strcmp(kind, "terminate")) { []() noexcept { throw 1; }(); }
}

}  // extern "C"
