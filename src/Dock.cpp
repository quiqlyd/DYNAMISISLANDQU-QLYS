// Guti Dock: yeni taskbar. Bkz. Dock.h
#include "Dock.h"

#include <appmodel.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_map>

#include "IslandUi.h"

using Microsoft::WRL::ComPtr;
using namespace isl;

namespace {

// Ölçüler (DIP)
constexpr float kIcon = 44.f;      // ikon boyu
constexpr float kSlot = 54.f;      // ikon + aralık
constexpr float kSepW = 16.f;      // ayırıcı genişliği
constexpr float kBodyH = 62.f;     // cam gövde yüksekliği
constexpr float kBottom = 8.f;     // ekranın altından boşluk
constexpr float kPadX = 9.f;       // gövde iç yan boşluğu
constexpr float kRadius = 21.f;    // gövde köşe yarıçapı
constexpr float kMaxGrow = 22.f;   // büyütme: 44 -> 66 (1.5x)
constexpr float kRange = 150.f;    // büyütmenin etki yarıçapı
constexpr float kTopRoom = 96.f;   // gövdenin üstünde büyüyen ikon + isim balonu için yer
constexpr float kSideRoom = 110.f; // yanlarda büyüme ve gölge payı
constexpr float kIconBottom = kBodyH - (kBodyH - kIcon) / 2.f;
constexpr float kBounce = 15.f, kBouncePeriod = 0.62f;

constexpr UINT kFrameMsg = WM_APP + 60;
constexpr UINT kAppBarMsg = WM_APP + 61;
constexpr UINT kRehideMsg = WM_APP + 62;
constexpr UINT_PTR kTimerRescan = 1;
constexpr UINT_PTR kTimerHoverLabel = 2;

constexpr wchar_t kPinnedDir[] = L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

// PKEY_AppUserModel_ID (propkey.h'yi INITGUID ile çekmemek için)
const PROPERTYKEY kAumidKey = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

Dock* g_dock = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    Dock::RestoreTaskbarNow();  // çökse bile kullanıcı taskbar'sız kalmasın
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* tail) {
    size_t n = wcslen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::wstring ProcessPath(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return out;
    wchar_t buf[MAX_PATH * 2];
    DWORD len = ARRAYSIZE(buf);
    if (QueryFullProcessImageNameW(p, 0, buf, &len)) out.assign(buf, len);
    CloseHandle(p);
    return out;
}

std::wstring ProcessAumid(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return out;
    wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH];
    UINT32 len = ARRAYSIZE(buf);
    if (GetApplicationUserModelId(p, &len, buf) == ERROR_SUCCESS) out = buf;
    CloseHandle(p);
    return out;
}

std::wstring WindowAumid(HWND hwnd) {
    std::wstring out;
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return out;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(store->GetValue(kAumidKey, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
    PropVariantClear(&pv);
    return out;
}

std::wstring FileDescription(const std::wstring& exe) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    struct Lang { WORD lang, cp; }* langs = nullptr;
    UINT n = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &n) || !n)
        return {};
    wchar_t q[80];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* text = nullptr;
    UINT tl = 0;
    if (VerQueryValueW(data.data(), q, reinterpret_cast<void**>(&text), &tl) && text && *text) return text;
    return {};
}

std::wstring ShellName(const std::wstring& parsing) {
    std::wstring out;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item)))) return out;
    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        out = name;
        CoTaskMemFree(name);
    }
    return out;
}

// .lnk dosyasının hedef exe'si ve AUMID'si
void ResolveLink(const std::wstring& lnk, std::wstring& exe, std::wstring& aumid) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ))) return;
    wchar_t path[MAX_PATH * 2] = {};
    if (SUCCEEDED(link->GetPath(path, ARRAYSIZE(path), nullptr, 0)) && path[0]) {
        wchar_t expanded[MAX_PATH * 2];
        if (ExpandEnvironmentStringsW(path, expanded, ARRAYSIZE(expanded))) exe = expanded;
        else exe = path;
    }
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link.As(&store))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kAumidKey, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) aumid = pv.pwszVal;
        PropVariantClear(&pv);
    }
}

bool LightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradient;
    int animation;
};
struct CompositionData {
    int attribute;
    PVOID data;
    SIZE_T size;
};
using SetWca = BOOL(WINAPI*)(HWND, CompositionData*);

void SetAccentBlur(HWND hwnd, bool light) {
    static auto fn = reinterpret_cast<SetWca>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return;
    // Renk AABBGGRR: koyu temada dumanlı koyu cam, açıkta süt beyazı cam.
    AccentPolicy ap{4 /* ACRYLIC */, 2, light ? 0x70F4F2F2u : 0x5A1E1A1Au, 0};
    CompositionData d{19 /* WCA_ACCENT_POLICY */, &ap, sizeof(ap)};
    if (!fn(hwnd, &d)) {
        ap.state = 3;  // eski sistemlerde düz bulanıklık
        fn(hwnd, &d);
    }
}

std::unordered_map<HWND, std::pair<std::wstring, std::wstring>> g_describeCache;  // hwnd -> (aumid, exe)

}  // namespace

// ---------------------------------------------------------------------------
// Windows taskbar'ını gizleme / geri getirme
// ---------------------------------------------------------------------------

void Dock::RestoreTaskbarNow() {
    if (ReadSetting(L"DockTbHidden", 0) == 0) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = tray;
        abd.lParam = static_cast<LPARAM>(ReadSetting(L"DockTbState", ABS_ALWAYSONTOP));
        SHAppBarMessage(ABM_SETSTATE, &abd);
        ShowWindow(tray, SW_SHOWNA);
    }
    WriteSetting(L"DockTbHidden", 0);
}

