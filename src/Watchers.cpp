#include "Watchers.h"

#include <shlobj.h>
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
