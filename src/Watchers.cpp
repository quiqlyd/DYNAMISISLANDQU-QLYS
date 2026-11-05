#include "Watchers.h"

#include <shlobj.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <cfgmgr32.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Sağlık
// ---------------------------------------------------------------------------

namespace {
ULONGLONG ToU64(const FILETIME& f) {
    return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

// netioapi.h'deki NL_NETWORK_CONNECTIVITY_HINT ile aynı yerleşim. Fonksiyon Windows 10
// 2004 ile geldi; eski sistemlerde açılış bozulmasın diye çalışma anında yüklenir.
struct ConnectivityHint {
    int level;  // 0 bilinmiyor, 1 yok, 2 sadece yerel, 3 internet, 4 kısıtlı, 5 gizli
    int cost;
    BOOLEAN approachingDataLimit;
    BOOLEAN overDataLimit;
    BOOLEAN roaming;
};
}  // namespace

void SystemHealth::Sample() {
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        ULONGLONG i = ToU64(idle), t = ToU64(kernel) + ToU64(user);  // çekirdek boşta süreyi de içerir
        if (lastTotal_ && t > lastTotal_) {
            double busy = 1.0 - static_cast<double>(i - lastIdle_) / static_cast<double>(t - lastTotal_);
            cpu_ = static_cast<float>(std::clamp(busy, 0.0, 1.0));
        }
        lastIdle_ = i;
        lastTotal_ = t;
    }

    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms)) ram_ = ms.dwMemoryLoad / 100.f;

    if (!hintLoaded_) {
        hintLoaded_ = true;
        if (HMODULE m = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            hint_ = reinterpret_cast<HintFn>(GetProcAddress(m, "GetNetworkConnectivityHint"));
    }
    if (hint_) {
        ConnectivityHint h{};
        if (hint_(&h) == NO_ERROR) {
            if (h.level == 1 || h.level == 2) net_ = Net::Offline;
            else if (h.level == 3 || h.level == 4) net_ = Net::Online;
        }
    }
}

// ---------------------------------------------------------------------------
// Kamera / mikrofon
// ---------------------------------------------------------------------------

namespace {
constexpr wchar_t kConsentRoot[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";

bool AppInUse(HKEY app) {
    ULONGLONG start = 0, stop = 0;
    DWORD size = sizeof(start);
    if (RegQueryValueExW(app, L"LastUsedTimeStart", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&start), &size) != ERROR_SUCCESS)
        return false;
    size = sizeof(stop);
    if (RegQueryValueExW(app, L"LastUsedTimeStop", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&stop), &size) != ERROR_SUCCESS)
        return false;
    return start != 0 && stop == 0;  // başladı ama bitmedi = şu an kullanılıyor
}

bool AnySubkeyInUse(HKEY parent, int depth) {
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = ARRAYSIZE(name);
        if (RegEnumKeyExW(parent, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY child;
        if (RegOpenKeyExW(parent, name, 0, KEY_READ, &child) != ERROR_SUCCESS) continue;
        bool used = AppInUse(child) || (depth > 0 && AnySubkeyInUse(child, depth - 1));
        RegCloseKey(child);
        if (used) return true;
    }
    return false;
}
}  // namespace

PrivacyWatch::~PrivacyWatch() {
    for (Watch* w : {&cam_, &micW_}) {
        if (w->key) RegCloseKey(w->key);
        if (w->event) CloseHandle(w->event);
    }
}

bool PrivacyWatch::InUse(HKEY key) {
    return key && AnySubkeyInUse(key, 1);  // uygulamalar + NonPackaged\<exe yolu>
}

void PrivacyWatch::Arm(Watch& w) {
    if (!w.key || !w.event) return;
    RegNotifyChangeKeyValue(w.key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME |
                                             REG_NOTIFY_THREAD_AGNOSTIC,
                            w.event, TRUE);
}

void PrivacyWatch::Start() {
    const wchar_t* names[] = {L"webcam", L"microphone"};
    Watch* watches[] = {&cam_, &micW_};
    for (int i = 0; i < 2; ++i) {
        std::wstring path = std::wstring(kConsentRoot) + names[i];
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ | KEY_NOTIFY,
                          &watches[i]->key) != ERROR_SUCCESS) {
            watches[i]->key = nullptr;
            continue;
        }
        watches[i]->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Arm(*watches[i]);
    }
    camera_ = InUse(cam_.key);
    mic_ = InUse(micW_.key);
}