bool Dock::ForegroundFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64];
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    LONG_PTR ex = GetWindowLongPtrW(fg, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return false;  // ekran kaydı / kenar ışığı gibi katmanlar
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

void Dock::SetFullscreen(bool on) {
    if (on == fullscreen_) return;
    // Oyun / tam ekran video: dock tamamen çekilir.
    fullscreen_ = on;
    ShowWindow(glass_, on ? SW_HIDE : SW_SHOWNOACTIVATE);
    ShowWindow(overlay_, on ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (!on) {
        SetWindowPos(glass_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        StartAnimation();
    }
}

void Dock::RepairOnStartup(bool dockEnabled) {
    if (!dockEnabled) RestoreTaskbarNow();
}

void Dock::HideTaskbar() {
    tray_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = tray_;
    if (ReadSetting(L"DockTbHidden", 0) == 0) {
        // Kullanıcının asıl ayarını sakla: dock kapanınca aynen geri koyarız.
        WriteSetting(L"DockTbState", static_cast<DWORD>(SHAppBarMessage(ABM_GETSTATE, &abd)));
        WriteSetting(L"DockTbHidden", 1);
    }
    // Otomatik gizle: Windows taskbar'ı ekranda yer kaplamasın; görünmez yap: hiç çıkmasın.
    abd.lParam = ABS_AUTOHIDE;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    ShowWindow(tray_, SW_HIDE);
}

void Dock::RestoreTaskbar() {
    RestoreTaskbarNow();
    tray_ = nullptr;
}

// ---------------------------------------------------------------------------
// Açma / kapama
// ---------------------------------------------------------------------------

bool Dock::Enable(HINSTANCE instance, ID2D1Factory* d2d, IDWriteFactory* dwrite) {
    if (overlay_) return true;
    instance_ = instance;
    d2d_ = d2d;
    dwrite_ = dwrite;
    g_dock = this;
    QueryPerformanceFrequency(&qpcFreq_);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"tr-tr", &labelFormat_);
    if (labelFormat_) {
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = GlassProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"GutiDockGlass";
        RegisterClassExW(&wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"GutiDock";
        RegisterClassExW(&wc);
        registered = true;
    }
    glass_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GutiDockGlass", L"Guti Dock Glass",
                             WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!glass_) return false;
    // Üst pencerenin sahibi cam pencere: Windows onu her zaman camın üstünde tutar.
    overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GutiDock",
                               L"Guti Dock", WS_POPUP, 0, 0, 1, 1, glass_, nullptr, instance, this);
    if (!overlay_) {
        DestroyWindow(glass_);
        glass_ = nullptr;
        return false;
    }
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    DWORD corner = 1;  // DWMWCP_DONOTROUND: köşeleri biz yuvarlatıyoruz
    DwmSetWindowAttribute(glass_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(glass_, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof(none));

    labelA_.stiffness = 420.f;
    labelA_.damping = 36.f;
    UpdateMonitor();
    ApplyGlass();
    LoadPins();
    Rescan();
    for (auto& a : apps_) a.appear.Snap(100.f);

    quit_ = false;
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    frameThread_ = std::thread([this] { FrameLoop(); });

    // Pencere olayları: açılan/kapanan/öne gelen pencereleri sorgulamadan öğreniriz.
    auto hook = [&](DWORD a, DWORD b) {
        HWINEVENTHOOK h = SetWinEventHook(a, b, nullptr, WinEventProc, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) hooks_.push_back(h);
    };
    hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);

    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);
    OnFrame();  // ilk kare: pencereler yerine otursun
    ShowWindow(glass_, SW_SHOWNOACTIVATE);
    ShowWindow(overlay_, SW_SHOWNOACTIVATE);
    HideTaskbar();  // önce Windows taskbar'ı çekilsin ki dock'un yeri en alttan ayrılsın
    RegisterAppBar();
    StartAnimation();
    return true;
}

void Dock::Disable() {
    if (!overlay_) return;
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    quit_ = true;
    wantFrames_ = false;
    if (frameEvent_) SetEvent(frameEvent_);
    if (frameThread_.joinable()) frameThread_.join();
    if (frameEvent_) CloseHandle(frameEvent_);
    frameEvent_ = nullptr;
    RemoveAppBar();
    RestoreTaskbar();
    SetUnhandledExceptionFilter(g_prevFilter);
    DiscardDevice();
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) DeleteObject(dib_);
    dib_ = nullptr;
    HWND o = overlay_, g = glass_;
    overlay_ = glass_ = nullptr;
    DestroyWindow(o);
    DestroyWindow(g);
    apps_.clear();
    g_describeCache.clear();
    animating_ = false;
    framePending_ = false;
}

void Dock::SetAccent(D2D1_COLOR_F c) {
    accent_ = c;
    if (overlay_) StartAnimation();
}

void Dock::UpdateMonitor() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float scale = dx / 96.f;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = static_cast<int>(std::ceil((kTopRoom + kBodyH + kBottom) * scale));
    bool changed = scale != scale_ || w != texW_ || h != texH_;
    monitor_ = mi.rcMonitor;
    scale_ = scale;
    if (changed) {
        texW_ = w;
        texH_ = h;
        DiscardDevice();
        for (auto& a : apps_) {
            a.icon.Reset();
            a.iconTried = false;
        }
        if (memDC_) {
            SelectObject(memDC_, oldBmp_);
            DeleteDC(memDC_);
            memDC_ = nullptr;
        }
        if (dib_) DeleteObject(dib_);
        dib_ = nullptr;
    }
}

void Dock::ApplyGlass() {
    light_ = LightTheme();
    SetAccentBlur(glass_, light_);
}

