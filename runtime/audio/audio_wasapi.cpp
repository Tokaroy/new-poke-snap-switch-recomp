// SPDX-License-Identifier: MIT
// WASAPI shared-mode, event-driven render backend (Windows). Follows the default render endpoint: when the default
// device changes, or the current one is invalidated or unplugged, the stream is rebuilt on the new default device;
// while no endpoint exists a timer keeps consuming at stream rate so the guest never stalls.
#if defined(_WIN32)
#include "audio_internal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <ks.h>
#include <ksmedia.h>
#include <functiondiscoverykeys_devpkey.h>
#include <SpatialAudioClient.h>
#include <propidl.h>
#include "audio_mix.h"
#include "audio_dsp.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")

namespace snap_audio {
namespace {

template <class T> void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

class Notify : public IMMNotificationClient {
 public:
  explicit Notify(HANDLE ev) : ev_(ev) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override { ULONG r = --ref_; if (!r) delete this; return r; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** o) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) { *o = this; AddRef(); return S_OK; }
    *o = nullptr; return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole && !pinned_.load()) SetEvent(ev_);   // a stream pinned to a chosen device does not follow the Windows default
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  // Only the render endpoint in use matters (a USB mic, a headset's capture side or an idle second card must not rebuild the stream and
  // signal the game). With no endpoint open (cur_ empty) any render device becoming active is worth a retry.
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
    std::lock_guard<std::mutex> g(mu_);
    if (!id) return S_OK;
    if (!cur_.empty()) { if (cur_ == id) SetEvent(ev_); }
    else if (state == DEVICE_STATE_ACTIVE) SetEvent(ev_);
    // The chosen device was missing and the stream fell back to the default: when any render device comes back, try the chosen one again.
    if (retry_.load() && state == DEVICE_STATE_ACTIVE) SetEvent(ev_);
    return S_OK;
  }
  void set_current(const wchar_t* id) { std::lock_guard<std::mutex> g(mu_); cur_ = id ? id : L""; }
  // pinned: the stream is on a device the user chose (ignore default-device changes). retry: that device was not found (watch for it coming back).
  void set_pinned(bool pinned, bool retry) { pinned_.store(pinned); retry_.store(retry); }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
 private:
  std::atomic<ULONG> ref_{1};
  std::atomic<bool> pinned_{false}, retry_{false};
  HANDLE ev_;
  std::mutex mu_;
  std::wstring cur_;   // id of the render endpoint the stream is built on
};

void make_format(WAVEFORMATEXTENSIBLE& w, uint32_t rate, uint32_t ch) {
  ZeroMemory(&w, sizeof(w));
  w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  w.Format.nChannels = (WORD)ch;
  w.Format.nSamplesPerSec = rate;
  w.Format.wBitsPerSample = 32;
  w.Format.nBlockAlign = (WORD)(ch * 4);
  w.Format.nAvgBytesPerSec = rate * ch * 4;
  w.Format.cbSize = 22;
  w.Samples.wValidBitsPerSample = 32;
  w.dwChannelMask = ch == 8 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
          : ch == 6 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                               SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
                            : (ch == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT));
  w.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
}

std::string to_utf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string o((size_t)(n > 1 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, o.data(), n, nullptr, nullptr);
  return o;
}

std::string friendly_name(IMMDevice* d) {
  std::string out;
  IPropertyStore* ps = nullptr;
  if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
    PROPVARIANT v; PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = to_utf8(v.pwszVal);
    PropVariantClear(&v);
    ps->Release();
  }
  return out;
}

bool icontains(const std::string& hay, const std::string& needle) {
  if (needle.empty()) return true;
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() && std::tolower((unsigned char)hay[i + k]) == std::tolower((unsigned char)needle[k])) ++k;
    if (k == needle.size()) return true;
  }
  return false;
}