bool PrivacyWatch::Poll() {
    bool changed = false;
    if (cam_.event && WaitForSingleObject(cam_.event, 0) == WAIT_OBJECT_0) {
        Arm(cam_);
        bool now = InUse(cam_.key);
        changed |= now != camera_;
        camera_ = now;
    }
    if (micW_.event && WaitForSingleObject(micW_.event, 0) == WAIT_OBJECT_0) {
        Arm(micW_);
        bool now = InUse(micW_.key);
        changed |= now != mic_;
        mic_ = now;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// İndirmeler
// ---------------------------------------------------------------------------

namespace {
std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool IsTempName(const std::wstring& name) {
    if (name.empty() || name[0] == L'.' || name[0] == L'~') return true;
    std::wstring l = Lower(name);
    for (const wchar_t* ext : {L".crdownload", L".part", L".partial", L".tmp", L".download",
                               L".opdownload", L".!ut", L".ini", L".lnk"})
        if (EndsWith(l, ext)) return true;
    return l.rfind(L"unconfirmed ", 0) == 0;
}

bool LooksFinished(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (fa.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) return false;
    if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0) return false;
    // Firefox önce boş bir yer tutucu + ".part" açar: ".part" hâlâ varsa bitmemiştir.
    for (const wchar_t* ext : {L".part", L".crdownload"})
        if (GetFileAttributesW((path + ext).c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    return true;
}
}  // namespace

void DownloadWatch::Start(HWND notify) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw))) return;
    std::wstring dir = raw;
    CoTaskMemFree(raw);

    std::thread([notify, dir] {
        HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<DWORD> buf(16 * 1024 / sizeof(DWORD));
        std::wstring lastSent;
        ULONGLONG lastSentAt = 0;
        for (;;) {
            DWORD bytes = 0;
            // Değişiklik olana kadar uyur: işlemci harcamaz.
            if (!ReadDirectoryChangesW(h, buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORD)),
                                       FALSE, FILE_NOTIFY_CHANGE_FILE_NAME, &bytes, nullptr, nullptr))
                break;
            if (bytes == 0) continue;  // taşma: bu turu atla

            std::wstring candidate;
            auto* p = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data());
            for (;;) {
                if (p->Action == FILE_ACTION_ADDED || p->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                    std::wstring name(p->FileName, p->FileNameLength / sizeof(wchar_t));
                    if (name.find(L'\\') == std::wstring::npos && !IsTempName(name))
                        candidate = name;  // aynı anda birkaç dosya gelirse sonuncusu
                }
                if (!p->NextEntryOffset) break;
                p = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(p) +
                                                                p->NextEntryOffset);
            }
            if (candidate.empty()) continue;

            Sleep(1500);  // tarayıcı son yeniden adlandırmayı bitirsin
            std::wstring path = dir + L"\\" + candidate;
            ULONGLONG now = GetTickCount64();
            if (!LooksFinished(path)) continue;
            if (path == lastSent && now - lastSentAt < 30000) continue;
            lastSent = path;
            lastSentAt = now;
            auto* msg = new std::wstring(path);
            if (!PostMessageW(notify, WM_DOWNLOAD_DONE, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
        }
        CloseHandle(h);
    }).detach();
}

// ---------------------------------------------------------------------------
// Ses ölçer
// ---------------------------------------------------------------------------

AudioMeter::~AudioMeter() {
    if (meter_) meter_->Release();
}

void AudioMeter::Acquire() {
    if (meter_) {
        meter_->Release();
        meter_ = nullptr;
    }
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&en))))
        return;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr,
                      reinterpret_cast<void**>(&meter_));
        dev->Release();
    }
    en->Release();
}

float AudioMeter::Peak(double now) {
    // Varsayılan hoparlör değişebilir (kulaklık takıldı vb.): arada bir yeniden bağlan.
    if (!meter_ ? now - acquiredAt_ > 5 : now - acquiredAt_ > 20) {
        acquiredAt_ = now;
        Acquire();
    }
    if (!meter_) return -1.f;
    float peak = 0;
    if (FAILED(meter_->GetPeakValue(&peak))) {
        meter_->Release();
        meter_ = nullptr;
        return -1.f;
    }
    return peak;
}

// ---------------------------------------------------------------------------
// Ses düzeyi
// ---------------------------------------------------------------------------

namespace {
class VolumeCallback : public IAudioEndpointVolumeCallback, public IMMNotificationClient {
public:
    explicit VolumeCallback(HWND hwnd) : hwnd_(hwnd) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback))
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
        else if (riid == __uuidof(IMMNotificationClient))
            *out = static_cast<IMMNotificationClient*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (d) {
            int pct = static_cast<int>(d->fMasterVolume * 100.f + 0.5f);
            PostMessageW(hwnd_, VolumeWatch::WM_VOLUME_CHANGED, static_cast<WPARAM>(pct), d->bMuted ? 1 : 0);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) PostMessageW(hwnd_, VolumeWatch::WM_VOLUME_DEVICE, 0, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HWND hwnd_;
    std::atomic<ULONG> refs_{1};
};