void Dock::RegisterAppBar() {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = glass_;
    abd.uCallbackMessage = kAppBarMsg;
    if (!appBar_) appBar_ = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
    if (!appBar_) return;
    // Dock'un yeri ayrılır: büyütülen pencereler dock'un üstünde biter.
    int reserve = static_cast<int>(std::ceil((kBodyH + kBottom + 4.f) * scale_));
    abd.uEdge = ABE_BOTTOM;
    abd.rc = monitor_;
    abd.rc.top = monitor_.bottom - reserve;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - reserve;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

void Dock::RemoveAppBar() {
    if (!appBar_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = glass_;
    SHAppBarMessage(ABM_REMOVE, &abd);
    appBar_ = false;
}

// ---------------------------------------------------------------------------
// Uygulamalar
// ---------------------------------------------------------------------------

void Dock::LoadPins() {
    pinOrder_.clear();
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DockPins", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) ==
            ERROR_SUCCESS &&
        bytes > 2) {
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DockPins", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(),
                         &bytes) == ERROR_SUCCESS) {
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) pinOrder_.push_back(p);
        }
    } else {
        // İlk açılış: Windows taskbar'ına sabitlenmiş uygulamaları al.
        wchar_t appData[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
            std::wstring dir = std::wstring(appData) + kPinnedDir;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    pinOrder_.push_back(dir + L"\\" + fd.cFileName);
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
        }
        // Dosya Gezgini en başta dursun.
        std::stable_partition(pinOrder_.begin(), pinOrder_.end(), [](const std::wstring& p) {
            std::wstring l = Lower(p);
            return l.find(L"file explorer") != std::wstring::npos || l.find(L"dosya gezgini") != std::wstring::npos;
        });
        SavePins();
    }

    apps_.clear();
    App start;
    start.isStart = true;
    start.name = L"Başlat";
    apps_.push_back(std::move(start));
    App sep;
    sep.isSeparator = true;
    apps_.push_back(std::move(sep));
    for (const auto& path : pinOrder_) {
        App a;
        a.pinned = true;
        a.launch = path;
        std::wstring exe, aumid;
        if (EndsWith(Lower(path), L".lnk")) {
            ResolveLink(path, exe, aumid);
            size_t slash = path.find_last_of(L'\\');
            a.name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            a.name.resize(a.name.size() - 4);
        } else if (path.rfind(L"shell:AppsFolder\\", 0) == 0) {
            aumid = path.substr(17);
            a.name = ShellName(path);
        } else {
            exe = path;
            a.name = FileDescription(path);
        }
        a.exe = Lower(exe);
        a.aumid = Lower(aumid);
        a.key = !a.aumid.empty() ? a.aumid : a.exe;
        if (a.name.empty()) a.name = ShellName(path);
        apps_.push_back(std::move(a));
    }
}

