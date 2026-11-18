// SnapRecompSetup.exe: the one-click setup wizard (Win32 + GDI+, drawn in the launcher v2 look: analysis/launcher_v2/d_wizard.html).
//
// The exe carries a zip payload after the PE image: the bundled Python, the builder, the prebuilt runtime objects and NSTool.
// On start it unpacks the payload once into <this exe's folder>\setup\app\<version> (SetupDir() below) and then only drives the builder's
// `setup` command (builder/snapbuild/setup_engine.py), which does the work and prints one JSON object per line.
//
// It never downloads, asks for or points to game files, keys or firmware: the player picks a folder, the engine reads it in place.
//
// One exe (release standard 1): once work\out\SnapRecomp.exe exists this exe starts it with the same arguments (HandOff below).
// Command line: --setup | --game <folder> | --build-dir <folder> | --yes | --repair | --uninstall | --desktop-shortcut |
//               --start-menu-shortcut | --help | --version (SETUP_HELP below); the older /repair, /uninstall, /folder <path> still work.
// Tests only:   /auto (click through with defaults), /hidden, /snapdir <dir>, /noshortcuts, /root <path>,
//               /shot <dir> (render every page to BMP on a private hidden desktop, then exit)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <windows.h>
#include <objidl.h>
using std::min;
using std::max;
#include <gdiplus.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifndef SETUP_VERSION
#define SETUP_VERSION "1.1.0"
#endif
#define SETUP_WIDEN2(x) L##x
#define SETUP_WIDEN(x) SETUP_WIDEN2(x)
#define SETUP_VERSION_W SETUP_WIDEN(SETUP_VERSION)

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

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
    // Portable by default: everything goes into the folder this exe sits in (setup, tools, cache, game and work folders next to it), so
    // deleting or moving that folder deletes or moves the install. A folder that cannot be written (Program Files, a read-only share)
    // falls back to the per-user folder used before.
    static wstring cached;
    if (!cached.empty()) return cached;
    wchar_t me[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, me, MAX_PATH * 4);
    wstring dir(me, n);
    size_t sl = dir.find_last_of(L"\\/");
    dir = sl == wstring::npos ? L"." : dir.substr(0, sl);
    wstring here = dir + L"\\setup";
    bool ok = Exists(here + L"\\settings.json");
    if (!ok) {
        CreateDirectoryW(here.c_str(), nullptr);
        wstring probe = here + L"\\.write_test";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); ok = true; }
        else RemoveDirectoryW(here.c_str());   // only goes when empty
    }
    cached = ok ? here : Env(L"LOCALAPPDATA") + L"\\SnapRecomp\\setup";
    return cached;
}
// The built game: where settings.json says, or (the folder was moved since) work\out\SnapRecomp.exe next to the setup folder.
static wstring BuiltExe() {
    string s = ReadAll(SetupDir() + L"\\settings.json");
    wstring exe = W(JGet(s, "exe"));
    size_t p;
    while ((p = exe.find(L"\\\\")) != wstring::npos) exe.erase(p, 1);
    if (!exe.empty() && Exists(exe)) return exe;
    wstring sd = SetupDir();
    size_t sl = sd.find_last_of(L"\\/");
    if (sl == wstring::npos) return exe;
    wstring moved = sd.substr(0, sl) + L"\\work\\out\\SnapRecomp.exe";
    return Exists(moved) ? moved : exe;
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
// The look follows analysis/launcher_v2/d_wizard.html (the game's Settings screen, redrawn): a sky backdrop, five step tabs, one grey panel,
// a dark hint bar with the Back / Next buttons. Every pixel of the page is drawn in DrawScene() into a back buffer (GDI+, design size 1040 x 600,
// scaled by the monitor DPI); the buttons, check rows and text boxes are real child windows (owner-drawn / coloured) on top, so keyboard use,
// focus and the screen reader keep working and every control keeps its id and its job.
enum Page { P_WELCOME, P_GAME, P_OPTIONS, P_BUILD, P_DONE, P_ERROR, P_MAINT, P_UNINST, P_COUNT };
enum { ID_NEXT = 100, ID_BACK, ID_CANCEL, ID_BROWSE, ID_KEYS, ID_MSLIC, ID_DESK, ID_START, ID_FSR, ID_DLSS, ID_XESS, ID_DETAILS, ID_PLAY, ID_RETRY,
       ID_REPAIR, ID_CHANGE, ID_UNINST, ID_RMTOOLS, ID_RMSAVES, ID_CLOSE, ID_LOG, ID_MSURL };
#define WM_ENGINE (WM_APP + 1)
#define WM_SCANDONE (WM_APP + 2)
#define WM_PREPARED (WM_APP + 3)

enum BtnKind { BK_PRIMARY, BK_BACK, BK_ROUND, BK_HEXPILL, BK_LINK, BK_ROW, BK_PLAY, BK_CHECK };

static HWND g_wnd, g_next, g_back, g_cancel, g_path, g_detect, g_keysBtn, g_chk[16], g_log, g_detBtn, g_msurl;
static HFONT g_fontEdit, g_fontMono;
static HBRUSH g_brCream, g_brDark;
static int g_page = P_WELCOME;
static int g_dpi = 96;
static wstring g_folder, g_keys, g_scanText, g_exePlay, g_err, g_workdir;
static wstring g_titleT, g_subT;                      // the page title and the line under it
static int g_pct10 = 0;                               // build progress in tenths of a percent
static wstring g_stageT, g_etaT;
static bool g_noShortcuts = false;
static bool g_scanOk = false, g_needKeys = false, g_busy = false, g_repair = false, g_auto = false, g_showLog = false, g_uninstMode = false;
static bool g_chkOn[16];
static int g_exitCode = 0;                            // what the process returns (--yes: 0 = done, 1 = failed)
static bool g_yes = false;                            // --yes: click through, close when finished
static bool g_removed = false;                        // the uninstall finished
static Proc g_proc;
static std::mutex g_mu;
static string g_logText;
static string g_doneJson;
static wstring g_scanRoot;     // optional --root for tests
static std::map<HWND, int> g_kind;
static const int LW = 1040, LH = 600;                 // design size of the client area
static const int CHK_ID[8] = {ID_MSLIC, ID_DESK, ID_START, ID_FSR, ID_DLSS, ID_XESS, ID_RMTOOLS, ID_RMSAVES};

static bool ChkGet(int i) { return g_chkOn[i]; }
static void ChkSet(int i, bool on) { g_chkOn[i] = on; if (g_chk[i]) InvalidateRect(g_chk[i], nullptr, FALSE); }
static int D(int v) { return MulDiv(v, g_dpi, 96); }
static void Show(HWND h, bool on) { if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE); }
static void Place(HWND h, int x, int y, int w, int hh) { if (h) MoveWindow(h, D(x), D(y), D(w), D(hh), TRUE); }
static HWND Ctl(int id) { return GetDlgItem(g_wnd, id); }

// ---- drawing helpers (GDI+)
namespace G = Gdiplus;
static ULONG_PTR g_gdip;
static HDC g_sceneDC;
static HBITMAP g_sceneBmp;
static int g_sceneW, g_sceneH;

