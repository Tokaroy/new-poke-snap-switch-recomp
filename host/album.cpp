// SPDX-License-Identifier: MIT
// PC photo album reader and exporter. See album.h for what the save does and does not hold. Read-only toward the save.
#include "album.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "bcrypt")

namespace fs = std::filesystem;

namespace album {
namespace {

// ---------------------------------------------------------------------------------------------------------------- small helpers
std::wstring W(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)std::max(n, 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
std::string U8(const std::wstring& w) {
  if (w.empty()) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)std::max(n, 0), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
fs::path P(const std::string& s) { return fs::path(W(s)); }

bool read_file(const fs::path& p, std::vector<uint8_t>& out, size_t max_bytes = (size_t)1 << 30) {
  FILE* f = _wfsopen(p.c_str(), L"rb", _SH_DENYNO);
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  const long long n = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (n < 0 || (unsigned long long)n > max_bytes) { fclose(f); return false; }
  out.resize((size_t)n);
  const bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  return ok;
}

uint32_t crc32_of(const uint8_t* p, size_t n) {
  static uint32_t t[256];
  static bool init;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}

uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

std::string utf16_to_utf8(const uint8_t* p, size_t max_chars) {
  std::wstring w;
  for (size_t i = 0; i < max_chars; ++i) { const wchar_t c = (wchar_t)rd16(p + i * 2); if (!c) break; w += c; }
  return U8(w);
}

// ---------------------------------------------------------------------------------------------------------------- the save cipher
// AES-128-CBC. The key is not in this program: it is read out of the converted main ELF (movz/movk immediates), checked by its digest.
struct Aes {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE key = nullptr;
  std::array<uint8_t, 16> raw{};
  bool ready = false;
};
Aes g_aes;
std::string g_key_elf;   // the ELF the key was taken from (a different path searches again)

bool sha256_is(const uint8_t* p, size_t n, const uint8_t want[32]) {
  uint8_t h[32];
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)p, (ULONG)n, h, 32) != 0) return false;
  return memcmp(h, want, 32) == 0;
}