void Dock::SavePins() {
    std::wstring multi;
    for (const auto& p : pinOrder_) {
        multi += p;
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, L"DockPins", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()),
                       static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

bool Dock::Eligible(HWND hwnd) const {
    // Windows taskbar'ının gösterdiği pencerelerle aynı kurallar.
    if (!IsWindowVisible(hwnd) || hwnd == glass_ || hwnd == overlay_) return false;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if (!appWindow) {
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
        if (GetWindow(hwnd, GW_OWNER)) return false;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    return true;
}

void Dock::Describe(HWND hwnd, std::wstring& aumid, std::wstring& exe) const {
    auto it = g_describeCache.find(hwnd);
    if (it != g_describeCache.end()) {
        aumid = it->second.first;
        exe = it->second.second;
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    exe = ProcessPath(pid);
    aumid = WindowAumid(hwnd);
    if (EndsWith(Lower(exe), L"\\applicationframehost.exe")) {
        // Mağaza uygulaması: asıl süreç içteki CoreWindow'un sahibidir.
        HWND core = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        if (core) {
            DWORD cpid = 0;
            GetWindowThreadProcessId(core, &cpid);
            if (cpid && cpid != pid) {
                exe = ProcessPath(cpid);
                if (aumid.empty()) aumid = ProcessAumid(cpid);
            }
        }
    }
    if (aumid.empty()) aumid = ProcessAumid(pid);
    g_describeCache[hwnd] = {aumid, exe};
}

std::wstring Dock::NameFor(const App& a, HWND hwnd) const {
    std::wstring n;
    if (!a.aumidRaw.empty()) n = ShellName(L"shell:AppsFolder\\" + a.aumidRaw);
    if (n.empty() && !a.exe.empty()) n = FileDescription(a.exe);
    if (n.empty() && hwnd) {
        wchar_t t[128];
        GetWindowTextW(hwnd, t, ARRAYSIZE(t));
        n = t;
    }
    return n;
}

Dock::App* Dock::FindApp(const std::wstring& aumid, const std::wstring& exe) {
    if (!aumid.empty())
        for (auto& a : apps_)
            if (!a.isStart && !a.isSeparator && a.aumid == aumid) return &a;
    if (!exe.empty() && !EndsWith(exe, L"\\applicationframehost.exe"))
        for (auto& a : apps_)
            if (!a.isStart && !a.isSeparator && a.exe == exe) return &a;
    return nullptr;
}

void Dock::Rescan() {
    for (auto& a : apps_) {
        a.windows.clear();
        a.active = false;
    }
    std::vector<HWND> list;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    std::unordered_map<HWND, std::pair<std::wstring, std::wstring>> keep;
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    for (HWND h : list) {
        if (!Eligible(h)) continue;
        std::wstring aumidRaw, exeRaw;
        Describe(h, aumidRaw, exeRaw);
        keep[h] = {aumidRaw, exeRaw};
        std::wstring aumid = Lower(aumidRaw), exe = Lower(exeRaw);
        App* app = FindApp(aumid, exe);
        if (!app) {
            App a;
            a.aumid = aumid;
            a.aumidRaw = aumidRaw;
            a.exe = exe;
            a.key = !aumid.empty() ? aumid : exe;
            bool packaged = exe.find(L"\\windowsapps\\") != std::wstring::npos ||
                            EndsWith(exe, L"\\applicationframehost.exe") || exe.empty();
            a.launch = (!aumidRaw.empty() && packaged) ? L"shell:AppsFolder\\" + aumidRaw : exeRaw;
            a.name = NameFor(a, h);
            a.appear.Snap(0.f);
            apps_.push_back(std::move(a));
            app = &apps_.back();
        }
        app->windows.push_back(h);
        if (h == fg || h == fgRoot) app->active = true;
    }
    g_describeCache.swap(keep);
    if (fullscreen_ && !ForegroundFullscreen()) SetFullscreen(false);
    for (auto& a : apps_) {
        if (a.isStart || a.isSeparator) continue;
        bool open = !a.windows.empty();
        a.dot.target = open ? 100.f : 0.f;
        a.appear.target = (open || a.pinned) ? 100.f : 0.f;
    }
    StartAnimation();
}

void Dock::OnClick(int i) {
    if (i < 0 || i >= static_cast<int>(apps_.size())) return;
    App& a = apps_[i];
    if (a.isSeparator) return;
    if (a.isStart) {
        OpenStart();
        return;
    }
    if (a.windows.empty()) {
        if (a.launch.empty()) return;
        // Açılıyor: ikon pencere gelene kadar zıplar.
        a.launchAt = Now();
        StartAnimation();
        HINSTANCE r = ShellExecuteW(nullptr, L"open", a.launch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) a.launchAt = -1;
        return;
    }
    Activate(a);
}

void Dock::Activate(App& a) {
    HWND target = a.windows.front();
    if (a.active) {
        if (a.windows.size() == 1) {
            // Zaten öndeyse küçült (Windows taskbar'ı gibi).
            ShowWindowAsync(target, SW_MINIMIZE);
            return;
        }
        target = a.windows.back();  // birden çok pencere: sırayla dolaş
    }
    if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
    // Tıklama bize geldiği için öne getirme izni bizde; yine de olmazsa Alt hilesi.
    if (!SetForegroundWindow(target)) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(target);
    }
}

void Dock::OpenStart() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_LWIN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Yerleşim: macOS benzeri büyütme. Fare altındaki nokta sabit kalır, ikonlar iki yana açılır.
// Koordinatlar: x = 0 ekranın ortası, y = 0 gövdenin üstü (DIP).
// ---------------------------------------------------------------------------

void Dock::Layout(float* bodyL, float* bodyR) {
    size_t n = apps_.size();
    std::vector<float> baseW(n), w(n);
    float baseTotal = 0, total = 0;
    for (size_t i = 0; i < n; ++i) {
        const App& a = apps_[i];
        float appear = std::clamp(a.appear.value / 100.f, 0.f, 1.f);
        baseW[i] = a.isSeparator ? kSepW : kSlot * appear;
        w[i] = a.isSeparator ? kSepW : (kSlot + a.grow.value * (kSlot / kIcon)) * appear;
        baseTotal += baseW[i];
        total += w[i];
    }
    baseW_ = baseTotal;
    float baseL = -baseTotal / 2.f;
    // Sabit noktanın taban düzendeki yeri: hangi öğenin hangi kesrinde?
    float anchor = std::clamp(anchorX_, baseL, baseL + baseTotal);
    float acc = baseL, before = 0;  // before: yeni düzende o öğeye kadar olan genişlik
    float left = baseL;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        if (!found && (anchor <= acc + baseW[i] || i + 1 == n)) {
            float t = baseW[i] > 0 ? std::clamp((anchor - acc) / baseW[i], 0.f, 1.f) : 0.f;
            left = anchor - (before + t * w[i]);
            found = true;
        }
        acc += baseW[i];
        before += w[i];
    }
    float x = left;
    for (size_t i = 0; i < n; ++i) {
        App& a = apps_[i];
        a.w = w[i];
        a.x = x + w[i] / 2.f;
        float appear = std::clamp(a.appear.value / 100.f, 0.f, 1.f);
        a.size = a.isSeparator ? 0.f : (kIcon + a.grow.value) * appear;
        x += w[i];
    }
    *bodyL = left - kPadX;
    *bodyR = left + total + kPadX;
}

int Dock::HitTest(float x, float y) const {
    // y: gövde üstüne göre (DIP)
    for (size_t i = 0; i < apps_.size(); ++i) {
        const App& a = apps_[i];
        if (a.isSeparator || a.appear.target <= 0.f) continue;
        if (x < a.x - a.w / 2.f || x >= a.x + a.w / 2.f) continue;
        float top = kIconBottom - a.size - 6.f;
        if (y >= top && y <= kBodyH) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Kare döngüsü
// ---------------------------------------------------------------------------

double Dock::Now() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / qpcFreq_.QuadPart;
}

void Dock::StartAnimation() {
    if (!overlay_) return;
    if (!animating_) QueryPerformanceCounter(&lastTick_);
    animating_ = true;
    if (!wantFrames_.exchange(true) && frameEvent_) SetEvent(frameEvent_);
}

void Dock::FrameLoop() {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
        adapter->EnumOutputs(0, &output);
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (!quit_) {
        WaitForSingleObject(frameEvent_, INFINITE);
        while (wantFrames_.load() && !quit_) {
            bool waited = output && SUCCEEDED(output->WaitForVBlank());
            QueryPerformanceCounter(&now);
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            if (!waited || ms < 4) {
                if (ms < 16) Sleep(static_cast<DWORD>(16 - ms));
                QueryPerformanceCounter(&now);
            }
            last = now;
            HWND o = overlay_;
            if (o && !framePending_.exchange(true)) PostMessageW(o, kFrameMsg, 0, 0);
        }
    }
}

void Dock::OnFrame() {
    if (!overlay_) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - lastTick_.QuadPart) / qpcFreq_.QuadPart;
    lastTick_ = now;
    dt = std::clamp(dt, 0.001f, 0.05f);
    double t = Now();

    // Büyütme hedefleri: fareye yakın ikon büyür, komşuları kosinüs eğrisiyle daha az.
    float baseL = -baseW_ / 2.f, acc = baseL;
    for (auto& a : apps_) {
        float bw = a.isSeparator ? kSepW : kSlot * std::clamp(a.appear.value / 100.f, 0.f, 1.f);
        float c = acc + bw / 2.f;
        acc += bw;
        float g = 0;
        if (mouseIn_ && !a.isSeparator && !fullscreen_) {
            float d = std::fabs(c - mouseX_);
            if (d < kRange) g = kMaxGrow * (std::cos(3.14159265f * d / kRange) + 1.f) / 2.f;
        }
        a.grow.target = g;
        a.press.target = (pressed_ >= 0 && &apps_[pressed_] == &a && hover_ == pressed_) ? 5.f : 0.f;
    }
    labelA_.target = (hover_ >= 0 && pressed_ < 0) ? 100.f : 0.f;

    bool moving = false;
    for (float left = dt; left > 0; left -= 1.f / 240.f) {
        float step = std::min(left, 1.f / 240.f);
        for (auto& a : apps_) {
            a.grow.stiffness = 380.f;
            a.grow.damping = 32.f;
            a.press.stiffness = 700.f;
            a.press.damping = 34.f;
            a.dot.stiffness = 260.f;
            a.dot.damping = 30.f;
            a.appear.stiffness = 260.f;
            a.appear.damping = 30.f;
            moving |= a.grow.Step(step);
            moving |= a.press.Step(step);
            moving |= a.dot.Step(step);
            moving |= a.appear.Step(step);
        }
        moving |= labelA_.Step(step);
    }
    // Açılış zıplaması: pencere gelene kadar (en çok 7 sn) yumuşak yaylar; bir yayı yarıda kesmez.
    for (auto& a : apps_) {
        a.lift = 0;
        if (a.launchAt < 0) continue;
        float e = static_cast<float>(t - a.launchAt);
        int n = static_cast<int>(e / kBouncePeriod);
        float phase = e - n * kBouncePeriod;
        bool done = (!a.windows.empty() || e > 7.f) && n >= 1;
        if (done && phase < kBouncePeriod * 0.12f) {
            a.launchAt = -1;
            continue;
        }
        a.lift = kBounce * std::sin(3.14159265f * phase / kBouncePeriod);
        moving = true;
    }
    // Kapanmış (sabitli olmayan) uygulamalar küçülerek kaybolur.
    for (size_t i = apps_.size(); i-- > 0;) {
        App& a = apps_[i];
        if (!a.pinned && !a.isStart && !a.isSeparator && a.appear.target <= 0.f && a.appear.value <= 0.5f) {
            apps_.erase(apps_.begin() + static_cast<long long>(i));
            if (hover_ >= static_cast<int>(i)) hover_ = -1;
            if (pressed_ >= static_cast<int>(i)) pressed_ = -1;
            if (labelFor_ >= static_cast<int>(i)) labelFor_ = -1;
        }
    }

    float bodyL, bodyR;
    Layout(&bodyL, &bodyR);

    // Cam pencereyi gövdeye oturt.
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    RECT g;
    g.left = static_cast<LONG>(std::lround(cxPx + bodyL * scale_));
    g.right = static_cast<LONG>(std::lround(cxPx + bodyR * scale_));
    g.bottom = monitor_.bottom - static_cast<LONG>(std::lround(kBottom * scale_));
    g.top = g.bottom - static_cast<LONG>(std::lround(kBodyH * scale_));
    if (!EqualRect(&g, &glassRect_)) {
        bool sizeChanged = (g.right - g.left) != (glassRect_.right - glassRect_.left) ||
                           (g.bottom - g.top) != (glassRect_.bottom - glassRect_.top);
        glassRect_ = g;
        SetWindowPos(glass_, nullptr, g.left, g.top, g.right - g.left, g.bottom - g.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        if (sizeChanged) {
            int d = static_cast<int>(std::lround(2 * kRadius * scale_));
            HRGN rgn = CreateRoundRectRgn(0, 0, g.right - g.left + 1, g.bottom - g.top + 1, d, d);
            SetWindowRgn(glass_, rgn, TRUE);  // bölge artık pencereye ait
        }
    }

    Render();

    if (!moving) {
        animating_ = false;
        wantFrames_ = false;
    }
}

// ---------------------------------------------------------------------------
// Çizim
// ---------------------------------------------------------------------------

bool Dock::CreateGpu() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr)))
        return false;
    ComPtr<ID2D1Factory1> f1;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> dev;
    if (FAILED(d2d_.As(&f1)) || FAILED(d3d_.As(&dxgi)) || FAILED(f1->CreateDevice(dxgi.Get(), &dev)) ||
        FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx_))) {
        d3d_.Reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(texW_);
    td.Height = static_cast<UINT>(texH_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &tex)) || FAILED(tex.As(&surface_))) {
        ctx_.Reset();
        d3d_.Reset();
        return false;
    }
    auto bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * scale_, 96.f * scale_);
    if (FAILED(ctx_->CreateBitmapFromDxgiSurface(surface_.Get(), &bp, &target_))) {
        surface_.Reset();
        ctx_.Reset();
        d3d_.Reset();
        return false;
    }
    ctx_->SetTarget(target_.Get());
    ctx_->SetDpi(96.f * scale_, 96.f * scale_);
    return true;
}

