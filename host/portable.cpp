// Portable mode (host/portable.h). The layout must match builder/snapbuild/userdata.py: <exe dir>\portable_data\Roaming and \Local.
#include "portable.h"

#include <windows.h>

#include <filesystem>
#include <cstring>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <sstream>
#include "hle/json_lite.h"

namespace {

std::wstring env_w(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring v(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(got);
    return v;
}

}  // namespace

bool snap_portable_init(const wchar_t* data_dir) {
    wchar_t mp[MAX_PATH * 2] = {};
    DWORD len = GetModuleFileNameW(nullptr, mp, (DWORD)(sizeof mp / sizeof mp[0]));
    if (!len || len >= sizeof mp / sizeof mp[0]) return false;
    std::filesystem::path dir = std::filesystem::path(mp).parent_path();
    std::error_code ec;
    std::filesystem::path base;
    if (data_dir && *data_dir) {   // --data-dir: wins over portable.txt and over a parent's redirect
        base = std::filesystem::absolute(std::filesystem::path(data_dir), ec);
        if (ec) return false;
    } else {
        if (env_w(L"SNAP_PORTABLE") == L"1" && !env_w(L"SNAP_DATA_DIR").empty()) return true;   // a child process of a --data-dir run: already redirected
        if (!std::filesystem::exists(dir / L"portable.txt", ec) || ec) return false;
        if (env_w(L"SNAP_PORTABLE") == L"1") return true;   // a child process of a portable run: already redirected
        base = dir / L"portable_data";
    }
    std::filesystem::path roaming = base / L"Roaming";
    std::filesystem::path local = base / L"Local";
    std::filesystem::create_directories(roaming, ec);
    std::filesystem::create_directories(local, ec);
    if (!std::filesystem::is_directory(roaming, ec) || !std::filesystem::is_directory(local, ec)) return false;   // read-only folder: keep the normal places
    const bool nested = env_w(L"SNAP_PORTABLE") == L"1";   // --data-dir inside an already redirected run: the real folders are the ones saved before
    std::wstring real_roaming = nested ? env_w(L"SNAP_REAL_APPDATA") : env_w(L"APPDATA"), real_local = nested ? env_w(L"SNAP_REAL_LOCALAPPDATA") : env_w(L"LOCALAPPDATA");
    if (data_dir && *data_dir) {
        _wputenv_s(L"SNAP_DATA_DIR", base.c_str());
        SetEnvironmentVariableW(L"SNAP_DATA_DIR", base.c_str());
    }
    if (!real_roaming.empty()) _wputenv_s(L"SNAP_REAL_APPDATA", real_roaming.c_str());
    if (!real_local.empty()) _wputenv_s(L"SNAP_REAL_LOCALAPPDATA", real_local.c_str());
    _wputenv_s(L"APPDATA", roaming.c_str());
    _wputenv_s(L"LOCALAPPDATA", local.c_str());
    _wputenv_s(L"SNAP_PORTABLE", L"1");
    SetEnvironmentVariableW(L"APPDATA", roaming.c_str());   // the Win32 block too: GetEnvironmentVariable readers use this one
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    // std::getenv readers (the mods root, texture cache, ...) use the CRT's narrow copy, which _wputenv_s did not update in a test run.
    // The exe's manifest makes the ANSI code page UTF-8 (host/snap_utf8.manifest), so a non-ASCII folder survives the conversion.
    auto narrow = [](const std::wstring& w) {
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n : 1), '\0');
        if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        s.resize(strlen(s.c_str()));
        return s;
    };
    _putenv_s("APPDATA", narrow(roaming.wstring()).c_str());
    _putenv_s("LOCALAPPDATA", narrow(local.wstring()).c_str());
    _putenv_s("SNAP_PORTABLE", "1");
    if (data_dir && *data_dir) _putenv_s("SNAP_DATA_DIR", narrow(base.wstring()).c_str());
    if (!real_roaming.empty()) _putenv_s("SNAP_REAL_APPDATA", narrow(real_roaming).c_str());
    if (!real_local.empty()) _putenv_s("SNAP_REAL_LOCALAPPDATA", narrow(real_local).c_str());
    return true;
}

namespace {

namespace pfs = std::filesystem;

bool has_player_data(const pfs::path& d) {
    std::error_code ec;
    return pfs::is_directory(d / L"profiles", ec) || pfs::is_directory(d / L"save", ec) || pfs::is_regular_file(d / L"settings.cfg", ec);
}

unsigned long long tree_bytes(const pfs::path& d) {
    std::error_code ec;
    if (pfs::is_regular_file(d, ec)) { auto n = pfs::file_size(d, ec); return ec ? 0 : (unsigned long long)n; }
    unsigned long long n = 0;
    for (pfs::recursive_directory_iterator it(d, pfs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) { auto s = it->file_size(e2); if (!e2) n += (unsigned long long)s; }
    }
    return n;
}

}  // namespace

