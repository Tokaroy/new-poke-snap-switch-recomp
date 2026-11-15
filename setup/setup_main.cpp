// SnapRecompSetup.exe: the one-click setup wizard (plain Win32; the launcher v2 theme can skin it later).
//
// The exe carries a zip payload after the PE image: the bundled Python, the builder, the prebuilt runtime objects and NSTool.
// On start it unpacks the payload once into %LOCALAPPDATA%\SnapRecomp\setup\app\<version> and then only drives the builder's
// `setup` command (builder/snapbuild/setup_engine.py), which does the work and prints one JSON object per line.
//
// It never downloads, asks for or points to game files, keys or firmware: the player picks a folder, the engine reads it in place.
//
// Command line: (none) wizard | /repair | /uninstall | /folder <path> (preselect) | /auto (click through with defaults, tests only)
//               /shot <dir> (render every page to BMP on a private hidden desktop, then exit)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifndef SETUP_VERSION
#define SETUP_VERSION "1.1.0"
#endif

#pragma comment(lib, "comctl32.lib")

using std::string;
using std::wstring;

// ------------------------------------------------------------------------------------------------ helpers
static wstring W(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static string U(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring Env(const wchar_t* name) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    return n && n < 2048 ? wstring(buf, n) : L"";
}
static bool Exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static void MkDirs(const wstring& p) {
    for (size_t i = 3; i < p.size(); i++)
        if (p[i] == L'\\') CreateDirectoryW(p.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(p.c_str(), nullptr);
}
static string ReadAll(const wstring& p) {
    string out;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    char buf[65536];
    DWORD n;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(f);
    return out;
}
static bool WriteAll(const wstring& p, const string& data) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &n, nullptr) && n == data.size();
    CloseHandle(f);
    return ok;
}
// minimal JSON line access: flat objects, string and number values
static string JUnescape(const string& s, size_t i, size_t* end) {
    string o;
    for (; i < s.size() && s[i] != '"'; i++) {
        if (s[i] != '\\') { o += s[i]; continue; }
        if (++i >= s.size()) break;
        switch (s[i]) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': break;
        case 'u': {
            unsigned c = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            wstring w(1, (wchar_t)c);
            o += U(w);
            break;
        }
        default: o += s[i];
        }
    }
    if (end) *end = i;
    return o;
}
static string JGet(const string& line, const char* key) {
    string k = string("\"") + key + "\":";
    size_t p = line.find(k);
    if (p == string::npos) return "";
    p += k.size();
    while (p < line.size() && line[p] == ' ') p++;
    if (p < line.size() && line[p] == '"') return JUnescape(line, p + 1, nullptr);
    size_t e = line.find_first_of(",}", p);
    return line.substr(p, e == string::npos ? string::npos : e - p);
}
static string JEsc(const wstring& w) {
    string s = U(w), o;
    for (char c : s) {
        if (c == '\\' || c == '"') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

// ------------------------------------------------------------------------------------------------ paths and payload
static wstring SetupDir() {
    wstring d = Env(L"SNAP_SETUP_DIR");
    if (!d.empty()) return d;
    wstring l = Env(L"LOCALAPPDATA");
    return l + L"\\SnapRecomp\\setup";
}
static wstring g_app;      // unpacked payload
static wstring g_exe;      // this exe

struct Trailer { unsigned long long size; char magic[8]; };

static bool ExtractPayload(wstring* err) {
    g_app = SetupDir() + L"\\app\\" + W(SETUP_VERSION);
    if (Exists(g_app + L"\\.ok")) return true;
    HANDLE f = CreateFileW(g_exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = L"The setup program could not read itself."; return false; }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    Trailer t;
    LARGE_INTEGER pos; pos.QuadPart = sz.QuadPart - (LONGLONG)sizeof t;
    DWORD n;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    if (!ReadFile(f, &t, sizeof t, &n, nullptr) || memcmp(t.magic, "SNPSET01", 8) != 0 || (LONGLONG)t.size > sz.QuadPart) {
        CloseHandle(f);
        *err = L"This setup file is incomplete. Download it again.";
        return false;
    }
    MkDirs(SetupDir() + L"\\app");
    wstring zip = SetupDir() + L"\\payload.tmp.zip";
    HANDLE o = CreateFileW(zip.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (o == INVALID_HANDLE_VALUE) { CloseHandle(f); *err = L"Could not write to " + SetupDir(); return false; }
    pos.QuadPart = sz.QuadPart - (LONGLONG)sizeof t - (LONGLONG)t.size;
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
    std::vector<char> buf(1 << 20);
    unsigned long long left = t.size;
    while (left) {
        DWORD want = (DWORD)(left < buf.size() ? left : buf.size());
        if (!ReadFile(f, buf.data(), want, &n, nullptr) || !n) break;
        WriteFile(o, buf.data(), n, &n, nullptr);
        left -= n;
    }
    CloseHandle(o);
    CloseHandle(f);
    if (left) { DeleteFileW(zip.c_str()); *err = L"This setup file is incomplete. Download it again."; return false; }
    wstring tmp = g_app + L".part";
    {   // a half-unpacked folder from an earlier crash is removed first
        wstring cmd = L"cmd.exe /c rmdir /s /q \"" + tmp + L"\"";
        STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 60000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
    }
    MkDirs(tmp);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wstring cmd = wstring(L"\"") + sys + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + tmp + L"\"";
    STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *err = L"Windows' tar.exe is missing, so the setup files cannot be unpacked."; return false;
    }
    WaitForSingleObject(pi.hProcess, 600000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    DeleteFileW(zip.c_str());
    if (code != 0) { *err = L"The setup files could not be unpacked (is the disk full?)."; return false; }
    if (!MoveFileExW(tmp.c_str(), g_app.c_str(), 0)) { *err = L"Could not finish unpacking the setup files."; return false; }
    WriteAll(g_app + L"\\.ok", "ok");
    return true;
}

// ------------------------------------------------------------------------------------------------ running the engine
struct Proc {
    HANDLE proc = nullptr, out = nullptr;
    DWORD pid = 0;
};
static wstring EngineCmd(const wstring& args) {
    return L"\"" + g_app + L"\\python\\python.exe\" -I \"" + g_app + L"\\builder\\snap_builder.py\" setup " + args;
}
static bool Spawn(const wstring& args, Proc* p) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return false;
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = w;
    si.hStdError = w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    wstring cmd = EngineCmd(args);
    PROCESS_INFORMATION pi;
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, g_app.c_str(), &si, &pi);
    CloseHandle(w);
    if (!ok) { CloseHandle(r); return false; }
    CloseHandle(pi.hThread);
    p->proc = pi.hProcess; p->out = r; p->pid = pi.dwProcessId;
    return true;
}
static bool ReadLine(HANDLE h, string* line) {
    line->clear();
    char c; DWORD n;
    while (ReadFile(h, &c, 1, &n, nullptr) && n) {
        if (c == '\n') return true;
        if (c != '\r') *line += c;
    }
    return !line->empty();
}

// ------------------------------------------------------------------------------------------------ UI
enum Page { P_WELCOME, P_GAME, P_OPTIONS, P_BUILD, P_DONE, P_ERROR, P_MAINT, P_UNINST, P_COUNT };
enum { ID_NEXT = 100, ID_BACK, ID_CANCEL, ID_BROWSE, ID_KEYS, ID_MSLIC, ID_DESK, ID_START, ID_FSR, ID_DLSS, ID_XESS, ID_DETAILS, ID_PLAY, ID_RETRY,
       ID_REPAIR, ID_CHANGE, ID_UNINST, ID_RMTOOLS, ID_RMSAVES, ID_CLOSE, ID_LOG, ID_MSURL };
#define WM_ENGINE (WM_APP + 1)
#define WM_SCANDONE (WM_APP + 2)
#define WM_PREPARED (WM_APP + 3)

static HWND g_wnd, g_title, g_body, g_next, g_back, g_cancel, g_path, g_detect, g_keysBtn, g_chk[16], g_bar, g_stage, g_eta, g_log, g_detBtn, g_msurl;
static HFONT g_font, g_fontBig;
static int g_page = P_WELCOME;
static wstring g_folder, g_keys, g_scanText, g_exePlay, g_err, g_workdir;
static bool g_noShortcuts = false;
static bool g_scanOk = false, g_needKeys = false, g_busy = false, g_repair = false, g_auto = false, g_showLog = false, g_uninstMode = false;
static Proc g_proc;
static std::mutex g_mu;
static string g_logText;
static string g_doneJson;
static wstring g_scanRoot;     // optional --root for tests

static void SetFontAll(HWND h) { SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE); }
static HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SetFontAll(c);
    return c;
}
static void Show(HWND h, bool on) { if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE); }