void Dock::DiscardDevice() {
    ctx_.Reset();
    target_.Reset();
    surface_.Reset();
    d3d_.Reset();
    dcRt_.Reset();
    brush_.Reset();
    for (auto& a : apps_) {
        a.icon.Reset();
        a.iconTried = false;
    }
    rt_.Reset();
}

bool Dock::EnsureResources() {
    if (rt_) return true;
    if (!gpuFailed_ && CreateGpu()) {
        gpu_ = true;
        rt_ = ctx_;
    } else {
        gpuFailed_ = true;
        gpu_ = false;
        if (!memDC_) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = texW_;
            bi.bmiHeader.biHeight = -texH_;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HDC screen = GetDC(nullptr);
            memDC_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib_) return false;
            oldBmp_ = SelectObject(memDC_, dib_);
        }
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * scale_,
            96.f * scale_);
        if (FAILED(d2d_->CreateDCRenderTarget(&props, &dcRt_))) return false;
        rt_ = dcRt_;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(kWhite, &brush_);
    return brush_ != nullptr;
}

ID2D1Bitmap* Dock::Icon(App& a) {
    if (a.icon || a.iconTried || !wic_) return a.icon.Get();
    a.iconTried = true;
    std::wstring parsing = a.launch;
    if (parsing.empty()) parsing = a.exe;
    if (parsing.empty()) return nullptr;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    int px = static_cast<int>(std::ceil((kIcon + kMaxGrow) * scale_));
    HBITMAP hbmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) || !hbmp)
        return nullptr;
    ComPtr<IWICBitmap> wbmp;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(wic_->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &wbmp)) &&
        SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wbmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &a.icon);
    }
    DeleteObject(hbmp);
    return a.icon.Get();
}