bool find_key(const std::string& elf, std::string& err) {
  if (g_aes.ready && g_key_elf == elf) return true;
  // SHA-256 of the 16 key bytes
  static const uint8_t kDigest[32] = {0xd2, 0x47, 0x62, 0xcf, 0x7a, 0x5e, 0xec, 0x0a, 0x21, 0x28, 0xcd, 0xd3, 0x8e, 0x68, 0xca, 0x15,
                                      0x2a, 0xda, 0x4f, 0x57, 0xe2, 0x8f, 0x36, 0x92, 0x9f, 0xab, 0x0e, 0x32, 0x38, 0x83, 0xf4, 0x8d};
  std::vector<uint8_t> d;
  if (elf.empty() || !read_file(P(elf), d, (size_t)256 << 20) || d.size() < 0x40 || memcmp(d.data(), "\x7f" "ELF", 4) != 0) {
    err = "The converted game files are needed to read the album (the main program file was not found).";
    return false;
  }
  const uint64_t phoff = rd64(&d[0x20]);
  const unsigned phentsize = rd16(&d[0x36]), phnum = rd16(&d[0x38]);
  std::vector<std::pair<uint64_t, uint64_t>> consts;   // (address, value)
  for (unsigned i = 0; i < phnum; ++i) {
    const size_t ph = (size_t)(phoff + (uint64_t)i * phentsize);
    if (ph + 0x38 > d.size()) break;
    const uint32_t type = rd32(&d[ph]), flags = rd32(&d[ph + 4]);
    const uint64_t off = rd64(&d[ph + 8]), va = rd64(&d[ph + 16]), fsz = rd64(&d[ph + 32]);
    if (type != 1 || !(flags & 1) || off + fsz > d.size()) continue;
    uint64_t val[32]; int hw[32];   // register -> (value so far, next halfword)
    for (int r = 0; r < 32; ++r) hw[r] = -1;
    for (uint64_t k = 0; k + 3 < fsz; k += 4) {
      const uint32_t w = rd32(&d[(size_t)(off + k)]);
      if (((w >> 23) & 0x3f) != 0x25 || !(w >> 31)) continue;   // 64-bit move wide
      const unsigned opc = (w >> 29) & 3, h = (w >> 21) & 3, imm = (w >> 5) & 0xffff, rd = w & 31;
      if (opc == 2) { val[rd] = imm; hw[rd] = 1; }   // movz (halfword 0 only starts a sequence)
      else if (opc == 3 && hw[rd] == (int)h) {
        val[rd] |= (uint64_t)imm << (16 * h);
        hw[rd] = (int)h + 1;
        if (h == 3) { consts.emplace_back(va + k, val[rd]); hw[rd] = -1; }
      }
    }
  }
  for (size_t i = 0; i < consts.size(); ++i)
    for (size_t j = i + 1; j < std::min(i + 6, consts.size()); ++j) {
      uint8_t k[16];
      memcpy(k, &consts[i].second, 8);
      memcpy(k + 8, &consts[j].second, 8);
      if (!sha256_is(k, 16, kDigest)) continue;
      if (g_aes.key) BCryptDestroyKey(g_aes.key);
      if (g_aes.alg) BCryptCloseAlgorithmProvider(g_aes.alg, 0);
      g_aes = Aes();
      if (BCryptOpenAlgorithmProvider(&g_aes.alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) { err = "Windows cryptography is not available."; return false; }
      BCryptSetProperty(g_aes.alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
      if (BCryptGenerateSymmetricKey(g_aes.alg, &g_aes.key, nullptr, 0, k, 16, 0) != 0) { err = "Windows cryptography is not available."; return false; }
      memcpy(g_aes.raw.data(), k, 16);
      g_aes.ready = true;
      g_key_elf = elf;
      return true;
    }
  err = "The save key was not found in the converted main program file, so the album cannot be read. Rebuild the game files with the builder.";
  return false;
}

bool aes_decrypt(const uint8_t iv[16], const uint8_t* in, size_t n, uint8_t* out) {
  if (!g_aes.ready || n % 16) return false;
  uint8_t ivc[16];
  memcpy(ivc, iv, 16);
  ULONG got = 0;
  return BCryptDecrypt(g_aes.key, (PUCHAR)in, (ULONG)n, nullptr, ivc, 16, out, (ULONG)n, &got, 0) == 0;
}

// Header and entry IVs are stored xor-masked with a xorshift stream seeded per file and entry.
struct XorShift {
  uint32_t x, y, z, w;
  explicit XorShift(uint32_t seed) {
    w = 0x41C64E6Du * seed + 12345u;
    z = 0x41C64E6Du * w + 12345u;
    y = 0x41C64E6Du * z + 12345u;
    x = 0x41C64E6Du * y + 12345u;
  }
  uint32_t next() {
    const uint32_t t = x ^ (x << 11);
    const uint32_t nw = w ^ (uint32_t)((int32_t)w >> 19) ^ (t ^ (uint32_t)((int32_t)t >> 8));
    x = y; y = z; z = w; w = nw;
    return w & 0x7FFFFFFFu;
  }
  uint32_t limit(uint32_t lim) {
    const uint32_t d = 0x80000000u / (lim + 1);
    for (;;) { const uint32_t r = next() / d; if (r <= lim) return r; }
  }
};
void unmask_iv(const uint8_t* raw, uint32_t seed, uint8_t out[16]) {
  XorShift r(seed);
  for (int i = 0; i < 16; ++i) out[i] = raw[i] ^ (uint8_t)(r.limit(0xFE) + 1);
}

const char* const kFileNames[16] = {"5652712535912f19", "398ecf2b56c614e3", "6706e7d0f3dab2ab", "00c31e53dde3ac51", "4f342de95be7450e", "0e7280b1c3ce5a0c",
                                    "04289da492638279", "9c1c815c54797c18", "bf0b4b223f011bf9", "4b1a0e859a3f7816", "b8b0ce894762e7a8", "76401e93e6565bd5",
                                    "0a980178a54e26f3", "4207cfe275441231", "75006a27cf0c9275", "9661edcf41d19929"};
constexpr uint32_t kSeedBase = 0xB519FC35u;
constexpr size_t kHdr = 0x30;

struct SaveFile {
  FILE* f = nullptr;
  int index = 0, enc_size = 0, count = 0;
  SaveFile() = default;
  SaveFile(const SaveFile&) = delete;
  SaveFile& operator=(const SaveFile&) = delete;
  ~SaveFile() { if (f) fclose(f); }
  bool open(const fs::path& p, int idx) {
    f = _wfsopen(p.c_str(), L"rb", _SH_DENYNO);   // read only, shared: the game may have it open
    if (!f) return false;
    index = idx;
    uint8_t h[kHdr];
    if (fread(h, 1, kHdr, f) != kHdr) return false;
    uint8_t iv[16], hd[0x20];
    unmask_iv(h + 0x20, kSeedBase + (uint32_t)idx, iv);
    if (!aes_decrypt(iv, h, 0x20, hd)) return false;
    enc_size = (int)rd32(hd + 4);
    count = (int)rd32(hd + 12);
    return enc_size > 0x30 && enc_size % 16 == 0 && count > 0 && count < (1 << 20);
  }
  bool occupied(int e) {   // cheap: only the first bytes of the entry
    if (e < 0 || e >= count) return false;
    uint8_t b[8];
    if (_fseeki64(f, (long long)kHdr + (long long)e * enc_size, SEEK_SET) != 0 || fread(b, 1, 8, f) != 8) return false;
    static const uint8_t z[8] = {};
    return memcmp(b, z, 8) != 0;
  }
  bool read(int e, std::vector<uint8_t>& data) {   // false for an empty or unreadable entry
    if (e < 0 || e >= count) return false;
    std::vector<uint8_t> b((size_t)enc_size);
    if (_fseeki64(f, (long long)kHdr + (long long)e * enc_size, SEEK_SET) != 0 || fread(b.data(), 1, b.size(), f) != b.size()) return false;
    static const uint8_t z[8] = {};
    if (memcmp(b.data(), z, 8) == 0) return false;
    uint8_t iv[16];
    unmask_iv(b.data(), kSeedBase + (uint32_t)index + (uint32_t)e, iv);
    std::vector<uint8_t> d(b.size() - 16);
    if (!aes_decrypt(iv, b.data() + 16, d.size(), d.data())) return false;
    const int size = (int)rd32(d.data() + 12);
    if (size < 0 || (size_t)size > d.size() - 0x20) return false;
    data.assign(d.begin() + 0x20, d.begin() + 0x20 + size);
    return true;
  }
};

// ---------------------------------------------------------------------------------------------------------------- images (WIC)
struct Com {
  bool ok;
  Com() { const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); ok = SUCCEEDED(hr); }
  ~Com() { if (ok) CoUninitialize(); }
};
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

IWICImagingFactory* factory() {
  IWICImagingFactory* f = nullptr;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
  return f;
}

// Decodes to RGBA. max_w > 0 asks for a reduced picture (the JPEG decoder scales by 1/2, 1/4 or 1/8 while decoding: much faster).
bool wic_decode(const uint8_t* p, size_t n, int max_w, Bitmap& out) {
  Com com;
  IWICImagingFactory* fac = factory();
  if (!fac) return false;
  IWICStream* st = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  bool ok = false;
  if (SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)p, (DWORD)n)) &&
      SUCCEEDED(fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    if (max_w > 0 && (int)w > max_w) {
      IWICBitmapSourceTransform* tr = nullptr;
      if (SUCCEEDED(fr->QueryInterface(IID_PPV_ARGS(&tr)))) {
        UINT cw = (w + 7) / 8, ch = (h + 7) / 8;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(tr->GetClosestSize(&cw, &ch)) && SUCCEEDED(tr->GetClosestPixelFormat(&fmt)) &&
            (fmt == GUID_WICPixelFormat32bppBGRA || fmt == GUID_WICPixelFormat24bppBGR) && cw > 0 && ch > 0 && (int)cw < (int)w) {
          const UINT bpp = fmt == GUID_WICPixelFormat24bppBGR ? 3 : 4, stride = cw * bpp;
          std::vector<uint8_t> raw((size_t)stride * ch);
          if (SUCCEEDED(tr->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride, (UINT)raw.size(), raw.data()))) {
            out.w = (int)cw; out.h = (int)ch;
            out.rgba.resize((size_t)cw * ch * 4);
            for (size_t i = 0; i < (size_t)cw * ch; ++i) {
              out.rgba[i * 4 + 0] = raw[i * bpp + 2]; out.rgba[i * 4 + 1] = raw[i * bpp + 1]; out.rgba[i * 4 + 2] = raw[i * bpp + 0];
              out.rgba[i * 4 + 3] = 255;
            }
            ok = true;
          }
        }
        rel(tr);
      }
    }
    if (!ok) {
      IWICFormatConverter* cv = nullptr;
      if (SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
          SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        out.w = (int)w; out.h = (int)h;
        out.rgba.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)out.rgba.size(), out.rgba.data()));
      }
      rel(cv);
    }
  }
  rel(fr); rel(dec); rel(st); rel(fac);
  return ok;
}