static void AppendLog(const string& s) {
    std::lock_guard<std::mutex> g(g_mu);
    g_logText += s + "\r\n";
    if (g_logText.size() > 60000) g_logText.erase(0, g_logText.size() - 40000);
}

static void Layout() {
    // everything is placed for a 760x540 client area
    Show(g_msurl, false);
    for (HWND c : g_chk) Show(c, false);
    for (HWND c : {g_path, g_detect, g_keysBtn, g_bar, g_stage, g_eta, g_log, g_detBtn}) Show(c, false);
    Show(GetDlgItem(g_wnd, ID_BROWSE), false);
    Show(GetDlgItem(g_wnd, ID_PLAY), false);
    Show(GetDlgItem(g_wnd, ID_RETRY), false);
    Show(GetDlgItem(g_wnd, ID_REPAIR), false);
    Show(GetDlgItem(g_wnd, ID_CHANGE), false);
    Show(GetDlgItem(g_wnd, ID_UNINST), false);
    Show(GetDlgItem(g_wnd, ID_CLOSE), false);
    Show(g_back, false); Show(g_cancel, false); Show(g_next, true);
    EnableWindow(g_next, TRUE);
    SetWindowTextW(g_next, L"Next");
    wstring t, b;
    switch (g_page) {
    case P_WELCOME:
        t = L"Snap Recomp setup";
        b = L"This builds the game program from your own game files, on this PC. It takes a while the first time (about an hour, mostly "
            L"unattended) and needs roughly 25 GB of free disk space.\r\n\r\n"
            L"Nothing is uploaded. Your game files are only read, never changed, and never copied anywhere except into the work "
            L"folder this setup creates. When it finishes you get a desktop shortcut and can press Play.\r\n\r\n"
            L"You need your own copy of the game. Setup does not download, request or point to game files, keys or firmware.";
        SetWindowTextW(g_next, L"Start");
        break;
    case P_GAME:
        t = L"Choose your game files";
        b = L"Choose the folder with your game files, or drag it onto this window. Either a folder with the game already "
            L"extracted (main, sdk and romfs), or a folder with your base game file, its updates and your prod.keys. "
            L"Setup picks the right version (2.0.1) and ignores older updates.";
        Show(g_path, true); Show(g_detect, true); Show(GetDlgItem(g_wnd, ID_BROWSE), true);
        Show(g_keysBtn, g_needKeys); Show(g_back, true);
        EnableWindow(g_next, g_scanOk && !g_busy);
        break;
    case P_OPTIONS:
        t = L"Ready to build";
        b = L"Setup downloads the free build tools (LLVM, Java and Ghidra, about 1.7 GB) and Microsoft's C runtime files, then builds "
            L"the game program. Please read and accept Microsoft's license to continue.";
        for (int i : {0, 1, 2, 3, 4, 5}) Show(g_chk[i], true);
        Show(g_msurl, true); Show(g_back, true);
        SetWindowTextW(g_next, L"Build");
        EnableWindow(g_next, SendMessageW(g_chk[0], BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case P_BUILD:
        t = g_repair ? L"Repairing" : L"Building Snap Recomp";
        b = L"You can leave this running. It continues where it stopped if the PC restarts: run setup again.";
        Show(g_bar, true); Show(g_stage, true); Show(g_eta, true); Show(g_detBtn, true); Show(g_log, g_showLog); Show(g_cancel, true);
        Show(g_next, false);
        break;
    case P_DONE:
        t = L"Snap Recomp is ready";
        b = L"A shortcut is on your desktop and in the Start menu. The first start compiles shaders and can take a minute.";
        Show(GetDlgItem(g_wnd, ID_PLAY), true); Show(GetDlgItem(g_wnd, ID_CLOSE), true); Show(g_next, false);
        break;
    case P_ERROR:
        t = L"Setup stopped";
        b = g_err;
        Show(GetDlgItem(g_wnd, ID_RETRY), true); Show(GetDlgItem(g_wnd, ID_CLOSE), true); Show(g_detBtn, true); Show(g_log, g_showLog); Show(g_next, false);
        break;
    case P_MAINT:
        t = L"Snap Recomp is installed";
        b = L"Choose what to do.";
        Show(GetDlgItem(g_wnd, ID_PLAY), true); Show(GetDlgItem(g_wnd, ID_REPAIR), true); Show(GetDlgItem(g_wnd, ID_CHANGE), true);
        Show(GetDlgItem(g_wnd, ID_UNINST), true); Show(GetDlgItem(g_wnd, ID_CLOSE), true); Show(g_next, false);
        break;
    case P_UNINST:
        t = L"Remove Snap Recomp";
        b = L"This removes the shortcuts, the work folder and the game data setup extracted. Your own game files are never touched.";
        Show(g_chk[6], true); Show(g_chk[7], true);
        SetWindowTextW(g_next, L"Remove");
        Show(g_cancel, true);
        break;
    }
    SetWindowTextW(g_title, t.c_str());
    SetWindowTextW(g_body, b.c_str());
    InvalidateRect(g_wnd, nullptr, TRUE);
}

static void SetPage(int p) { g_page = p; Layout(); }

static void SetProgress(int pct10, const wstring& stage, const wstring& eta) {
    SendMessageW(g_bar, PBM_SETPOS, pct10, 0);
    SetWindowTextW(g_stage, stage.c_str());
    SetWindowTextW(g_eta, eta.c_str());
}

static void RefreshLog() {
    string s;
    { std::lock_guard<std::mutex> g(g_mu); s = g_logText; }
    SetWindowTextW(g_log, W(s).c_str());
    SendMessageW(g_log, EM_LINESCROLL, 0, 100000);
}

// Runs the engine and forwards its lines to the UI thread.
static void EngineThread(wstring args) {
    Proc p;
    if (!Spawn(args, &p)) {
        string* m = new string("{\"t\":\"error\",\"text\":\"The setup engine could not start. Run setup again, or download it again.\",\"retry\":true}");
        PostMessageW(g_wnd, WM_ENGINE, 0, (LPARAM)m);
        return;
    }
    g_proc = p;
    string line;
    while (ReadLine(p.out, &line)) PostMessageW(g_wnd, WM_ENGINE, 0, (LPARAM) new string(line));
    DWORD code = 0;
    WaitForSingleObject(p.proc, INFINITE);
    GetExitCodeProcess(p.proc, &code);
    CloseHandle(p.proc); CloseHandle(p.out);
    g_proc = Proc();
    PostMessageW(g_wnd, WM_ENGINE, 1, (LPARAM) new string(std::to_string(code)));
}
static std::thread g_thread;
static void StartEngine(const wstring& args) {
    if (g_thread.joinable()) g_thread.join();
    g_busy = true;
    g_thread = std::thread(EngineThread, args);
}

static wstring WriteConfig() {
    string j = "{";
    j += "\"game_dir\": \"" + JEsc(g_folder) + "\", ";
    if (!g_keys.empty()) j += "\"keys\": \"" + JEsc(g_keys) + "\", ";
    j += "\"kind\": \"" + string(g_needKeys || true ? "auto" : "auto") + "\", ";
    if (!g_scanRoot.empty()) j += "\"root\": \"" + JEsc(g_scanRoot) + "\", ";
    j += "\"downloads_ok\": true, \"accept_ms_license\": true, ";
    j += "\"offline_dirs\": [\"" + JEsc(g_app + L"\\offline") + "\"], ";
    j += "\"prebuilt\": \"" + JEsc(g_app + L"\\prebuilt") + "\", ";
    j += "\"setup_exe\": \"" + JEsc(g_exe) + "\", ";
    j += string("\"shortcuts\": {\"desktop\": ") + (SendMessageW(g_chk[1], BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false") +
         ", \"start\": " + (SendMessageW(g_chk[2], BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false") + "}, ";
    if (g_noShortcuts) {
        size_t p = j.rfind("\"shortcuts\"");
        size_t e = j.find("}, ", p);
        j.replace(p, e - p + 1, "\"shortcuts\": {\"desktop\": false, \"start\": false}");
    }
    string ups, acc;
    const char* names[] = {"fsr", "dlss", "xess"};
    for (int i = 0; i < 3; i++)
        if (SendMessageW(g_chk[3 + i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            if (!ups.empty()) { ups += ", "; acc += ", "; }
            ups += string("\"") + names[i] + "\""; acc += string("\"") + names[i] + "\"";
        }
    j += "\"upscalers\": [" + ups + "], \"accept_license\": [" + acc + "]}";
    wstring path = SetupDir() + L"\\run-config.json";
    MkDirs(SetupDir());
    WriteAll(path, j);
    return path;
}

static void BeginBuild() {
    g_repair = false;
    wstring cfg = WriteConfig();
    { std::lock_guard<std::mutex> g(g_mu); g_logText.clear(); }
    SetProgress(0, L"Starting", L"");
    SetPage(P_BUILD);
    StartEngine(L"run --config \"" + cfg + L"\"");
}

static void BeginScan() {
    g_scanOk = false; g_needKeys = false;
    SetWindowTextW(g_detect, L"Looking at the files in this folder ...");
    Layout();
    wstring args = L"scan --folder \"" + g_folder + L"\" --yes --offline-dir \"" + g_app + L"\\offline\"";
    if (!g_keys.empty()) args += L" --keys \"" + g_keys + L"\"";
    if (!g_scanRoot.empty()) args += L" --root \"" + g_scanRoot + L"\"";
    StartEngine(args);
}

static bool PickFolder(wstring* out, const wchar_t* title, bool file) {
    wchar_t buf[MAX_PATH] = {};
    if (file) {
        OPENFILENAMEW o{sizeof o};
        o.hwndOwner = g_wnd; o.lpstrFile = buf; o.nMaxFile = MAX_PATH; o.lpstrTitle = title;
        o.lpstrFilter = L"prod.keys\0prod.keys\0All files\0*.*\0";
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&o)) return false;
        *out = buf;
        return true;
    }
    BROWSEINFOW bi{};
    bi.hwndOwner = g_wnd; bi.lpszTitle = title; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi);
    if (!id) return false;
    bool ok = SHGetPathFromIDListW(id, buf) != 0;
    CoTaskMemFree(id);
    if (ok) *out = buf;
    return ok;
}

static void ChooseFolder(const wstring& f) {
    g_folder = f; g_keys.clear();
    SetWindowTextW(g_path, f.c_str());
    BeginScan();
}

static void OnEngine(WPARAM kind, string* line) {
    string l = *line; delete line;
    if (kind == 1) {   // the engine process ended
        g_busy = false;
        if (g_page == P_GAME) { Layout(); }
        else if (g_page == P_BUILD && !g_doneJson.size()) {
            if (g_err.empty()) g_err = L"Setup was stopped before it finished. Run it again to continue where it stopped.";
            SetPage(P_ERROR);
        }
        return;
    }
    if (g_page == P_GAME) {   // the scan prints exactly one JSON object
        string ok = JGet(l, "ok"), text = JGet(l, "text");
        g_scanOk = ok == "true";
        g_needKeys = JGet(l, "needs_keys") == "true";
        for (size_t i = 0; i < text.size(); i++) {}
        wstring wt = W(text);
        wstring fixed;
        for (wchar_t c : wt) { if (c == L'\n') fixed += L"\r\n"; else fixed += c; }
        SetWindowTextW(g_detect, fixed.c_str());
        string keys = JGet(l, "keys");
        if (!keys.empty() && keys != "null" && g_keys.empty()) g_keys = W(keys);
        Layout();
        return;
    }
    string t = JGet(l, "t");
    if (t == "log") {
        AppendLog(JGet(l, "text"));
        if (g_showLog) RefreshLog();
    } else if (t == "progress") {
        double pct = atof(JGet(l, "pct").c_str());
        int eta = atoi(JGet(l, "eta_s").c_str());
        wchar_t e[96];
        if (eta > 0) swprintf(e, 96, L"%.0f%%  -  about %d min left", pct, (eta + 59) / 60);
        else swprintf(e, 96, L"%.0f%%", pct);
        SetProgress((int)(pct * 10), W(JGet(l, "text")), e);
    } else if (t == "stage") {
        AppendLog("== " + JGet(l, "title"));
    } else if (t == "error") {
        g_err = W(JGet(l, "text"));
        if (JGet(l, "action") == "choose_folder") { g_err += L"\r\nUse 'Change game folder' to choose it again."; }
        AppendLog("ERROR: " + JGet(l, "text"));
        g_doneJson.clear();
        SetPage(P_ERROR);
    } else if (t == "done") {
        if (g_uninstMode) { g_doneJson = l; SetWindowTextW(g_title, L"Removed"); SetWindowTextW(g_body, L"Snap Recomp was removed. Your game files were not touched."); Show(g_cancel, false); Show(g_next, false); Show(GetDlgItem(g_wnd, ID_CLOSE), true); return; }
        g_doneJson = l;
        g_exePlay = W(JGet(l, "exe"));
        SetPage(P_DONE);
    }
}

static void PlayNow() {
    wstring exe = g_exePlay;
    if (exe.empty()) {
        string s = ReadAll(SetupDir() + L"\\settings.json");
        exe = W(JGet(s, "exe"));
        size_t p;
        while ((p = exe.find(L"\\\\")) != wstring::npos) exe.erase(p, 1);
    }
    if (exe.empty() || !Exists(exe)) { MessageBoxW(g_wnd, L"The game program was not found. Choose Repair.", L"Snap Recomp", MB_ICONWARNING); return; }
    size_t sl = exe.rfind(L'\\');
    ShellExecuteW(g_wnd, L"open", exe.c_str(), nullptr, exe.substr(0, sl).c_str(), SW_SHOWNORMAL);
}

static void Cancel() {
    if (g_busy && g_proc.pid) {
        wstring cmd = L"taskkill /PID " + std::to_wstring(g_proc.pid) + L" /T /F";
        STARTUPINFOW si{sizeof si}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 10000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        g_err = L"Setup was stopped. Run it again to continue where it stopped.";
    } else {
        DestroyWindow(g_wnd);
    }
}

static void OnCommand(int id) {
    switch (id) {
    case ID_NEXT:
        if (g_page == P_WELCOME) SetPage(P_GAME);
        else if (g_page == P_GAME) SetPage(P_OPTIONS);
        else if (g_page == P_OPTIONS) BeginBuild();
        else if (g_page == P_UNINST) {
            g_uninstMode = true; g_busy = false;
            wstring a = L"uninstall";
            if (SendMessageW(g_chk[6], BM_GETCHECK, 0, 0) == BST_CHECKED) a += L" --remove-tools";
            if (SendMessageW(g_chk[7], BM_GETCHECK, 0, 0) == BST_CHECKED) a += L" --remove-saves";
            EnableWindow(g_next, FALSE);
            StartEngine(a);
        }
        break;
    case ID_BACK:
        if (g_page == P_GAME) SetPage(P_WELCOME);
        else if (g_page == P_OPTIONS) SetPage(P_GAME);
        break;
    case ID_CANCEL: Cancel(); break;
    case ID_CLOSE: DestroyWindow(g_wnd); break;
    case ID_BROWSE: { wstring f; if (PickFolder(&f, L"Choose the folder with your game files", false)) ChooseFolder(f); break; }
    case ID_KEYS: { wstring f; if (PickFolder(&f, L"Choose your prod.keys file", true)) { g_keys = f; BeginScan(); } break; }
    case ID_MSLIC: case ID_FSR: case ID_DLSS: case ID_XESS: case ID_DESK: case ID_START:
        if (g_page == P_OPTIONS) EnableWindow(g_next, SendMessageW(g_chk[0], BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case ID_MSURL: ShellExecuteW(g_wnd, L"open", L"https://go.microsoft.com/fwlink/?LinkId=2086102", nullptr, nullptr, SW_SHOWNORMAL); break;
    case ID_DETAILS: g_showLog = !g_showLog; RefreshLog(); Show(g_log, g_showLog); SetWindowTextW(g_detBtn, g_showLog ? L"Hide details" : L"Show details"); break;
    case ID_PLAY: PlayNow(); break;
    case ID_RETRY:
        if (g_repair) { g_err.clear(); g_doneJson.clear(); SetPage(P_BUILD); StartEngine(L"repair"); }
        else { g_err.clear(); g_doneJson.clear(); BeginBuild(); }
        break;
    case ID_REPAIR: g_repair = true; g_err.clear(); g_doneJson.clear(); SetProgress(0, L"Starting", L""); SetPage(P_BUILD); StartEngine(L"repair"); break;
    case ID_CHANGE: g_repair = false; SetPage(P_GAME); break;
    case ID_UNINST: SetPage(P_UNINST); break;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: OnCommand(LOWORD(w)); return 0;
    case WM_ENGINE: OnEngine(w, (string*)l); return 0;
    case WM_DROPFILES: {
        wchar_t p[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, p, MAX_PATH)) {
            wstring s = p;
            if (!(GetFileAttributesW(s.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) s = s.substr(0, s.rfind(L'\\'));
            if (g_page == P_GAME || g_page == P_WELCOME) { SetPage(P_GAME); ChooseFolder(s); }
        }
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)w, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        if (g_busy && g_page == P_BUILD) {
            if (MessageBoxW(h, L"Stop the setup? It continues where it stopped the next time.", L"Snap Recomp", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            Cancel();
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void CreateUi() {
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBig = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_title = Mk(L"STATIC", L"", WS_VISIBLE, 28, 22, 700, 40);
    SendMessageW(g_title, WM_SETFONT, (WPARAM)g_fontBig, TRUE);
    g_body = Mk(L"STATIC", L"", WS_VISIBLE, 28, 74, 700, 150);
    g_path = Mk(L"EDIT", L"", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, 28, 200, 560, 28);
    Mk(L"BUTTON", L"Choose folder...", WS_TABSTOP | BS_PUSHBUTTON, 598, 198, 130, 32, ID_BROWSE);
    g_detect = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 28, 240, 700, 220);
    g_keysBtn = Mk(L"BUTTON", L"Choose prod.keys...", WS_TABSTOP | BS_PUSHBUTTON, 28, 468, 190, 32, ID_KEYS);
    const wchar_t* chk[] = {L"I accept Microsoft's license for the C runtime and Windows SDK files setup downloads",
                            L"Add a shortcut to the desktop", L"Add a shortcut to the Start menu",
                            L"Also install AMD FSR upscaling (AMD's license applies, downloaded from AMD)",
                            L"Also install NVIDIA DLSS (NVIDIA's license applies, downloaded from NVIDIA)",
                            L"Also install Intel XeSS (Intel's license applies, downloaded from Intel)",
                            L"Also delete the downloaded build tools (about 5 GB)", L"Also delete my saves and settings"};
    int ys[] = {170, 230, 262, 310, 342, 374, 150, 190};
    int ids[] = {ID_MSLIC, ID_DESK, ID_START, ID_FSR, ID_DLSS, ID_XESS, ID_RMTOOLS, ID_RMSAVES};
    for (int i = 0; i < 8; i++) {
        g_chk[i] = Mk(L"BUTTON", chk[i], WS_TABSTOP | BS_AUTOCHECKBOX, 28, ys[i], 700, 26, ids[i]);
    }
    SendMessageW(g_chk[1], BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_chk[2], BM_SETCHECK, BST_CHECKED, 0);
    g_msurl = Mk(L"BUTTON", L"Read Microsoft's license", WS_TABSTOP | BS_PUSHBUTTON, 28, 198, 210, 28, ID_MSURL);
    g_bar = Mk(PROGRESS_CLASSW, L"", 0, 28, 150, 700, 26);
    SendMessageW(g_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    g_stage = Mk(L"STATIC", L"", 0, 28, 186, 700, 24);
    g_eta = Mk(L"STATIC", L"", 0, 28, 212, 700, 24);
    g_detBtn = Mk(L"BUTTON", L"Show details", WS_TABSTOP | BS_PUSHBUTTON, 28, 250, 140, 30, ID_DETAILS);
    g_log = Mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY, 28, 290, 700, 184);
    Mk(L"BUTTON", L"Play now", WS_TABSTOP | BS_DEFPUSHBUTTON, 28, 160, 160, 40, ID_PLAY);
    Mk(L"BUTTON", L"Try again", WS_TABSTOP | BS_PUSHBUTTON, 28, 160, 160, 40, ID_RETRY);
    Mk(L"BUTTON", L"Repair", WS_TABSTOP | BS_PUSHBUTTON, 28, 210, 160, 36, ID_REPAIR);
    Mk(L"BUTTON", L"Change game folder", WS_TABSTOP | BS_PUSHBUTTON, 28, 256, 160, 36, ID_CHANGE);
    Mk(L"BUTTON", L"Uninstall", WS_TABSTOP | BS_PUSHBUTTON, 28, 302, 160, 36, ID_UNINST);
    Mk(L"BUTTON", L"Close", WS_TABSTOP | BS_PUSHBUTTON, 28, 440, 120, 36, ID_CLOSE);
    g_back = Mk(L"BUTTON", L"Back", WS_TABSTOP | BS_PUSHBUTTON, 480, 490, 110, 36, ID_BACK);
    g_next = Mk(L"BUTTON", L"Next", WS_TABSTOP | BS_DEFPUSHBUTTON, 610, 490, 118, 36, ID_NEXT);
    g_cancel = Mk(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 480, 490, 110, 36, ID_CANCEL);
}

// ------------------------------------------------------------------------------------------------ test screenshots
static bool SaveBmp(HWND h, const wstring& file) {
    RECT r; GetClientRect(h, &r);
    int w = r.right, hh = r.bottom;
    HDC dc = GetDC(h), mem = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w, hh);
    SelectObject(mem, bm);
    PrintWindow(h, mem, PW_CLIENTONLY | 2 /*PW_RENDERFULLCONTENT*/);
    BITMAPINFOHEADER bi{sizeof bi, w, -hh, 1, 24, BI_RGB};
    int stride = ((w * 3 + 3) & ~3);
    std::vector<char> px((size_t)stride * hh);
    GetDIBits(mem, bm, 0, hh, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{0x4d42, (DWORD)(sizeof fh + sizeof bi + px.size()), 0, 0, (DWORD)(sizeof fh + sizeof bi)};
    bi.biHeight = -hh;
    string out((char*)&fh, sizeof fh);
    out.append((char*)&bi, sizeof bi);
    out.append(px.data(), px.size());
    DeleteObject(bm); DeleteDC(mem); ReleaseDC(h, dc);
    return WriteAll(file, out);
}
static void PumpFor(int ms) {
    DWORD end = GetTickCount() + ms;
    MSG m;
    while (GetTickCount() < end) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}
static void ShotMode(const wstring& dir) {
    MkDirs(dir);
    // sample content only; real scan text comes from the engine
    SetWindowTextW(g_path, L"E:\\Pokemon Snap");
    SetWindowTextW(g_detect, L"Found 5 game file(s):\r\n   n-pokemonsnap.xci  (14.93 GB)  1.0.0\r\n   n-new_pokemon_snap_v196608.nsp  (2.1 GB)  2.0.0\r\n"
                             L"   n-new_pokemon_snap_v262144.nsp  (2.2 GB)  2.0.1\r\nUsing the 2.0.1 update with n-pokemonsnap.xci.\r\nOlder updates in this folder are ignored.\r\n"
                             L"Keys: found next to the game files (read from there, never copied).\r\n\r\nDisk space:\r\n   Drive D: needs up to 31.2 GB while building and 17.0 GB afterwards; 4.8 TB free\r\n"
                             L"The extracted game data stays on disk because the game reads it while it runs, so it cannot be deleted after the build.");
    g_scanOk = true;
    SendMessageW(g_chk[0], BM_SETCHECK, BST_CHECKED, 0);
    AppendLog("Using n-new_pokemon_snap_v262144.nsp (version 2.0.1) with n-pokemonsnap.xci.");
    AppendLog("step elf done in 3s");
    AppendLog("step maps");
    g_err = L"Not enough free space on D:: the build needs about 12.0 GB for a short time and 3.1 GB is free. Choose another drive.";
    struct S { int page; const wchar_t* name; };
    S shots[] = {{P_WELCOME, L"1_welcome"}, {P_GAME, L"2_game_files"}, {P_OPTIONS, L"3_options"}, {P_BUILD, L"4_build"}, {P_DONE, L"5_done"},
                 {P_ERROR, L"6_error"}, {P_MAINT, L"7_maintenance"}, {P_UNINST, L"8_uninstall"}};
    for (auto& s : shots) {
        if (s.page == P_BUILD) { SetProgress(412, L"Compiling the game code (step 5 of 8)", L"41%  -  about 38 min left"); g_showLog = true; RefreshLog(); }
        if (s.page == P_ERROR) g_showLog = true;
        SetPage(s.page);
        if (s.page == P_BUILD || s.page == P_ERROR) { RefreshLog(); Show(g_log, true); SetWindowTextW(g_detBtn, L"Hide details"); }
        PumpFor(250);
        RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        PumpFor(100);
        SaveBmp(g_wnd, dir + L"\\" + s.name + L".bmp");
    }
    DestroyWindow(g_wnd);
}

// ------------------------------------------------------------------------------------------------ main
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t me[MAX_PATH];
    GetModuleFileNameW(nullptr, me, MAX_PATH);
    g_exe = me;
    wstring preFolder, shotDir;
    int startPage = -1;
    bool repair = false, uninst = false;
    bool hidden = false;
    wstring snapDir;
    for (int i = 1; i < argc; i++) {
        wstring a = argv[i];
        if (a == L"/hidden") hidden = true;                                   // test: run on a private hidden desktop
        else if (a == L"/snapdir" && i + 1 < argc) snapDir = argv[++i];       // test: save a BMP of every page change
        else if (a == L"/noshortcuts") g_noShortcuts = true;                  // test: leave the desktop and Start menu alone
        else if (a == L"/repair") repair = true;
        else if (a == L"/uninstall") uninst = true;
        else if (a == L"/auto") g_auto = true;
        else if (a == L"/folder" && i + 1 < argc) preFolder = argv[++i];
        else if (a == L"/root" && i + 1 < argc) g_scanRoot = argv[++i];
        else if (a == L"/shot" && i + 1 < argc) shotDir = argv[++i];
    }
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wstring err;
    if (shotDir.empty() && !ExtractPayload(&err)) { MessageBoxW(nullptr, err.c_str(), L"Snap Recomp setup", MB_ICONERROR); return 1; }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW); wc.lpszClassName = L"SnapRecompSetup";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r{0, 0, 760, 540};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HDESK shotDesk = nullptr;
    if (!shotDir.empty() || hidden) {   // test mode: a private hidden desktop, so nothing appears on the player's screen
        shotDesk = CreateDesktopW(L"snap_setup_shot", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (shotDesk) SetThreadDesktop(shotDesk);
    }
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"SnapRecompSetup", L"Snap Recomp setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    DragAcceptFiles(g_wnd, TRUE);
    CreateUi();

    // which page first
    string settings = ReadAll(SetupDir() + L"\\settings.json");
    bool installed = settings.find("\"completed\": true") != string::npos;
    if (!shotDir.empty()) { ShowWindow(g_wnd, SW_SHOWNOACTIVATE); ShotMode(shotDir); return 0; }
    ShowWindow(g_wnd, SW_SHOW);
    if (uninst) SetPage(P_UNINST);
    else if (repair) { g_repair = true; SetPage(P_BUILD); StartEngine(L"repair"); }
    else if (installed && preFolder.empty()) SetPage(P_MAINT);
    else if (!preFolder.empty()) { SetPage(P_GAME); ChooseFolder(preFolder); }
    else SetPage(P_WELCOME);
    (void)startPage;

    MSG m;
    UINT_PTR timer = SetTimer(g_wnd, 1, 700, nullptr);
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_TIMER && m.hwnd == g_wnd && g_auto) {   // /auto: click through with the defaults (tests only)
            if (g_page == P_GAME && g_scanOk && !g_busy) { OnCommand(ID_NEXT); }
            else if (g_page == P_OPTIONS) { SendMessageW(g_chk[0], BM_SETCHECK, BST_CHECKED, 0); OnCommand(ID_NEXT); }
        }
        if (m.message == WM_TIMER && m.hwnd == g_wnd && g_showLog) RefreshLog();
        if (m.message == WM_TIMER && m.hwnd == g_wnd && !snapDir.empty()) {   // test screenshots: one per page change, one a minute on the build page
            static int lastPage = -1; static DWORD lastTick = 0; static int n = 0;
            DWORD now = GetTickCount();
            int key = g_page * 10 + (g_scanOk ? 1 : 0);
            static int pending = -1;
            bool stable = key == pending;   // painted for a full timer tick
            pending = key;
            if (stable && (key != lastPage || (g_page == P_BUILD && now - lastTick > 60000))) {
                wchar_t nm[64]; swprintf(nm, 64, L"\\%02d_page%d.bmp", ++n, g_page);
                MkDirs(snapDir);
                RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                SaveBmp(g_wnd, snapDir + nm);
                lastPage = key; lastTick = now;
            }
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    KillTimer(g_wnd, timer);
    if (g_thread.joinable()) g_thread.detach();
    return 0;
}