static G::Color Col(unsigned rgb, int a = 255) { return G::Color((BYTE)a, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb); }
static void RRect(G::GraphicsPath& p, float x, float y, float w, float h, float r) {
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}
static void HexP(G::GraphicsPath& p, float x, float y, float w, float h, float c) {   // the six-sided row shape of the game's Settings screen
    G::PointF pts[6] = {G::PointF(x + c, y), G::PointF(x + w - c, y), G::PointF(x + w, y + h / 2), G::PointF(x + w - c, y + h), G::PointF(x + c, y + h), G::PointF(x, y + h / 2)};
    p.AddPolygon(pts, 6);
}
static void Txt(G::Graphics& g, const wstring& s, float x, float y, float w, float h, float px, int weight, G::Color c, int ha = 0, int va = 0) {
    G::Font f(weight == 0 ? L"Segoe UI" : weight == 1 ? L"Segoe UI Semibold" : L"Segoe UI", px, weight == 2 ? G::FontStyleBold : G::FontStyleRegular, G::UnitPixel);
    G::StringFormat sf;
    sf.SetAlignment(ha == 0 ? G::StringAlignmentNear : ha == 1 ? G::StringAlignmentCenter : G::StringAlignmentFar);
    sf.SetLineAlignment(va == 0 ? G::StringAlignmentNear : G::StringAlignmentCenter);
    sf.SetTrimming(G::StringTrimmingEllipsisCharacter);
    G::SolidBrush b(c);
    g.DrawString(s.c_str(), -1, &f, G::RectF(x, y, w, h), &sf, &b);
}
static float TxtW(G::Graphics& g, const wstring& s, float px, int weight) {
    G::Font f(weight == 0 ? L"Segoe UI" : weight == 1 ? L"Segoe UI Semibold" : L"Segoe UI", px, weight == 2 ? G::FontStyleBold : G::FontStyleRegular, G::UnitPixel);
    G::RectF r;
    g.MeasureString(s.c_str(), -1, &f, G::PointF(0, 0), &r);
    return r.Width;
}
static void Fill(G::Graphics& g, G::GraphicsPath& p, G::Color c) { G::SolidBrush b(c); g.FillPath(&b, &p); }
static void Stroke(G::Graphics& g, G::GraphicsPath& p, G::Color c, float w) { G::Pen pen(c, w); pen.SetLineJoin(G::LineJoinRound); pen.SetStartCap(G::LineCapRound); pen.SetEndCap(G::LineCapRound); g.DrawPath(&pen, &p); }
static void Line(G::Graphics& g, G::Color c, float w, float x0, float y0, float x1, float y1) { G::Pen pen(c, w); pen.SetStartCap(G::LineCapRound); pen.SetEndCap(G::LineCapRound); g.DrawLine(&pen, x0, y0, x1, y1); }
static void Disc(G::Graphics& g, float cx, float cy, float r, G::Color c) { G::SolidBrush b(c); g.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r); }
static void CheckMark(G::Graphics& g, float cx, float cy, float s, G::Color c, float w) {
    G::Pen pen(c, w); pen.SetLineJoin(G::LineJoinRound); pen.SetStartCap(G::LineCapRound); pen.SetEndCap(G::LineCapRound);
    G::PointF p[3] = {G::PointF(cx - s * 0.45f, cy + s * 0.02f), G::PointF(cx - s * 0.1f, cy + s * 0.38f), G::PointF(cx + s * 0.5f, cy - s * 0.32f)};
    g.DrawLines(&pen, p, 3);
}
static void BadgeA(G::Graphics& g, float cx, float cy, float r, const wchar_t* letter, G::Color bg, G::Color fg) {
    Disc(g, cx, cy, r, bg);
    Txt(g, letter, cx - r, cy - r, 2 * r, 2 * r, r * 1.15f, 2, fg, 1, 1);
}
static void Diamonds(G::Graphics& g, float x, float y, float w, float h, G::Color c) {   // the yellow diamond pattern of the selected row
    G::GraphicsPath clip; RRect(clip, x, y, w, h, 14);
    G::Region old; g.GetClip(&old);
    g.SetClip(&clip, G::CombineModeIntersect);
    G::SolidBrush b(c);
    for (float dx = 0; dx < w + 40; dx += 40) {
        G::PointF a[3] = {G::PointF(x + dx, y), G::PointF(x + dx + 20, y + h / 2), G::PointF(x + dx, y + h)};
        g.FillPolygon(&b, a, 3);
        G::PointF c2[3] = {G::PointF(x + dx + 40, y), G::PointF(x + dx + 40, y + h), G::PointF(x + dx + 20, y + h / 2)};
        g.FillPolygon(&b, c2, 3);
    }
    g.SetClip(&old);
}

static void IconShield(G::Graphics& g, float cx, float cy, G::Color c) {
    G::PointF p[6] = {G::PointF(cx - 11, cy - 11), G::PointF(cx, cy - 15), G::PointF(cx + 11, cy - 11), G::PointF(cx + 11, cy + 2), G::PointF(cx, cy + 15), G::PointF(cx - 11, cy + 2)};
    G::Pen pen(c, 2.6f); pen.SetLineJoin(G::LineJoinRound);
    g.DrawPolygon(&pen, p, 6);
    CheckMark(g, cx, cy, 18, c, 2.6f);
}
static void IconDownload(G::Graphics& g, float cx, float cy, G::Color c) {
    Line(g, c, 2.8f, cx, cy - 13, cx, cy + 5);
    Line(g, c, 2.8f, cx - 8, cy - 3, cx, cy + 6);
    Line(g, c, 2.8f, cx + 8, cy - 3, cx, cy + 6);
    Line(g, c, 2.8f, cx - 11, cy + 13, cx + 11, cy + 13);
}
static void IconClock(G::Graphics& g, float cx, float cy, G::Color c) {
    G::Pen pen(c, 2.6f);
    g.DrawEllipse(&pen, cx - 12, cy - 11, 24.f, 24.f);
    Line(g, c, 2.6f, cx, cy + 1, cx, cy - 6);
    Line(g, c, 2.6f, cx, cy + 1, cx + 5, cy + 4);
    Line(g, c, 2.6f, cx - 4, cy - 15, cx + 4, cy - 15);
}
static void IconFolder(G::Graphics& g, float cx, float cy, G::Color c) {
    G::GraphicsPath p;
    p.AddLine(cx - 30, cy - 22, cx - 12, cy - 22); p.AddLine(cx - 12, cy - 22, cx - 6, cy - 14); p.AddLine(cx - 6, cy - 14, cx + 30, cy - 14);
    p.AddLine(cx + 30, cy - 14, cx + 30, cy + 24); p.AddLine(cx + 30, cy + 24, cx - 30, cy + 24); p.CloseFigure();
    Stroke(g, p, c, 3.2f);
    Line(g, c, 3.2f, cx, cy - 4, cx, cy + 14);
    Line(g, c, 3.2f, cx - 8, cy + 7, cx, cy + 15);
    Line(g, c, 3.2f, cx + 8, cy + 7, cx, cy + 15);
}

static const unsigned C_PANEL = 0x6e7180, C_TAB = 0x9a9da7, C_ROW = 0x989899, C_GREEN = 0xb6d291, C_YELLOW = 0xf8dc56, C_ORANGE = 0xf8ab79, C_ORDARK = 0xda743a,
                      C_TEXT = 0x646464, C_RED = 0xd65f55, C_CREAM = 0xf4f2f0, C_WHITE = 0xfcfcfc, C_GD = 0x5f8a62, C_BAR = 0x5e6360, C_DARK = 0x2f313b;

static void StepOf(int page, int* cur, bool* bad) {
    *bad = page == P_ERROR;
    *cur = page == P_WELCOME ? 0 : page == P_GAME ? 1 : page == P_OPTIONS ? 2 : page == P_BUILD || page == P_ERROR ? 3 : page == P_DONE ? 4 : -1;
}

