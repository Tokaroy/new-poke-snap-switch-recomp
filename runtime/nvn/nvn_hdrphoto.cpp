// SPDX-License-Identifier: MIT
// HDR photo copies. See nvn_hdrphoto.h and docs/hdr-photos.md. The math is in hdrphoto_core.h.
#include "nvn_hdrphoto.h"

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "hdrphoto_core.h"
#include "hle/settings.h"
#include "nvn_hdr.h"

#pragma comment(lib, "windowscodecs")

namespace nvn {
namespace {
std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
bool EnvBool(const char* name, bool& out) {
  const char* e = getenv(name);
  if (!e || !*e) return false;
  out = *e == '1' || !_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes");
  return true;
}
// 16-bit RGB PNG through WIC into memory.
bool EncodePng16(const uint16_t* rgb, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
  IWICImagingFactory* f = nullptr; IStream* ms = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &ms)) &&
      SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(ms, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(w, h))) {
    WICPixelFormatGUID pf = GUID_WICPixelFormat48bppRGB;
    if (SUCCEEDED(fr->SetPixelFormat(&pf)) && IsEqualGUID(pf, GUID_WICPixelFormat48bppRGB)) {
      constexpr uint32_t kStrip = 64;
      ok = true;
      for (uint32_t y0 = 0; ok && y0 < h; y0 += kStrip) {
        const uint32_t n = h - y0 < kStrip ? h - y0 : kStrip;
        ok = SUCCEEDED(fr->WritePixels(n, w * 6, n * w * 6, (BYTE*)(rgb + (size_t)y0 * w * 3)));
      }
      ok = ok && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (ok) {
      HGLOBAL hg = nullptr;
      STATSTG ss{};
      if (SUCCEEDED(GetHGlobalFromStream(ms, &hg)) && SUCCEEDED(ms->Stat(&ss, STATFLAG_NONAME))) {
        const void* p = GlobalLock(hg);
        if (p) { out.assign((const uint8_t*)p, (const uint8_t*)p + (size_t)ss.cbSize.QuadPart); GlobalUnlock(hg); } else ok = false;
      } else ok = false;
    }
  }
  if (pb) pb->Release();
  if (fr) fr->Release();
  if (enc) enc->Release();
  if (ms) ms->Release();
  if (f) f->Release();
  return ok;
}
}  // namespace

bool HdrPhotoWanted() {
  // The variable is read each time (the dev control channel can flip it for same-photo checks); the settings row is read once.
  static const bool stored = hle::settings::get_bool("hdr_photos", false);
  bool v = false;
  return EnvBool("SNAP_HDR_PHOTOS", v) ? v : stored;
}

bool HdrPhotoEnabled() {
  if (!HdrPhotoWanted()) return false;
  const bool hdr = snap_hdr::Active();
  static std::atomic<int> last{-1};
  const int now = hdr ? 1 : 0;
  if (last.exchange(now) != now) {
    if (hdr) fprintf(stderr, "[hdrphoto] HDR photo copies on: <name>.hdr.png (PQ, BT.2020, reconstructed highlights, peak %.0f nits)\n", snap_hdr::Peak());
    else fprintf(stderr, "[hdrphoto] HDR photo copies are on but HDR output is off (set SNAP_HDR=hdr10 or scrgb and use an HDR display): nothing is written\n");
  }
  return hdr;
}

std::string HdrPhotoPathFor(const std::string& path) {
  const size_t dot = path.find_last_of('.'), sl = path.find_last_of("\/");
  const std::string stem = (dot != std::string::npos && (sl == std::string::npos || dot > sl)) ? path.substr(0, dot) : path;
  return stem + ".hdr.png";
}

bool HdrPhotoWrite(const std::string& path, const uint8_t* px, uint32_t w, uint32_t h, bool bgra) {
  if (!px || !w || !h) return false;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  snap_hdrphoto::Params pr;
  pr.paper = snap_hdr::Paper(); pr.peak = snap_hdr::Peak(); pr.expand = snap_hdr::ExpandAmount();
  std::vector<uint16_t> rgb; snap_hdrphoto::Stats st;
  snap_hdrphoto::convert(px, w, h, bgra, pr, rgb, st);
  std::vector<uint8_t> png;
  bool ok = EncodePng16(rgb.data(), w, h, png) && snap_hdrphoto::add_hdr_chunks(png, pr, st);
  const std::wstring tmp = Widen(path + ".tmp");
  if (ok) {
    HANDLE fh = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    ok = fh != INVALID_HANDLE_VALUE && WriteFile(fh, png.data(), (DWORD)png.size(), &wr, nullptr) && wr == png.size();
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    if (ok) ok = MoveFileExW(tmp.c_str(), Widen(path).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
  }
  if (ok) fprintf(stderr, "[hdrphoto] wrote %s (%ux%u, %zu bytes, max %.0f nits, avg-max %.0f nits)\n", path.c_str(), w, h, png.size(), st.maxCll, st.maxFall);
  else fprintf(stderr, "[hdrphoto] FAILED to write %s\n", path.c_str());
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}
}  // namespace nvn