bool write_png(const fs::path& path, const Bitmap& b) {
  Com com;
  IWICImagingFactory* fac = factory();
  if (!fac) return false;
  IWICStream* st = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* fr = nullptr;
  IPropertyBag2* props = nullptr;
  bool ok = false;
  if (SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&fr, &props)) && SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize((UINT)b.w, (UINT)b.h))) {
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(fr->SetPixelFormat(&fmt)) && fmt == GUID_WICPixelFormat32bppRGBA &&
        SUCCEEDED(fr->WritePixels((UINT)b.h, (UINT)b.w * 4, (UINT)b.rgba.size(), (BYTE*)b.rgba.data())) && SUCCEEDED(fr->Commit()) &&
        SUCCEEDED(enc->Commit()))
      ok = true;
  }
  rel(props); rel(fr); rel(enc); rel(st); rel(fac);
  if (!ok) { std::error_code ec; fs::remove(path, ec); }
  return ok;
}

// 32x20 luma grid of a reduced decode: the same picture gives the same grid, so best shots can be recognised in the album.
using Sig = std::array<uint8_t, 640>;
bool signature(const uint8_t* jpg, size_t n, Sig& s) {
  Bitmap b;
  if (!wic_decode(jpg, n, 160, b) || b.w < 32 || b.h < 20) return false;
  for (int cy = 0; cy < 20; ++cy)
    for (int cx = 0; cx < 32; ++cx) {
      const int x0 = cx * b.w / 32, x1 = std::max(x0 + 1, (cx + 1) * b.w / 32), y0 = cy * b.h / 20, y1 = std::max(y0 + 1, (cy + 1) * b.h / 20);
      unsigned long long acc = 0;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          const uint8_t* q = &b.rgba[((size_t)y * b.w + x) * 4];
          acc += (q[0] * 77u + q[1] * 150u + q[2] * 29u) >> 8;
        }
      s[(size_t)cy * 32 + cx] = (uint8_t)(acc / (unsigned long long)((x1 - x0) * (y1 - y0)));
    }
  return true;
}
double sig_diff(const Sig& a, const Sig& b) {
  unsigned long long d = 0;
  for (size_t i = 0; i < a.size(); ++i) d += (unsigned)std::abs((int)a[i] - (int)b[i]);
  return (double)d / (double)a.size();
}