static void DrawTabs(G::Graphics& g) {
    static const wchar_t* names[5] = {L"Welcome", L"Game files", L"Options", L"Build", L"Done"};
    int cur; bool bad; StepOf(g_page, &cur, &bad);
    int n = cur < 0 ? 1 : 5;
    float x0 = 70, total = 900, gap = 6, w = (total - gap * (n - 1)) / n;
    if (n == 1) w = 250;
    for (int i = 0; i < n; i++) {
        float x = x0 + i * (w + gap), y = 34, h = 47;
        bool sel = cur < 0 || i == cur;
        G::GraphicsPath p;
        p.AddArc(x, y, 20.f, 20.f, 180.f, 90.f); p.AddArc(x + w - 20, y, 20.f, 20.f, 270.f, 90.f); p.AddLine(x + w, y + 10, x + w, y + h); p.AddLine(x + w, y + h, x, y + h); p.CloseFigure();
        Fill(g, p, sel ? Col(C_PANEL) : Col(C_TAB, 230));
        float bx = x + w / 2 - (cur < 0 ? 0 : 58), by = y + 23;
        if (cur < 0) { Txt(g, L"Snap Recomp", x, y, w, 46, 20, 1, Col(0xffffff), 1, 1); continue; }
        bool done = i < cur;
        if (sel && bad) { Disc(g, bx - 22, by, 12, Col(C_RED)); Txt(g, L"!", bx - 34, by - 12, 24, 24, 15, 2, Col(0xffffff), 1, 1); }
        else if (done) { Disc(g, bx - 22, by, 12, Col(C_GREEN)); CheckMark(g, bx - 22, by, 14, Col(0x4d6b3a), 2.4f); }
        else if (sel) { Disc(g, bx - 22, by, 12, Col(C_YELLOW)); Txt(g, std::to_wstring(i + 1), bx - 34, by - 12, 24, 24, 14, 2, Col(0x4f3d29), 1, 1); }
        else { Disc(g, bx - 22, by, 12, Col(0xffffff, 70)); Txt(g, std::to_wstring(i + 1), bx - 34, by - 12, 24, 24, 14, 2, Col(0xffffff), 1, 1); }
        Txt(g, names[i], bx - 4, y, w / 2 + 62, 46, 19.5f, 1, Col(sel ? 0xffffff : 0xf4f4f6), 0, 1);
    }
}

static wstring HintOf() {
    switch (g_page) {
    case P_WELCOME: return L"Takes about an hour the first time. You can keep using your PC while it runs.";
    case P_GAME: return L"Your game files stay on this PC. Nothing is uploaded or shared.";
    case P_OPTIONS: return L"Nothing is downloaded until you accept Microsoft's license.";
    case P_BUILD: return L"This can take a while. It is safe to close the window: setup continues where it stopped.";
    case P_DONE: return L"You can change any of this later from the launcher.";
    case P_ERROR: return L"Nothing was lost. Try again continues from the step that stopped.";
    default: return L"Your own game files are never touched.";
    }
}

static void DrawScene(G::Graphics& g) {
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
    // backdrop: a soft sky over green hills, like the blurred game scene behind the game's own menus
    {
        G::LinearGradientBrush sky(G::PointF(0, 0), G::PointF(0, (float)LH), Col(0x9db8c6), Col(0x86a98c));
        g.FillRectangle(&sky, 0, 0, LW, LH);
        G::SolidBrush h1(Col(0x8fb287, 200)), h2(Col(0x7ea57f, 200));
        g.FillEllipse(&h1, -260, 300, 1000, 560);
        g.FillEllipse(&h2, 420, 360, 1000, 480);
        G::SolidBrush haze(Col(0x8f9a90, 70));
        g.FillRectangle(&haze, 0, 0, LW, LH);
    }
    DrawTabs(g);
    // panel with a soft shadow
    for (int i = 6; i >= 1; i--) {
        G::GraphicsPath sh; RRect(sh, 56.f - i, 84.f + i * 1.5f, 928.f + 2 * i, 444.f + i, 14.f + i);
        Fill(g, sh, Col(0x000000, 9));
    }
    G::GraphicsPath pn;
    pn.AddLine(56, 81, 984, 81); pn.AddArc(984.f - 28, 528.f - 28, 28.f, 28.f, 0.f, 90.f); pn.AddArc(56.f, 528.f - 28, 28.f, 28.f, 90.f, 90.f); pn.CloseFigure();
    Fill(g, pn, Col(C_PANEL));
    for (int k = 0; k < 2; k++) {
        float xs[2] = {70, 960};
        G::GraphicsPath d; G::PointF q[4] = {G::PointF(xs[k], 90), G::PointF(xs[k] + 6, 96), G::PointF(xs[k], 102), G::PointF(xs[k] - 6, 96)};
        d.AddPolygon(q, 4); Fill(g, d, Col(0xffffff, 55));
    }
    // bottom hint bar
    { G::SolidBrush b(Col(C_BAR)); g.FillRectangle(&b, 0, 544, LW, LH - 544); }
    { G::GraphicsPath d; G::PointF q[4] = {G::PointF(28, 566), G::PointF(34, 572), G::PointF(28, 578), G::PointF(22, 572)}; d.AddPolygon(q, 4); Fill(g, d, Col(0xffffff, 200)); }
    Txt(g, HintOf(), 44, 544, g_page == P_OPTIONS || g_page == P_WELCOME || g_page == P_BUILD ? 620.f : 600.f, 56, 15.5f, 1, Col(0xffffff), 0, 1);

    const float X = 108, W = 816;
    if (g_page != P_DONE) Txt(g, g_titleT, X, 98, g_page == P_BUILD ? 560.f : W, 46, 34, 1, Col(0xffffff), 0, 1);
    switch (g_page) {
    case P_WELCOME: {
        Txt(g, g_subT, X, 146, W, 44, 16, 1, Col(0xf1f1f5), 0, 0);
        struct Card { const wchar_t *t, *d; int icon; };
        Card cs[3] = {{L"Your files stay here", L"The game is read from your copy and built on this PC. Nothing is uploaded, and setup never asks for or downloads game files.", 0},
                      {L"About 1.7 GB", L"Free build tools (LLVM, Java and Ghidra) and Microsoft's C runtime files. They are downloaded once and reused.", 1},
                      {L"About an hour", L"The first build is the slow one and mostly runs on its own. The game ends up in this folder, next to this program.", 2}};
        for (int i = 0; i < 3; i++) {
            float cw = 253, cx = X + i * (cw + 28.5f), cy = 206, ch = 244;
            G::GraphicsPath p; RRect(p, cx, cy, cw, ch, 18);
            for (int s = 4; s >= 1; s--) { G::GraphicsPath sh; RRect(sh, cx - s, cy + s * 1.5f, cw + 2 * s, ch + s, 18 + s); Fill(g, sh, Col(0x000000, 8)); }
            Fill(g, p, Col(C_CREAM));
            Disc(g, cx + 50, cy + 52, 28, Col(C_YELLOW));
            if (cs[i].icon == 0) IconShield(g, cx + 50, cy + 52, Col(0x3b3835));
            else if (cs[i].icon == 1) IconDownload(g, cx + 50, cy + 52, Col(0x3b3835));
            else IconClock(g, cx + 50, cy + 52, Col(0x3b3835));
            Txt(g, cs[i].t, cx + 22, cy + 96, cw - 40, 30, 21.5f, 2, Col(0x4a4745), 0, 0);
            Txt(g, cs[i].d, cx + 22, cy + 130, cw - 40, 108, 14.5f, 1, Col(0x77726f), 0, 0);
        }
        G::GraphicsPath r; HexP(r, X, 462, W, 46, 22); Fill(g, r, Col(C_ROW));
        Txt(g, L"Disk space", X + 40, 462, 200, 46, 19.5f, 1, Col(0xf7f7f7), 0, 1);
        Txt(g, L"for the tools, the game files and the build", X + 168, 462, 360, 46, 13.5f, 1, Col(0xffffff, 190), 0, 1);
        G::GraphicsPath v; HexP(v, X + W - 224, 468, 204, 34, 14); Fill(g, v, Col(C_WHITE));
        Txt(g, L"About 30 GB free", X + W - 224, 468, 204, 34, 17, 1, Col(C_TEXT), 1, 1);
        break;
    }
    case P_GAME: {
        Txt(g, g_subT, X, 146, W, 28, 16, 1, Col(0xf1f1f5), 0, 0);
        // the drop zone
        G::GraphicsPath z; RRect(z, X, 194, 340, 306, 22);
        Fill(g, z, Col(0xffffff, 14));
        G::Pen dash(Col(0xffffff, 170), 2.6f); dash.SetDashStyle(G::DashStyleDash); g.DrawPath(&dash, &z);
        IconFolder(g, X + 170, 244, Col(0xffffff));
        Txt(g, L"Drop your game folder here", X + 10, 286, 320, 30, 21, 2, Col(0xffffff), 1, 0);
        Txt(g, L"Either a folder with the game already extracted (main, sdk and romfs), or a folder with your base game file, its updates and your prod.keys.",
            X + 28, 320, 284, 100, 14, 1, Col(0xffffff, 215), 1, 0);
        // the folder and what setup found
        Txt(g, L"Your folder", 476, 192, 300, 22, 15, 1, Col(0xffffff, 215), 0, 0);
        G::GraphicsPath pe; RRect(pe, 476, 216, 448, 38, 10); Fill(g, pe, Col(C_CREAM));
        Txt(g, L"What setup found", 476, 266, 300, 22, 15, 1, Col(0xffffff, 215), 0, 0);
        G::GraphicsPath de; RRect(de, 476, 290, 448, 176, 14); Fill(g, de, Col(C_CREAM));
        break;
    }
    case P_OPTIONS: {
        Txt(g, g_subT, X, 146, W, 44, 16, 1, Col(0xf1f1f5), 0, 0);
        break;
    }
    case P_BUILD: {
        wchar_t pc[16]; swprintf(pc, 16, L"%d%%", g_pct10 / 10);
        Txt(g, pc, X + W - 240, 92, 240, 44, 34, 2, Col(0xffffff), 2, 1);
        Txt(g, g_etaT, X + W - 300, 136, 300, 24, 15, 1, Col(0xf1f1f5), 2, 0);
        G::GraphicsPath tr; HexP(tr, X, 168, W, 48, 22); Fill(g, tr, Col(0x4a4d5a));
        float fw = W * (g_pct10 / 1000.f);
        if (fw > 1) {
            G::Region old; g.GetClip(&old);
            g.SetClip(&tr, G::CombineModeIntersect);
            g.IntersectClip(G::RectF(X, 168, fw, 48));
            Fill(g, tr, Col(g_err.empty() ? C_GREEN : C_RED));
            g.SetClip(&old);
        }
        Txt(g, g_stageT, X, 224, W, 28, 18, 1, Col(0xffffff), 0, 0);
        Txt(g, L"You can leave this running. It continues where it stopped if the PC restarts: run setup again.", X, 254, W, 24, 14.5f, 0, Col(0xffffff, 200), 0, 0);
        G::GraphicsPath c; RRect(c, X, 294, W, 214, 16);
        if (g_showLog) { Fill(g, c, Col(C_DARK)); Txt(g, L"Log", X + 20, 296, 200, 36, 16, 2, Col(0xffffff), 0, 1); }
        else {
            G::GraphicsPath cc; RRect(cc, X, 294, W, 96, 16); Fill(g, cc, Col(C_CREAM));
            Txt(g, L"Setup works through the steps by itself. Nothing needs you until it finishes or stops.", X + 24, 306, W - 48, 44, 15.5f, 1, Col(0x5b5754), 0, 0);
        }
        break;
    }
    case P_DONE: {
        Disc(g, X + 48, 152, 48, Col(C_GREEN));
        G::Pen ring(Col(0xffffff), 4.f); g.DrawEllipse(&ring, X + 48 - 33, 152.f - 33, 66.f, 66.f);
        CheckMark(g, X + 48, 152, 44, Col(0xffffff), 6.f);
        Txt(g, g_titleT, X + 120, 104, 700, 54, 40, 1, Col(0xffffff), 0, 1);
        Txt(g, L"The game program was built on this PC.", X + 120, 160, 700, 26, 17, 1, Col(0xf1f1f5), 0, 0);
        G::GraphicsPath c; RRect(c, X, 338, W, 118, 16); Fill(g, c, Col(C_CREAM));
        Txt(g, g_subT, X + 26, 352, W - 52, 92, 16.5f, 1, Col(0x5b5754), 0, 0);
        break;
    }
    case P_ERROR: {
        G::GraphicsPath c; RRect(c, X, 168, W, 150, 16); Fill(g, c, Col(C_CREAM));
        Txt(g, L"What happened", X + 26, 180, 400, 28, 20, 2, Col(0xb8483f), 0, 0);
        Txt(g, g_err, X + 26, 212, W - 52, 100, 16.5f, 1, Col(0x4f4b49), 0, 0);
        if (g_showLog) { G::GraphicsPath l; RRect(l, X, 376, W, 132, 14); Fill(g, l, Col(C_DARK)); }
        break;
    }
    case P_MAINT: Txt(g, g_subT, X, 146, W, 28, 16, 1, Col(0xf1f1f5), 0, 0); break;
    case P_UNINST: Txt(g, g_subT, X, 146, W, 44, 16, 1, Col(0xf1f1f5), 0, 0); break;
    }
}