void Dock::DrawStart(float cx, float bottom, float size) {
    // Mini Guti: siyah yuvarlak kare, iki beyaz göz; gözler fareye bakar.
    float r = size * 0.30f;
    D2D1_ROUNDED_RECT face{D2D1::RectF(cx - size / 2, bottom - size, cx + size / 2, bottom), r, r};
    brush_->SetColor(Rgba(0.02f, 0.02f, 0.03f, 1.f));
    rt_->FillRoundedRectangle(face, brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.14f));
    D2D1_ROUNDED_RECT edge{D2D1::RectF(cx - size / 2 + 0.5f, bottom - size + 0.5f, cx + size / 2 - 0.5f, bottom - 0.5f),
                           r - 0.5f, r - 0.5f};
    rt_->DrawRoundedRectangle(edge, brush_.Get(), 1.f);
    float look = mouseIn_ ? std::clamp((mouseX_ - cx) / 260.f, -1.f, 1.f) : 0.f;
    float ex = size * 0.17f, ew = size * 0.15f, eh = size * 0.30f;
    float ey = bottom - size * 0.52f;
    float dx = look * size * 0.07f;
    for (int s = -1; s <= 1; s += 2) {
        float x = cx + s * ex + dx;
        D2D1_ROUNDED_RECT eye{D2D1::RectF(x - ew / 2, ey - eh / 2, x + ew / 2, ey + eh / 2), ew / 2, ew / 2};
        brush_->SetColor(accent_);
        rt_->FillRoundedRectangle(eye, brush_.Get());
    }
}

void Dock::DrawIcon(App& a, float cx, float bottom, float size) {
    ID2D1Bitmap* bmp = Icon(a);
    D2D1_RECT_F rc = D2D1::RectF(cx - size / 2, bottom - size, cx + size / 2, bottom);
    if (bmp) {
        rt_->DrawBitmap(bmp, rc, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    // İkon yoksa: baş harfli yuvarlak kare
    float r = size * 0.24f;
    brush_->SetColor(light_ ? Rgba(0.35f, 0.38f, 0.45f, 1.f) : Rgba(0.30f, 0.32f, 0.38f, 1.f));
    rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{rc, r, r}, brush_.Get());
    if (!a.name.empty() && labelFormat_) {
        wchar_t c[2] = {static_cast<wchar_t>(std::towupper(a.name[0])), 0};
        brush_->SetColor(kWhite);
        D2D1::Matrix3x2F old;
        rt_->GetTransform(&old);
        float k = size / 22.f;
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(cx, bottom - size / 2)) * old);
        rt_->DrawText(c, 1, labelFormat_.Get(), D2D1::RectF(cx - 10, bottom - size / 2 - 10, cx + 10, bottom - size / 2 + 10),
                      brush_.Get());
        rt_->SetTransform(old);
    }
}

