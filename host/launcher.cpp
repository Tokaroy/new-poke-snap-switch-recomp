// Launcher mode of the game exe (builder release): started with no arguments, next to a config.json, the exe asks Play / Change game
// folder / Open save folder, checks the romfs sentinels at every start, and fills in --main, --sdk and the romfs path from config.json.
// Without a config.json next to the exe nothing here runs (the dev build keeps its usage message).
//
// config.json (written by builder/snapbuild/pipeline.py): romfs_root, main_elf, sdk_elf. launcher_data.json (same folder): the
// published romfs check ("sentinels": SHA-256 of the lowercase relative path and the exact size of 32 files).
// Test switches: SNAP_LAUNCHER_AUTOPLAY=1 skips the dialog; SNAP_BOOT_TRACE=<file> sets the boot trace file.
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hle/hle_config.h"
#include "hle/profiles.h"
#include "hle/json_lite.h"
#include "hle/save_port.h"
#include "launcher.h"

namespace fs = std::filesystem;

namespace {

std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}
std::string U8(const std::wstring& w) {
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::string sha256_hex(const std::string& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
  unsigned char h[32];
  BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32);
  BCryptCloseAlgorithmProvider(alg, 0);
  static const char* hx = "0123456789abcdef";
  std::string out;
  for (unsigned char c : h) { out += hx[c >> 4]; out += hx[c & 15]; }
  return out;
}

bool read_json(const fs::path& p, hle::Json& j) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  std::string err;
  return hle::json_parse(ss.str(), j, err);
}

void message(const std::string& title, const std::string& text, UINT icon = MB_ICONINFORMATION) {
  MessageBoxW(nullptr, W(text).c_str(), W(title).c_str(), MB_OK | icon | MB_SETFOREGROUND);
}

std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return s;
}

}  // namespace

namespace snap_launcher {

// Returns an empty string when the romfs folder passes, else the message to show.
std::string check_romfs(const std::string& root, const hle::Json& data) {
  std::error_code ec;
  if (!fs::is_directory(fs::u8path(root), ec))
    return "The game folder cannot be found:\n" + root + "\n\nIs the drive or network share connected? Use \"Change game folder\" to pick it again.";
  const hle::Json* sents = data.get("sentinels");
  if (!sents || sents->a.empty()) return "";   // no published check shipped: nothing to compare
  std::map<std::string, long long> want;
  for (auto& s : sents->a) {
    const hle::Json* k = s.get("path_sha256");
    const hle::Json* z = s.get("size");
    if (k && z) want[k->str()] = (long long)z->num();
  }
  size_t ok = 0, files = 0;
  fs::path base = fs::u8path(root);
  for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    ++files;
    std::string rel = lower(U8(fs::relative(it->path(), base, e2).generic_wstring()));
    auto f = want.find(sha256_hex(rel));
    if (f != want.end() && (long long)it->file_size(e2) == f->second) ++ok;
  }
  if (ok == want.size()) return "";
  if (files == 0) return "The game folder is empty:\n" + root + "\n\nUse \"Change game folder\" to pick the extracted romfs again.";
  return "The game folder does not look like the 2.0.1 romfs this build was made from: " + std::to_string(want.size() - ok) + " of " +
         std::to_string(want.size()) + " check files are missing or changed.\n" + root + "\n\nUse \"Change game folder\" to pick the right folder.";
}