int snap_portable_adopt(std::wstring* from, std::wstring* to, bool* mods_skipped, std::string* why) {
    if (mods_skipped) *mods_skipped = false;
    if (env_w(L"SNAP_PORTABLE") != L"1" || !env_w(L"SNAP_DATA_DIR").empty()) return 0;   // --data-dir: a folder the caller chose starts empty
    const std::wstring real = env_w(L"SNAP_REAL_APPDATA"), cur = env_w(L"APPDATA");
    if (real.empty() || cur.empty()) return 0;
    const pfs::path src = pfs::path(real) / L"SnapRecomp", dst = pfs::path(cur) / L"SnapRecomp", tmp = pfs::path(cur) / L"SnapRecomp.importing";
    std::error_code ec;
    if (pfs::exists(dst, ec) || !has_player_data(src)) return 0;
    if (pfs::equivalent(pfs::path(real), pfs::path(cur), ec)) return 0;
    auto fail = [&](const std::string& m) { std::error_code e; pfs::remove_all(tmp, e); if (why) *why = m; return -1; };
    pfs::remove_all(tmp, ec);   // a copy that was stopped half way
    ec.clear();
    std::vector<pfs::path> items;
    unsigned long long need = 0, mods = 0;
    for (pfs::directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (!_wcsicmp(name.c_str(), L"shadercache") || !_wcsicmp(name.c_str(), L"shadercache_safe") || !_wcsicmp(name.c_str(), L"update-backups")) continue;
        const unsigned long long n = tree_bytes(it->path());
        if (!_wcsicmp(name.c_str(), L"mods")) mods = n;
        need += n;
        items.push_back(it->path());
    }
    if (ec) return fail("the folder " + src.u8string() + " could not be read: " + ec.message());
    const pfs::space_info sp = pfs::space(pfs::path(cur), ec);
    const unsigned long long margin = 256ull << 20;
    bool skip_mods = false;
    if (!ec && sp.available != (std::uintmax_t)-1 && sp.available < need + margin) {
        if (mods && sp.available >= need - mods + margin) skip_mods = true;   // saves and settings still fit: leave the mods where they are and say so
        else return fail("not enough free space next to the game for a copy of your saves and settings (" + std::to_string((need >> 20) + 1) + " MB needed)");
    }
    ec.clear();
    pfs::create_directories(tmp, ec);
    if (ec) return fail("the folder " + tmp.u8string() + " could not be created: " + ec.message());
    for (auto& p : items) {
        if (skip_mods && !_wcsicmp(p.filename().c_str(), L"mods")) continue;
        pfs::copy(p, tmp / p.filename(), pfs::copy_options::recursive, ec);
        if (ec) return fail("copying " + p.u8string() + " failed: " + ec.message());
    }
    pfs::rename(tmp, dst, ec);
    if (ec) return fail("the copy could not be put in place: " + ec.message());
    if (from) *from = src.wstring();
    if (to) *to = dst.wstring();
    if (mods_skipped) *mods_skipped = skip_mods;
    return 1;
}

// ---- a moved install (host/portable.h) ----
namespace {

std::wstring lower_w(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring strip_sep(std::wstring s) {
    for (auto& c : s) if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

// old root -> new root in every string below j; a match is the root itself or the root followed by a separator
void rebase_strings(hle::Json& j, const std::wstring& old_root, const std::wstring& new_root, int* changed) {
    if (j.type == hle::Json::Str) {
        std::wstring v;
        try { v = pfs::u8path(j.s).wstring(); } catch (...) { return; }
        if (v.size() < old_root.size()) return;
        if (lower_w(v.substr(0, old_root.size())) != lower_w(old_root)) return;
        if (v.size() > old_root.size() && v[old_root.size()] != L'\\' && v[old_root.size()] != L'/') return;
        j.s = pfs::path(new_root + v.substr(old_root.size())).u8string();
        ++*changed;
    } else if (j.type == hle::Json::Arr) {
        for (auto& e : j.a) rebase_strings(e, old_root, new_root, changed);
    } else if (j.type == hle::Json::Obj) {
        for (auto& kv : j.o) rebase_strings(kv.second, old_root, new_root, changed);
    }
}

}  // namespace

bool snap_config_rebase() {
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    if (!n || n >= 32768) return false;
    std::error_code ec;
    const pfs::path dir = pfs::path(buf).parent_path();
    const pfs::path cfg_path = dir / L"config.json";
    std::string text;
    {
        std::ifstream f(cfg_path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    }
    hle::Json cfg;
    std::string err;
    if (!hle::json_parse(text, cfg, err) || cfg.type != hle::Json::Obj) return false;
    const hle::Json* jr = cfg.get("install_root");
    const hle::Json* je = cfg.get("exe_rel");
    if (!jr || !je || jr->str().empty() || je->str().empty()) return false;   // a build from before this, or one outside an install folder
    std::wstring old_root, rel;
    try {
        old_root = strip_sep(pfs::u8path(jr->str()).wstring());
        rel = strip_sep(pfs::u8path(je->str()).wstring());
    } catch (...) { return false; }
    const std::wstring here = strip_sep(dir.wstring());
    // the exe must sit in <some root>\<exe_rel>; a staged copy (work\builds\<id>) does not, and keeps the paths it was built with
    if (here.size() <= rel.size() + 1) return false;
    if (lower_w(here.substr(here.size() - rel.size())) != lower_w(rel) || here[here.size() - rel.size() - 1] != L'\\') return false;
    const std::wstring new_root = here.substr(0, here.size() - rel.size() - 1);
    if (new_root.size() < 2 || lower_w(new_root) == lower_w(old_root)) return false;
    int changed = 0;
    for (auto& kv : cfg.o) if (kv.first != "exe_rel") rebase_strings(kv.second, old_root, new_root, &changed);
    if (!changed) return false;
    std::string out;
    hle::json_dump(cfg, out);
    const pfs::path tmp = dir / L"config.json.moving";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << out << "\n";
        if (!f.good()) { f.close(); pfs::remove(tmp, ec); return false; }
    }
    if (!MoveFileExW(tmp.c_str(), cfg_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { pfs::remove(tmp, ec); return false; }
    return true;
}