void Dock::DrawLabel(const App& a, float cx, float top) {
    if (a.name.empty() || !labelFormat_) return;
    float alpha = std::clamp(labelA_.value / 100.f, 0.f, 1.f);
    if (alpha <= 0.01f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(a.name.c_str(), static_cast<UINT32>(a.name.size()), labelFormat_.Get(), 400.f,
                                         24.f, &layout)))
        return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    float w = std::min(400.f, m.width) + 22.f, h = 26.f;
    float y1 = top - 9.f + (1.f - alpha) * 4.f, y0 = y1 - h;
    D2D1_ROUNDED_RECT rr{D2D1::RectF(cx - w / 2, y0, cx + w / 2, y1), h / 2, h / 2};
    brush_->SetColor(light_ ? Rgba(0.98f, 0.98f, 0.99f, 0.94f * alpha) : Rgba(0.08f, 0.08f, 0.10f, 0.86f * alpha));
    rt_->FillRoundedRectangle(rr, brush_.Get());
    brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.10f * alpha) : Rgba(1, 1, 1, 0.14f * alpha));
    rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.f);
    brush_->SetColor(light_ ? Rgba(0.08f, 0.08f, 0.10f, alpha) : Rgba(1, 1, 1, 0.95f * alpha));
    layout->SetMaxWidth(w);
    layout->SetMaxHeight(h);
    rt_->DrawTextLayout(D2D1::Point2F(cx - w / 2, y0), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Dock::Render() {
    if (fullscreen_ || !EnsureResources()) return;
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    float bodyL = (glassRect_.left - cxPx) / scale_;
    float bodyR = (glassRect_.right - cxPx) / scale_;
    // Pencere: taban genişlik + büyüme/gölge payı; piksele hizalı.
    float half = std::max(baseW_ / 2.f + kPadX + kSideRoom, std::max(-bodyL, bodyR) + 40.f);
    int leftPx = static_cast<int>(std::floor(cxPx - half * scale_));
    int widthPx = std::min(texW_, static_cast<int>(std::ceil(2 * half * scale_)) + 2);
    leftPx = std::clamp(leftPx, static_cast<int>(monitor_.left), static_cast<int>(monitor_.right) - widthPx);
    float originX = (leftPx - cxPx) / scale_;  // pencerenin sol kenarının dock koordinatı

    if (!gpu_) {
        RECT bind{0, 0, widthPx, texH_};
        if (FAILED(dcRt_->BindDC(memDC_, &bind))) return;
    }
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(-originX, kTopRoom));

    float bw = bodyR - bodyL;
    float r = kRadius;
    // Yumuşak gölge: yalnız gövdenin dışında (camı karartmasın).
    for (int i = 10; i >= 1; --i) {
        float d = i * 1.6f;
        float a = (light_ ? 0.030f : 0.045f) * (1.f - i / 11.f);
        brush_->SetColor(Rgba(0, 0, 0, a));
        D2D1_ROUNDED_RECT rr{D2D1::RectF(bodyL - d + 0.5f, -d + 3.f, bodyR + d - 0.5f, kBodyH + d + 3.f), r + d, r + d};
        rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.7f);
    }
    // Gövde: camın üstüne çok hafif bir dolgu (tıklamaları yakalasın) ve üstte parlak bir ışık.
    D2D1_ROUNDED_RECT body{D2D1::RectF(bodyL, 0, bodyR, kBodyH), r, r};
    brush_->SetColor(light_ ? Rgba(1, 1, 1, 0.10f) : Rgba(1, 1, 1, 0.035f));
    rt_->FillRoundedRectangle(body, brush_.Get());
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0.f, Rgba(1, 1, 1, light_ ? 0.30f : 0.10f)}, {1.f, Rgba(1, 1, 1, 0.f)}};
        rt_->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> sheen;
        if (stops &&
            SUCCEEDED(rt_->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kBodyH * 0.55f)),
                stops.Get(), &sheen))) {
            rt_->FillRoundedRectangle(body, sheen.Get());
        }
    }
    // Kenar ışığı: üstte belirgin, altta silik. Bölgenin pürüzlü kenarını da örter.
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0.f, light_ ? Rgba(1, 1, 1, 0.85f) : Rgba(1, 1, 1, 0.24f)},
                                    {1.f, light_ ? Rgba(0, 0, 0, 0.10f) : Rgba(1, 1, 1, 0.07f)}};
        rt_->CreateGradientStopCollection(gs, 2, &stops);
        ComPtr<ID2D1LinearGradientBrush> rim;
        if (stops && SUCCEEDED(rt_->CreateLinearGradientBrush(
                         D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, kBodyH)),
                         stops.Get(), &rim))) {
            D2D1_ROUNDED_RECT edge{D2D1::RectF(bodyL + 0.5f, 0.5f, bodyR - 0.5f, kBodyH - 0.5f), r - 0.5f, r - 0.5f};
            rt_->DrawRoundedRectangle(edge, rim.Get(), 1.2f);
        }
    }
    (void)bw;

    // Öğeler
    for (size_t i = 0; i < apps_.size(); ++i) {
        App& a = apps_[i];
        if (a.isSeparator) {
            brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.14f) : Rgba(1, 1, 1, 0.16f));
            rt_->FillRectangle(D2D1::RectF(a.x - 0.5f, 15.f, a.x + 0.5f, kBodyH - 15.f), brush_.Get());
            continue;
        }
        if (a.size < 1.f) continue;
        float press = a.press.value;
        float size = a.size * (1.f - press / 60.f);
        float bottom = kIconBottom - a.lift - press * 0.25f;
        // Zıplamada yere değerken hafif ezilme
        if (a.launchAt >= 0 && a.lift < 3.f) {
            float squash = (3.f - a.lift) / 3.f * 0.06f;
            D2D1::Matrix3x2F old;
            rt_->GetTransform(&old);
            rt_->SetTransform(D2D1::Matrix3x2F::Scale(1.f + squash, 1.f - squash, D2D1::Point2F(a.x, bottom)) * old);
            if (a.isStart) DrawStart(a.x, bottom, size);
            else DrawIcon(a, a.x, bottom, size);
            rt_->SetTransform(old);
        } else {
            if (a.isStart) DrawStart(a.x, bottom, size);
            else DrawIcon(a, a.x, bottom, size);
        }
        // Açık göstergesi: küçük nokta; öndeki uygulamada vurgu renginde ve biraz daha uzun.
        float dot = std::clamp(a.dot.value / 100.f, 0.f, 1.f);
        if (dot > 0.01f) {
            float dw = (a.active ? 10.f : 4.f) * dot, dh = 4.f * dot;
            D2D1_COLOR_F c = a.active ? accent_ : (light_ ? Rgba(0, 0, 0, 0.55f) : Rgba(1, 1, 1, 0.60f));
            if (a.active && light_) c = Rgba(0.10f, 0.10f, 0.12f, 0.9f);
            brush_->SetColor(WithAlpha(c, dot));
            float y = kBodyH - 4.5f;
            rt_->FillRoundedRectangle(
                D2D1_ROUNDED_RECT{D2D1::RectF(a.x - dw / 2, y - dh / 2, a.x + dw / 2, y + dh / 2), dh / 2, dh / 2},
                brush_.Get());
        }
    }
    // İsim balonu
    int li = hover_ >= 0 ? hover_ : labelFor_;
    if (li >= 0 && li < static_cast<int>(apps_.size())) {
        labelFor_ = li;
        const App& a = apps_[li];
        DrawLabel(a, a.x, kIconBottom - a.size - a.lift);
    }

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDevice();
        return;
    }
    POINT dst{leftPx, monitor_.bottom - texH_};
    SIZE size{widthPx, texH_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (gpu_) {
        HDC dc = nullptr;
        if (SUCCEEDED(surface_->GetDC(FALSE, &dc))) {
            UpdateLayeredWindow(overlay_, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
            surface_->ReleaseDC(nullptr);
        }
    } else {
        UpdateLayeredWindow(overlay_, nullptr, &dst, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    }
    overlayLeft_ = leftPx;
}

// ---------------------------------------------------------------------------
// Pencere işlemleri
// ---------------------------------------------------------------------------

void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    Dock* d = g_dock;
    if (!d || !d->overlay_ || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (event == EVENT_OBJECT_SHOW && d->tray_ && hwnd == d->tray_) {
        PostMessageW(d->overlay_, kRehideMsg, 0, 0);  // Windows taskbar'ı kendini gösterdi: yine gizle
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND) {
        // Yalnız taskbar'da görünen pencereler ilgilendirir; araç ipuçları, menüler, alt pencereler değil.
        bool known = g_describeCache.count(hwnd) != 0;
        if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) {
            if (!known) return;
        } else if (!known) {
            if (GetAncestor(hwnd, GA_ROOT) != hwnd || !d->Eligible(hwnd)) return;
        }
    }
    // Olaylar art arda gelir: kısa bir süre bekleyip bir kez tara.
    SetTimer(d->overlay_, kTimerRescan, event == EVENT_SYSTEM_FOREGROUND ? 30 : 120, nullptr);
}

LRESULT CALLBACK Dock::GlassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* d = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case kAppBarMsg:
        if (!d || !d->overlay_) return 0;
        if (wp == ABN_POSCHANGED) {
            d->RegisterAppBar();
        } else if (wp == ABN_FULLSCREENAPP) {
            d->SetFullscreen(lp != 0 && ForegroundFullscreen());
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Dock::OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* d = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d || !d->overlay_) return DefWindowProcW(hwnd, msg, wp, lp);
    return d->OnOverlay(msg, wp, lp);
}

