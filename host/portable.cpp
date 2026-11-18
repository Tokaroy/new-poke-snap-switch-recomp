// Portable mode (host/portable.h). The layout must match builder/snapbuild/userdata.py: <exe dir>\portable_data\Roaming and \Local.
#include "portable.h"

#include <windows.h>

#include <filesystem>
#include <cstring>
#include <string>

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