Microsoft::WRL::ComPtr<IMMDeviceEnumerator> g_enum;
Microsoft::WRL::ComPtr<IAudioEndpointVolume> g_volume;
VolumeCallback* g_volumeCb = nullptr;
}  // namespace

void VolumeWatch::Start(HWND notify) {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_enum))))
        return;
    g_volumeCb = new VolumeCallback(notify);
    g_enum->RegisterEndpointNotificationCallback(g_volumeCb);
    Rebind();
}

void VolumeWatch::Rebind() {
    if (!g_enum || !g_volumeCb) return;
    if (g_volume) {
        g_volume->UnregisterControlChangeNotify(g_volumeCb);
        g_volume.Reset();
    }
    Microsoft::WRL::ComPtr<IMMDevice> dev;
    if (FAILED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                             reinterpret_cast<void**>(g_volume.GetAddressOf()))))
        return;
    g_volume->RegisterControlChangeNotify(g_volumeCb);
}

bool VolumeWatch::Get(int& percent, bool& muted) {
    if (!g_volume) return false;
    float v = 0;
    BOOL m = FALSE;
    if (FAILED(g_volume->GetMasterVolumeLevelScalar(&v)) || FAILED(g_volume->GetMute(&m))) return false;
    percent = static_cast<int>(v * 100.f + 0.5f);
    muted = m != FALSE;
    return true;
}

void VolumeWatch::Set(int percent) {
    if (!g_volume) return;
    percent = std::clamp(percent, 0, 100);
    g_volume->SetMasterVolumeLevelScalar(percent / 100.f, nullptr);
    BOOL m = FALSE;
    if (percent > 0 && SUCCEEDED(g_volume->GetMute(&m)) && m) g_volume->SetMute(FALSE, nullptr);
}

// ---------------------------------------------------------------------------
// Bluetooth kulaklık pili
// ---------------------------------------------------------------------------

namespace {
// Windows'un Bluetooth pil özelliği (Ayarlar > Bluetooth'ta görünen yüzde).
const DEVPROPKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const DEVPROPKEY kBtBattery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};

bool ReadBattery(int& level, std::wstring& name) {
    for (const wchar_t* enumerator : {L"BTHENUM", L"BTHLE", L"BTHLEDEVICE"}) {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, enumerator, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (set == INVALID_HANDLE_VALUE) continue;
        SP_DEVINFO_DATA dev{sizeof(dev)};
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
            DEVPROPTYPE type = 0;
            BYTE value = 0;
            if (!SetupDiGetDevicePropertyW(set, &dev, &kBtBattery, &type, &value, sizeof(value), nullptr, 0) ||
                type != DEVPROP_TYPE_BYTE || value > 100)
                continue;
            ULONG status = 0, problem = 0;
            if (CM_Get_DevNode_Status(&status, &problem, dev.DevInst, 0) != CR_SUCCESS ||
                !(status & DN_STARTED))
                continue;  // eşleşmiş ama bağlı değil
            wchar_t buf[256] = L"";
            DEVPROPTYPE t2 = 0;
            if (!SetupDiGetDevicePropertyW(set, &dev, &kFriendlyName, &t2,
                                           reinterpret_cast<BYTE*>(buf), sizeof(buf), nullptr, 0))
                buf[0] = 0;
            level = value;
            name = buf;
            // "Hands-Free AG" gibi ekleri at: sadece cihaz adı kalsın.
            for (const wchar_t* suffix : {L" Hands-Free AG", L" Hands-Free", L" Avrcp Transport", L" Stereo"}) {
                size_t pos = name.find(suffix);
                if (pos != std::wstring::npos) name.erase(pos);
            }
            SetupDiDestroyDeviceInfoList(set);
            return true;
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    return false;
}
}  // namespace

void BtBattery::Start(HWND notify) {
    std::thread([notify] {
        int lastLevel = -1;
        std::wstring lastName;
        for (;;) {
            int level = 255;
            std::wstring name;
            if (!ReadBattery(level, name)) level = 255;
            if (level != lastLevel || name != lastName) {
                lastLevel = level;
                lastName = name;
                auto* msg = new std::wstring(name);
                if (!PostMessageW(notify, WM_BT_BATTERY, static_cast<WPARAM>(level), reinterpret_cast<LPARAM>(msg)))
                    delete msg;
            }
            Sleep(30000);
        }
    }).detach();
}