LRESULT Dock::OnOverlay(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        // Explorer yeniden başladı: taskbar yine geldi ve dock'un yer kaydı silindi.
        appBar_ = false;
        RegisterAppBar();
        HideTaskbar();
        Rescan();
        return 0;
    }
    auto toDock = [&](LPARAM l, float* x, float* y) {
        float cxPx = (monitor_.left + monitor_.right) / 2.f;
        *x = (overlayLeft_ + GET_X_LPARAM(l) - cxPx) / scale_;
        *y = GET_Y_LPARAM(l) / scale_ - kTopRoom;
    };
    switch (msg) {
    case kFrameMsg:
        framePending_ = false;
        OnFrame();
        return 0;
    case kRehideMsg:
        if (tray_ && IsWindowVisible(tray_)) ShowWindow(tray_, SW_HIDE);
        return 0;
    case WM_TIMER:
        if (wp == kTimerRescan) {
            KillTimer(overlay_, kTimerRescan);
            Rescan();
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        float x, y;
        toDock(lp, &x, &y);
        if (!mouseIn_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, overlay_, 0};
            TrackMouseEvent(&tme);
            mouseIn_ = true;
        }
        // Büyütme yalnız gövdenin üstündeyken başlar; büyümüş ikonların üstünde de sürer.
        mouseX_ = x;
        anchorX_ = x;
        hover_ = HitTest(x, y);
        StartAnimation();
        return 0;
    }
    case WM_MOUSELEAVE:
        mouseIn_ = false;
        hover_ = -1;
        if (pressed_ < 0) StartAnimation();
        return 0;
    case WM_LBUTTONDOWN: {
        float x, y;
        toDock(lp, &x, &y);
        pressed_ = HitTest(x, y);
        if (pressed_ >= 0) SetCapture(overlay_);
        StartAnimation();
        return 0;
    }
    case WM_LBUTTONUP: {
        float x, y;
        toDock(lp, &x, &y);
        int hit = HitTest(x, y);
        int p = pressed_;
        pressed_ = -1;
        if (GetCapture() == overlay_) ReleaseCapture();
        if (p >= 0 && hit == p) OnClick(p);
        StartAnimation();
        return 0;
    }
    case WM_RBUTTONUP:
        // Windows taskbar'ı gizli: tepsi ikonu da yok. Guti'nin menüsü buradan açılır.
        if (onMenu) onMenu();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        UpdateMonitor();
        glassRect_ = RECT{};
        RegisterAppBar();
        StartAnimation();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) {
            ApplyGlass();
            StartAnimation();
        }
        return 0;
    }
    return DefWindowProcW(overlay_, msg, wp, lp);
}