// ---------------------------------------------------------------------------------------------------------------- game text (MSBT)
// Pokemon and course names are read from the player's own extracted text, never shipped.
void read_msbt(const std::vector<uint8_t>& d, const char* prefix, std::unordered_map<int, std::string>& out, const char* suffix) {
  if (d.size() < 0x20 || memcmp(d.data(), "MsgStdBn", 8) != 0 || rd16(&d[8]) != 0xFEFF) return;
  std::map<std::string, uint32_t> labels;
  std::vector<std::string> strs;
  size_t o = 0x20;
  while (o + 16 <= d.size()) {
    const uint32_t sz = rd32(&d[o + 4]);
    const size_t s = o + 16;
    if (s + sz > d.size()) break;
    if (memcmp(&d[o], "LBL1", 4) == 0 && sz >= 4) {
      const uint32_t groups = rd32(&d[s]);
      for (uint32_t g = 0; g < groups && s + 4 + (size_t)g * 8 + 8 <= s + sz; ++g) {
        const uint32_t cnt = rd32(&d[s + 4 + (size_t)g * 8]), off = rd32(&d[s + 8 + (size_t)g * 8]);
        size_t q = s + off;
        for (uint32_t k = 0; k < cnt && q < s + sz; ++k) {
          const unsigned l = d[q];
          if (q + 5 + l > s + sz) break;
          labels[std::string((const char*)&d[q + 1], l)] = rd32(&d[q + 1 + l]);
          q += 5 + l;
        }
      }
    } else if (memcmp(&d[o], "TXT2", 4) == 0 && sz >= 4) {
      const uint32_t cnt = rd32(&d[s]);
      for (uint32_t i = 0; i < cnt && 4 + (size_t)i * 4 + 4 <= sz; ++i) {
        const uint32_t a = rd32(&d[s + 4 + (size_t)i * 4]), b = i + 1 < cnt ? rd32(&d[s + 8 + (size_t)i * 4]) : sz;
        std::wstring w;
        for (uint32_t k = a; k + 2 <= b && k + 2 <= sz; k += 2) {
          const wchar_t c = (wchar_t)rd16(&d[s + k]);
          if (c == 0) break;
          if (c == 0x0E) { if (k + 8 <= b) k += 6 + rd16(&d[s + k + 6]); continue; }   // a control tag: skip it and its parameters
          w += c;
        }
        strs.push_back(U8(w));
      }
    }
    o = (s + sz + 15) & ~(size_t)15;
  }
  const size_t pl = strlen(prefix);
  for (auto& kv : labels) {
    const std::string& k = kv.first;
    if (k.compare(0, pl, prefix) != 0 || k.size() < pl + 3 + strlen(suffix) || k.compare(pl + 3, std::string::npos, suffix) != 0) continue;
    const int id = atoi(k.substr(pl, 3).c_str());
    if (id > 0 && kv.second < strs.size() && !strs[kv.second].empty()) out[id] = strs[kv.second];
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- library
namespace {
struct PdxRec { int species = 0, score = 0, img = 0, course = 0, stars = 0; std::string caption; Sig sig{}; bool has_sig = false; };
struct PngRec { std::string path, taken; unsigned size = 0, crc = 0; };
}  // namespace

struct Library::Impl {
  Inputs in;
  std::unique_ptr<SaveFile> f[16];
  std::vector<PdxRec> pdx;
  std::vector<PngRec> pngs;
  std::unordered_map<unsigned, int> png_by_crc, png_by_size;   // size index only keeps unique sizes (-1 when several)
  std::unordered_map<int, std::string> species, courses, islands;
  size_t pdx_next = 0, photo_next = 0;
  std::vector<std::string> names_cache;
  std::string hd_dir;
};

namespace {

SaveFile* open_file(Library::Impl& I, const fs::path& gen, int idx) {
  if (I.f[idx]) return I.f[idx].get();
  auto sf = std::make_unique<SaveFile>();
  if (!sf->open(gen / kFileNames[idx], idx)) return nullptr;
  I.f[idx] = std::move(sf);
  return I.f[idx].get();
}

fs::path find_gen(const std::string& root, int& gen, std::string& err) {
  std::error_code ec;
  fs::path base = P(root);
  fs::path uid;
  if (fs::exists(base / "CURRENT", ec)) uid = base;
  else {
    for (auto& e : fs::directory_iterator(base, ec))
      if (e.is_directory(ec) && fs::exists(e.path() / "CURRENT", ec)) { uid = e.path(); break; }
  }
  if (uid.empty()) { err = "No save found yet. Play the game once and take a photo."; return {}; }
  std::vector<uint8_t> cur;
  if (!read_file(uid / "CURRENT", cur, 4096)) { err = "The save folder could not be read."; return {}; }
  std::string s((const char*)cur.data(), cur.size());
  size_t a = s.find_first_of(" \t\r\n");
  if (a == std::string::npos) { err = "The save folder could not be read."; return {}; }
  a = s.find_first_not_of(" \t\r\n", a);
  const size_t b = a == std::string::npos ? a : s.find_first_of(" \t\r\n", a);
  gen = a == std::string::npos ? 0 : atoi(s.substr(a, b == std::string::npos ? std::string::npos : b - a).c_str());
  fs::path g = uid / "gens" / std::to_string(gen);
  if (!fs::exists(g, ec)) { err = "The save has no readable generation."; return {}; }
  return g;
}

void load_names(Library::Impl& I) {
  std::vector<fs::path> dirs;
  const char* rel = "drp-v262144/common/Text/master/TextResource_en.drp";
  if (const char* e = std::getenv("SNAP_TEXT_DIR")) if (*e) dirs.push_back(P(e));
  if (!I.in.romfs_root.empty()) {
    fs::path r = P(I.in.romfs_root);
    while (!r.empty() && (r.filename().empty() || r.filename() == ".")) r = r.parent_path();
    dirs.push_back(r.parent_path() / rel);
    dirs.push_back(r / ".." / rel);
  }
  dirs.push_back(fs::path("extracted") / rel);
  for (const fs::path& d : dirs) {
    std::vector<uint8_t> b;
    if (I.species.empty() && read_file(d / "Poke_Name.msbt", b, 1 << 22)) read_msbt(b, "P", I.species, "_Name");
    if (I.courses.empty() && read_file(d / "Place_Name.msbt", b, 1 << 22)) {
      read_msbt(b, "Course", I.courses, "_Name");
      read_msbt(b, "Island", I.islands, "_Name");
    }
    if (!I.species.empty() && !I.courses.empty()) break;
  }
}

void index_pngs(Library::Impl& I) {
  if (I.in.png_dir.empty()) return;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(P(I.in.png_dir), ec)) {
    if (e.path().extension() != L".json") continue;
    std::vector<uint8_t> b;
    if (!read_file(e.path(), b, 1 << 16)) continue;
    const std::string s((const char*)b.data(), b.size());
    auto field = [&](const char* k) -> std::string {
      const size_t p = s.find(std::string("\"") + k + "\"");
      if (p == std::string::npos) return "";
      size_t q = s.find(':', p);
      if (q == std::string::npos) return "";
      q = s.find_first_not_of(" \"", q + 1);
      const size_t r = s.find_first_of("\",}", q);
      return q == std::string::npos ? "" : s.substr(q, r - q);
    };
    PngRec r;
    const std::string png = field("png");
    if (png.empty()) continue;
    fs::path pp = e.path().parent_path() / W(png);
    if (!fs::exists(pp, ec)) continue;
    r.path = U8(pp.wstring());
    r.size = (unsigned)strtoul(field("jpeg_bytes").c_str(), nullptr, 10);
    r.crc = (unsigned)strtoul(field("jpeg_crc32").c_str(), nullptr, 16);
    unsigned y, mo, d, h, mi, sec;   // photo_YYYYMMDD_HHMMSS_NNN.png
    if (sscanf(png.c_str(), "photo_%4u%2u%2u_%2u%2u%2u", &y, &mo, &d, &h, &mi, &sec) == 6) {
      char t[32];
      snprintf(t, sizeof t, "%04u-%02u-%02u %02u:%02u", y, mo, d, h, mi);
      r.taken = t;
    }
    I.pngs.push_back(std::move(r));
  }
  for (size_t i = 0; i < I.pngs.size(); ++i) {
    if (I.pngs[i].crc) I.png_by_crc[I.pngs[i].crc] = (int)i;
    if (I.pngs[i].size) { auto it = I.png_by_size.find(I.pngs[i].size); I.png_by_size[I.pngs[i].size] = it == I.png_by_size.end() ? (int)i : -1; }
  }
}

}  // namespace

const std::string& species_name(const Library& lib, int species) {
  static std::string tmp;
  if (lib.impl) { auto it = lib.impl->species.find(species); if (it != lib.impl->species.end()) return it->second; }
  char b[32];
  snprintf(b, sizeof b, "Pokemon %03d", species);
  tmp = b;
  return tmp;
}
std::string course_name(const Library& lib, int course) {
  if (lib.impl) { auto it = lib.impl->courses.find(course); if (it != lib.impl->courses.end()) return it->second; }
  return course > 0 ? "Course " + std::to_string(course) : "";
}
std::string island_name(const Library& lib, int course) {
  const int isl = course <= 0 ? 0 : course <= 5 ? 1 : course <= 11 ? 2 : course <= 17 ? 3 : course <= 23 ? 4 : course <= 27 ? 5 : course <= 29 ? 6 : 0;
  if (!isl) return course == 30 ? "Research Camp" : "";
  if (lib.impl) { auto it = lib.impl->islands.find(isl); if (it != lib.impl->islands.end()) return it->second; }
  return "Island " + std::to_string(isl);
}

std::string course_label(const std::string& romfs_root, int course) {   // "Florio - Park (Day)", read from the player's own text; empty when it is missing
  static std::mutex mu;
  static std::string root_done;
  static Library lib;
  std::lock_guard<std::mutex> g(mu);
  if (!lib.impl || root_done != romfs_root) {
    lib = Library();
    lib.impl = std::make_shared<Library::Impl>();
    lib.impl->in.romfs_root = romfs_root;
    load_names(*lib.impl);
    root_done = romfs_root;
  }
  auto it = lib.impl->courses.find(course);
  if (it == lib.impl->courses.end()) return "";
  const std::string isl = island_name(lib, course);
  return isl.empty() ? it->second : isl + " - " + it->second;
}

bool load(const Inputs& in, Library& out) {
  out = Library();
  out.impl = std::make_shared<Library::Impl>();
  Library::Impl& I = *out.impl;
  I.in = in;
  std::string err;
  if (!find_key(in.main_elf, err)) { out.error = err; return false; }
  const fs::path gen = find_gen(in.save_root, out.generation, err);
  if (gen.empty()) { out.error = err; return false; }
  out.save_dir = U8(gen.wstring());
  load_names(I);
  index_pngs(I);
  {
    fs::path r = P(in.save_root);
    std::error_code ec;
    if (!fs::exists(r / "photos_hd", ec) && r.has_parent_path() && fs::exists(r.parent_path() / "photos_hd", ec)) r = r.parent_path();   // the <uid> folder was given
    I.hd_dir = U8((r / "photos_hd").wstring());
  }
  SaveFile* meta = open_file(I, gen, 6);
  SaveFile* full = open_file(I, gen, 7);
  if (!meta || !full) { out.error = "The save's album files could not be read."; return false; }

  // Photodex records (file 2): one best shot per Pokemon; research entries (file 9) say which course and star slot hold the score.
  std::vector<uint8_t> research;
  if (SaveFile* f9 = open_file(I, gen, 9)) f9->read(0, research);
  if (SaveFile* f2 = open_file(I, gen, 2)) {
    std::vector<uint8_t> d;
    for (int e = 0; e < f2->count; ++e) {
      if (!f2->read(e, d) || d.size() < 0xC8 + 4) continue;
      PdxRec r;
      r.species = e + 1;
      r.score = (int)rd32(&d[200]);
      r.img = rd16(&d[4]);
      if (r.score <= 0) continue;
      if (d.size() >= 0x1c8 + 8) r.caption = utf16_to_utf8(&d[0x1c8], std::min<size_t>(0x40, (d.size() - 0x1c8) / 2));
      for (int route = 0; route < 30 && !r.course; ++route) {
        const size_t base = (size_t)route * 0x1f8 + 8;
        for (int k = 0; k < 40 && base + (size_t)k * 12 + 12 <= research.size(); ++k) {
          const uint8_t* q = &research[base + (size_t)k * 12];
          if (rd16(q) == 0) break;
          if (rd16(q) != (unsigned)r.species) continue;
          for (int s = 0; s < 4; ++s)
            if (rd16(q + 4 + s * 2) == (unsigned)r.score) { r.course = route + 1; r.stars = s + 1; break; }
          if (r.course) break;
        }
      }
      I.pdx.push_back(std::move(r));
    }
  }
  // album slots (file 6): byte 0x14 = in use
  std::vector<uint8_t> d;
  for (int e = 0; e < meta->count; ++e) {
    if (!meta->read(e, d) || d.size() < 0x20 || !d[0x14]) continue;
    if (!full->occupied(e)) continue;
    Photo p;
    p.slot = e;
    p.tick = rd64(d.data());
    if (d.size() >= 0x60) {
      const std::string c = utf16_to_utf8(&d[0x1e], 0x30);
      int letters = 0;
      for (unsigned char ch : c) if (isalpha(ch) || ch >= 0x80) ++letters;
      if (letters >= 3) p.caption = c;
    }
    out.photos.push_back(std::move(p));
  }
  std::stable_sort(out.photos.begin(), out.photos.end(), [](const Photo& a, const Photo& b) { return a.tick != b.tick ? a.tick > b.tick : a.slot < b.slot; });
  int imgs = 0;
  for (auto& r : I.pdx) if (r.img >= 0) ++imgs;
  out.total = imgs + (int)out.photos.size();
  return true;
}

bool step(Library& lib, double budget_ms) {
  if (!lib.impl) return true;
  Library::Impl& I = *lib.impl;
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  std::error_code ec;
  const fs::path gen = P(lib.save_dir);
  while (I.pdx_next < I.pdx.size() || I.photo_next < lib.photos.size()) {
    if (I.pdx_next < I.pdx.size()) {
      PdxRec& r = I.pdx[I.pdx_next++];
      std::vector<uint8_t> d;
      SaveFile* f3 = open_file(I, gen, 3);
      if (f3 && f3->read(r.img, d) && d.size() > 16) {
        const uint32_t sz = rd32(&d[4]);
        if (sz > 0 && sz <= d.size() - 16) r.has_sig = signature(&d[16], sz, r.sig);
      }
    } else {
      Photo& p = lib.photos[I.photo_next++];
      p.probed = true;
      std::vector<uint8_t> d;
      SaveFile* f7 = open_file(I, gen, 7);
      if (f7 && f7->read(p.slot, d) && d.size() > 16) {
        const uint32_t sz = rd32(&d[4]);
        if (sz > 0 && sz <= d.size() - 16) {
          const uint8_t* jp = &d[16];
          p.jpeg_size = sz;
          p.jpeg_crc = crc32_of(jp, sz);
          char nm[40];
          snprintf(nm, sizeof nm, "\\%08x.jpg", p.jpeg_crc);
          const std::string hd = I.hd_dir + nm;
          if (fs::exists(P(hd), ec)) p.hd_path = hd;
          int pi = -1;
          auto c = I.png_by_crc.find(p.jpeg_crc);
          if (c != I.png_by_crc.end()) pi = c->second;
          else { auto s = I.png_by_size.find(p.jpeg_size); if (s != I.png_by_size.end()) pi = s->second; }
          if (pi >= 0) { p.png_path = I.pngs[(size_t)pi].path; p.taken = I.pngs[(size_t)pi].taken; }
          Sig sg;
          if (!I.pdx.empty() && signature(jp, sz, sg)) {
            double best = 4.0;
            const PdxRec* hit = nullptr;
            for (const PdxRec& r : I.pdx) {
              if (!r.has_sig) continue;
              const double df = sig_diff(sg, r.sig);
              if (df < best) { best = df; hit = &r; }
            }
            if (hit) {
              p.rated = true; p.species = hit->species; p.score = hit->score; p.course = hit->course; p.stars = hit->stars;
              if (p.caption.empty()) p.caption = hit->caption;
            }
          }
        }
      }
    }
    ++lib.done;
    if (std::chrono::duration<double, std::milli>(clk::now() - t0).count() >= budget_ms) break;
  }
  const bool fin = I.pdx_next >= I.pdx.size() && I.photo_next >= lib.photos.size();
  if (fin) {
    lib.done = lib.total;
    int n = 0;
    for (auto& p : lib.photos) if (p.species) ++n;
    lib.species_known = n;
  }
  return fin;
}

bool read_jpeg(const Library& lib, int slot, bool thumb, std::vector<uint8_t>& out) {
  if (!lib.impl) return false;
  Library::Impl& I = *lib.impl;
  SaveFile* f = open_file(I, P(lib.save_dir), thumb ? 10 : 7);
  std::vector<uint8_t> d;
  if (!f || !f->read(slot, d) || d.size() <= 16) return false;
  const uint32_t sz = rd32(&d[4]);
  if (sz == 0 || sz > d.size() - 16 || d[16] != 0xFF || d[17] != 0xD8) return false;
  out.assign(d.begin() + 16, d.begin() + 16 + sz);
  return true;
}

bool decode_image(const uint8_t* data, size_t n, Bitmap& out) { return wic_decode(data, n, 0, out); }

bool best_image(const Library& lib, const Photo& p, std::vector<uint8_t>& out, const char** kind) {
  if (!p.png_path.empty() && read_file(P(p.png_path), out, (size_t)256 << 20)) { if (kind) *kind = "png"; return true; }
  if (!p.hd_path.empty() && read_file(P(p.hd_path), out, (size_t)256 << 20)) { if (kind) *kind = "jpg"; return true; }
  if (read_jpeg(lib, p.slot, false, out)) { if (kind) *kind = "jpg"; return true; }
  return false;
}

// ---------------------------------------------------------------------------------------------------------------- export
namespace {
std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
std::string csv_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) { if (c == '"') o += '"'; o += c; }
  return o + "\"";
}
std::string slug(const std::string& s) {
  std::string o;
  for (unsigned char c : s) o += (isalnum(c) ? (char)c : (c == ' ' || c == '-' || c == '_') ? '_' : '\0');
  o.erase(std::remove(o.begin(), o.end(), '\0'), o.end());
  return o;
}
}  // namespace