static void EnsureScene() {
    RECT r; GetClientRect(g_wnd, &r);
    if (g_sceneBmp && g_sceneW == r.right && g_sceneH == r.bottom) return;
    if (g_sceneBmp) { DeleteObject(g_sceneBmp); }
    if (!g_sceneDC) { HDC dc = GetDC(g_wnd); g_sceneDC = CreateCompatibleDC(dc); ReleaseDC(g_wnd, dc); }
    BITMAPINFO bi{}; bi.bmiHeader = {sizeof bi.bmiHeader, r.right, -r.bottom, 1, 32, BI_RGB};
    void* bits;
    g_sceneBmp = CreateDIBSection(g_sceneDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(g_sceneDC, g_sceneBmp);
    g_sceneW = r.right; g_sceneH = r.bottom;
}
static void Rescene() {
    if (!g_wnd) return;
    EnsureScene();
    {
        G::Graphics g(g_sceneDC);
        g.ScaleTransform(g_dpi / 96.f, g_dpi / 96.f);
        DrawScene(g);
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

// ---- the owner-drawn buttons
struct RowInfo { const wchar_t *title, *sub; };
static const RowInfo kRows[8] = {
    {L"Microsoft C++ runtime and Windows SDK", L"I accept Microsoft's license for the files setup downloads"},
    {L"Desktop shortcut", L"Add a shortcut to the desktop"},
    {L"Start menu shortcut", L"Add a shortcut to the Start menu"},
    {L"AMD FSR upscaling", L"Optional. AMD's license applies, downloaded from AMD"},
    {L"NVIDIA DLSS", L"Optional. NVIDIA's license applies, downloaded from NVIDIA"},
    {L"Intel XeSS", L"Optional. Intel's license applies, downloaded from Intel"},
    {L"Also delete the downloaded build tools", L"About 5 GB"},
    {L"Also delete my saves and settings", L"Your saves and your launcher settings"}};

static void DrawButton(DRAWITEMSTRUCT* d) {
    HWND h = d->hwndItem;
    auto it = g_kind.find(h);
    if (it == g_kind.end()) return;
    int kind = it->second;
    RECT wr; GetWindowRect(h, &wr);
    POINT pt{wr.left, wr.top}; ScreenToClient(g_wnd, &pt);
    int pw = d->rcItem.right, ph = d->rcItem.bottom;
    // a double-buffered copy of the scene behind the button, then the button on top (the shapes are not rectangles)
    HDC mem = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, pw, ph);
    HGDIOBJ old = SelectObject(mem, bm);
    BitBlt(mem, 0, 0, pw, ph, g_sceneDC, pt.x, pt.y, SRCCOPY);
    {
        G::Graphics g(mem);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
        float s = g_dpi / 96.f;
        g.ScaleTransform(s, s);
        float W = pw / s, H = ph / s;
        bool dis = (d->itemState & ODS_DISABLED) != 0, down = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0;
        wchar_t tx[160]; GetWindowTextW(h, tx, 160);
        int ix = -1;
        for (int i = 0; i < 8; i++) if (g_chk[i] == h) ix = i;
        G::GraphicsPath shape;
        switch (kind) {
        case BK_PRIMARY: {
            G::PointF p[5] = {G::PointF(0, 0), G::PointF(W - 24, 0), G::PointF(W, H / 2), G::PointF(W - 24, H), G::PointF(0, H)};
            shape.AddPolygon(p, 5);
            Fill(g, shape, dis ? Col(0xbab8b8) : down ? Col(0xe99a66) : Col(C_ORANGE));
            BadgeA(g, 26, H / 2, 13, L"A", dis ? Col(0xe9e7e7) : Col(0xfdeee2), dis ? Col(0xa5a2a2) : Col(C_ORDARK));
            Txt(g, tx, 48, 0, W - 76, H, 20, 2, dis ? Col(0xf3f2f2) : Col(0xffffff), 1, 1);
            break;
        }
        case BK_BACK: {
            G::PointF p[5] = {G::PointF(0, H / 2), G::PointF(24, 0), G::PointF(W, 0), G::PointF(W, H), G::PointF(24, H)};
            shape.AddPolygon(p, 5);
            Fill(g, shape, down ? Col(0x868a95) : Col(C_TAB));
            BadgeA(g, 44, H / 2, 13, L"B", Col(0xf4f2f0), Col(C_ORDARK));
            Txt(g, tx, 62, 0, W - 70, H, 20, 2, Col(0xffffff), 0, 1);
            break;
        }
        case BK_ROUND: {
            RRect(shape, 0, 0, W, H, H / 2);
            Fill(g, shape, dis ? Col(0xd9d7d6) : down ? Col(0xe2dfdc) : Col(C_CREAM));
            BadgeA(g, H / 2, H / 2, H / 2 - 7, L"A", Col(C_ORANGE), Col(0xffffff));
            Txt(g, tx, H - 2, 0, W - H, H, 17.5f, 2, Col(0x5b5754), 1, 1);
            break;
        }
        case BK_HEXPILL: {
            HexP(shape, 0, 0, W, H, 12);
            Fill(g, shape, down ? Col(0xe6e6e8) : Col(C_WHITE));
            Txt(g, tx, 0, 0, W, H, 17, 2, Col(C_TEXT), 1, 1);
            break;
        }
        case BK_LINK: {
            bool dark = g_page != P_ERROR && g_showLog ? true : (g_page == P_ERROR);
            Txt(g, tx, 0, 0, W, H, 15, 2, dark ? Col(0xffffff, down ? 255 : 215) : Col(0x5b5754), 0, 1);
            float tw = TxtW(g, tx, 15, 2);
            Line(g, dark ? Col(0xffffff, 150) : Col(0x5b5754, 150), 1.2f, 1, H / 2 + 11, tw - 4, H / 2 + 11);
            RRect(shape, 0, 0, W, H, 6);
            break;
        }
        case BK_ROW: {
            HexP(shape, 0, 0, W, H, 22);
            Fill(g, shape, dis ? Col(0xa9aaae) : down ? Col(0x8a8a8b) : Col(C_ROW));
            Txt(g, tx, 40, 0, W - 80, H, 21, 1, Col(0xf7f7f7), 0, 1);
            Line(g, Col(0xffffff, 220), 3.f, W - 40, H / 2 - 7, W - 32, H / 2);
            Line(g, Col(0xffffff, 220), 3.f, W - 32, H / 2, W - 40, H / 2 + 7);
            break;
        }
        case BK_PLAY: {
            RRect(shape, 1.5f, 1.5f, W - 3, H - 3, 16);
            Fill(g, shape, down ? Col(0xeacb44) : Col(C_YELLOW));
            Diamonds(g, 1.5f, 1.5f, W - 3, H - 3, Col(0xf1b93a, down ? 60 : 105));
            Stroke(g, shape, Col(C_ORANGE), 3.f);
            float r = H > 80 ? 24.f : 19.f;
            G::GraphicsPath bd; RRect(bd, 24, H / 2 - r, 2 * r, 2 * r, 8); Fill(g, bd, Col(C_CREAM));
            Txt(g, L"A", 24, H / 2 - r, 2 * r, 2 * r, r * 1.15f, 2, Col(C_ORDARK), 1, 1);
            Txt(g, tx, 24 + 2 * r + 20, 0, W - 300, H, H > 80 ? 40.f : 30.f, 2, Col(0x4f3d29), 0, 1);
            Txt(g, L"Starts the game", W - 300, 0, 276, H, 15, 1, Col(0x5b4a30), 2, 1);
            break;
        }
        case BK_CHECK: {
            bool on = ix >= 0 && g_chkOn[ix];
            HexP(shape, 0, 0, W, H, 20);
            Fill(g, shape, on ? Col(C_GREEN) : Col(C_ROW));
            float cy = H / 2;
            if (on) { Disc(g, 38, cy, 13, Col(0xffffff)); CheckMark(g, 38, cy, 15, Col(C_GD), 3.f); }
            else { G::Pen p(Col(0xffffff, 200), 2.4f); g.DrawEllipse(&p, 38 - 12.f, cy - 12.f, 24.f, 24.f); }
            const RowInfo& ri = kRows[ix < 0 ? 0 : ix];
            float ty = H >= 54 ? 7.f : 4.f;
            Txt(g, ri.title, 64, ty, W - 270, 26, 19.5f, 1, Col(0xffffff), 0, 0);
            Txt(g, ri.sub, 64, ty + 25, W - 150, 20, 13.5f, 1, Col(0xffffff, on ? 235 : 195), 0, 0);
            if (ix == 0) {
                float tw = TxtW(g, ri.title, 19.5f, 1);
                G::GraphicsPath b; RRect(b, 64 + tw + 14, ty + 3, 78, 20, 10); Fill(g, b, Col(C_ORANGE));
                Txt(g, L"REQUIRED", 64 + tw + 14, ty + 3, 78, 20, 11.5f, 2, Col(0xffffff), 1, 1);
            }
            break;
        }
        }
        if (focus && kind != BK_LINK) { G::Pen fp(Col(0xffffff, 230), 2.2f); fp.SetDashStyle(G::DashStyleDot); g.DrawPath(&fp, &shape); }
        else if (focus) { G::Pen fp(Col(0xffffff, 200), 1.f); fp.SetDashStyle(G::DashStyleDot); g.DrawPath(&fp, &shape); }
    }
    BitBlt(d->hDC, 0, 0, pw, ph, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
}

static HWND MkBtn(const wchar_t* text, int kind, int id) {
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
    g_kind[c] = kind;
    return c;
}
static HWND MkEdit(DWORD style, HFONT f) {
    HWND c = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | style, 0, 0, 10, 10, g_wnd, nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void AppendLog(const string& s) {
    std::lock_guard<std::mutex> g(g_mu);
    g_logText += s + "\r\n";
    if (g_logText.size() > 60000) g_logText.erase(0, g_logText.size() - 40000);
}

// the primary (Next) and Back slots of the bottom bar
static void PlacePrimary(HWND h) { Place(h, 852, 548, 178, 48); }
static void PlaceBack(HWND h) { Place(h, 684, 548, 158, 48); }

static void Layout() {
    g_kind[Ctl(ID_CLOSE)] = g_page == P_ERROR ? BK_BACK : BK_PRIMARY;
    // every control is hidden, then the ones of this page are placed (design units; Place() scales them)
    for (HWND c : g_chk) Show(c, false);
    for (HWND c : {g_msurl, g_path, g_detect, g_keysBtn, g_log, g_detBtn, g_back, g_cancel, g_next}) Show(c, false);
    for (int id : {ID_BROWSE, ID_PLAY, ID_RETRY, ID_REPAIR, ID_CHANGE, ID_UNINST, ID_CLOSE}) Show(Ctl(id), false);
    SetWindowTextW(g_next, L"Next");
    EnableWindow(g_next, TRUE);
    wstring t, b;
    PlacePrimary(g_next);
    switch (g_page) {
    case P_WELCOME:
        t = L"Let's set up Snap Recomp";
        b = L"This builds a native PC version of New Pokémon Snap from your own copy of the game. You only do it once.";
        SetWindowTextW(g_next, L"Start");
        Show(g_next, true);
        break;
    case P_GAME:
        t = L"Choose your game files";
        b = L"Setup picks the newest update and ignores older ones. You can also drop the folder here.";
        Place(Ctl(ID_BROWSE), 108 + 70, 448, 200, 44);
        Place(g_path, 486, 222, 428, 26);
        Place(g_detect, 488, 298, 424, 160);
        Place(g_keysBtn, 476, 470, 230, 38);
        Show(g_path, true); Show(g_detect, true); Show(Ctl(ID_BROWSE), true);
        Show(g_keysBtn, g_needKeys); PlaceBack(g_back); Show(g_back, true); Show(g_next, true);
        EnableWindow(g_next, g_scanOk && !g_busy);
        break;
    case P_OPTIONS:
        t = L"Ready to build";
        b = L"Setup downloads the free build tools (LLVM, Java and Ghidra, about 1.7 GB) and Microsoft's C runtime files, then builds the game program. "
            L"Please read and accept Microsoft's license to continue.";
        for (int i = 0; i < 6; i++) { Place(g_chk[i], 108, 204 + i * 51, 816, 46); Show(g_chk[i], true); }
        Place(g_msurl, 108 + 816 - 146, 204 + 6, 112, 34);
        Show(g_msurl, true); PlaceBack(g_back); Show(g_back, true); Show(g_next, true);
        SetWindowTextW(g_next, L"Build");
        EnableWindow(g_next, ChkGet(0));
        break;
    case P_BUILD:
        t = g_repair ? L"Repairing" : L"Building Snap Recomp";
        b = L"";
        Place(g_detBtn, 108 + 22, 294 + 106 - 40, 150, 28);
        PlaceBack(g_cancel);
        SetWindowTextW(g_detBtn, g_showLog ? L"Hide details" : L"Show details");
        if (g_showLog) { Place(g_detBtn, 108 + 816 - 170, 294 + 4, 150, 28); Place(g_log, 124, 336, 784, 164); }
        else Place(g_detBtn, 108 + 22, 294 + 52, 150, 28);
        Show(g_detBtn, true); Show(g_log, g_showLog); Show(g_cancel, true);
        SetWindowTextW(g_next, L"Working");
        EnableWindow(g_next, FALSE);
        Show(g_next, true);
        break;
    case P_DONE:
        t = L"Snap Recomp is ready";
        b = L"The game lives in the folder you ran this from: move or delete that folder to move or remove it. The first start translates the game's shaders and can take 5 to 10 minutes. Later starts are quick.";
        Place(Ctl(ID_PLAY), 108, 224, 816, 92);
        PlacePrimary(Ctl(ID_CLOSE));
        Show(Ctl(ID_PLAY), true); Show(Ctl(ID_CLOSE), true);
        break;
    case P_ERROR:
        t = L"Setup stopped";
        b = g_err;
        Place(g_detBtn, 108, 330, 160, 30);
        Place(g_log, 122, 384, 788, 118);
        SetWindowTextW(g_detBtn, g_showLog ? L"Hide details" : L"Show details");
        PlacePrimary(Ctl(ID_RETRY)); PlaceBack(Ctl(ID_CLOSE));
        Show(Ctl(ID_RETRY), true); Show(Ctl(ID_CLOSE), true); Show(g_detBtn, true); Show(g_log, g_showLog);
        break;
    case P_MAINT:
        t = L"Snap Recomp is installed";
        b = L"Choose what to do.";
        Place(Ctl(ID_PLAY), 108, 196, 816, 76);
        Place(Ctl(ID_REPAIR), 108, 286, 816, 56);
        Place(Ctl(ID_CHANGE), 108, 352, 816, 56);
        Place(Ctl(ID_UNINST), 108, 418, 816, 56);
        PlacePrimary(Ctl(ID_CLOSE));
        for (int id : {ID_PLAY, ID_REPAIR, ID_CHANGE, ID_UNINST, ID_CLOSE}) Show(Ctl(id), true);
        break;
    case P_UNINST:
        t = L"Remove Snap Recomp";
        b = L"This removes the work folder, the game data setup extracted and any shortcuts it made. Your own game files are never touched.";
        for (int i : {6, 7}) { Place(g_chk[i], 108, 204 + (i - 6) * 60, 816, 50); Show(g_chk[i], true); }
        SetWindowTextW(g_next, L"Remove");
        PlaceBack(g_cancel);
        Show(g_cancel, true); Show(g_next, true);
        break;
    }
    g_titleT = t; g_subT = b;
    if (g_page == P_ERROR) g_subT = L"";
    Rescene();
    for (int id : {ID_PLAY, ID_RETRY, ID_REPAIR, ID_CHANGE, ID_UNINST, ID_CLOSE, ID_BROWSE})   // a button that moves or reappears must repaint at once
        if (HWND c = Ctl(id)) if (IsWindowVisible(c)) RedrawWindow(c, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

static void SetPage(int p) { g_page = p; Layout(); }

static void SetProgress(int pct10, const wstring& stage, const wstring& eta) {
    g_pct10 = pct10; g_stageT = stage; g_etaT = eta;
    if (g_page == P_BUILD) Rescene();
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
    j += string("\"shortcuts\": {\"desktop\": ") + (ChkGet(1) ? "true" : "false") +
         ", \"start\": " + (ChkGet(2) ? "true" : "false") + "}, ";
    if (g_noShortcuts) {
        size_t p = j.rfind("\"shortcuts\"");
        size_t e = j.find("}, ", p);
        j.replace(p, e - p + 1, "\"shortcuts\": {\"desktop\": false, \"start\": false}");
    }
    string ups, acc;
    const char* names[] = {"fsr", "dlss", "xess"};
    for (int i = 0; i < 3; i++)
        if (ChkGet(3 + i)) {
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
        if (g_yes && !g_scanOk) {   // nobody is there to pick another folder
            fprintf(stderr, "SnapRecomp setup: %s\n", text.c_str());
            g_exitCode = 1;
            PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        }
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
        if (eta > 0) swprintf(e, 96, L"About %d min left", (eta + 59) / 60);
        else e[0] = 0;
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
        if (g_uninstMode) {
            g_doneJson = l; g_removed = true;
            g_titleT = L"Removed"; g_subT = L"Snap Recomp was removed. Your game files were not touched.";
            Show(g_cancel, false); Show(g_next, false); Show(g_chk[6], false); Show(g_chk[7], false);
            g_kind[Ctl(ID_CLOSE)] = BK_PRIMARY; PlacePrimary(Ctl(ID_CLOSE)); Show(Ctl(ID_CLOSE), true);
            Rescene();
            return;
        }
        g_doneJson = l;
        g_exePlay = W(JGet(l, "exe"));
        SetPage(P_DONE);
    }
}

static void PlayNow() {
    wstring exe = g_exePlay;
    if (exe.empty() || !Exists(exe)) exe = BuiltExe();
    if (exe.empty() || !Exists(exe)) { MessageBoxW(g_wnd, L"The game program was not found. Choose Repair.", L"Snap Recomp", MB_ICONWARNING); return; }
    size_t sl = exe.rfind(L'\\');
    ShellExecuteW(g_wnd, L"open", exe.c_str(), nullptr, exe.substr(0, sl).c_str(), SW_SHOWNORMAL);
    DestroyWindow(g_wnd);   // the setup is finished: from now on this exe starts the game
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
            if (ChkGet(6)) a += L" --remove-tools";
            if (ChkGet(7)) a += L" --remove-saves";
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
    case ID_MSLIC: case ID_FSR: case ID_DLSS: case ID_XESS: case ID_DESK: case ID_START: case ID_RMTOOLS: case ID_RMSAVES:
        for (int i = 0; i < 8; i++) if (CHK_ID[i] == id) ChkSet(i, !ChkGet(i));   // the rows are owner-drawn: the check state is kept here
        if (g_page == P_OPTIONS) EnableWindow(g_next, ChkGet(0));
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
    case WM_DRAWITEM: DrawButton((DRAWITEMSTRUCT*)l); return TRUE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_sceneDC) BitBlt(dc, 0, 0, g_sceneW, g_sceneH, g_sceneDC, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (g_sceneDC) BitBlt((HDC)w, 0, 0, g_sceneW, g_sceneH, g_sceneDC, 0, 0, SRCCOPY);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {   // the text boxes sit on a cream card (folder and scan text) or a dark one (log)
        HWND c = (HWND)l; HDC dc = (HDC)w;
        if (c == g_log) { SetTextColor(dc, RGB(0xe6, 0xe6, 0xea)); SetBkColor(dc, RGB(0x2f, 0x31, 0x3b)); return (LRESULT)g_brDark; }
        if (c == g_detect || c == g_path) { SetTextColor(dc, RGB(0x4a, 0x47, 0x45)); SetBkColor(dc, RGB(0xf4, 0xf2, 0xf0)); return (LRESULT)g_brCream; }
        break;
    }
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
    g_fontEdit = CreateFontW(-D(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontMono = CreateFontW(-D(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g_brCream = CreateSolidBrush(RGB(0xf4, 0xf2, 0xf0));
    g_brDark = CreateSolidBrush(RGB(0x2f, 0x31, 0x3b));
    g_path = MkEdit(ES_READONLY | ES_AUTOHSCROLL, g_fontEdit);
    MkBtn(L"Choose folder...", BK_ROUND, ID_BROWSE);
    g_detect = MkEdit(WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, g_fontEdit);
    g_keysBtn = MkBtn(L"Choose prod.keys...", BK_ROUND, ID_KEYS);
    const wchar_t* chk[] = {L"I accept Microsoft's license for the C runtime and Windows SDK files setup downloads",
                            L"Add a shortcut to the desktop", L"Add a shortcut to the Start menu",
                            L"Also install AMD FSR upscaling (AMD's license applies, downloaded from AMD)",
                            L"Also install NVIDIA DLSS (NVIDIA's license applies, downloaded from NVIDIA)",
                            L"Also install Intel XeSS (Intel's license applies, downloaded from Intel)",
                            L"Also delete the downloaded build tools (about 5 GB)", L"Also delete my saves and settings"};
    for (int i = 0; i < 8; i++) g_chk[i] = MkBtn(chk[i], BK_CHECK, CHK_ID[i]);
    ChkSet(1, false);   // shortcuts are off unless asked for: a portable folder leaves nothing outside itself
    ChkSet(2, false);
    g_msurl = MkBtn(L"View", BK_HEXPILL, ID_MSURL);
    g_detBtn = MkBtn(L"Show details", BK_LINK, ID_DETAILS);
    g_log = MkEdit(WS_VSCROLL | ES_MULTILINE | ES_READONLY, g_fontMono);
    MkBtn(L"Play now", BK_PLAY, ID_PLAY);
    MkBtn(L"Try again", BK_PRIMARY, ID_RETRY);
    MkBtn(L"Repair", BK_ROW, ID_REPAIR);
    MkBtn(L"Change game folder", BK_ROW, ID_CHANGE);
    MkBtn(L"Uninstall", BK_ROW, ID_UNINST);
    MkBtn(L"Close", BK_PRIMARY, ID_CLOSE);
    g_back = MkBtn(L"Back", BK_BACK, ID_BACK);
    g_next = MkBtn(L"Next", BK_PRIMARY, ID_NEXT);
    g_cancel = MkBtn(L"Cancel", BK_BACK, ID_CANCEL);
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
                             L"Keys: found next to the game files (read from there, never copied).\r\n\r\nDisk space:\r\n   Drive D: needs up to 30.0 GB while building and 22.0 GB afterwards; 4.8 TB free\r\n"
                             L"The extracted game data stays on disk because the game reads it while it runs, so it cannot be deleted after the build.");
    g_scanOk = true;
    AppendLog("Using n-new_pokemon_snap_v262144.nsp (version 2.0.1) with n-pokemonsnap.xci.");
    AppendLog("step elf done in 3s");
    AppendLog("step maps");
    g_err = L"Not enough free space on drive D: the build needs about 12.0 GB for a short time and 3.1 GB is free. Choose another drive.";
    struct S { int page; const wchar_t* name; bool ticked, log; };
    S shots[] = {{P_WELCOME, L"1_welcome", false, false}, {P_GAME, L"2_game_files", false, false}, {P_OPTIONS, L"3_options", false, false},
                 {P_OPTIONS, L"3b_options_accepted", true, false}, {P_BUILD, L"4_build", false, true}, {P_BUILD, L"4b_build_compact", false, false},
                 {P_DONE, L"5_done", false, false}, {P_ERROR, L"6_error", false, true}, {P_ERROR, L"6b_error_compact", false, false},
                 {P_MAINT, L"7_maintenance", false, false}, {P_UNINST, L"8_uninstall", false, false}};
    for (auto& s : shots) {
        ChkSet(0, s.ticked);
        g_showLog = s.log;
        if (s.page == P_BUILD) SetProgress(412, L"Compiling the game code (step 5 of 8)", L"About 38 min left");
        SetPage(s.page);
        if (s.log) RefreshLog();
        PumpFor(250);
        RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        PumpFor(100);
        SaveBmp(g_wnd, dir + L"\\" + s.name + L".bmp");
    }
    DestroyWindow(g_wnd);
}

// ------------------------------------------------------------------------------------------------ one exe: hand off to the built game
// Release standard 1: the player downloads and starts one exe. Before a build exists it is the setup wizard; once the build is there
// (work\out\SnapRecomp.exe with its config.json, in this exe's folder or in the --build-dir folder) it starts that program with the same
// arguments, the same standard handles and the same environment, waits for it, and returns its exit code. The game runs in a job that
// dies with this process, so a launcher that stops this exe stops the game too; a restart the game does itself stays in the job and is
// waited for, and its background shader preparation leaves the job on purpose (CREATE_BREAKAWAY_FROM_JOB in host/shaderprep.cpp).
static wstring QuoteArg(const wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == wstring::npos) return a;
    wstring o = L"\"";
    for (size_t i = 0;; i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { i++; bs++; }
        if (i == a.size()) { o.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; }
        else { o.append(bs, L'\\'); o += a[i]; }
    }
    return o + L"\"";
}

// The built program for this install: <build dir>\work\out when --build-dir named one, else where settings.json says, else next to this exe.
static wstring FindBuilt(const wstring& buildDir) {
    wstring exe;
    if (!buildDir.empty()) exe = buildDir + L"\\work\\out\\SnapRecomp.exe";
    else {
        exe = BuiltExe();
        if (exe.empty() || !Exists(exe)) {   // no settings.json (a folder copied without its setup folder): the default place
            wstring d = g_exe.substr(0, g_exe.find_last_of(L"\\/"));
            exe = d + L"\\work\\out\\SnapRecomp.exe";
        }
    }
    if (!Exists(exe)) return L"";
    wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    if (!Exists(dir + L"\\config.json")) return L"";          // a build that was never finished
    wchar_t full[MAX_PATH * 4];
    if (GetFullPathNameW(exe.c_str(), MAX_PATH * 4, full, nullptr) && _wcsicmp(full, g_exe.c_str()) == 0) return L"";   // never start ourselves
    return exe;
}

// Starts the built program and waits. Returns false when it could not be started (the wizard then opens); *code = its exit code.
static bool HandOff(const wstring& exe, const std::vector<wstring>& args, int* code) {
    wstring cmd = QuoteArg(exe);
    for (auto& a : args) cmd += L" " + QuoteArg(a);
    wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    SetEnvironmentVariableW(L"SNAP_TOP_EXE", g_exe.c_str());   // the exe the player starts (the game's updater and shortcuts name this one)
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si{sizeof si};
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE), hout = GetStdHandle(STD_OUTPUT_HANDLE), herr = GetStdHandle(STD_ERROR_HANDLE);
    auto usable = [](HANDLE h) { return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN; };
    BOOL inherit = FALSE;
    if (usable(hout) || usable(herr) || usable(hin)) {         // a launcher's pipes or a redirect: the game writes to the same ones
        for (HANDLE h : {hin, hout, herr}) if (usable(h)) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = usable(hin) ? hin : nullptr; si.hStdOutput = usable(hout) ? hout : nullptr; si.hStdError = usable(herr) ? herr : nullptr;
        inherit = TRUE;
    }
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, inherit, CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi)) {
        if (job) CloseHandle(job);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    while (job) {   // the game restarted itself (a settings change, an update): stay until the job is empty, so nothing is cut off
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai{};
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ai, sizeof ai, nullptr) || ai.ActiveProcesses == 0) break;
        Sleep(250);
    }
    if (job) CloseHandle(job);
    *code = (int)ec;
    return true;
}

static void SayLine(const wstring& text) {   // to the caller's pipe or console when there is one, else a message box
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    bool have = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    if (!have && AttachConsole(ATTACH_PARENT_PROCESS)) { h = GetStdHandle(STD_OUTPUT_HANDLE); have = h && h != INVALID_HANDLE_VALUE; }
    if (!have) { MessageBoxW(nullptr, text.c_str(), L"Snap Recomp", MB_OK); return; }
    string u = U(text + L"\r\n");
    DWORD n;
    WriteFile(h, u.data(), (DWORD)u.size(), &n, nullptr);
}

static const wchar_t* const SETUP_HELP =
    L"Snap Recomp " SETUP_VERSION_W L"\r\n"
    L"\r\n"
    L"Started with no build in its folder, this exe is the setup: it builds the game from your own game files.\r\n"
    L"Once the build is there it starts the game, and every flag it does not list below goes to the game\r\n"
    L"(--play, --fullscreen, --data-dir and the rest: docs/command-line.md, or --help after the build).\r\n"
    L"\r\n"
    L"  --setup               open the setup even when the game is built (repair, change game files, remove)\r\n"
    L"  --game <folder>       the folder with your game files (the setup opens with it chosen)\r\n"
    L"  --build-dir <folder>  where the build goes and is looked for (default: this exe's folder)\r\n"
    L"  --yes                 no questions: accept Microsoft's C runtime license, use the defaults, close when done\r\n"
    L"                        (exit code 0 = built, 1 = failed)\r\n"
    L"  --repair              rebuild from the game files chosen before\r\n"
    L"  --uninstall           remove the build (with --yes: without asking; saves and tools are kept)\r\n"
    L"  --desktop-shortcut    with --yes: also make a desktop shortcut\r\n"
    L"  --start-menu-shortcut with --yes: also make a Start menu shortcut\r\n"
    L"  --help, --version     this text, the version\r\n";

// ------------------------------------------------------------------------------------------------ main
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t me[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, me, MAX_PATH * 4);
    g_exe = me;
    wstring preFolder, shotDir;
    int startPage = -1;
    bool repair = false, uninst = false;
    bool hidden = false;
    wstring snapDir;
    bool setupAsked = false, wantDesk = false, wantStart = false, help = false, version = false;
    wstring buildDir;
    std::vector<wstring> pass;   // everything that is not a setup flag: handed to the built game unchanged
    for (int i = 1; i < argc; i++) {
        wstring a = argv[i];
        bool mine = true;
        if (a == L"/hidden") hidden = true;                                   // test: run on a private hidden desktop
        else if (a == L"/snapdir" && i + 1 < argc) snapDir = argv[++i];       // test: save a BMP of every page change
        else if (a == L"/noshortcuts") g_noShortcuts = true;                  // test: leave the desktop and Start menu alone
        else if (a == L"/repair" || a == L"--repair") repair = true;
        else if (a == L"/uninstall" || a == L"--uninstall") uninst = true;
        else if (a == L"/auto") g_auto = true;
        else if ((a == L"/folder" || a == L"--game") && i + 1 < argc) preFolder = argv[++i];
        else if (a == L"/root" && i + 1 < argc) g_scanRoot = argv[++i];
        else if (a == L"/shot" && i + 1 < argc) shotDir = argv[++i];
        else if (a == L"--setup") {}
        else if (a == L"--yes") { g_yes = true; g_auto = true; }
        else if (a == L"--desktop-shortcut") wantDesk = true;
        else if (a == L"--start-menu-shortcut") wantStart = true;
        else mine = false;
        if (mine) { setupAsked = true; continue; }
        if (a == L"--build-dir" && i + 1 < argc) { buildDir = argv[++i]; continue; }   // names the place for both halves; not passed on
        if (a == L"--help" || a == L"-h" || a == L"/?") help = true;
        if (a == L"--version") version = true;
        pass.push_back(a);
    }
    if (!buildDir.empty()) {
        wchar_t full[MAX_PATH * 4];
        if (GetFullPathNameW(buildDir.c_str(), MAX_PATH * 4, full, nullptr)) buildDir = full;
        while (buildDir.size() > 3 && (buildDir.back() == L'\\' || buildDir.back() == L'/')) buildDir.pop_back();
        g_scanRoot = buildDir;   // the engine's "root": work\ and game\ go there
    }
    if (!setupAsked) {
        wstring built = FindBuilt(buildDir);
        int code = 0;
        if (!built.empty() && HandOff(built, pass, &code)) return code;
        if (help) { SayLine(SETUP_HELP); return 0; }
        if (version) { SayLine(L"Snap Recomp " SETUP_VERSION_W L" (not built yet)"); return 0; }
    }
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (g_yes && !uninst && !repair && preFolder.empty() && ReadAll(SetupDir() + L"\\settings.json").find("\"completed\": true") == string::npos) {
        SayLine(L"--yes needs the game files: add --game <folder with your game files>.");
        return 2;
    }
    wstring err;
    if (shotDir.empty() && !ExtractPayload(&err)) {
        if (g_yes) SayLine(err); else MessageBoxW(nullptr, err.c_str(), L"Snap Recomp setup", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; wc.lpszClassName = L"SnapRecompSetup";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    G::GdiplusStartupInput gsi;
    G::GdiplusStartup(&g_gdip, &gsi, nullptr);
    g_dpi = (int)GetDpiForSystem();
    if (g_dpi < 96) g_dpi = 96;
    RECT r{0, 0, D(LW), D(LH)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, (UINT)g_dpi);
    HDESK shotDesk = nullptr;
    if (!shotDir.empty() || hidden) {   // test mode: a private hidden desktop, so nothing appears on the player's screen
        shotDesk = CreateDesktopW(L"snap_setup_shot", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (shotDesk) SetThreadDesktop(shotDesk);
    }
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"SnapRecompSetup", L"Snap Recomp setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hi, nullptr);
    DragAcceptFiles(g_wnd, TRUE);
    CreateUi();

    if (wantDesk) ChkSet(1, true);
    if (wantStart) ChkSet(2, true);

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
            else if (g_page == P_OPTIONS) { ChkSet(0, true); OnCommand(ID_NEXT); }
            else if (g_yes) {   // --yes: no one to press the last button
                static bool removing = false;
                if (g_page == P_WELCOME) OnCommand(ID_NEXT);
                else if (g_page == P_DONE || g_page == P_MAINT || g_removed) { g_exitCode = 0; DestroyWindow(g_wnd); }
                else if (g_page == P_ERROR) { fprintf(stderr, "SnapRecomp setup: %s\n", U(g_err).c_str()); g_exitCode = 1; DestroyWindow(g_wnd); }
                else if (g_page == P_UNINST && !removing) { removing = true; OnCommand(ID_NEXT); }
            }
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
        if (!IsDialogMessageW(g_wnd, &m)) {   // Tab, arrows and Space work on the buttons
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    KillTimer(g_wnd, timer);
    if (g_thread.joinable()) g_thread.detach();
    G::GdiplusShutdown(g_gdip);
    return g_exitCode;
}