// The render endpoint to play on. `sel` = "" -> the Windows default (eConsole). Otherwise the active render endpoint whose id equals `sel` or whose
// friendly name contains it (case-insensitive); not found -> the default and *missing = true. *pinned = a chosen device is in use.
bool pick_endpoint(IMMDeviceEnumerator* en, const std::string& sel, IMMDevice** out, bool* pinned = nullptr, bool* missing = nullptr) {
  if (pinned) *pinned = false;
  if (missing) *missing = false;
  *out = nullptr;
  if (!sel.empty()) {
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
      UINT n = 0; col->GetCount(&n);
      for (UINT i = 0; i < n && !*out; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(col->Item(i, &d)) || !d) continue;
        LPWSTR id = nullptr; bool hit = false;
        if (SUCCEEDED(d->GetId(&id)) && id) { hit = to_utf8(id) == sel; CoTaskMemFree(id); }
        if (!hit) hit = icontains(friendly_name(d), sel);
        if (hit) *out = d; else d->Release();
      }
      col->Release();
    }
    if (*out) { if (pinned) *pinned = true; return true; }
    if (missing) *missing = true;
  }
  return SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, out)) && *out;
}

class WasapiBackend : public Backend {
 public:
  void set_device(const std::string& d) override { fmt_.device = d; }
  bool start(const DeviceFormat& f) override {
    fmt_ = f;
    // Probe once so a machine with no usable COM or enumerator falls back to the timer backend.
    IMMDeviceEnumerator* e = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    if (FAILED(hr)) {
      if (hr == CO_E_NOTINITIALIZED) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
      }
      if (FAILED(hr)) return false;
    }
    e->Release();
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    run_.store(true);
    th_ = std::thread([this] { thread_main(); });
    return true;
  }
  void stop() override {
    run_.store(false);
    if (stopEv_) SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    if (stopEv_) { CloseHandle(stopEv_); stopEv_ = nullptr; }
  }
  int kind() const override { return SNAP_AUDIO_BACKEND_WASAPI; }
  int spatial_available() override {
    HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int ok = 0;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        pick_endpoint(en, fmt_.device, &dev)) {
      ok = probe_spatial(dev, 48000, nullptr) ? 1 : 0;
    }
    safe_release(dev); safe_release(en);
    if (SUCCEEDED(hi)) CoUninitialize();
    return ok;
  }
  uint32_t native_channels() override {
    HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    uint32_t ch = 2;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    WAVEFORMATEX* mf = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        pick_endpoint(en, fmt_.device, &dev) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && SUCCEEDED(ac->GetMixFormat(&mf))) {
      ch = mf->nChannels;
    }
    if (mf) CoTaskMemFree(mf);
    safe_release(ac); safe_release(dev); safe_release(en);
    if (SUCCEEDED(hi)) CoUninitialize();
    return ch;
  }

 private:
  struct Stream {
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    HANDLE ev = nullptr;
    UINT32 frames = 0;
    uint32_t rate = 48000;   // rate the stream really runs at (the mix rate when our own resampler is on)
    ~Stream() {
      if (ac) ac->Stop();
      safe_release(rc); safe_release(ac);
      if (ev) CloseHandle(ev);
    }
  };


  // ---- Windows spatial audio: static bed objects (Windows Sonic, Dolby Atmos for Headphones, DTS) ---------------------
  struct Spatial {
    ISpatialAudioClient* sac = nullptr;
    ISpatialAudioObjectRenderStream* st = nullptr;
    ISpatialAudioObject* obj[6] = {};
    HANDLE ev = nullptr;
    bool started = false;
    ~Spatial() {
      if (st && started) st->Stop();
      for (auto& o : obj) safe_release(o);
      safe_release(st); safe_release(sac);
      if (ev) CloseHandle(ev);
    }
  };

  static AudioObjectType bed_type(mix::BedObject b) {
    switch (b) {
      case mix::BedObject::FrontLeft: return AudioObjectType_FrontLeft;
      case mix::BedObject::FrontRight: return AudioObjectType_FrontRight;
      case mix::BedObject::FrontCenter: return AudioObjectType_FrontCenter;
      case mix::BedObject::LowFrequency: return AudioObjectType_LowFrequency;
      case mix::BedObject::SideLeft: return AudioObjectType_SideLeft;
      default: return AudioObjectType_SideRight;
    }
  }

  static void spatial_format(WAVEFORMATEX& f, uint32_t rate) {
    ZeroMemory(&f, sizeof(f));
    f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; f.nChannels = 1; f.nSamplesPerSec = rate; f.wBitsPerSample = 32;
    f.nBlockAlign = 4; f.nAvgBytesPerSec = rate * 4; f.cbSize = 0;
  }

  static AudioObjectType bed_mask() {
    unsigned m = 0;
    for (auto b : mix::kBed51) m |= (unsigned)bed_type(b);
    return (AudioObjectType)m;
  }

  // Probe: activate the client and ask whether the 6 bed objects are available. Nothing is started.
  static bool probe_spatial(IMMDevice* dev, uint32_t rate, ISpatialAudioClient** out) {
    ISpatialAudioClient* sac = nullptr;
    if (FAILED(dev->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, (void**)&sac)) || !sac) return false;
    WAVEFORMATEX f; spatial_format(f, rate);
    bool ok = SUCCEEDED(sac->IsAudioObjectFormatSupported(&f));
    AudioObjectType native = AudioObjectType_None;
    if (ok) ok = SUCCEEDED(sac->GetNativeStaticObjectTypeMask(&native));
    if (ok) ok = (native & bed_mask()) == bed_mask();
    if (ok && out) { *out = sac; return true; }
    sac->Release();
    return ok;
  }

  bool open_spatial(IMMDeviceEnumerator* en, Spatial& sp) {
    IMMDevice* dev = nullptr;
    if (!pick_endpoint(en, fmt_.device, &dev)) return false;
    bool ok = probe_spatial(dev, fmt_.rate, &sp.sac);
    safe_release(dev);
    if (!ok) return false;
    WAVEFORMATEX f; spatial_format(f, fmt_.rate);
    sp.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SpatialAudioObjectRenderStreamActivationParams ap = {};
    ap.ObjectFormat = &f;
    ap.StaticObjectTypeMask = bed_mask();
    ap.MinDynamicObjectCount = 0;
    ap.MaxDynamicObjectCount = 0;
    ap.Category = AudioCategory_GameMedia;
    ap.EventHandle = sp.ev;
    ap.NotifyObject = nullptr;
    PROPVARIANT pv; PropVariantInit(&pv);
    pv.vt = VT_BLOB; pv.blob.cbSize = sizeof(ap); pv.blob.pBlobData = (BYTE*)&ap;
    HRESULT hr = sp.sac->ActivateSpatialAudioStream(&pv, IID_PPV_ARGS(&sp.st));
    if (FAILED(hr) || !sp.st) { std::fprintf(stderr, "[audio] spatial stream activation failed 0x%08lx\n", (unsigned long)hr); return false; }
    for (int i = 0; i < 6; ++i)
      if (FAILED(sp.st->ActivateSpatialAudioObject(bed_type(mix::kBed51[i]), &sp.obj[i]))) return false;
    if (FAILED(sp.st->Start())) return false;
    sp.started = true;
    return true;
  }

  // Returns false if the stream broke (rebuild), true if stopped.
  bool run_spatial(Spatial& sp, HANDLE changeEv) {
    HANDLE h[3] = {sp.ev, changeEv, stopEv_};
    std::vector<float> mixbuf;
    bool first = true;
    while (run_.load()) {
      DWORD w = WaitForMultipleObjects(3, h, FALSE, 250);
      if (w == WAIT_OBJECT_0 + 2) return true;
      if (w == WAIT_OBJECT_0 + 1) return false;
      if (w == WAIT_TIMEOUT) continue;
      UINT32 dyn = 0, frames = 0;
      HRESULT hr = sp.st->BeginUpdatingAudioObjects(&dyn, &frames);
      if (FAILED(hr)) return false;
      mixbuf.resize((size_t)frames * 6);
      core_mix(mixbuf.data(), frames);
      for (int c = 0; c < 6; ++c) {
        BYTE* buf = nullptr; UINT32 bytes = 0;
        if (SUCCEEDED(sp.obj[c]->GetBuffer(&buf, &bytes)) && buf) {
          float* o = reinterpret_cast<float*>(buf);
          const UINT32 n = std::min<UINT32>(frames, bytes / 4);
          for (UINT32 i = 0; i < n; ++i) o[i] = mixbuf[(size_t)i * 6 + c];
        }
      }
      hr = sp.st->EndUpdatingAudioObjects();
      if (FAILED(hr)) return false;
      if (first) { first = false; core_set_device_info(true, frames, 1000.0 * frames / fmt_.rate * 2.0, 6, fmt_.rate, SNAP_AUDIO_OUT_SPATIAL); }
    }
    return true;
  }

  bool open_stream(IMMDeviceEnumerator* en, Stream& s, uint32_t ch) {
    IMMDevice* dev = nullptr;
    if (!pick_endpoint(en, fmt_.device, &dev)) return false;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.ac);
    safe_release(dev);
    if (FAILED(hr)) return false;
    // Own resampler: run the stream at the device's mix rate so the Windows engine does no conversion, and convert the 48 kHz mix ourselves.
    uint32_t rate = fmt_.rate;
    if (fmt_.resampler != SNAP_AUDIO_RS_SYSTEM) {
      WAVEFORMATEX* mf = nullptr;
      if (SUCCEEDED(s.ac->GetMixFormat(&mf)) && mf) { if (mf->nSamplesPerSec >= 8000 && mf->nSamplesPerSec <= 384000) rate = mf->nSamplesPerSec; CoTaskMemFree(mf); }
    }
    s.rate = rate;
    WAVEFORMATEXTENSIBLE wf;
    make_format(wf, rate, ch);
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    bool done = false, triedLowLat = false;
    lowLat_ = false;
    // Latency: low = the engine's shortest period, normal = its default period (both through IAudioClient3), safe = a 40 ms buffer.
    // An explicit deviceBufferMs always takes the classic path with that size.
    if (!std::getenv("SNAP_AUDIO_NO_LOWLAT") && fmt_.bufferMs == 0 && fmt_.latency != SNAP_AUDIO_LAT_SAFE) {
      IAudioClient3* a3 = nullptr;
      if (SUCCEEDED(s.ac->QueryInterface(IID_PPV_ARGS(&a3)))) {
        triedLowLat = true;
        UINT32 defP = 0, fund = 0, minP = 0, maxP = 0;
        if (SUCCEEDED(a3->GetSharedModeEnginePeriod(&wf.Format, &defP, &fund, &minP, &maxP)) &&
            SUCCEEDED(a3->InitializeSharedAudioStream(flags, fmt_.latency == SNAP_AUDIO_LAT_LOW ? minP : defP, &wf.Format, nullptr))) {
          done = true; lowLat_ = true;
        }
        a3->Release();
      }
    }
    if (!done) {
      if (triedLowLat) {   // a client whose InitializeSharedAudioStream failed cannot be initialized again: take a fresh one
        safe_release(s.ac);
        IMMDevice* d2 = nullptr;
        if (!pick_endpoint(en, fmt_.device, &d2)) return false;
        hr = d2->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&s.ac);
        safe_release(d2);
        if (FAILED(hr)) return false;
      }
      const uint32_t ms = fmt_.bufferMs ? fmt_.bufferMs : (fmt_.latency == SNAP_AUDIO_LAT_SAFE ? 40u : 0u);
      const REFERENCE_TIME dur = (REFERENCE_TIME)ms * 10000;
      hr = s.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, &wf.Format, nullptr);
      if (FAILED(hr)) {
        std::fprintf(stderr, "[audio] WASAPI Initialize failed 0x%08lx\n", (unsigned long)hr);
        return false;
      }
    }
    if (FAILED(s.ac->SetEventHandle(s.ev)) || FAILED(s.ac->GetBufferSize(&s.frames)) ||
        FAILED(s.ac->GetService(IID_PPV_ARGS(&s.rc))))
      return false;
    // prefill with silence, then start
    BYTE* p = nullptr;
    if (SUCCEEDED(s.rc->GetBuffer(s.frames, &p))) s.rc->ReleaseBuffer(s.frames, AUDCLNT_BUFFERFLAGS_SILENT);
    REFERENCE_TIME lat = 0;
    s.ac->GetStreamLatency(&lat);
    const double bufMs = 1000.0 * s.frames / s.rate;
    core_set_device_info(true, s.frames, lat / 10000.0 + bufMs, ch, s.rate, ch == 2 ? SNAP_AUDIO_OUT_STEREO : SNAP_AUDIO_OUT_SPEAKERS);
    return SUCCEEDED(s.ac->Start());
  }

  void timer_wait(std::chrono::milliseconds total) {  // consume at stream rate while there is no device
    using clk = std::chrono::steady_clock;
    const uint32_t block = fmt_.rate / 100;
    std::vector<float> buf((size_t)block * 8);
    const auto period = std::chrono::nanoseconds(1000000000ll * block / fmt_.rate);
    auto next = clk::now() + period;
    const auto end = clk::now() + total;
    while (run_.load() && clk::now() < end) {
      core_mix(buf.data(), block);   // the core's current channel count never exceeds 8
      std::this_thread::sleep_until(next);
      next += period;
    }
  }

  void thread_main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (mm) AvSetMmThreadPriority(mm, AVRT_PRIORITY_HIGH);
    HANDLE changeEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    Notify* nc = new Notify(changeEv);
    if (en) en->RegisterEndpointNotificationCallback(nc);
    bool first = true, warnedMissing = false;
    while (run_.load()) {
      Stream s;
      Spatial sp;
      // Mode ladder: the configured mode first, then speakers 5.1, then stereo. Re-tried from the top on every rebuild.
      int mode = fmt_.mode;
      uint32_t ch = fmt_.channels;
      bool ok = false, spatialOpen = false;
      {  // remember which endpoint this build uses (device-state notifications for other endpoints are ignored)
        IMMDevice* cd = nullptr; LPWSTR cid = nullptr;
        bool pinned = false, missing = false;
        if (en && pick_endpoint(en, fmt_.device, &cd, &pinned, &missing) && SUCCEEDED(cd->GetId(&cid))) nc->set_current(cid);
        else nc->set_current(nullptr);
        nc->set_pinned(pinned, missing);
        if (missing && !warnedMissing) { warnedMissing = true; std::fprintf(stderr, "[audio] output device \"%s\" not found, using the Windows default until it appears\n", fmt_.device.c_str()); }
        if (cid) CoTaskMemFree(cid);
        safe_release(cd);
      }
      if (en && mode == SNAP_AUDIO_OUT_SPATIAL) {
        if (open_spatial(en, sp)) { ok = true; spatialOpen = true; }
        else {
          std::fprintf(stderr, "[audio] spatial audio unavailable on the default device (enable Windows Sonic or Dolby Atmos for it), falling back to 5.1 speakers\n");
          sp.~Spatial(); new (&sp) Spatial();
          mode = SNAP_AUDIO_OUT_SPEAKERS; ch = 6;
        }
      }
      if (en && !ok && mode == SNAP_AUDIO_OUT_SPEAKERS) {
        if (open_stream(en, s, ch)) ok = true;
        else { std::fprintf(stderr, "[audio] %u-channel stream failed, falling back to stereo\n", ch); s.~Stream(); new (&s) Stream(); mode = SNAP_AUDIO_OUT_STEREO; ch = 2; }
      }
      if (en && !ok) { ok = open_stream(en, s, 2); ch = 2; mode = SNAP_AUDIO_OUT_STEREO; }
      if (!ok) {
        core_set_device_info(false, 0, 0.0, 0, fmt_.rate, mode);
        timer_wait(std::chrono::milliseconds(500));  // no endpoint: keep time, retry
        continue;
      }
      if (!first) core_notify_device_change();
      first = false;
      if (spatialOpen) {
        std::fprintf(stderr, "[audio] spatial audio stream: 6 static bed objects (FL FR FC LFE SL SR)\n");
        core_set_device_info(true, 480, 20.0, 6, fmt_.rate, SNAP_AUDIO_OUT_SPATIAL);
        if (!run_spatial(sp, changeEv)) core_set_device_info(false, 0, 0.0, 0, fmt_.rate, SNAP_AUDIO_OUT_SPATIAL);
        continue;
      }
      std::fprintf(stderr, "[audio] WASAPI stream: %u ch, %u Hz, %u frames (%.1f ms)%s, latency %s, resampler %s\n", ch, s.rate, s.frames, 1000.0 * s.frames / s.rate,
                   lowLat_ ? " via IAudioClient3" : "", fmt_.latency == SNAP_AUDIO_LAT_LOW ? "low" : fmt_.latency == SNAP_AUDIO_LAT_SAFE ? "safe" : "normal",
                   s.rate == fmt_.rate ? "none (48 kHz)" : fmt_.resampler == SNAP_AUDIO_RS_SINC ? "sinc" : "linear");
      dsp::Resampler rs;   // 48 kHz mix -> stream rate; passthrough when they match
      rs.init(fmt_.rate, s.rate, ch, fmt_.resampler == SNAP_AUDIO_RS_LINEAR ? dsp::ResampleQuality::Linear : dsp::ResampleQuality::Sinc);
      HANDLE h[3] = {s.ev, changeEv, stopEv_};
      bool rebuild = false;
      while (run_.load() && !rebuild) {
        DWORD w = WaitForMultipleObjects(3, h, FALSE, 250);
        if (w == WAIT_OBJECT_0 + 2) break;
        if (w == WAIT_OBJECT_0 + 1) { rebuild = true; break; }
        UINT32 pad = 0;
        HRESULT hr = s.ac->GetCurrentPadding(&pad);
        if (FAILED(hr)) { rebuild = true; break; }
        const UINT32 avail = s.frames - pad;
        if (!avail) continue;
        BYTE* p = nullptr;
        hr = s.rc->GetBuffer(avail, &p);
        if (FAILED(hr)) { rebuild = true; break; }  // AUDCLNT_E_DEVICE_INVALIDATED etc.
        rs.produce(reinterpret_cast<float*>(p), avail, [](float* d, uint32_t n) { core_mix(d, n); });
        s.rc->ReleaseBuffer(avail, 0);
      }
      if (rebuild) core_set_device_info(false, 0, 0.0, 0, fmt_.rate, mode);
    }
    if (en) { en->UnregisterEndpointNotificationCallback(nc); en->Release(); }
    nc->Release();
    CloseHandle(changeEv);
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
  }

  DeviceFormat fmt_{};
  bool lowLat_ = false;
  std::atomic<bool> run_{false};
  HANDLE stopEv_ = nullptr;
  std::thread th_;
};

}  // namespace

Backend* make_wasapi_backend() { return new WasapiBackend(); }

std::vector<std::string> list_render_devices() {
  std::vector<std::string> out;
  const HRESULT hi = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE (an STA caller) is fine: COM is already up on this thread
  IMMDeviceEnumerator* en = nullptr;
  IMMDeviceCollection* col = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
      SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
      IMMDevice* d = nullptr;
      if (FAILED(col->Item(i, &d)) || !d) continue;
      const std::string name = friendly_name(d);
      if (!name.empty()) out.push_back(name);
      d->Release();
    }
  }
  safe_release(col); safe_release(en);
  if (SUCCEEDED(hi)) CoUninitialize();
  return out;
}

}  // namespace snap_audio
#endif