bool export_begin(ExportJob& job, const std::string& dest, const std::vector<int>& slots) {
  job = ExportJob();
  job.dest = dest;
  job.slots = slots;
  std::error_code ec;
  fs::create_directories(P(dest), ec);
  if (!fs::is_directory(P(dest), ec)) { job.error = "The folder could not be created."; return false; }
  job.json = "[\n";
  job.csv = "file,slot,pokemon,pokemon_number,course,course_number,stars,score,date_taken,game_tick_ms,rated,caption,source\n";
  job.started = true;
  return true;
}

bool export_step(const Library& lib, ExportJob& job, double budget_ms) {
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  while (job.started && job.next < job.slots.size()) {
    const int slot = job.slots[job.next++];
    const Photo* p = nullptr;
    for (const Photo& q : lib.photos) if (q.slot == slot) { p = &q; break; }
    std::vector<uint8_t> img;
    const char* kind = "jpg";
    char nm[96] = "";
    const std::string pk = p && p->species ? species_name(lib, p->species) : "Photo";
    if (p) snprintf(nm, sizeof nm, "Photo_%04d_%s.png", p->slot + 1, slug(pk).c_str());
    bool ok = false;
    if (p && best_image(lib, *p, img, &kind)) {
      const fs::path out = P(job.dest) / W(nm);
      if (!strcmp(kind, "png")) {
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        o.write((const char*)img.data(), (std::streamsize)img.size());
        ok = o.good();
      } else {
        Bitmap b;
        ok = wic_decode(img.data(), img.size(), 0, b) && write_png(out, b);
      }
    }
    if (!ok) ++job.failed;
    else {
      ++job.exported;
      const std::string src = !p->png_path.empty() ? "png export" : !p->hd_path.empty() ? "hd copy" : "album";
      const std::string course = p->course ? course_name(lib, p->course) : "", isl = p->course ? island_name(lib, p->course) : "";
      const std::string courseFull = isl.empty() ? course : isl + " - " + course;
      if (job.exported > 1) job.json += ",\n";
      job.json += "  {\"file\": " + json_str(nm) + ", \"slot\": " + std::to_string(p->slot + 1) + ", \"pokemon\": " + (p->species ? json_str(pk) : "null") +
                  ", \"pokemon_number\": " + (p->species ? std::to_string(p->species) : "null") + ", \"course\": " + (p->course ? json_str(courseFull) : "null") +
                  ", \"course_number\": " + (p->course ? std::to_string(p->course) : "null") + ", \"stars\": " + (p->stars ? std::to_string(p->stars) : "null") +
                  ", \"score\": " + (p->score >= 0 ? std::to_string(p->score) : "null") + ", \"date_taken\": " + (p->taken.empty() ? "null" : json_str(p->taken)) +
                  ", \"game_tick_ms\": " + std::to_string(p->tick) + ", \"rated\": " + (p->rated ? "true" : "false") + ", \"caption\": " + json_str(p->caption) +
                  ", \"source\": " + json_str(src) + "}";
      job.csv += csv_str(nm) + "," + std::to_string(p->slot + 1) + "," + csv_str(p->species ? pk : "") + "," + (p->species ? std::to_string(p->species) : "") + "," +
                 csv_str(courseFull) + "," + (p->course ? std::to_string(p->course) : "") + "," + (p->stars ? std::to_string(p->stars) : "") + "," +
                 (p->score >= 0 ? std::to_string(p->score) : "") + "," + csv_str(p->taken) + "," + std::to_string(p->tick) + "," + (p->rated ? "yes" : "no") + "," +
                 csv_str(p->caption) + "," + csv_str(src) + "\n";
    }
    if (std::chrono::duration<double, std::milli>(clk::now() - t0).count() >= budget_ms) break;
  }
  const bool fin = !job.started || job.next >= job.slots.size();
  if (fin && job.started) {
    job.started = false;
    if (job.exported > 0) {
      job.json += "\n]\n";
      std::ofstream a(P(job.dest) / L"album.json", std::ios::binary | std::ios::trunc);
      a << job.json;
      std::ofstream b(P(job.dest) / L"album.csv", std::ios::binary | std::ios::trunc);
      b << job.csv;
    }
    job.json.clear();
    job.csv.clear();
  }
  return fin;
}

ExportJob export_photos(const Library& lib, const std::vector<int>& slots, const std::string& dest) {
  ExportJob job;
  if (export_begin(job, dest, slots)) while (!export_step(lib, job, 1e9)) {}
  return job;
}

}  // namespace album