static std::string pick_folder(const std::string& start, const wchar_t* title = L"Pick the folder with the extracted game romfs") {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  if (!start.empty()) {
    IShellItem* si = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(W(start).c_str(), nullptr, IID_PPV_ARGS(&si)))) { dlg->SetFolder(si); si->Release(); }
  }
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U8(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

static std::string pick_zip() {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
  COMDLG_FILTERSPEC ft[1] = {{L"Save backup (.zip)", L"*.zip"}};
  dlg->SetFileTypes(1, ft);
  dlg->SetTitle(L"Pick the save backup (.zip)");
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U8(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

static std::string pick_save_zip() {
  std::string out;
  IFileSaveDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
  COMDLG_FILTERSPEC ft[1] = {{L"Save backup (.zip)", L"*.zip"}};
  dlg->SetFileTypes(1, ft);
  dlg->SetDefaultExtension(L"zip");
  dlg->SetFileName(L"New Pokemon Snap save.zip");
  dlg->SetTitle(L"Export the save as a .zip");
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = U8(p); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// A task dialog of command links. Returns the index of the link picked, -1 on cancel.
static int choose(const std::wstring& title, const std::wstring& content, const std::vector<std::wstring>& links) {
  using TDI = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE cc = LoadLibraryW(L"comctl32.dll");
  TDI td = cc ? (TDI)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
  if (!td) return -1;
  std::vector<TASKDIALOG_BUTTON> b;
  for (size_t i = 0; i < links.size(); ++i) b.push_back({(int)(3000 + i), links[i].c_str()});
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = title.c_str();
  c.pszContent = content.c_str();
  c.pButtons = b.data();
  c.cButtons = (UINT)b.size();
  int sel = 0;
  if (FAILED(td(&c, &sel, nullptr, nullptr))) return -1;
  return sel >= 3000 && sel < 3000 + (int)links.size() ? sel - 3000 : -1;
}

// Loads the game's own config (the launcher's config.json is the same file) so the save folder is the one the game will use.
static bool save_target(const fs::path& cfg_path, std::string* root, std::string* label) {
  hle::load_config(U8(cfg_path.wstring()));
  *root = hle::saveport::target_save_root("", label);
  if (root->empty()) { message("Snap Recomp", "No save folder could be chosen: " + *label, MB_ICONWARNING); return false; }
  return true;
}

static void import_save_flow(const fs::path& cfg_path) {
  std::string root, label;
  if (!save_target(cfg_path, &root, &label)) return;
  auto cands = hle::saveport::detect_emulator_saves();
  std::vector<std::wstring> links;
  const size_t shown = std::min<size_t>(cands.size(), 6);
  for (size_t i = 0; i < shown; ++i) {
    std::string when = hle::profiles::format_time(cands[i].mtime);
    links.push_back(W(cands[i].label + "\nSaved " + when + (cands[i].old_version ? " (older game version)" : "")));
  }
  links.push_back(L"Pick a folder of save files...\nA JKSV or Checkpoint backup folder, or an emulator save folder");
  links.push_back(L"Pick a .zip backup...\nA JKSV backup zip");
  int sel = choose(L"Import a save into " + W(label),
                   shown ? L"Found saves from emulators on this PC. Your current save is kept as a backup." : L"Your current save is kept as a backup.", links);
  if (sel < 0) return;
  std::string path;
  if ((size_t)sel < shown) path = cands[(size_t)sel].path;
  else if ((size_t)sel == shown) path = pick_folder("", L"Pick the folder with the save files");
  else path = pick_zip();
  if (path.empty()) return;
  HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
  hle::saveport::Report r;
  std::string err;
  bool running = false;
  bool ok = hle::saveport::import_save(path, root, hle::saveport::game_uid_dir(), &r, &err, &running);
  SetCursor(old);
  if (ok) message("Snap Recomp", hle::saveport::format_report(r, true), r.warnings.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
  else message("Snap Recomp", "The save was not imported.\n\n" + err, MB_ICONWARNING);
}

static void export_save_flow(const fs::path& cfg_path) {
  std::string root, label;
  if (!save_target(cfg_path, &root, &label)) return;
  int how = choose(L"Export the save", L"Both are the plain save files, named the way the game names them.",
                   {L"To a folder...\nFor Checkpoint, or to copy into an emulator", L"To a .zip file...\nFor JKSV"});
  if (how < 0) return;
  std::string dest = how == 0 ? pick_folder("", L"Pick the folder to export the save to") : pick_save_zip();
  if (dest.empty()) return;
  HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
  hle::saveport::Report r;
  std::string err;
  bool ok = hle::saveport::export_save(root, "", dest, &r, &err);
  SetCursor(old);
  if (ok) message("Snap Recomp", hle::saveport::format_report(r, false), MB_ICONINFORMATION);
  else message("Snap Recomp", "The save was not exported.\n\n" + err, MB_ICONWARNING);
}

static std::string save_folder(const hle::Json& cfg) {
  if (const char* e = std::getenv("SNAP_SAVE_DIR")) if (*e) return e;
  if (const hle::Json* s = cfg.get("save_root")) if (!s->str().empty()) return s->str();
  wchar_t* ad = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &ad))) { out = U8(ad) + "\\SnapRecomp\\save"; CoTaskMemFree(ad); }
  return out;
}

// Dialog result: 1 Play, 2 Change game folder, 3 Open save folder, 4 Upscalers, 5 Import save, 6 Export save, 0 Quit.
static int ask() {
  using TDI = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
  HMODULE cc = LoadLibraryW(L"comctl32.dll");
  TDI td = cc ? (TDI)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
  if (!td) return 1;   // no task dialog (old system): just play
  TASKDIALOG_BUTTON b[6] = {{1001, L"Play"}, {1002, L"Change game folder..."}, {1003, L"Open save folder"}, {1005, L"Import save...\nFrom a console backup (JKSV, Checkpoint) or an emulator"}, {1006, L"Export save...\nCopy your save out for a save manager or an emulator"}, {1004, L"Upscalers: FSR, DLSS, XeSS..."}};
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.dwCommonButtons = 0;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = L"Snap Recomp";
  c.pszContent = L"Works with your own copy of the game.";
  c.pButtons = b;
  c.cButtons = 6;
  c.nDefaultButton = 1001;
  int sel = 0;
  if (FAILED(td(&c, &sel, nullptr, nullptr))) return 1;
  return sel == 1001 ? 1 : sel == 1002 ? 2 : sel == 1003 ? 3 : sel == 1004 ? 4 : sel == 1005 ? 5 : sel == 1006 ? 6 : 0;
}

bool prepare(Result& r) {
  wchar_t mp[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  fs::path exe_dir = fs::path(mp).parent_path();
  fs::path cfg_path = exe_dir / L"config.json";
  std::error_code ec;
  if (!fs::exists(cfg_path, ec)) return false;   // not launcher mode
  r.active = true;
  hle::Json cfg, data;
  if (!read_json(cfg_path, cfg) || cfg.type != hle::Json::Obj) {
    message("Snap Recomp", "config.json next to the game could not be read. Run the builder again.", MB_ICONERROR);
    return true;
  }
  read_json(exe_dir / L"launcher_data.json", data);   // optional
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool autoplay = std::getenv("SNAP_LAUNCHER_AUTOPLAY") != nullptr;
  // First start: offer FSR (pre-selected, never accepted for the user). A test run skips this unless SNAP_UPSCALER_AUTOACCEPT is set.
  if (!cfg.get("upscalers_offered") && (!autoplay || std::getenv("SNAP_UPSCALER_AUTOACCEPT"))) {
    offer_upscalers(exe_dir, data, true);
    cfg.set("upscalers_offered", hle::Json::boolean_(true));
    std::string text;
    hle::json_dump(cfg, text);
    std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
    f << text << "\n";
  }
  if (std::getenv("SNAP_LAUNCHER_FETCH_ONLY")) return true;   // test switch: only the upscaler offer, never start the game
  for (;;) {
    std::string romfs = cfg.get("romfs_root") ? cfg.get("romfs_root")->str() : "";
    int choice = autoplay ? 1 : ask();
    if (choice == 0) return true;   // quit
    if (choice == 4) { offer_upscalers(exe_dir, data, false); continue; }
    if (choice == 5) { import_save_flow(cfg_path); continue; }
    if (choice == 6) { export_save_flow(cfg_path); continue; }
    if (choice == 3) {   // the active profile's save folder (the one the game would use), else the legacy folder
      std::string sf, label;
      if (!save_target(cfg_path, &sf, &label)) sf = save_folder(cfg);
      std::error_code e3;
      fs::create_directories(fs::u8path(sf), e3);
      ShellExecuteW(nullptr, L"explore", W(sf).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      continue;
    }
    if (choice == 2) {
      std::string p = pick_folder(romfs);
      if (p.empty()) continue;
      std::string bad = check_romfs(p, data);
      if (!bad.empty()) { message("Snap Recomp", bad, MB_ICONWARNING); continue; }
      cfg.set("romfs_root", hle::Json::string(p));
      std::string text;
      hle::json_dump(cfg, text);
      std::ofstream f(cfg_path, std::ios::binary | std::ios::trunc);
      f << text << "\n";
      continue;
    }
    std::string bad = check_romfs(romfs, data);
    if (!bad.empty()) {
      if (autoplay) { std::fprintf(stderr, "launcher: %s\n", bad.c_str()); return true; }
      message("Snap Recomp", bad, MB_ICONWARNING);
      continue;
    }
    break;
  }
  auto s = [&](const char* k) { return cfg.get(k) ? cfg.get(k)->str() : std::string(); };
  r.main_elf = s("main_elf");
  r.sdk_elf = s("sdk_elf");
  if (r.main_elf.empty() || r.sdk_elf.empty() || !fs::exists(fs::u8path(r.main_elf), ec) || !fs::exists(fs::u8path(r.sdk_elf), ec)) {
    message("Snap Recomp", "The converted game files are missing (" + (r.main_elf.empty() ? "main_elf" : r.main_elf) +
            "). Run the builder again.", MB_ICONERROR);
    return true;
  }
  r.config_path = U8(cfg_path.wstring());
  r.proceed = true;
  // logs: %LOCALAPPDATA%\SnapRecomp\logs (stderr to a file; the trace file for the boot log)
  wchar_t* la = nullptr;
  fs::path logs;
  wchar_t env_la[MAX_PATH] = {};   // the LOCALAPPDATA variable first (the same folder the shader cache uses; a test run can redirect it)
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", env_la, MAX_PATH) && env_la[0]) logs = fs::path(env_la) / L"SnapRecomp" / L"logs";
  else if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la))) { logs = fs::path(la) / L"SnapRecomp" / L"logs"; CoTaskMemFree(la); }
  if (!logs.empty()) {
    fs::create_directories(logs, ec);
    FILE* f = nullptr;
    if (_wfreopen_s(&f, (logs / L"stderr.log").c_str(), L"w", stderr) == 0 && f) setvbuf(stderr, nullptr, _IOLBF, 1 << 16);
    r.trace_file = std::getenv("SNAP_BOOT_TRACE") ? std::getenv("SNAP_BOOT_TRACE") : U8((logs / L"trace.log").wstring());
    DWORD pids[2];
    if (GetConsoleProcessList(pids, 2) <= 1) FreeConsole();   // started by double-click: no console window
  }
  return true;
}

}  // namespace snap_launcher
