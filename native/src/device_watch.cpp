/**
 * DeviceWatcher implementation (see device_watch.h).
 */

#include "device_watch.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#endif

namespace ainoiceguard {

DeviceWatcher& DeviceWatcher::instance() {
  static DeviceWatcher w;
  return w;
}

void DeviceWatcher::setCallback(Callback cb) {
  std::lock_guard<std::mutex> lock(cbMutex_);
  cb_ = std::move(cb);
}

#ifdef _WIN32

namespace {

/* PKEY_Device_FriendlyName ("Speakers (Synaptics HD Audio)"), the name the
 * PortAudio WASAPI host reports. Defined here to avoid initguid tricks. */
const PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string toUtf8(const wchar_t* w) {
  if (!w) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s(size_t(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}

std::string friendlyName(IMMDevice* dev) {
  std::string name;
  IPropertyStore* props = nullptr;
  if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = toUtf8(v.pwszVal);
    PropVariantClear(&v);
    props->Release();
  }
  return name;
}

/* Minimal COM object: every notification just pokes the watcher. Must not
 * block (Windows calls these on its own threads). */
class Client : public IMMNotificationClient {
 public:
  explicit Client(DeviceWatcher* w) : w_(w) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return poke(); }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return poke(); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return poke(); }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override {
    /* eConsole is the "Default Device" users pick; ignore duplicate roles. */
    return role == eConsole ? poke() : S_OK;
  }
  /* Property changes (volume names, formats...) are frequent and irrelevant. */
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

 private:
  HRESULT poke() {
    w_->notifyRaw();
    return S_OK;
  }
  DeviceWatcher* w_;
  LONG ref_ = 1;
};

}  // namespace

void DeviceWatcher::notifyRaw() {
  if (changeEvent_) SetEvent(static_cast<HANDLE>(changeEvent_));
}

bool DeviceWatcher::start() {
  if (running_.exchange(true)) return true;
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  changeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!stopEvent_ || !changeEvent_) {
    running_.store(false);
    return false;
  }
  thread_ = std::thread(&DeviceWatcher::run, this);
  return true;
}

void DeviceWatcher::stop() {
  if (!running_.exchange(false)) return;
  SetEvent(static_cast<HANDLE>(stopEvent_));
  if (thread_.joinable()) thread_.join();
  CloseHandle(static_cast<HANDLE>(stopEvent_));
  CloseHandle(static_cast<HANDLE>(changeEvent_));
  stopEvent_ = changeEvent_ = nullptr;
}

void DeviceWatcher::run() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr;
  Client* client = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) {
    client = new Client(this);
    if (FAILED(en->RegisterEndpointNotificationCallback(client))) {
      client->Release();
      client = nullptr;
    }
  }

  HANDLE events[2] = {static_cast<HANDLE>(stopEvent_), static_cast<HANDLE>(changeEvent_)};
  for (;;) {
    const DWORD w = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0 + 1) break; /* stop (or error) */
    /* Debounce: wait until no further notification for 300 ms. */
    while (WaitForSingleObject(events[1], 300) == WAIT_OBJECT_0) {
    }
    if (WaitForSingleObject(events[0], 0) == WAIT_OBJECT_0) break;
    changes_.fetch_add(1, std::memory_order_acq_rel);
    Callback cb;
    {
      std::lock_guard<std::mutex> lock(cbMutex_);
      cb = cb_;
    }
    if (cb) cb();
  }

  if (en && client) en->UnregisterEndpointNotificationCallback(client);
  if (client) client->Release();
  if (en) en->Release();
  if (SUCCEEDED(co)) CoUninitialize();
}

bool DeviceWatcher::snapshot(EndpointSnapshot& out) {
  out = EndpointSnapshot{};
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  /* RPC_E_CHANGED_MODE: the thread is already STA (e.g. Electron's main
   * thread); MMDevice works there too, but we must not CoUninitialize. */
  const bool uninit = SUCCEEDED(co);
  if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;

  bool ok = false;
  IMMDeviceEnumerator* en = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) {
    ok = true;
    for (int flow = 0; flow < 2; flow++) {
      const EDataFlow df = flow == 0 ? eCapture : eRender;
      auto& list = flow == 0 ? out.capture : out.render;
      IMMDeviceCollection* col = nullptr;
      if (SUCCEEDED(en->EnumAudioEndpoints(df, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
          IMMDevice* d = nullptr;
          if (SUCCEEDED(col->Item(i, &d))) {
            const std::string name = friendlyName(d);
            if (!name.empty()) list.push_back(name);
            d->Release();
          }
        }
        col->Release();
      }
      IMMDevice* def = nullptr;
      if (SUCCEEDED(en->GetDefaultAudioEndpoint(df, eConsole, &def))) {
        (flow == 0 ? out.defaultCapture : out.defaultRender) = friendlyName(def);
        def->Release();
      }
    }
    en->Release();
  }
  if (uninit) CoUninitialize();
  return ok;
}

#else  /* !_WIN32: not implemented yet (see docs/phase5-robustness.md) */

void DeviceWatcher::notifyRaw() {}
bool DeviceWatcher::start() { return false; }
void DeviceWatcher::stop() {}
void DeviceWatcher::run() {}
bool DeviceWatcher::snapshot(EndpointSnapshot& out) {
  out = EndpointSnapshot{};
  return false;
}

#endif

}  // namespace ainoiceguard
