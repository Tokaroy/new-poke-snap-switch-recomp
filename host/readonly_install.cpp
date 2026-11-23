// SPDX-License-Identifier: GPL-3.0-or-later
// An exe in a folder it cannot write to: the plain message, the choice, the copy and the pointer file. Rules: host/readonly_logic.h.
#include "readonly_install.h"

#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "hle/json_lite.h"
#include "readonly_logic.h"

namespace snap_ro {
namespace {

std::wstring env_w(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (!n) return {};
  std::wstring v(n, L'\0');
  DWORD got = GetEnvironmentVariableW(name, v.data(), n);
  v.resize(got);
  return v;
}

std::string u8(const fs::path& p) { return to_u8(p); }
fs::path from_u8(const std::string& s) { return fs::u8path(s); }
std::wstring W(const std::string& s) { return fs::u8path(s).wstring(); }

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The folder the player can always write: the real %LOCALAPPDATA% (a redirected run keeps it in SNAP_REAL_LOCALAPPDATA).
fs::path real_local_snap() {
  std::wstring l = env_w(L"SNAP_REAL_LOCALAPPDATA");
  if (l.empty()) l = env_w(L"LOCALAPPDATA");
  return l.empty() ? fs::path() : fs::path(l) / L"SnapRecomp";
}

fs::path pointer_file() {
  const fs::path d = real_local_snap();
  return d.empty() ? d : d / L"data_dirs.txt";
}

void remember_data_dir(const std::string& exe_path, const std::string& dir) {
  const fs::path pf = pointer_file();
  if (pf.empty()) return;
  std::error_code ec;
  fs::create_directories(pf.parent_path(), ec);
  const std::string text = pointer_update(read_all(pf), exe_path, dir);
  const fs::path tmp = pf.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << text;
  }
  MoveFileExW(tmp.c_str(), pf.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

// Folder picker (same shape as the launcher's). start = the folder it opens in, when it exists.
std::string pick_folder(const fs::path& start, const wchar_t* title) {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(title);
  std::error_code ec;
  if (!start.empty() && fs::is_directory(start, ec)) {
    IShellItem* si = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) { dlg->SetFolder(si); si->Release(); }
  }
  if (SUCCEEDED(dlg->Show(nullptr))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = u8(fs::path(p)); CoTaskMemFree(p); }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

void say(const std::string& title, const std::string& text, UINT icon) {
  MessageBoxW(nullptr, W(text).c_str(), W(title).c_str(), MB_OK | icon | MB_SETFOREGROUND);
}

// Command-link dialog: 0 = move, 1 = data folder elsewhere, -1 = cancel.
int ask(const std::string& exe_dir, const std::string& why) {
  const std::wstring content = W("Snap Recomp keeps its build, your saves and your settings next to the game's exe, and Windows does not let it write to this folder:\n\n" +
                                 exe_dir + "\n\nWhat went wrong: " + why +
                                 ".\n\nThis happens in Program Files, in a read-only folder, or when Windows' Controlled Folder Access protects the folder. "
                                 "Nothing has been changed. Pick one:");
  const std::wstring l0 = W("Move to a folder I pick\nCopies the game to a folder you can write to, checks the copy, and starts it. The old folder is left as it is and you can delete it yourself afterwards.");
  const std::wstring l1 = W("Use a data folder elsewhere\nKeeps running from this folder, but saves, settings, caches and logs go to a folder you pick. The choice is remembered for this exe.");
  TASKDIALOG_BUTTON b[2] = {{3000, l0.c_str()}, {3001, l1.c_str()}};
  TASKDIALOGCONFIG c{};
  c.cbSize = sizeof c;
  c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c.pszWindowTitle = L"Snap Recomp";
  c.pszMainInstruction = L"This folder is read-only for the game";
  c.pszContent = content.c_str();
  c.pButtons = b;
  c.cButtons = 2;
  int sel = 0;
  if (FAILED(TaskDialogIndirect(&c, &sel, nullptr, nullptr))) return -1;
  return sel == 3000 ? 0 : sel == 3001 ? 1 : -1;
}

struct CopyJob {
  fs::path src, stage, dest, exe_rel_file;
  CopyStats st;
  std::atomic<bool> done{false};
  bool ok = false;
  std::string why;
};

HRESULT CALLBACK copy_cb(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR data) {
  CopyJob* j = (CopyJob*)data;
  if (msg == TDN_CREATED) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 30);
  if (msg == TDN_TIMER) {
    if (j->done) { SendMessageW(hwnd, TDM_CLICK_BUTTON, IDOK, 0); return S_OK; }
    wchar_t t[160];
    swprintf(t, 160, L"%llu files, %.1f GB copied so far", (unsigned long long)j->st.files.load(), (double)j->st.bytes.load() / 1e9);
    SendMessageW(hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)t);
  }
  return S_OK;
}

// Copies src to stage, checks it, renames stage to dest. The window (when show_ui) only shows that something is happening.
bool copy_install(CopyJob& j, bool show_ui) {
  std::thread worker([&j] {
    std::string why;
    j.ok = copy_tree(j.src, j.stage, &j.st, &why) && verify_tree(j.src, j.stage, j.exe_rel_file, &why);
    if (j.ok) {
      std::error_code ec;
      fs::rename(j.stage, j.dest, ec);
      if (ec) { j.ok = false; why = "could not rename the finished copy: " + ec.message(); }
    }
    j.why = why;
    j.done = true;
  });
  if (show_ui) {
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    c.pszWindowTitle = L"Snap Recomp";
    c.pszMainInstruction = L"Copying the game";
    c.pszContent = L"Copying and checking the files. This can take a while.";
    c.pfCallback = copy_cb;
    c.lpCallbackData = (LONG_PTR)&j;
    if (FAILED(TaskDialogIndirect(&c, nullptr, nullptr, nullptr))) { /* no dialog: wait below */ }
  }
  worker.join();
  return j.ok;
}

// Returns an exit code, or -1 when the copy was not made and the question should be asked again.
int do_move(const fs::path& exe, const fs::path& exe_dir, const std::string& chosen, bool show_ui) {
  std::string root_s, rel_s;
  {
    hle::Json cfg;
    std::string err;
    if (hle::json_parse(read_all(exe_dir / L"config.json"), cfg, err) && cfg.type == hle::Json::Obj) {
      if (const hle::Json* r = cfg.get("install_root")) root_s = r->str();
      if (const hle::Json* e = cfg.get("exe_rel")) rel_s = e->str();
    }
  }
  const fs::path src = move_source(exe_dir, root_s, rel_s);
  std::error_code ec;
  const fs::path parent = fs::absolute(from_u8(chosen), ec);
  auto refuse = [&](const std::string& m) {
    fprintf(stderr, "SnapRecomp: %s\n", m.c_str());
    if (show_ui) say("Snap Recomp", m, MB_ICONWARNING);
    return -1;
  };
  if (inside_or_same(parent, src)) return refuse("Pick a folder outside the game's own folder:\n" + u8(src));
  if (!fs::is_directory(parent, ec) && !fs::create_directories(parent, ec)) return refuse("The folder cannot be created: " + u8(parent));
  std::string why;
  if (!probe_writable(parent, &why)) return refuse("The game cannot write to the folder you picked (" + why + "):\n" + u8(parent));
  CopyJob j;
  j.src = src;
  j.dest = parent / L"SnapRecomp";
  j.stage = parent / L"SnapRecomp.copying";
  j.exe_rel_file = fs::relative(exe, src, ec);
  if (fs::exists(j.dest, ec)) return refuse("There is already a folder named SnapRecomp in\n" + u8(parent) + "\nPick another folder.");
  if (fs::exists(j.stage, ec)) return refuse("A stopped copy is still in\n" + u8(j.stage) + "\nDelete that folder, or pick another one.");
  unsigned long long need = 0;
  for (fs::recursive_directory_iterator it(src, fs::directory_options::none, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) { const auto s = it->file_size(e2); if (!e2) need += s; }
  }
  const auto sp = fs::space(parent, ec);
  if (!ec && sp.available < need + (256ull << 20))
    return refuse("There is not enough room on that drive: the game needs about " + std::to_string(need >> 20) + " MB and " + std::to_string(sp.available >> 20) + " MB are free.");
  fprintf(stderr, "SnapRecomp: copying %s to %s\n", u8(src).c_str(), u8(j.dest).c_str());
  if (!copy_install(j, show_ui)) return refuse("The game could not be copied: " + j.why + "\n\nThe old folder was not changed. Delete " + u8(j.stage) + " when you no longer need it.");
  const fs::path new_exe = j.dest / j.exe_rel_file;
  const std::string msg = "The game was copied and checked:\n" + u8(j.dest) + "\n\nIt starts from there now. The old folder was left as it was; you can delete it yourself once the new one works:\n" + u8(src);
  fprintf(stderr, "SnapRecomp: copied to %s and checked. The old folder %s can be deleted.\n", u8(j.dest).c_str(), u8(src).c_str());
  if (env_w(L"SNAP_RO_NO_START") == L"1") return 0;
  // the copy gets the same arguments
  std::wstring cl = GetCommandLineW();
  size_t k = 0;
  if (!cl.empty() && cl[0] == L'"') { k = cl.find(L'"', 1); k = k == std::wstring::npos ? cl.size() : k + 1; }
  else { k = cl.find(L' '); if (k == std::wstring::npos) k = cl.size(); }
  std::wstring cmd = L"\"" + new_exe.wstring() + L"\"" + cl.substr(k);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = new_exe.parent_path().wstring();
  if (!CreateProcessW(new_exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
    return refuse("The copy was made, but it could not be started (error " + std::to_string(GetLastError()) + "). Start it yourself:\n" + u8(new_exe));
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (show_ui) say("Snap Recomp", msg, MB_ICONINFORMATION);
  return 0;
}

}  // namespace

int startup(const StartArgs& a, std::wstring* data_dir_out) {
  data_dir_out->clear();
  wchar_t mp[MAX_PATH * 2] = {};
  const DWORD len = GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
  if (!len || len >= sizeof mp / sizeof mp[0]) return -1;
  const fs::path exe = mp, exe_dir = exe.parent_path();
  std::error_code ec;
  const std::wstring autochoice = env_w(L"SNAP_RO_AUTOCHOICE");
  const bool test_run = !env_w(L"SNAP_LAUNCHER_AUTOPLAY").empty();
  Inputs in;
  in.data_dir_flag = !a.data_dir.empty();
  in.redirected_by_parent = env_w(L"SNAP_PORTABLE") == L"1";
  in.managed_layout = fs::exists(exe_dir / L"config.json", ec) || fs::exists(exe_dir / L"portable.txt", ec);
  if (in.data_dir_flag || in.redirected_by_parent || !in.managed_layout) return -1;   // nothing to probe: keep the start cheap
  std::string why;
  in.exe_dir_writable = probe_writable(exe_dir, &why);
  if (in.exe_dir_writable) return -1;
  in.interactive = !a.play && !a.has_action && !a.no_prompt && !a.has_rest && !test_run;
  const std::string key = u8(exe);
  in.pointer_dir = pointer_lookup(read_all(pointer_file()), key);
  if (!in.pointer_dir.empty()) in.pointer_dir_writable = probe_writable(from_u8(in.pointer_dir));
  if (!autochoice.empty()) in.interactive = true;   // answered by the test switch
  const Plan p = resolve(in);
  if (p.action == Action::UseDataDir) {
    *data_dir_out = W(p.data_dir);
    fprintf(stderr, "SnapRecomp: %s is read-only; saves and settings go to %s (remembered for this exe)\n", u8(exe_dir).c_str(), p.data_dir.c_str());
    return -1;
  }
  const std::string head = "this folder is read-only for the game (" + why + "): " + u8(exe_dir);
  if (p.action == Action::Fail) {
    fprintf(stderr,
            "SnapRecomp: %s\nThe game keeps its build, saves and settings next to the exe. Start it with --data-dir <folder> to keep them in a folder you can write to, "
            "or move the game folder somewhere writable.\n",
            head.c_str());
    return 3;
  }
  const bool ui = autochoice.empty();
  if (ui) CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const fs::path local = real_local_snap();
  for (;;) {
    int choice = -1;
    std::string folder;
    if (!ui) {
      const std::string s = u8(fs::path(autochoice));
      if (s.rfind("move:", 0) == 0) { choice = 0; folder = s.substr(5); }
      else if (s.rfind("data:", 0) == 0) { choice = 1; folder = s.substr(5); }
    } else {
      choice = ask(u8(exe_dir), why);
      if (choice >= 0) {
        const fs::path def = local.empty() ? fs::path() : local / (choice == 0 ? L"Game" : L"Data");
        if (!def.empty()) fs::create_directories(def, ec);
        folder = pick_folder(def, choice == 0 ? L"Pick the folder to copy the game into" : L"Pick the folder for saves and settings");
        if (folder.empty()) continue;   // cancelled the picker: back to the question
      }
    }
    if (choice < 0) return 1;
    if (choice == 0) {
      const int rc = do_move(exe, exe_dir, folder, ui);
      if (rc >= 0) return rc;
      if (!ui) return 3;
      continue;
    }
    const fs::path d = fs::absolute(from_u8(folder), ec);
    fs::create_directories(d, ec);
    std::string w2;
    if (!probe_writable(d, &w2)) {
      const std::string m = "The game cannot write to the folder you picked (" + w2 + "):\n" + u8(d);
      fprintf(stderr, "SnapRecomp: %s\n", m.c_str());
      if (!ui) return 3;
      say("Snap Recomp", m, MB_ICONWARNING);
      continue;
    }
    remember_data_dir(key, u8(d));
    *data_dir_out = d.wstring();
    fprintf(stderr, "SnapRecomp: saves and settings go to %s (remembered for this exe)\n", u8(d).c_str());
    return -1;
  }
}

}  // namespace snap_ro
