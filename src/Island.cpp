#include "Island.h"
#include "IslandUi.h"
#include "Sounds.h"
#include "Sys.h"
#include "Updater.h"
#include "Net.h"
#include "Notify.h"

#include <objbase.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <thread>
#include <shlguid.h>
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;
using namespace isl;

namespace {

constexpr UINT_PTR kTimerTick = 2;
constexpr UINT_PTR kTimerHover = 3;
constexpr UINT_PTR kTimerCollapse = 4;
constexpr UINT_PTR kTimerAlert = 5;
constexpr UINT_PTR kTimerLook = 6;
constexpr UINT_PTR kTimerUpdateCheck = 7;
constexpr UINT_PTR kTimerTint = 8;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyCommand = 2;
constexpr int kHotkeyShot = 3;
constexpr int kHotkeyPicker = 4;
constexpr int kHotkeyOcr = 5;
constexpr int kHotkeyNote = 6;
constexpr int kHotkeyInk = 7;

constexpr UINT kTrayMsg = WM_APP + 2;
constexpr UINT kFrameMsg = WM_APP + 8;  // kare saati: bir sonraki kareyi çiz
constexpr UINT kTrayId = 1;
constexpr UINT kCmdAutostart = 100;
constexpr UINT kCmdExit = 101;
constexpr UINT kCmdToggle = 102;
constexpr UINT kCmdUpdate = 103;
constexpr UINT kCmdSounds = 104;
constexpr UINT kCmdFocus = 105;
constexpr UINT kCmdCancelTimer = 106;
constexpr UINT kCmdCommand = 107;
constexpr UINT kCmdShot = 108;
constexpr UINT kCmdClearShelf = 109;
constexpr UINT kCmdPicker = 110;
constexpr UINT kCmdVolumeOsd = 111;
constexpr UINT kCmdOcr = 112;
constexpr UINT kCmdNote = 113;
constexpr UINT kCmdInk = 114;
constexpr UINT kCmdCoffee = 115;
constexpr UINT kCmdAlbumTint = 116;
constexpr UINT kCmdLyrics = 117;
constexpr UINT kCmdLyricsPill = 118;
constexpr UINT kCmdRename = 119;
constexpr UINT kCmdSleep = 120;
constexpr UINT kCmdHome = 121;
constexpr UINT kCmdAlbumGlow = 122;
constexpr UINT kCmdAccessories = 123;
constexpr UINT kCmdWeather = 124;
constexpr UINT kCmdCity = 125;
constexpr UINT kCmdLikes = 126;
constexpr UINT kCmdNotify = 127;
constexpr UINT kCmdEyeWhite = 130;   // 130-136 hazır renkler
constexpr UINT kCmdEyeAccent = 140;  // Windows vurgu rengi
constexpr UINT kCmdEyePick = 141;    // ekrandan seç
constexpr UINT kCmdDock = 150;       // Guti Dock (yeni taskbar)

struct EyePreset {
    const wchar_t* name;
    COLORREF rgb;
};
const EyePreset kEyePresets[] = {
    {L"Beyaz", RGB(255, 255, 255)}, {L"Pembe", RGB(255, 140, 190)}, {L"Mavi", RGB(115, 190, 255)},
    {L"Yeşil", RGB(115, 240, 150)}, {L"Sarı", RGB(255, 225, 90)},   {L"Mor", RGB(190, 150, 255)},
    {L"Turuncu", RGB(255, 165, 90)},
};
std::wstring FormatClock(const SYSTEMTIME& st) {
    wchar_t buf[64];
    if (GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, buf, 64))
        return buf;
    swprintf_s(buf, L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

std::wstring FormatDate(const SYSTEMTIME& st) {
    wchar_t buf[128];
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd, d MMMM", buf, 128, nullptr))
        return buf;
    return L"";
}

std::wstring FormatShortDate(const SYSTEMTIME& st) {
    wchar_t buf[64];
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", buf, 64, nullptr))
        return buf;
    return L"";
}

ULONGLONG WallSeconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000000ull;
}

std::wstring FormatDuration(double sec) {
    int s = static_cast<int>(sec + 0.5);
    wchar_t buf[32];
    if (s >= 3600)
        swprintf_s(buf, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        swprintf_s(buf, L"%d:%02d", s / 60, s % 60);
    return buf;
}

// Kapak resminin ortalama rengini alıp canlandırır: ekolayzer ve ilerleme çubuğu bu renkte.
D2D1_COLOR_F AccentFromArt(IWICBitmapSource* src) {
    D2D1_COLOR_F fallback = Rgba(0.62f, 0.80f, 1.f);
    if (!src) return fallback;
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || !w || !h) return fallback;
    std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
    if (FAILED(src->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data())))
        return fallback;
    double r = 0, g = 0, b = 0, n = 0;
    for (size_t i = 0; i < px.size(); i += 16) {  // her 4 pikselden biri yeter
        double bb = px[i], gg = px[i + 1], rr = px[i + 2];
        double mx = std::max({rr, gg, bb}), mn = std::min({rr, gg, bb});
        double weight = 0.15 + (mx - mn) / 255.0;  // renkli pikseller daha ağır
        r += rr * weight; g += gg * weight; b += bb * weight; n += weight;
    }
    if (n <= 0) return fallback;
    r /= n; g /= n; b /= n;
    double mx = std::max({r, g, b, 1.0});
    // En parlak kanal 1 olacak şekilde ölçekle, biraz beyazla karıştır.
    auto f = [&](double c) { return static_cast<float>(0.25 + 0.75 * (c / mx)); };
    return Rgba(f(r), f(g), f(b));
}

}  // namespace

// ---------------------------------------------------------------------------
// Dosya rafı: adaya bırakılan dosyaları alan küçük OLE hedefi
// ---------------------------------------------------------------------------

class DropTarget : public IDropTarget {
public:
    explicit DropTarget(Island* island) : island_(island) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        hasFiles_ = data && data->QueryGetData(&fe) == S_OK;
        // Tarayıcıdan sürüklenen bağlantı: favorilere eklenir.
        FORMATETC fu{UrlFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        hasUrl_ = !hasFiles_ && data && fu.cfFormat && data->QueryGetData(&fu) == S_OK;
        *effect = (hasFiles_ || hasUrl_) ? DROPEFFECT_COPY : DROPEFFECT_NONE;  // kopya: kaynak dosya silinmez
        if (hasFiles_ || hasUrl_) {
            island_->dropUrl_ = hasUrl_;
            island_->OnDropEnter();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = (hasFiles_ || hasUrl_) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (hasFiles_ || hasUrl_) island_->OnDropLeave();
        hasFiles_ = hasUrl_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
        if (hasUrl_) {
            hasUrl_ = false;
            std::wstring url;
            FORMATETC fu{UrlFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM sm{};
            if (SUCCEEDED(data->GetData(&fu, &sm))) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(sm.hGlobal))) {
                    url.assign(p, wcsnlen(p, 2048));
                    GlobalUnlock(sm.hGlobal);
                }
                ReleaseStgMedium(&sm);
                *effect = DROPEFFECT_COPY;
            }
            if (url.empty()) island_->OnDropLeave();
            else island_->OnDropUrl(url);
            return S_OK;
        }
        std::vector<std::wstring> paths;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (hasFiles_ && SUCCEEDED(data->GetData(&fe, &sm))) {
            auto drop = static_cast<HDROP>(sm.hGlobal);
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, path, ARRAYSIZE(path))) paths.push_back(path);
            }
            ReleaseStgMedium(&sm);
            *effect = DROPEFFECT_COPY;
        }
        hasFiles_ = false;
        if (paths.empty()) island_->OnDropLeave();
        else island_->OnDropFiles(paths);
        return S_OK;
    }

private:
    static CLIPFORMAT UrlFormat() {
        static const UINT f = RegisterClipboardFormatW(L"UniformResourceLocatorW");
        return static_cast<CLIPFORMAT>(f);
    }
    Island* island_;
    ULONG refs_ = 1;
    bool hasFiles_ = false, hasUrl_ = false;
};

// ---------------------------------------------------------------------------
// Kurulum
// ---------------------------------------------------------------------------

bool Island::Create(HINSTANCE instance) {
    instance_ = instance;
    QueryPerformanceFrequency(&qpcFreq_);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf())))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DynamisIslandWindow";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"Dynamis Island", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance,
        this);
    if (!hwnd_) return false;

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    UpdateMonitor();
    UpdateDpi(GetDpiForWindow(hwnd_));

    width_.Snap(kIdle.w);
    // Biraz daha sönümlü yaylar: hedefe tek, yumuşak bir esnemeyle oturur.
    width_.stiffness = 240.f;
    width_.damping = 25.f;
    height_.stiffness = 300.f;
    height_.damping = 27.f;
    radius_.stiffness = 300.f;
    radius_.damping = 30.f;
    for (int i = 0; i < 3; ++i) {
        btnScale_[i].Snap(1.f);
        btnScale_[i].stiffness = 520.f;
        btnScale_[i].damping = 20.f;
        btnGlow_[i].stiffness = 260.f;
        btnGlow_[i].damping = 32.f;
    }
    selY_.stiffness = 420.f;
    selY_.damping = 38.f;
    volBar_.stiffness = 260.f;
    volBar_.damping = 30.f;
    ghost_.Snap(1.f);
    ghost_.stiffness = 220.f;
    ghost_.damping = 30.f;
    pageX_.stiffness = 260.f;
    pageX_.damping = 28.f;
    for (auto& g : toolGlow_) {
        g.stiffness = 300.f;
        g.damping = 30.f;
    }
    for (auto& g : favGlow_) {
        g.stiffness = 300.f;
        g.damping = 30.f;
    }
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread([this] { FrameLoop(); }).detach();
    shake_.stiffness = 700.f;
    shake_.damping = 9.f;
    lastActivity_ = Now();
    height_.Snap(kIdle.h);
    radius_.Snap(kIdle.r);

    media_.Start(hwnd_);
    OnPowerChanged(true);
    health_.Sample();
    privacy_.Start();
    DownloadWatch::Start(hwnd_);
    AddClipboardFormatListener(hwnd_);  // ekran görüntüsü alınınca göz kırpsın
    Sounds::SetEnabled(ReadSetting(L"Sounds", 1) != 0);
    companion_.Load();
    eyes_.SetAffection(companion_.Affection());
    if (companion_.DaysAway() >= 2) {
        eyes_.SetMood(Eyes::Mood::Sulk, Now(), 3.0);  // "neredeydin?"
        reunionAt_ = Now() + 3.2;
    } else {
        eyes_.SetMood(Eyes::Mood::Happy, Now(), 1.2);
    }
    AddTrayIcon();
    {
        bool dockOn = ReadSetting(L"Dock", 0) != 0;
        Dock::RepairOnStartup(dockOn);  // önceki oturum taskbar'ı gizli bıraktıysa düzelt
        // Windows taskbar'ı (ve tepsi ikonu) gizliyken menüye dock'taki Guti'ye sağ tıklayarak ulaşılır.
        dock_.onMenu = [this] { ShowTrayMenu(); };
        if (dockOn && dock_.Enable(instance_, d2d_.Get(), dwrite_.Get())) ApplyEyeColor();
    }

    SetTimer(hwnd_, kTimerTick, 1000, nullptr);
    SetTimer(hwnd_, kTimerLook, 100, nullptr);
    SetTimer(hwnd_, kTimerUpdateCheck, 20 * 1000, nullptr);  // açılıştan 20 sn sonra, sonra 6 saatte bir
    RegisterHotKey(hwnd_, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
    RegisterHotKey(hwnd_, kHotkeyCommand, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    RegisterHotKey(hwnd_, kHotkeyShot, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    RegisterHotKey(hwnd_, kHotkeyPicker, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');
    // Ctrl+Alt = AltGr: Türkçe klavyede AltGr+T (₺) gibi işaretleri kapatmayan harfler seçildi.
    RegisterHotKey(hwnd_, kHotkeyOcr, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');
    RegisterHotKey(hwnd_, kHotkeyNote, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'N');
    RegisterHotKey(hwnd_, kHotkeyInk, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
    startedAt_ = Now();
    volumeOsd_ = ReadSetting(L"VolumeOsd", 1) != 0;
    VolumeWatch::Start(hwnd_);
    BtBattery::Start(hwnd_);
    dropTarget_ = new DropTarget(this);
    RegisterDragDrop(hwnd_, dropTarget_);
    {
        // Önceki oturumun geçici ekran görüntülerini temizle.
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) {
            std::wstring dir = std::wstring(tmp) + L"Guti\\";
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((dir + L"*.png").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do DeleteFileW((dir + fd.cFileName).c_str());
                while (FindNextFileW(f, &fd));
                FindClose(f);
            }
        }
    }
    LoadReminders();
    LoadNote();
    LoadLook();
    LoadFavorites();
    LoadDiary();
    heartPop_.Snap(1.f);
    heartPop_.stiffness = 420.f;
    heartPop_.damping = 13.f;
    {
        wchar_t city[64]{};
        DWORD size = sizeof(city);
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"City", RRF_RT_REG_SZ, nullptr, city, &size) == ERROR_SUCCESS)
            city_ = city;
        homeOn_ = ReadSetting(L"Home", 1) != 0;
        weatherOn_ = ReadSetting(L"Weather", 1) != 0;
        albumGlow_ = ReadSetting(L"AlbumGlow", 1) != 0;
        accessories_ = ReadSetting(L"Accessories", 1) != 0;
        notifyOn_ = ReadSetting(L"NotifyMirror", 1) != 0;
        NotifyMirror::SetEnabled(notifyOn_);
        NotifyMirror::Start(hwnd_);
        SetTimer(hwnd_, kTimerWeather, 5000, nullptr);  // açılıştan biraz sonra hava durumu
    }
    userHidden_ = ReadSetting(L"Hidden", 0) != 0;
    Update();
    Render();
    ApplyVisibility();
    return true;
}

int Island::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void Island::UpdateMonitor() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    monitor_ = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) UpdateDpi(dx);
}

void Island::UpdateDpi(UINT dpi) {
    if (!dpi) dpi = 96;
    float newScale = dpi / 96.f;
    int needW = static_cast<int>(std::ceil((kMaxW + 2 * kPad) * newScale)) + 2;
    int needH = static_cast<int>(std::ceil((kMaxH + kPadBottom) * newScale)) + 2;
    if (memDC_ && newScale == scale_ && needW == dibW_ && needH == dibH_) return;
    scale_ = newScale;

    if (memDC_) {
        SelectObject(memDC_, oldBitmap_);
        DeleteObject(dib_);
        DeleteDC(memDC_);
    }
    memDC_ = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = needW;
    bi.bmiHeader.biHeight = -needH;  // yukarıdan aşağı
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    oldBitmap_ = SelectObject(memDC_, dib_);
    dibW_ = needW;
    dibH_ = needH;

    DiscardDeviceResources();
}

// ---------------------------------------------------------------------------
// Durum makinesi
// ---------------------------------------------------------------------------

Island::View Island::TargetView() const {
    if (commandMode_) return View::Command;
    if (noteMode_) return View::Note;
    if (ink_) return View::Ink;
    if (dropHover_) return View::Drop;
    if (picker_) return View::Picker;
    if (alertActive_) return View::Alert;
    if (diaryOpen_) return View::Diary;
    bool timer = companion_.TimerActive() || companion_.Pending();
    if (hover_) {
        if (quickOpen_) return View::Quick;
        if (companion_.Pending()) return View::ExpandedIdle;  // teker çevrilirken süreyi göster
        if (homeOn_ && !homeMedia_) return View::Home;
        return snapshot_.hasSession ? View::ExpandedMedia : View::ExpandedIdle;
    }
    if (timer) return View::Timer;
    return snapshot_.hasSession ? View::Media : View::Idle;
}

void Island::SetTargets(View v) {
    Size3 s = kIdle;
    switch (v) {
    case View::Idle: s = kIdle; break;
    case View::Media: s = slim_ ? kMediaSlim : (LyricPill() ? kMediaLyric : kMedia); break;
    case View::ExpandedMedia: s = kExpandedMedia; s.h += LyricsH(); break;
    case View::ExpandedIdle: s = kExpandedIdle; break;
    case View::Alert:
        s = !IsFileCard() ? kAlert : (alertKind_ == AlertKind::Screenshot ? kShot : kDownload);
        if (alertKind_ == AlertKind::Notification) s = {390, 40, 20};
        break;
    case View::Timer: s = kTimer; break;
    case View::Command: s = {kCommandW, CommandHeight(), 24}; break;
    case View::Drop: s = kDrop; break;
    case View::Picker: s = kPicker; break;
    case View::Note: s = kNote; break;
    case View::Ink: s = kInk; break;
    case View::Quick: s = kQuick; break;
    case View::Home: s = kHome; break;
    case View::Diary: s = kDiary; break;
    }
    if (ToolsShown(v)) s.h += kToolsH;
    if (ToolsShown(v) && ShelfVisible()) s.h += kShelfH;
    width_.target = s.w;
    height_.target = s.h;
    radius_.target = s.r;
}

void Island::Update() {
    if (!hover_) quickOpen_ = false;  // ada kapanınca bir dahaki açılış araçlarla başlar
    View v = TargetView();
    if (v != view_) {
        // Eski içerik sönerken yenisi yerine otursun.
        prevView_ = view_;
        prevW_ = viewW_;
        prevH_ = viewH_;
        trans_ = 0.f;
        view_ = v;
        if (v == View::Command) selY_.Snap(kCommandBar + 4.f + clipSel_ * kCommandRow);
    }
    SetTargets(v);
    viewW_ = width_.target;
    viewH_ = height_.target;
    // Küçük hâlde tıklamalar alttaki pencereye (ör. tarayıcı sekmeleri) geçer.
    // İndirme kartı ise tıklanır ve sürüklenir.
    bool interactive = v == View::ExpandedMedia || v == View::ExpandedIdle || IsFileCard() ||
                       v == View::Command || v == View::Drop || v == View::Note || v == View::Ink ||
                       v == View::Quick || v == View::Home || v == View::Diary || dropArmed_;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    LONG_PTR want = interactive ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
                                : (ex | WS_EX_TRANSPARENT);
    // Komut çubuğunda klavye odağı alabilmek için "etkinleşmez" bayrağı kalkar.
    want = Typing() ? (want & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE)) : (want | WS_EX_NOACTIVATE);
    if (want != ex) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, want);
    StartAnimation();
}

bool Island::NeedsContinuousFrames() const {
    bool mediaVisible = view_ == View::Media || view_ == View::ExpandedMedia;
    // Açık adada Guti hafifçe nefes alır (30 kare/sn yeter).
    return (mediaVisible && snapshot_.playing) || view_ == View::ExpandedIdle || view_ == View::Home;
}

void Island::StartAnimation() {
    if (hidden_) return;
    if (!animating_) QueryPerformanceCounter(&lastTick_);
    animating_ = true;
    frameDivider_ = 1;
    if (!wantFrames_.exchange(true)) SetEvent(frameEvent_);
}

void Island::FrameLoop() {
    // Ekranın dikey tazelemesini bekleyip kare ister: sabit sayaçtan doğan mikro takılmalar olmaz.
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
        adapter->EnumOutputs(0, &output);
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    for (;;) {
        WaitForSingleObject(frameEvent_, INFINITE);
        int n = 0;
        while (wantFrames_.load()) {
            bool waited = output && SUCCEEDED(output->WaitForVBlank());
            QueryPerformanceCounter(&now);
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            if (!waited || ms < 4) {
                // Dikey tazeleme beklenemedi: ~60 Hz'e kendimiz uyalım.
                if (ms < 16) Sleep(static_cast<DWORD>(16 - ms));
                QueryPerformanceCounter(&now);
            }
            last = now;
            if (++n % std::max(1, frameDivider_.load()) != 0) continue;
            if (!framePending_.exchange(true)) PostMessageW(hwnd_, kFrameMsg, 0, 0);
        }
    }
}

void Island::OnAnimationTick() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - lastTick_.QuadPart) / qpcFreq_.QuadPart;
    lastTick_ = now;
    dt = std::clamp(dt, 0.001f, 0.05f);
    time_ += dt;

    // Müzik çalarken hoparlörün sesine bakar: çubuklar gerçek sesle oynar, vuruşta Guti zıplar.
    if (NeedsContinuousFrames()) {
        float peak = meter_.Peak(time_);
        if (peak >= 0) {
            if (level_ < 0) level_ = 0;
            level_ = std::max(peak, level_ - dt * 2.2f);
            float k = std::min(1.f, dt * 1.5f);
            avgLevel_ += (peak - avgLevel_) * k;
            if (peak > 0.06f && peak > avgLevel_ * 1.45f && time_ - lastBeat_ > 0.26) {
                lastBeat_ = time_;
                beatPulse_ = 1.f;
                height_.velocity += view_ == View::Media ? 14.f : 9.f;
            }
        } else {
            level_ = -1.f;
        }
    }
    beatPulse_ = std::max(0.f, beatPulse_ - dt * 5.f);
    if (NeedsContinuousFrames() && !lyrics_.empty()) UpdateLyricLine(Now());
    nodPulse_ = std::max(0.f, nodPulse_ - dt * 3.2f);

    bool moving = false;
    // Küçük adımlarla entegre et: yay kararlı kalsın.
    for (float left = dt; left > 0; left -= 1.f / 240.f) {
        float step = std::min(left, 1.f / 240.f);
        moving |= width_.Step(step);
        moving |= height_.Step(step);
        moving |= radius_.Step(step);
        moving |= shake_.Step(step);
        for (int i = 0; i < 3; ++i) {
            moving |= btnScale_[i].Step(step);
            moving |= btnGlow_[i].Step(step);
        }
        moving |= selY_.Step(step);
        moving |= volBar_.Step(step);
        moving |= pageX_.Step(step);
        moving |= ghost_.Step(step);
        for (auto& g : toolGlow_) moving |= g.Step(step);
        for (auto& g : favGlow_) moving |= g.Step(step);
        moving |= heartPop_.Step(step);
    }
    if (nodPulse_ > 0.f) moving = true;
    if (trans_ < 1.f) {
        trans_ = std::min(1.f, trans_ + dt / kTransition);
        moving = true;
    }
    if (Now() < rollUntil_) moving = true;
    {
        double now = Now();
        for (float left = dt; left > 0; left -= 1.f / 240.f)
            moving |= eyes_.Step(now, std::min(left, 1.f / 240.f));
    }
    Render();

    if (!moving && !NeedsContinuousFrames()) {
        animating_ = false;
        wantFrames_ = false;  // boşta: kare saati uyur, işlemci sıfır
    } else {
        frameDivider_ = moving ? 1 : 2;  // sadece müzik/nefes: 30 kare/sn
    }
}

void Island::ShowAlert(AlertKind kind, int value, UINT durationMs) {
    alertKind_ = kind;
    alertValue_ = value;
    alertActive_ = true;
    if (durationMs) SetTimer(hwnd_, kTimerAlert, durationMs, nullptr);
    else KillTimer(hwnd_, kTimerAlert);
    double now = Now();
    lastActivity_ = now;
    (void)now;
    switch (kind) {
    case AlertKind::Charging: MoodWithSound(Eyes::Mood::Love, 4.5); break;
    case AlertKind::Unplugged: MoodWithSound(Eyes::Mood::Sad, 2.5); break;
    case AlertKind::LowBattery: MoodWithSound(Eyes::Mood::Sad, 4.5); break;
    case AlertKind::UpToDate:
    case AlertKind::UpdateAvailable: MoodWithSound(Eyes::Mood::Happy, 4.0); break;
    case AlertKind::UpdateFailed: MoodWithSound(Eyes::Mood::Dizzy, 2.0); break;
    case AlertKind::TimerDone:
        eyes_.SetMood(Eyes::Mood::Surprised, now, 1.2);
        Sounds::Play(Sounds::Sound::TimerDone);
        height_.velocity += 120.f;
        break;
    case AlertKind::FocusDone:
        eyes_.SetMood(Eyes::Mood::Yawn, now, 2.5);  // gerinir
        Sounds::Play(Sounds::Sound::TimerDone);
        height_.velocity += 140.f;
        break;
    case AlertKind::BreakDone:
        eyes_.SetMood(Eyes::Mood::Happy, now, 2.0);
        Sounds::Play(Sounds::Sound::TimerDone);
        height_.velocity += 100.f;
        break;
    case AlertKind::CpuBusy:
        eyes_.SetMood(Eyes::Mood::Sweaty, now, 3.5);  // "of, çok sıcak!"
        lastSweat_ = now;
        break;
    case AlertKind::MemoryFull:
        MoodWithSound(Eyes::Mood::Dizzy, 2.5);  // kafası doldu
        shake_.velocity += 200.f;
        break;
    case AlertKind::Offline: MoodWithSound(Eyes::Mood::Sad, 3.0); break;
    case AlertKind::Notification:
        eyes_.SetMood(Eyes::Mood::Curious, now, 1.4);  // başını bildirime çevirir
        height_.velocity += 70.f;
        break;
    case AlertKind::Online:
        MoodWithSound(Eyes::Mood::Happy, 2.0);
        height_.velocity += 90.f;
        break;
    case AlertKind::Download:
        MoodWithSound(Eyes::Mood::Happy, 2.0);  // "getirdim!"
        height_.velocity += 110.f;
        break;
    case AlertKind::Reminder:
        eyes_.SetMood(Eyes::Mood::Surprised, now, 1.4);  // "hatırladın mı?"
        Sounds::Play(Sounds::Sound::TimerDone);
        height_.velocity += 130.f;
        shake_.velocity += 120.f;
        break;
    case AlertKind::ReminderSet: MoodWithSound(Eyes::Mood::Happy, 1.4); break;
    case AlertKind::Headphones:
        MoodWithSound(Eyes::Mood::Happy, 1.6);  // kulaklık takıldı: müzik zamanı
        height_.velocity += 80.f;
        break;
    case AlertKind::HeadphonesLow: MoodWithSound(Eyes::Mood::Sad, 2.5); break;
    case AlertKind::Copied: MoodWithSound(Eyes::Mood::Happy, 1.4); break;
    case AlertKind::Reading: eyes_.SetMood(Eyes::Mood::Curious, now, 15.0); break;  // bir gözünü kısıp okur
    case AlertKind::OcrFailed: MoodWithSound(Eyes::Mood::Sad, 2.0); break;
    case AlertKind::Screenshot:
        eyes_.SetMood(Eyes::Mood::Wink, now, 1.2);  // "çektim!"
        Sounds::Play(Sounds::Sound::Shutter);
        height_.velocity += 90.f;
        break;
    default: break;
    }
    Update();
}

void Island::OnPowerChanged(bool initial) {
    SYSTEM_POWER_STATUS ps{};
    if (!GetSystemPowerStatus(&ps)) return;
    bool hasBattery = !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255;
    int ac = ps.ACLineStatus == 1 ? 1 : 0;
    if (!initial && hasBattery && lastAc_ != -1 && ac != lastAc_)
        ShowAlert(ac ? AlertKind::Charging : AlertKind::Unplugged);
    lastAc_ = ac;

    int pct = ps.BatteryLifePercent;
    if (hasBattery && pct <= 100) {
        if (!ac && pct <= 20 && !lowBatteryShown_) {
            lowBatteryShown_ = true;
            if (!initial) ShowAlert(AlertKind::LowBattery);
        } else if (ac || pct > 25) {
            lowBatteryShown_ = false;
        }
    }
}

void Island::CheckFullscreen() {
    // Oyun / video / sunum tam ekrandayken ada çekilir.
    QUERY_USER_NOTIFICATION_STATE state{};
    bool fullscreen = false;
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        fullscreen = state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
                     state == QUNS_PRESENTATION_MODE;
    }
    if (fullscreen != fullscreen_) {
        fullscreen_ = fullscreen;
        ApplyVisibility();
    }
}

void Island::ApplyVisibility() {
    bool hide = userHidden_ || fullscreen_;
    if (hide && !hidden_) {
        hidden_ = true;
        ShowWindow(hwnd_, SW_HIDE);
    } else if (!hide && (hidden_ || !IsWindowVisible(hwnd_))) {
        hidden_ = false;
        Render();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        StartAnimation();
    }
}

void Island::ToggleUserHidden() {
    userHidden_ = !userHidden_;
    WriteSetting(L"Hidden", userHidden_ ? 1 : 0);
    ApplyVisibility();
}

// ---------------------------------------------------------------------------
// Çizim
// ---------------------------------------------------------------------------

namespace {
ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* f, const wchar_t* family, float size,
                                     DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                                     bool ellipsis) {
    ComPtr<IDWriteTextFormat> fmt;
    f->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt);
    if (!fmt) return fmt;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        ComPtr<IDWriteInlineObject> sign;
        f->CreateEllipsisTrimmingSign(fmt.Get(), &sign);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trim, sign.Get());
    }
    return fmt;
}

bool HasFamily(IDWriteFactory* f, const wchar_t* name) {
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(f->GetSystemFontCollection(&fonts))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(name, &index, &exists);
    return exists != FALSE;
}
}  // namespace

bool Island::EnsureResources() {
    if (!fClock_) {
        IDWriteFactory* f = dwrite_.Get();
        const wchar_t* ui = HasFamily(f, L"Segoe UI Variable Display") ? L"Segoe UI Variable Display"
                                                                      : L"Segoe UI";
        const wchar_t* icons =
            HasFamily(f, L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
        const auto aL = DWRITE_TEXT_ALIGNMENT_LEADING, aC = DWRITE_TEXT_ALIGNMENT_CENTER,
                   aT = DWRITE_TEXT_ALIGNMENT_TRAILING;
        fClock_ = MakeFormat(f, ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aC, false);
        fTitle_ = MakeFormat(f, ui, 15.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aL, true);
        fArtist_ = MakeFormat(f, ui, 13.f, DWRITE_FONT_WEIGHT_NORMAL, aL, true);
        fSmall_ = MakeFormat(f, ui, 11.f, DWRITE_FONT_WEIGHT_NORMAL, aL, false);
        fSmallRight_ = MakeFormat(f, ui, 11.f, DWRITE_FONT_WEIGHT_NORMAL, aT, false);
        fBigClock_ = MakeFormat(f, ui, 34.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aL, false);
        fDate_ = MakeFormat(f, ui, 13.f, DWRITE_FONT_WEIGHT_NORMAL, aL, true);
        fIcon_ = MakeFormat(f, icons, 18.f, DWRITE_FONT_WEIGHT_NORMAL, aC, false);
        fIconBig_ = MakeFormat(f, icons, 26.f, DWRITE_FONT_WEIGHT_NORMAL, aC, false);
        fIconSmall_ = MakeFormat(f, icons, 12.f, DWRITE_FONT_WEIGHT_NORMAL, aC, false);
        fTiny_ = MakeFormat(f, ui, 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aL, false);
        fTinyRight_ = MakeFormat(f, ui, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, aT, false);
        fAlert_ = MakeFormat(f, ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aL, true);
        fAlertRight_ = MakeFormat(f, ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aT, false);
        fSmallCenter_ = MakeFormat(f, ui, 10.5f, DWRITE_FONT_WEIGHT_NORMAL, aC, true);
        fAa_ = MakeFormat(f, ui, 17.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aC, false);
        fLyric_ = MakeFormat(f, ui, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aL, true);
        fHomeTimer_ = MakeFormat(f, ui, 24.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, aC, false);
        fNote_ = MakeFormat(f, ui, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, aL, false);
        if (fNote_) {
            fNote_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            fNote_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        if (!fClock_) return false;
    }
    if (!roundStroke_) {
        auto sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                              D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d_->CreateStrokeStyle(&sp, nullptr, 0, &roundStroke_);
    }
    if (!tabular_ && SUCCEEDED(dwrite_->CreateTypography(&tabular_))) {
        // Eşit genişlikte rakamlar: sayılar değişirken yazı titremesin.
        tabular_->AddFontFeature(DWRITE_FONT_FEATURE{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    }
    if (!rt_) {
        if (!gpuFailed_ && CreateGpu()) {
            gpu_ = true;
            rt_ = ctx_;
        } else {
            gpuFailed_ = true;
            gpu_ = false;
            auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.f * scale_, 96.f * scale_);
            if (FAILED(d2d_->CreateDCRenderTarget(&props, &dcRt_))) return false;
            rt_ = dcRt_;
        }
        // Saydam zeminde ClearType renkli saçak yapar; gri tonlama temiz görünür.
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->CreateSolidColorBrush(kWhite, &brush_);
        artBitmap_.Reset();
    }
    if (!artBitmap_ && snapshot_.art) {
        rt_->CreateBitmapFromWicBitmap(snapshot_.art.Get(), nullptr, &artBitmap_);
    }
    return true;
}

bool Island::CreateGpu() {
    // Ekran kartında çiz, sonucu katmanlı pencereye GDI uyumlu bir dokudan aktar.
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
    td.Width = static_cast<UINT>(dibW_);
    td.Height = static_cast<UINT>(dibH_);
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

void Island::DiscardDeviceResources() {
    ctx_.Reset();
    target_.Reset();
    surface_.Reset();
    d3d_.Reset();
    dcRt_.Reset();
    brush_.Reset();
    shotBitmap_.Reset();
    for (auto& it : shelf_) it.icon.Reset();
    for (auto& f : favs_) {
        f.icon.Reset();
        f.iconTried = false;
    }
    artBitmap_.Reset();
    rt_.Reset();
}

void Island::Render() {
    if (!memDC_ || !EnsureResources()) return;

    float w = std::clamp(width_.value, 20.f, kMaxW);
    float h = std::clamp(height_.value, 20.f, kMaxH);
    // Hafif lastik etkisi: boy hızla uzarken en biraz daralır (ve tersi).
    w = std::clamp(w - std::clamp(height_.velocity * 0.006f, -4.f, 4.f), 20.f, kMaxW);
    float r = std::clamp(radius_.value, 0.f, std::min(w, h) / 2.f);
    int pxW = std::min(dibW_, static_cast<int>(std::ceil((w + 2 * kPad) * scale_)));
    int pxH = std::min(dibH_, static_cast<int>(std::ceil((h + kPadBottom) * scale_)));

    if (!gpu_) {
        RECT bind{0, 0, pxW, pxH};
        if (FAILED(dcRt_->BindDC(memDC_, &bind))) return;
    }
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(kPad, 0));
    fades_.clear();

    DrawShadow(w, h, r);
    D2D1_ROUNDED_RECT pill{D2D1::RectF(0, 0, w, h), r, r};
    brush_->SetColor(Rgba(0, 0, 0, 1));
    rt_->FillRoundedRectangle(pill, brush_.Get());
    // İnce kenar: koyu duvar kağıtlarında hap kaybolmasın.
    D2D1_ROUNDED_RECT edge{D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r - 0.5f, r - 0.5f};
    brush_->SetColor(Rgba(1, 1, 1, 0.07f));
    rt_->DrawRoundedRectangle(edge, brush_.Get(), 1.f);
    DrawPillGlow(w, h, r);  // müzik çalarken kapak renginde atan kenar

    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    d2d_->CreateRoundedRectangleGeometry(pill, &clip);
    ComPtr<ID2D1Layer> layer;
    rt_->CreateLayer(&layer);
    if (layer && clip) {
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get()), layer.Get());
    }

    // Geçiş: eski içerik hafifçe küçülüp söner, yenisi biraz büyükten yerine oturur.
    // İçerik son boyutuna göre yerleşir; hap büyürken onu "açığa çıkarır".
    float e = EaseOut(trans_);
    if (trans_ < 1.f && prevView_ != view_) {
        drawingOld_ = true;
        float a = 1.f - std::min(1.f, trans_ * 2.2f);
        if (a > 0.01f) {
            PushFade(a, (w - prevW_) / 2, 0, 1.f - 0.06f * e, prevW_ / 2, prevH_ / 2);
            DrawView(prevView_, prevW_, prevH_);
            PopFade();
        }
        drawingOld_ = false;
    }
    {
        float a = trans_ >= 1.f ? 1.f : EaseOut((trans_ - 0.12f) / 0.88f);
        PushFade(a, (w - viewW_) / 2, -3.f * (1 - e), 1.04f - 0.04f * e, viewW_ / 2, viewH_ / 2);
        DrawView(view_, viewW_, viewH_);
        PopFade();
    }
    if (layer && clip) rt_->PopLayer();

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        return;
    }

    float cx = (monitor_.left + monitor_.right) / 2.f + shake_.value * scale_;
    POINT dst{static_cast<LONG>(std::lround(cx - (w / 2 + kPad) * scale_)),
              monitor_.top + static_cast<LONG>(std::lround(kTopMargin * scale_))};
    SIZE size{pxW, pxH};
    POINT src{0, 0};
    // Fare sekmelerin üstünde gezinirken ada saydamlaşır: altındaki sekme görünür.
    BYTE alpha = static_cast<BYTE>(255.f * std::clamp(ghost_.value, 0.12f, 1.f));
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    if (gpu_) {
        HDC dc = nullptr;
        if (SUCCEEDED(surface_->GetDC(FALSE, &dc))) {
            UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
            surface_->ReleaseDC(nullptr);
        }
    } else {
        UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    }
}

void Island::DrawView(View v, float w, float h) {
    switch (v) {
    case View::Idle: DrawIdle(w, h); break;
    case View::Media: DrawCompactMedia(w, h); break;
    case View::ExpandedMedia: DrawExpandedMedia(w, h); break;
    case View::ExpandedIdle: DrawExpandedIdle(w, h); break;
    case View::Alert: DrawAlert(w, h); break;
    case View::Timer: DrawTimer(w, h); break;
    case View::Command: DrawCommand(w, h); break;
    case View::Drop: DrawDrop(w, h); break;
    case View::Picker: DrawPicker(w, h); break;
    case View::Note: DrawNote(w, h); break;
    case View::Ink: DrawInkBar(w, h); break;
    case View::Quick: DrawQuick(w, h); break;
    case View::Home: DrawHome(w, h); break;
    case View::Diary: DrawDiary(w, h); break;
    }
    if (ToolsShown(v)) DrawTools(v, w);
    if (ToolsShown(v) && ShelfVisible()) DrawShelf(w);
    if (v == View::Idle) {
        // Kahve açıkken solda minik fincan; not varken Guti'nin yanında kalem.
        if (coffee_) DrawCup(shelf_.empty() ? 6.f : 17.f, h / 2, 1.f);
        if (!note_.empty()) {
            float cx = w / 2, cy = h / 2;
            brush_->SetColor(kOrange);
            rt_->DrawLine(D2D1::Point2F(cx + 14.5f, cy + 5.f), D2D1::Point2F(cx + 19.5f, cy - 3.f), brush_.Get(), 1.8f,
                          roundStroke_.Get());
        }
    }
    if (v == View::Idle && !shelf_.empty()) {
        // Rafta dosya var: solda minik kâğıt yığını
        brush_->SetColor(Rgba(1, 1, 1, 0.5f));
        D2D1_ROUNDED_RECT a{D2D1::RectF(7, h / 2 - 4, 13, h / 2 + 3), 1.2f, 1.2f};
        rt_->DrawRoundedRectangle(a, brush_.Get(), 1.f);
        D2D1_ROUNDED_RECT b{D2D1::RectF(9, h / 2 - 2, 15, h / 2 + 5), 1.2f, 1.2f};
        rt_->FillRoundedRectangle(b, brush_.Get());
    }
    // Kamera / mikrofon açıkken küçük renkli nokta (yeşil kamera, turuncu mikrofon).
    switch (v) {
    case View::Idle: DrawPrivacyDots(w - 9.f, h / 2); break;
    case View::Media: DrawPrivacyDots(w - 44.f, h / 2); break;
    case View::Timer: DrawPrivacyDots(w - 6.5f, 6.5f); break;
    case View::ExpandedIdle:
    case View::ExpandedMedia:
    case View::Home: DrawPrivacyDots(w / 2 + 6.f, 9.f); break;
    default: break;
    }
}

void Island::DrawShadow(float w, float h, float r) {
    // Yumuşak gölge: üst üste binen, giderek büyüyen ve silikleşen hap şekilleri.
    // Açık duvar kâğıtlarında adayı zeminden ayırır; koyu zeminde neredeyse görünmez.
    float strength = std::clamp((h - 18.f) / 90.f, 0.35f, 1.f);
    constexpr int kSteps = 9;
    for (int i = kSteps; i >= 1; --i) {
        float d = i * 1.7f;
        float a = 0.028f * strength * (1.f - static_cast<float>(i) / (kSteps + 1));
        brush_->SetColor(Rgba(0, 0, 0, a));
        D2D1_ROUNDED_RECT rr{D2D1::RectF(-d, -d + 4.f, w + d, h + d + 4.f), r + d, r + d};
        rt_->FillRoundedRectangle(rr, brush_.Get());
    }
}

float Island::Stagger(int i) const {
    // Öğeler sırayla gelir: her biri bir öncekinden 45 ms sonra.
    if (drawingOld_ || trans_ >= 1.f) return 1.f;
    float t = trans_ * kTransition - 0.04f - i * 0.045f;
    return EaseOut(t / 0.22f);
}

void Island::PushFade(float alpha, float dx, float dy, float scale, float cx, float cy) {
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)) *
                          D2D1::Matrix3x2F::Translation(dx, dy) * *D2D1::Matrix3x2F::ReinterpretBaseType(&old);
    rt_->SetTransform(m);
    bool useLayer = alpha < 0.999f;
    if (useLayer) {
        ComPtr<ID2D1Layer> layer;
        rt_->CreateLayer(&layer);
        if (layer) {
            rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), std::max(0.f, alpha)),
                           layer.Get());
        } else {
            useLayer = false;
        }
    }
    fades_.push_back({old, useLayer});
}

void Island::PopFade() {
    if (fades_.empty()) return;
    Fade f = fades_.back();
    fades_.pop_back();
    if (f.layer) rt_->PopLayer();
    rt_->SetTransform(f.old);
}

void Island::DrawRolling(Roll& roll, const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                         D2D1_COLOR_F color) {
    if (!format) return;
    double now = Now();
    if (!drawingOld_ && text != roll.cur) {
        if (!roll.cur.empty()) {
            roll.prev = roll.cur;
            roll.at = now;
            rollUntil_ = std::max(rollUntil_, now + 0.32);
            StartAnimation();
        }
        roll.cur = text;
    }
    const std::wstring& cur = drawingOld_ ? roll.cur : text;
    float rw = rect.right - rect.left, rh = rect.bottom - rect.top;
    ComPtr<IDWriteTextLayout> full;
    dwrite_->CreateTextLayout(cur.c_str(), static_cast<UINT32>(cur.size()), format, rw, rh, &full);
    if (!full) return;
    DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(cur.size())};
    if (tabular_) full->SetTypography(tabular_.Get(), all);
    float t = static_cast<float>((now - roll.at) / 0.32);
    bool rolling = t < 1.f && roll.prev.size() == cur.size() && roll.prev != cur;
    brush_->SetColor(color);
    if (!rolling) {
        rt_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), full.Get(), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        return;
    }
    // Kilometre sayacı: değişen her rakam yukarı kayıp gider, yenisi aşağıdan gelir.
    float e = EaseOut(t);
    rt_->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
    for (UINT32 i = 0; i < cur.size(); ++i) {
        FLOAT px = 0, py = 0;
        DWRITE_HIT_TEST_METRICS hm{};
        full->HitTestTextPosition(i, FALSE, &px, &py, &hm);
        auto drawChar = [&](wchar_t c, float dy, float alpha) {
            ComPtr<IDWriteTextLayout> one;
            dwrite_->CreateTextLayout(&c, 1, format, 200.f, hm.height, &one);
            if (!one) return;
            one->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            one->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            if (tabular_) one->SetTypography(tabular_.Get(), DWRITE_TEXT_RANGE{0, 1});
            brush_->SetColor(WithAlpha(color, alpha));
            rt_->DrawTextLayout(D2D1::Point2F(rect.left + hm.left, rect.top + hm.top + dy), one.Get(), brush_.Get());
        };
        if (cur[i] == roll.prev[i]) {
            drawChar(cur[i], 0, 1);
        } else {
            float travel = hm.height * 0.75f;
            drawChar(roll.prev[i], -travel * e, 1 - e);
            drawChar(cur[i], travel * (1 - e), e);
        }
    }
    rt_->PopAxisAlignedClip();
}

void Island::Text(const wchar_t* text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                      D2D1_COLOR_F color) {
    if (!format || !text) return;
    brush_->SetColor(color);
    rt_->DrawText(text, static_cast<UINT32>(wcslen(text)), format, rect, brush_.Get(),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Island::DrawArt(D2D1_RECT_F rect, float radius) {
    D2D1_ROUNDED_RECT rr{rect, radius, radius};
    if (artBitmap_) {
        D2D1_SIZE_F bs = artBitmap_->GetSize();
        ComPtr<ID2D1BitmapBrush> bb;
        rt_->CreateBitmapBrush(artBitmap_.Get(), &bb);
        if (bb && bs.width > 0 && bs.height > 0) {
            // Kare kırp: kısa kenara göre ölçekle, ortala.
            float rw = rect.right - rect.left, rh = rect.bottom - rect.top;
            float s = std::max(rw / bs.width, rh / bs.height);
            float ox = rect.left + (rw - bs.width * s) / 2, oy = rect.top + (rh - bs.height * s) / 2;
            bb->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(ox, oy));
            bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->FillRoundedRectangle(rr, bb.Get());
            return;
        }
    }
    brush_->SetColor(Rgba(1, 1, 1, 0.12f));
    rt_->FillRoundedRectangle(rr, brush_.Get());
    float side = rect.bottom - rect.top;
    Text(kGlyphMusic, side > 40 ? fIconBig_.Get() : fIconSmall_.Get(), rect, kGray);
}

void Island::DrawBars(float right, float centerY, float maxHeight, float barWidth) {
    constexpr int kBars = 4;
    float gap = barWidth * 0.8f;
    float x = right - kBars * barWidth - (kBars - 1) * gap;
    brush_->SetColor(accent_);
    for (int i = 0; i < kBars; ++i) {
        float t = 0.18f;
        if (snapshot_.playing) {
            double v = std::sin(time_ * (5.0 + i * 1.7) + i * 1.3) *
                       std::sin(time_ * (2.3 + i * 0.9) + i);
            t = 0.3f + 0.7f * static_cast<float>(std::fabs(v));
            if (level_ >= 0) {
                // Gerçek ses seviyesi; her çubuğa biraz farklı salınım katılır.
                float lv = std::min(1.f, level_ * 1.5f);
                t = std::clamp(lv * (0.55f + 0.45f * static_cast<float>(std::fabs(v))), 0.15f, 1.f);
            }
        }
        float bh = std::max(barWidth, maxHeight * t);
        D2D1_ROUNDED_RECT bar{D2D1::RectF(x, centerY - bh / 2, x + barWidth, centerY + bh / 2),
                              barWidth / 2, barWidth / 2};
        rt_->FillRoundedRectangle(bar, brush_.Get());
        x += barWidth + gap;
    }
}

void Island::DrawIdle(float w, float h) {
    // Boştayken sadece küçük iki göz; üzerine gelince ada açılır.
    DrawGuti(w / 2, h / 2, 0.72f, 0.f);
}

void Island::DrawCompactMedia(float w, float h) {
    float pad = 6.f, side = h - pad * 2;
    DrawArt(D2D1::RectF(pad, pad, pad + side, pad + side), 7.f);
    DrawBars(w - 14.f, h / 2, 14.f, 3.f);
    if (sleepEnd_ > 0)  // uyku zamanlayıcısı kurulu: minik ay
        Text(kGlyphMoon, fIconSmall_.Get(), D2D1::RectF(w - 50, 0, w - 34, h), kGray);
    // Müzik çalarken gözler ritimle hafifçe sallanır; yeni söz satırında başını sallar.
    float bob = 0.f;
    if (snapshot_.playing) {
        bob = level_ >= 0 ? -beatPulse_ * 2.4f  // vuruşta hop
                          : static_cast<float>(std::sin(time_ * 7.0)) * 1.6f;
    }
    bob += 1.8f * std::sin(nodPulse_ * 3.14159f);
    if (w >= kMediaLyric.w - 1.f && HasLyrics()) {
        // Söz hapı: kapak, Guti, şu anki satır, ekolayzer
        float ex = pad + side + 22.f;
        DrawGuti(ex, h / 2, 0.62f, bob);
        D2D1_RECT_F area = D2D1::RectF(ex + 22.f, 0, w - (sleepEnd_ > 0 ? 54.f : 38.f), h);
        if (lyricLine_ < 0) Text(snapshot_.title.c_str(), fLyric_.Get(), area, kGray);
        else DrawLyricRows(area, h, true);
        return;
    }
    DrawGuti(w / 2, h / 2, 0.85f, bob);
}

void Island::DrawExpandedMedia(float w, float h) {
    DrawAlbumGlow(w, h);  // kart, kapak resminin rengine bürünür
    const float pad = 18.f, art = 64.f;
    float s0 = Stagger(0);
    PushFade(s0, 0, 0, 0.9f + 0.1f * s0, pad + art / 2, pad + art / 2);  // kapak hafifçe büyüyerek gelir
    DrawArt(D2D1::RectF(pad, pad, pad + art, pad + art), 14.f);
    PopFade();

    float tx = pad + art + 14.f, tr = w - pad - 30.f;
    float s1 = Stagger(1);
    PushFade(s1, 8.f * (1 - s1), 0);
    const wchar_t* title = snapshot_.title.empty() ? L"Bilinmeyen parça" : snapshot_.title.c_str();
    Text(title, fTitle_.Get(), D2D1::RectF(tx, pad + 6, tr, pad + 30), kWhite);
    Text(snapshot_.artist.c_str(), fArtist_.Get(), D2D1::RectF(tx, pad + 32, tr, pad + 54), kGray);
    DrawBars(w - pad, pad + 18, 16.f, 3.f);
    {
        // Kalp: şarkıyı beğen
        bool liked = IsLiked();
        float hx = w - pad - 10.f, hy = pad + 44.f;
        PushFade(1.f, 0, 0, std::max(0.5f, heartPop_.value), hx, hy);
        Text(liked ? kGlyphHeartFill : kGlyphHeart, fIcon_.Get(), D2D1::RectF(hx - 12, hy - 12, hx + 12, hy + 12),
             liked ? Rgba(1.f, 0.42f, 0.58f) : (mediaHover_ == 22 ? kWhite : kGray));
        PopFade();
    }
    PopFade();

    // Şarkı sözleri: önceki, şimdiki, sonraki satır; yeni satır alttan kayarak gelir.
    float ly = LyricsH();
    if (ly > 0) {
        float sl = Stagger(2);
        PushFade(sl, 0, 5.f * (1 - sl));
        DrawLyricRows(D2D1::RectF(pad, pad + art + 6.f, w - pad, pad + art + 6.f + 3 * 17.f), 17.f, false);
        PopFade();
    }

    // İlerleme çubuğu
    float s2 = Stagger(2);
    PushFade(s2, 0, 5.f * (1 - s2));
    float py = pad + art + 14.f + ly;
    double dur = snapshot_.durationSec, pos = snapshot_.CurrentPosition();
    bool seeking = sliderDrag_ == 2 && !drawingOld_;
    if (seeking) pos = seekPreview_;
    // Üstüne gelince çubuk kalınlaşır, tutup sürükleyince şarkı oraya sarılır.
    float thick = (seeking || mediaHover_ == 20) ? 3.f : 2.f;
    D2D1_ROUNDED_RECT track{D2D1::RectF(pad, py + 2 - thick, w - pad, py + 2 + thick), thick, thick};
    brush_->SetColor(Rgba(1, 1, 1, 0.18f));
    rt_->FillRoundedRectangle(track, brush_.Get());
    if (dur > 0) {
        float fx = pad + (w - pad * 2) * static_cast<float>(std::clamp(pos / dur, 0.0, 1.0));
        D2D1_ROUNDED_RECT fill{D2D1::RectF(pad, py + 2 - thick, std::max(pad + 4, fx), py + 2 + thick), thick, thick};
        brush_->SetColor(WithAlpha(kWhite, 0.9f));
        rt_->FillRoundedRectangle(fill, brush_.Get());
        if (seeking || mediaHover_ == 20) {
            brush_->SetColor(kWhite);
            float kr = seeking ? 7.f : 5.f;
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fx, py + 2), kr, kr), brush_.Get());
        }
        Text(FormatDuration(pos).c_str(), fSmall_.Get(), D2D1::RectF(pad, py + 6, w / 2, py + 20), kGray);
        Text((L"-" + FormatDuration(std::max(0.0, dur - pos))).c_str(), fSmallRight_.Get(),
                 D2D1::RectF(w / 2, py + 6, w - pad, py + 20), kGray);
    }

    PopFade();

    // Kontroller: üstüne gelince yumuşak bir hale, basınca minik bir çökme.
    float s3 = Stagger(3);
    PushFade(s3, 0, 6.f * (1 - s3));
    float cy = py + 42.f, cx = w / 2;
    {
        // Sağda küçük ses sürgüsü
        if (volume_ < 0) {
            int v = 0;
            bool m = false;
            if (VolumeWatch::Get(v, m)) {
                volume_ = v;
                muted_ = m;
            }
        }
        bool silent = muted_ || volume_ <= 0;
        Text(silent ? kGlyphMute : kGlyphVolume, fIconSmall_.Get(), D2D1::RectF(w - 112, cy - 10, w - 92, cy + 10), kGray);
        float x0 = w - 88, x1 = w - pad;
        float vt = (sliderDrag_ == 3 || mediaHover_ == 21) ? 2.5f : 2.f;
        brush_->SetColor(Rgba(1, 1, 1, 0.18f));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, cy - vt, x1, cy + vt), vt, vt}, brush_.Get());
        if (volume_ >= 0) {
            float fx = x0 + (x1 - x0) * (silent ? 0.f : volume_ / 100.f);
            brush_->SetColor(WithAlpha(kWhite, 0.85f));
            rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, cy - vt, std::max(x0 + 4, fx), cy + vt), vt, vt},
                                      brush_.Get());
            if (sliderDrag_ == 3 || mediaHover_ == 21)
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fx, cy), 5.f, 5.f), brush_.Get());
        }
    }
    const float offs[3] = {-66.f, 0.f, 66.f};
    for (int i = 0; i < 3; ++i) {
        float bx = cx + offs[i];
        float glow = std::clamp(btnGlow_[i].value, 0.f, 1.f);
        if (glow > 0.01f) {
            brush_->SetColor(Rgba(1, 1, 1, 0.13f * glow));
            float rr = (i == 1 ? 24.f : 20.f) * (0.85f + 0.15f * glow);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, cy), rr, rr), brush_.Get());
        }
        PushFade(1.f, 0, 0, btnScale_[i].value, bx, cy);
        if (i == 1)
            Text(snapshot_.playing ? kGlyphPause : kGlyphPlay, fIconBig_.Get(),
                 D2D1::RectF(bx - 22, cy - 22, bx + 22, cy + 22), kWhite);
        else
            Text(i == 0 ? kGlyphPrev : kGlyphNext, fIcon_.Get(), D2D1::RectF(bx - 20, cy - 20, bx + 20, cy + 20),
                 kWhite);
        PopFade();
    }
    PopFade();
}

int Island::ButtonAt(float x, float y) const {
    if (view_ != View::ExpandedMedia) return -1;
    const float pad = 18.f, art = 64.f;
    float cy = pad + art + 14.f + 42.f + LyricsH(), cx = viewW_ / 2;
    if (std::fabs(y - cy) >= 24) return -1;
    if (std::fabs(x - (cx - 66)) < 24) return 0;
    if (std::fabs(x - cx) < 26) return 1;
    if (std::fabs(x - (cx + 66)) < 24) return 2;
    return -1;
}

void Island::OnMouseMove(float x, float y) {
    if (!trackingLeave_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        trackingLeave_ = TrackMouseEvent(&tme) != FALSE;
    }
    if (sliderDrag_ >= 0) {
        if (sliderDrag_ >= 2) MediaSlider(sliderDrag_, x);
        else QuickSlider(sliderDrag_, x);
        return;
    }
    int mh = MediaHit(x, y);
    if (mh != mediaHover_) {
        mediaHover_ = mh;
        StartAnimation();
    }
    int b = ButtonAt(x, y);
    if (b != hoverBtn_) {
        hoverBtn_ = b;
        for (int i = 0; i < 3; ++i) btnGlow_[i].target = i == b ? 1.f : 0.f;
        StartAnimation();
    }
    int t = ToolHit(x, y);  // -10/-11 oklar, -20.. noktalar: üstüne gelince parlar
    if (t != hoverTool_) {
        hoverTool_ = t;
        for (int i = 0; i < kToolCount; ++i) toolGlow_[i].target = i == t ? 1.f : 0.f;
        SyncFavGlow();
    }
    int hh = HomeHit(x, y);
    if (hh != homeHover_) {
        homeHover_ = hh;
        SyncFavGlow();
    }
    // Pano listesinde fareyle gezinmek seçimi de kaydırır.
    if (view_ == View::Command && input_.empty() && y > kCommandBar) {
        int row = static_cast<int>((y - kCommandBar - 4) / kCommandRow);
        if (row >= 0 && row < VisibleClips() && row != clipSel_) {
            clipSel_ = row;
            StartAnimation();
        }
    }
}

void Island::DrawBattery(float x, float y, float w, float h, int percent, bool charging) {
    D2D1_ROUNDED_RECT body{D2D1::RectF(x, y, x + w, y + h), 4, 4};
    brush_->SetColor(Rgba(1, 1, 1, 0.45f));
    rt_->DrawRoundedRectangle(body, brush_.Get(), 1.2f);
    D2D1_ROUNDED_RECT cap{D2D1::RectF(x + w + 1.5f, y + h * 0.3f, x + w + 3.5f, y + h * 0.7f), 1, 1};
    rt_->FillRoundedRectangle(cap, brush_.Get());
    float inner = (w - 4) * std::clamp(percent, 0, 100) / 100.f;
    D2D1_COLOR_F c = charging ? kGreen : (percent <= 20 ? kRed : kWhite);
    brush_->SetColor(c);
    D2D1_ROUNDED_RECT lvl{D2D1::RectF(x + 2, y + 2, x + 2 + std::max(2.f, inner), y + h - 2), 2.5f, 2.5f};
    rt_->FillRoundedRectangle(lvl, brush_.Get());
}

void Island::DrawExpandedIdle(float w, float h) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    float s0 = Stagger(0), s1 = Stagger(1), s2 = Stagger(2), s3 = Stagger(3);
    PushFade(s0, 0, 6.f * (1 - s0));
    DrawRolling(rollClock_, FormatClock(st), fBigClock_.Get(), D2D1::RectF(26, 10, w - 150, 56), kWhite);
    PopFade();
    PushFade(s1, 0, 6.f * (1 - s1));
    if (companion_.Pending() || companion_.TimerActive()) {
        Text(TimerText().c_str(), fDate_.Get(), D2D1::RectF(28, 56, w - 150, 80), TimerColor());
    } else if (!reminders_.empty()) {
        // En yakın hatırlatma
        const Reminder* next = &reminders_[0];
        for (const auto& r : reminders_) if (r.at < next->at) next = &r;
        ULONGLONG now = WallSeconds();
        int mins = next->at > now ? static_cast<int>((next->at - now + 59) / 60) : 0;
        wchar_t buf[160];
        if (mins >= 60) swprintf_s(buf, L"%s · %d sa %d dk", next->text.c_str(), mins / 60, mins % 60);
        else swprintf_s(buf, L"%s · %d dk", next->text.c_str(), mins);
        Text(buf, fDate_.Get(), D2D1::RectF(28, 56, w - 150, 80), kOrange);
    } else {
        std::wstring date = FormatDate(st);
        if (weather_.ok && weatherOn_) date += L"  ·  " + Weather::Short(weather_);
        Text(date.c_str(), fDate_.Get(), D2D1::RectF(28, 56, w - 150, 80), kGray);
    }
    PopFade();
    h = kExpandedIdle.h;  // raf açıksa alt kısım rafındır
    // Guti hafifçe nefes alır.
    float breath = 1.f + 0.018f * static_cast<float>(std::sin(time_ * 1.9));
    PushFade(s2, 0, 0, 0.85f + 0.15f * s2, w - 128, h / 2);
    DrawGuti(w - 128, h / 2, 1.6f * breath, -0.8f * (breath - 1.f) * 40.f);
    PopFade();
    PushFade(s3, 0, 6.f * (1 - s3));

    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryLifePercent <= 100) {
        int pct = ps.BatteryLifePercent;
        bool charging = ps.ACLineStatus == 1;
        float bx = w - 28 - 36;
        DrawBattery(bx, h / 2 - 16, 34, 16, pct, charging);
        wchar_t buf[16];
        swprintf_s(buf, L"%%%d", pct);  // Türkçe: yüzde işareti önde
        Text(buf, fSmall_.Get(), D2D1::RectF(bx, h / 2 + 4, bx + 40, h / 2 + 20), kGray);
    }
    PopFade();
}

std::wstring Island::TimerText() const {
    wchar_t buf[64];
    if (companion_.Pending()) {
        swprintf_s(buf, L"Zamanlayıcı: %d dk", companion_.PendingMinutes());
        return buf;
    }
    int sec = static_cast<int>(companion_.Remaining(Now()) + 0.999);
    const wchar_t* label = L"";
    switch (companion_.Kind()) {
    case Companion::TimerKind::Focus: label = L"Odak · "; break;
    case Companion::TimerKind::Break: label = L"Mola · "; break;
    default: break;
    }
    if (sec >= 3600)
        swprintf_s(buf, L"%s%d:%02d:%02d", label, sec / 3600, (sec / 60) % 60, sec % 60);
    else
        swprintf_s(buf, L"%s%d:%02d", label, sec / 60, sec % 60);
    return buf;
}

D2D1_COLOR_F Island::TimerColor() const {
    switch (companion_.Kind()) {
    case Companion::TimerKind::Focus: return kOrange;
    case Companion::TimerKind::Break: return kGreen;
    default: return companion_.Pending() ? accent_ : kWhite;
    }
}

void Island::DrawTimer(float w, float h) {
    // Küçük gözler solda, kalan süre sağda, altta ince ilerleme çizgisi.
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 26, h / 2, 0.62f, 0.f);
    std::wstring t;
    if (companion_.Pending()) {
        wchar_t buf[16];
        swprintf_s(buf, L"%d dk", companion_.PendingMinutes());
        t = buf;
    } else {
        int sec = static_cast<int>(companion_.Remaining(Now()) + 0.999);
        wchar_t buf[16];
        swprintf_s(buf, L"%d:%02d", sec / 60, sec % 60);
        t = buf;
    }
    DrawRolling(rollTimer_, t, fTinyRight_.Get(), D2D1::RectF(44, 0, w - 12, h), TimerColor());
    if (companion_.TimerActive() && companion_.Total() > 0 && !companion_.Pending()) {
        float frac = static_cast<float>(companion_.Remaining(Now()) / companion_.Total());
        brush_->SetColor(WithAlpha(TimerColor(), 0.8f));
        rt_->FillRectangle(D2D1::RectF(h / 2, h - 2.f, h / 2 + (w - h) * frac, h - 0.5f), brush_.Get());
    }
}

void Island::DrawPrivacyDots(float right, float cy) {
    float x = right;
    if (privacy_.Camera()) {
        brush_->SetColor(kGreen);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 2.6f, 2.6f), brush_.Get());
        x -= 7.f;
    }
    if (privacy_.Mic()) {
        brush_->SetColor(kOrange);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), 2.6f, 2.6f), brush_.Get());
    }
}

void Island::DrawDownload(float w, float h) {
    Text(kGlyphDownload, fIcon_.Get(), D2D1::RectF(12, 0, 42, h), kGreen);
    size_t slash = cardPath_.find_last_of(L'\\');
    std::wstring name = slash == std::wstring::npos ? cardPath_ : cardPath_.substr(slash + 1);
    Text(L"İndirme bitti · sürükle ya da tıkla", fSmall_.Get(), D2D1::RectF(48, 4, w - 18, 20), kGray);
    Text(name.c_str(), fAlert_.Get(), D2D1::RectF(48, 18, w - 18, h - 4), kWhite);
}

void Island::DrawAlert(float w, float h) {
    if (alertKind_ == AlertKind::Download) {
        DrawDownload(w, h);
        return;
    }
    if (alertKind_ == AlertKind::Screenshot) {
        DrawScreenshot(w, h);
        return;
    }
    if (alertKind_ == AlertKind::Volume) {
        DrawVolume(w, h);
        return;
    }
    if (alertKind_ >= AlertKind::UpdateAvailable) {
        const wchar_t* glyph = kGlyphSync;
        const wchar_t* label = L"";
        D2D1_COLOR_F color = accent_;
        switch (alertKind_) {
        case AlertKind::UpdateAvailable: label = L"Yeni sürüm var"; color = kGreen; break;
        case AlertKind::Updating: label = L"Güncelleniyor..."; break;
        case AlertKind::UpToDate: label = L"En güncel sürüm"; glyph = kGlyphCheck; color = kGreen; break;
        case AlertKind::UpdateFailed: label = L"Güncelleme başarısız"; glyph = kGlyphError; color = kRed; break;
        case AlertKind::TimerDone: label = L"Süre doldu!"; glyph = kGlyphTimer; color = kWhite; break;
        case AlertKind::FocusDone: label = L"Odak bitti, mola zamanı"; glyph = kGlyphTimer; color = kGreen; break;
        case AlertKind::BreakDone: label = L"Mola bitti, hadi devam"; glyph = kGlyphTimer; color = kOrange; break;
        case AlertKind::CpuBusy: label = L"İşlemci çok yoğun"; glyph = kGlyphWarning; color = kOrange; break;
        case AlertKind::MemoryFull: label = L"Bellek dolmak üzere"; glyph = kGlyphWarning; color = kRed; break;
        case AlertKind::Offline: label = L"İnternet gitti"; glyph = kGlyphGlobe; color = kRed; break;
        case AlertKind::Online: label = L"İnternet geri geldi"; glyph = kGlyphGlobe; color = kGreen; break;
        case AlertKind::Reminder: label = alertLabel_.c_str(); glyph = kGlyphTimer; color = kOrange; break;
        case AlertKind::ReminderSet:
        case AlertKind::Copied: label = alertLabel_.c_str(); glyph = kGlyphCheck; color = kGreen; break;
        case AlertKind::Headphones: label = alertLabel_.c_str(); glyph = kGlyphHeadphone; color = kGreen; break;
        case AlertKind::HeadphonesLow: label = alertLabel_.c_str(); glyph = kGlyphHeadphone; color = kRed; break;
        case AlertKind::Reading: label = alertLabel_.c_str(); glyph = kGlyphSync; color = accent_; break;
        case AlertKind::OcrFailed: label = alertLabel_.c_str(); glyph = kGlyphError; color = kOrange; break;
        case AlertKind::Notification: label = alertLabel_.c_str(); glyph = L"\xEA8F"; color = accent_; break;
        default: break;
        }
        Text(glyph, fIcon_.Get(), D2D1::RectF(12, 0, 40, h), color);
        Text(label, fAlert_.Get(), D2D1::RectF(46, 0, w - 70, h), kWhite);
        wchar_t buf[16];
        bool percent = alertKind_ == AlertKind::CpuBusy || alertKind_ == AlertKind::MemoryFull ||
                       alertKind_ == AlertKind::Headphones || alertKind_ == AlertKind::HeadphonesLow;
        if (alertKind_ == AlertKind::UpdateFailed)
            swprintf_s(buf, L"hata %d", alertValue_);
        else if (percent)
            swprintf_s(buf, L"%%%d", alertValue_);
        else
            swprintf_s(buf, L"v%d", alertValue_);
        bool showValue = alertValue_ > 0 && (alertKind_ <= AlertKind::UpdateFailed || percent);
        if (showValue) Text(buf, fAlertRight_.Get(), D2D1::RectF(w - 80, 0, w - 18, h), color);
        return;
    }
    SYSTEM_POWER_STATUS ps{};
    GetSystemPowerStatus(&ps);
    int pct = ps.BatteryLifePercent <= 100 ? ps.BatteryLifePercent : 0;
    const wchar_t* label = L"";
    D2D1_COLOR_F color = kWhite;
    switch (alertKind_) {
    case AlertKind::Charging: label = L"Şarj oluyor"; color = kGreen; break;
    case AlertKind::Unplugged: label = L"Şarj kesildi"; color = kWhite; break;
    case AlertKind::LowBattery: label = L"Pil azaldı"; color = kRed; break;
    default: break;
    }
    if (alertKind_ == AlertKind::Charging)
        Text(kGlyphBolt, fIcon_.Get(), D2D1::RectF(12, 0, 40, h), color);
    else
        DrawBattery(14, h / 2 - 6, 22, 12, pct, false);
    Text(label, fAlert_.Get(), D2D1::RectF(46, 0, w - 70, h), kWhite);
    wchar_t buf[16];
    swprintf_s(buf, L"%%%d", pct);
    Text(buf, fAlertRight_.Get(), D2D1::RectF(w - 80, 0, w - 18, h), color);
}

// ---------------------------------------------------------------------------
// Girdi
// ---------------------------------------------------------------------------

void Island::OnClick(float x, float y) {
    if (ToolsShown(view_)) {
        int t = ToolHit(x, y);
        if (t >= kFavHit) {
            if (t == kFavHit + kFavMax) AddFavoriteFromClipboard();
            else OpenFavorite(t - kFavHit);
            return;
        }
        if (t >= 0) {
            RunTool(static_cast<Tool>(t));
            return;
        }
        if (t <= -20) {
            SetToolPage(-20 - t);  // noktaya tıkla: o sayfa
            return;
        }
        if (t <= -10) {
            SetToolPage(toolPage_ + (t == -10 ? -1 : 1));  // oklar: önceki / sonraki
            return;
        }
        if (y > ToolsTop()) return;  // araç sırasının boşlukları / raf
    }
    if (view_ == View::Note) {
        if (x > viewW_ - 46 && y < 38) {  // çöp kutusu: notu sil
            note_.clear();
            SaveNote();
            eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.6);
            StartAnimation();
        }
        return;
    }
    if (view_ == View::Ink) {
        int hit = InkBarHit(x, y);
        if (hit >= 0 && hit < 10) SetInkColor(hit);
        if (hit == 10) InkUndo();
        if (hit == 11) InkClear();
        if (hit == 12) EndInk();
        return;
    }
    if (view_ == View::Quick) {
        QuickClick(QuickHit(x, y));
        return;
    }
    if (view_ == View::Home) {
        HomeClick(HomeHit(x, y));
        return;
    }
    if (view_ == View::Diary) {
        diaryOpen_ = false;  // günlüğe tıkla: kapanır
        Sounds::Play(Sounds::Sound::Close);
        Update();
        return;
    }
    if (view_ == View::ExpandedMedia) {
        const float pad = 18.f, art = 64.f;
        if (MediaHit(x, y) == 22) {
            ToggleLike();
            return;
        }
        if (homeOn_ && homeMedia_ && x >= pad && x <= pad + art && y >= pad && y <= pad + art) {
            homeMedia_ = false;  // kapağa tıkla: ana sayfaya dön
            Sounds::Play(Sounds::Sound::Close);
            Update();
            return;
        }
        float cy = pad + art + 14.f + 42.f + LyricsH(), cx = width_.value / 2;
        if (std::fabs(y - cy) < 24) {
            if (std::fabs(x - (cx - 66)) < 24) { media_.Send(MediaCommand::Previous); return; }
            if (std::fabs(x - cx) < 26) { media_.Send(MediaCommand::PlayPause); return; }
            if (std::fabs(x - (cx + 66)) < 24) { media_.Send(MediaCommand::Next); return; }
        }
        return;
    }
    if (view_ == View::Command) {
        // Pano satırına tıklama: kopyala
        if (input_.empty() && y > kCommandBar) {
            int row = static_cast<int>((y - kCommandBar - 4) / kCommandRow);
            if (row >= 0 && row < VisibleClips()) {
                clipSel_ = row;
                AcceptCommand();
            }
        }
        return;
    }
    if (view_ == View::ExpandedIdle) {
        // Saate tıkla: komut çubuğu. Gözlere tıkla: sev.
        if (x < width_.value - 180.f) OpenCommand();
        else Pet();
        return;
    }
    if (view_ == View::Media || view_ == View::Idle) {
        // Dokunmatik ekran / hızlı tık: hemen aç.
        KillTimer(hwnd_, kTimerHover);
        hover_ = true;
        Update();
    }
}

double Island::Now() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(qpcFreq_.QuadPart);
}

bool Island::ViewShowsEyes() const {
    return view_ == View::Idle || view_ == View::Media || view_ == View::ExpandedIdle ||
           view_ == View::Timer || view_ == View::Command || view_ == View::Picker ||
           view_ == View::Note || view_ == View::Ink || view_ == View::Quick || view_ == View::Home || view_ == View::Diary ||
           (view_ == View::Alert && alertKind_ == AlertKind::Volume);
}

void Island::Pet() {
    // Tıklama serisine göre duygu: sevilmek hoşuna gider, abartınca kızar.
    double now = Now();
    lastActivity_ = now;
    clickStreak_ = (now - lastClick_ < 0.7) ? clickStreak_ + 1 : 1;
    lastClick_ = now;
    companion_.OnPet(now);
    ++today_.pets;
    eyes_.SetAffection(companion_.Affection());

    using M = Eyes::Mood;
    if (clickStreak_ >= 8) {
        MoodWithSound(M::Dizzy, 2.0);  // başı döndü
        shake_.velocity += 420.f;
        clickStreak_ = 0;
    } else if (clickStreak_ >= 5) {
        MoodWithSound(M::Angry, 2.2);  // "yeter!"
        shake_.velocity += 300.f;
    } else if (clickStreak_ == 4) {
        MoodWithSound(M::Love, 2.6);
        height_.velocity += 60.f;
    } else if (clickStreak_ == 3) {
        MoodWithSound(M::Laugh, 1.8);  // gıdıklandı
        height_.velocity += 80.f;
    } else {
        eyes_.SetMood(M::Happy, now, 1.4);
        Sounds::Play(Sounds::Sound::Pet);
        height_.velocity += 70.f;  // sevinçle zıplar
        width_.velocity += 50.f;
    }
    StartAnimation();
}

void Island::MoodWithSound(Eyes::Mood mood, double duration) {
    using M = Eyes::Mood;
    eyes_.SetMood(mood, Now(), duration);
    switch (mood) {
    case M::Happy: case M::Cute: Sounds::Play(Sounds::Sound::Happy); break;
    case M::Laugh: Sounds::Play(Sounds::Sound::Laugh); break;
    case M::Love: Sounds::Play(Sounds::Sound::Love); break;
    case M::Angry: case M::Dizzy: case M::Sulk: Sounds::Play(Sounds::Sound::Angry); break;
    case M::Sad: Sounds::Play(Sounds::Sound::Sad); break;
    case M::Yawn: Sounds::Play(Sounds::Sound::Yawn); break;
    default: break;
    }
    StartAnimation();
}

void Island::SetHover(bool hover) {
    if (hover == hover_) return;
    hover_ = hover;
    if (!hover) {
        quickOpen_ = false;
        homeMedia_ = false;
        if (diaryOpen_ && diaryUntil_ == 0) diaryOpen_ = false;
    }  // kapanınca bir dahaki açılış yine araçlarla başlar
    Sounds::Play(hover ? Sounds::Sound::Open : Sounds::Sound::Close);
}

void Island::OnCompanionTick() {
    double now = Now();
    using E = Companion::Event;
    using M = Eyes::Mood;
    switch (companion_.Tick(now)) {
    case E::TimerStarted:
        if (companion_.Kind() == Companion::TimerKind::Focus) eyes_.SetMood(M::Focused, now, 0);
        else MoodWithSound(M::Happy, 1.0);
        break;
    case E::TimerDone: ShowAlert(AlertKind::TimerDone, 0, 5000); break;
    case E::FocusDone: ShowAlert(AlertKind::FocusDone, 0, 5000); break;
    case E::BreakDone: ShowAlert(AlertKind::BreakDone, 0, 5000); break;
    case E::Distracted:
        // "Hani odaklanıyorduk?" — kaşlarını çatar ve hafifçe sallanır.
        MoodWithSound(M::Angry, 2.5);
        shake_.velocity += 260.f;
        break;
    case E::NeedBreak:
        // Bir saattir aralıksız çalışıyorsun: Guti esneyip gerinir.
        MoodWithSound(M::Yawn, 3.0);
        height_.velocity += 150.f;
        lastActivity_ = now;
        break;
    case E::None: break;
    }

    // Odak sürerken Guti'nin varsayılan yüzü ciddi; bitince normale döner.
    bool focus = companion_.Kind() == Companion::TimerKind::Focus;
    if (focus && eyes_.CurrentMood() == M::Normal) eyes_.SetMood(M::Focused, now, 0);
    if (!focus && eyes_.CurrentMood() == M::Focused) eyes_.SetMood(M::Normal, now, 0);

    // Uzun ayrılıktan sonra: önce küser, sonra dayanamayıp sevinir.
    if (reunionAt_ > 0 && now >= reunionAt_) {
        reunionAt_ = 0;
        MoodWithSound(M::Love, 2.5);
        height_.velocity += 100.f;
        StartAnimation();
    }
    if (TargetView() != view_) Update();
}

// ---------------------------------------------------------------------------
// Guti'nin duyuları: bilgisayar sağlığı, kamera/mikrofon, ekran görüntüsü, indirmeler
// ---------------------------------------------------------------------------

void Island::OnHealthTick() {
    health_.Sample();
    double now = Now();
    bool quiet = fullscreen_ || userHidden_;  // oyun oynarken araya girme

    // İşlemci: 10 sn boyunca %85 üstü -> terler. %60 altına inince rahatlar.
    float cpu = health_.Cpu();
    if (cpu > 0.85f) ++cpuHotSamples_;
    else if (cpu < 0.60f) { cpuHotSamples_ = 0; cpuHot_ = false; }
    if (!cpuHot_ && cpuHotSamples_ >= 5) {
        cpuHot_ = true;
        if (!quiet && now - lastCpuAlert_ > 600) {
            lastCpuAlert_ = now;
            ShowAlert(AlertKind::CpuBusy, static_cast<int>(cpu * 100 + 0.5f), 3500);
        }
    }
    // Yoğunluk sürerse arada bir alnını siler (sürekli animasyon yapıp yükü artırmadan).
    if (cpuHot_ && !quiet && now - lastSweat_ > 30 && eyes_.CurrentMood() == Eyes::Mood::Normal) {
        eyes_.SetMood(Eyes::Mood::Sweaty, now, 3.0);
        lastSweat_ = now;
        StartAnimation();
    }

    // Bellek: %92 üstü 10 sn -> başı döner.
    float ram = health_.Ram();
    ramHotSamples_ = ram >= 0.92f ? ramHotSamples_ + 1 : 0;
    if (ramHotSamples_ == 5 && !quiet && now - lastRamAlert_ > 600) {
        lastRamAlert_ = now;
        ShowAlert(AlertKind::MemoryFull, static_cast<int>(ram * 100 + 0.5f), 3500);
    }

    // İnternet: iki ölçüm üst üste yoksa üzülür, gelince sevinir.
    switch (health_.Network()) {
    case SystemHealth::Net::Offline:
        if (++offlineSamples_ == 2 && !offlineShown_) {
            offlineShown_ = true;
            if (!quiet) ShowAlert(AlertKind::Offline, 0, 3500);
        }
        break;
    case SystemHealth::Net::Online:
        offlineSamples_ = 0;
        if (offlineShown_) {
            offlineShown_ = false;
            if (!quiet) ShowAlert(AlertKind::Online, 0, 2500);
        }
        break;
    case SystemHealth::Net::Unknown: break;
    }
}

void Island::OnPrivacyTick() {
    bool cam = privacy_.Camera(), mic = privacy_.Mic();
    if (!privacy_.Poll()) return;
    if ((privacy_.Camera() && !cam) || (privacy_.Mic() && !mic)) {
        // Biri kamerayı / mikrofonu açtı: Guti merakla bakar.
        eyes_.SetMood(Eyes::Mood::Curious, Now(), 1.6);
        lastActivity_ = Now();
    }
    StartAnimation();
}

void Island::OnClipboardUpdate() {
    CaptureClipboardText();
    // Ekran görüntüsü: panoya sadece resim gelir (tarayıcıdan kopyalanan resimde HTML de olur).
    static const UINT html = RegisterClipboardFormatW(L"HTML Format");
    if (!IsClipboardFormatAvailable(CF_DIB) && !IsClipboardFormatAvailable(CF_BITMAP)) return;
    if (IsClipboardFormatAvailable(html) || IsClipboardFormatAvailable(CF_HDROP)) return;
    double now = Now();
    if (ocrPending_) {
        // "Yazı al" ile seçilen alan geldi: kart yerine Guti okusun.
        ocrPending_ = false;
        lastShot_ = now;  // alan seçici panoya iki kez yazabilir: ikincisi kart açmasın
        if (now - ocrAskedAt_ < 60.0) {
            ComPtr<IWICBitmap> bmp = ClipboardBitmap();
            if (bmp && RunOcr(bmp.Get())) return;
            OnOcrDone(-2, L"");
            return;
        }
    }
    if (now - lastShot_ < 2.0) return;
    lastShot_ = now;
    lastActivity_ = now;
    if (!commandMode_ && SaveClipboardImage()) {
        ShowAlert(AlertKind::Screenshot, 0, 8000);  // önizleme kartı: sürükle ya da aç
        return;
    }
    eyes_.SetMood(Eyes::Mood::Wink, now, 1.2);
    Sounds::Play(Sounds::Sound::Shutter);
    height_.velocity += 70.f;
    StartAnimation();
}

void Island::OnDownload(const std::wstring& path) {
    cardPath_ = path;
    ShowAlert(AlertKind::Download, 0, 8000);
}

bool Island::DragFiles(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> pidls;
    for (const auto& p : paths)
        if (PIDLIST_ABSOLUTE id = ILCreateFromPathW(p.c_str())) pidls.push_back(id);
    bool dropped = false;
    std::vector<LPCITEMIDLIST> cpidls(pidls.begin(), pidls.end());
    ComPtr<IShellItemArray> items;
    ComPtr<IDataObject> data;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), cpidls.data(), &items)) &&
        SUCCEEDED(items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) {
        eyes_.SetMood(Eyes::Mood::Surprised, Now(), 0.8);
        StartAnimation();
        DWORD effect = DROPEFFECT_NONE;
        HRESULT hr = SHDoDragDrop(hwnd_, data.Get(), nullptr,
                                  DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        dropped = hr == DRAGDROP_S_DROP;
        if (dropped) MoodWithSound(Eyes::Mood::Happy, 1.4);  // teslim edildi
    }
    for (auto id : pidls) ILFree(id);
    return dropped;
}

void Island::OpenCard() {
    if (cardPath_.empty()) return;
    AllowSetForegroundWindow(ASFW_ANY);
    if (alertKind_ == AlertKind::Screenshot) {
        ShellExecuteW(nullptr, L"open", cardPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(cardPath_.c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
    alertActive_ = false;
    KillTimer(hwnd_, kTimerAlert);
    eyes_.SetMood(Eyes::Mood::Happy, Now(), 1.2);
    Update();
}

void Island::StartCardDrag() {
    KillTimer(hwnd_, kTimerAlert);  // sürüklerken kart kaybolmasın
    if (DragFiles({cardPath_})) alertActive_ = false;
    else SetTimer(hwnd_, kTimerAlert, 3000, nullptr);
    Update();
}

IWICImagingFactory* Island::Wic() {
    if (!wic_)
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    return wic_.Get();
}

void Island::TakeScreenshot() {
    ocrPending_ = false;  // vazgeçilmiş bir "yazı al" bu görüntüyü okumasın
    // Windows'un kendi alan seçicisi; görüntü panoya gelince Guti kartı gösterir.
    ShellExecuteW(nullptr, L"open", L"ms-screenclip:", nullptr, nullptr, SW_SHOWNORMAL);
}

bool Island::SaveClipboardImage() {
    IWICImagingFactory* wic = Wic();
    if (!wic) return false;
    ComPtr<IWICBitmap> full = ClipboardBitmap();
    if (!full) return false;
    UINT w = 0, h = 0;
    full->GetSize(&w, &h);
    if (w < 8 || h < 8) return false;

    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    std::wstring dir = std::wstring(tmp) + L"Guti";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"\\Ekran %04d-%02d-%02d %02d.%02d.%02d.png", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    std::wstring path = dir + name;

    // PNG olarak kaydet: sürükleyip bırakmak için gerçek bir dosya gerekir.
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    bool ok = SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props.Get()));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        frame->SetSize(w, h);
        frame->SetPixelFormat(&fmt);
        ComPtr<IWICFormatConverter> conv;
        ok = SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
             SUCCEEDED(conv->Initialize(full.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0,
                                        WICBitmapPaletteTypeCustom)) &&
             SUCCEEDED(frame->WriteSource(conv.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit());
    }
    stream.Reset();
    if (!ok) {
        DeleteFileW(path.c_str());
        return false;
    }

    // Küçük önizleme: tam boy görüntüyü bellekte tutma.
    float k = std::min(216.f / w, 144.f / h);
    UINT tw = std::max(1u, static_cast<UINT>(w * k)), th = std::max(1u, static_cast<UINT>(h * k));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> pconv;
    shotThumb_.Reset();
    shotBitmap_.Reset();
    if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(full.Get(), tw, th, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateFormatConverter(&pconv)) &&
        SUCCEEDED(pconv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                    nullptr, 0, WICBitmapPaletteTypeCustom)))
        wic->CreateBitmapFromSource(pconv.Get(), WICBitmapCacheOnLoad, &shotThumb_);
    cardPath_ = path;
    return true;
}

void Island::DrawScreenshot(float w, float h) {
    const float boxW = 108.f, boxH = 72.f, x0 = 12.f, y0 = (h - boxH) / 2;
    float s0 = Stagger(0), s1 = Stagger(2);
    PushFade(s0, 0, 0, 0.85f + 0.15f * s0, x0 + boxW / 2, h / 2);
    if (!shotBitmap_ && shotThumb_) rt_->CreateBitmapFromWicBitmap(shotThumb_.Get(), nullptr, &shotBitmap_);
    if (shotBitmap_) {
        D2D1_SIZE_F bs = shotBitmap_->GetSize();
        float k = std::min(boxW / bs.width, boxH / bs.height);
        float dw = bs.width * k, dh = bs.height * k;
        D2D1_RECT_F r = D2D1::RectF(x0 + (boxW - dw) / 2, y0 + (boxH - dh) / 2, x0 + (boxW + dw) / 2,
                                    y0 + (boxH + dh) / 2);
        ComPtr<ID2D1BitmapBrush> bb;
        rt_->CreateBitmapBrush(shotBitmap_.Get(), &bb);
        if (bb) {
            bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(r.left, r.top));
            bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, 8, 8}, bb.Get());
        }
        brush_->SetColor(Rgba(1, 1, 1, 0.18f));
        rt_->DrawRoundedRectangle(D2D1_ROUNDED_RECT{r, 8, 8}, brush_.Get(), 1.f);
    }
    PopFade();
    float tx = x0 + boxW + 14;
    PushFade(s1, 8.f * (1 - s1), 0);
    Text(L"Ekran görüntüsü", fAlert_.Get(), D2D1::RectF(tx, h / 2 - 22, w - 20, h / 2 + 2), kWhite);
    Text(L"sürükle bırak · tıkla: aç", fSmall_.Get(), D2D1::RectF(tx, h / 2 + 2, w - 20, h / 2 + 20), kGray);
    PopFade();
}

void Island::DrawDrop(float w, float h) {
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 34, h / 2, 0.85f, 0.f);
    Text(dropUrl_ ? L"Bırak, favorilere eklesin" : L"Bırak, Guti tutsun", fAlert_.Get(),
         D2D1::RectF(66, 0, w - 16, h), kWhite);
}

void Island::OnDropEnter() {
    dropHover_ = true;
    eyes_.SetMood(Eyes::Mood::Cute, Now(), 0);  // kocaman gözlerle bekler
    Sounds::Play(Sounds::Sound::Open);
    Update();
}

void Island::OnDropLeave() {
    dropHover_ = false;
    dropUrl_ = false;
    dropArmed_ = false;
    eyes_.SetMood(Eyes::Mood::Normal, Now(), 0);
    Update();
}

void Island::OnDropFiles(const std::vector<std::wstring>& paths) {
    dropHover_ = false;
    dropArmed_ = false;
    for (const auto& p : paths) {
        auto it = std::find_if(shelf_.begin(), shelf_.end(), [&](const ShelfItem& s) { return s.path == p; });
        if (it == shelf_.end()) shelf_.push_back({p, nullptr});
    }
    if (shelf_.size() > static_cast<size_t>(kShelfMax))
        shelf_.erase(shelf_.begin(), shelf_.end() - kShelfMax);
    eyes_.SetMood(Eyes::Mood::Normal, Now(), 0);
    MoodWithSound(Eyes::Mood::Happy, 1.6);  // "tamam, bende!"
    height_.velocity += 120.f;
    lastActivity_ = Now();
    Update();
}

bool Island::ShelfVisible() const { return !shelf_.empty(); }

float Island::ShelfTop() const {
    return CardH(view_) + kToolsH;
}

int Island::ShelfHit(float x, float y) const {
    if (!ShelfVisible() || (view_ != View::ExpandedIdle && view_ != View::ExpandedMedia)) return -1;
    float top = ShelfTop();
    if (y < top - 4 || y > top + kShelfH) return -1;
    if (x > width_.value - 40) return -2;  // temizle
    int i = static_cast<int>((x - 18) / kShelfCell);
    int shown = std::min<int>(static_cast<int>(shelf_.size()), kShelfShown);
    return x >= 18 && i < shown ? i : -1;
}

ID2D1Bitmap* Island::ShelfIcon(size_t i) {
    ShelfItem& it = shelf_[i];
    if (it.icon) return it.icon.Get();
    IWICImagingFactory* wic = Wic();
    SHFILEINFOW sfi{};
    if (!wic || !SHGetFileInfoW(it.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) ||
        !sfi.hIcon)
        return nullptr;
    ComPtr<IWICBitmap> wb;
    wic->CreateBitmapFromHICON(sfi.hIcon, &wb);
    DestroyIcon(sfi.hIcon);
    ComPtr<IWICFormatConverter> conv;
    if (wb && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0, WICBitmapPaletteTypeCustom)))
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &it.icon);
    return it.icon.Get();
}

void Island::DrawShelf(float w) {
    float top = ShelfTop();
    brush_->SetColor(Rgba(1, 1, 1, 0.08f));
    rt_->FillRectangle(D2D1::RectF(18, top - 2, w - 18, top - 1), brush_.Get());
    int shown = std::min<int>(static_cast<int>(shelf_.size()), kShelfShown);
    for (int i = 0; i < shown; ++i) {
        float cx = 18 + i * kShelfCell + kShelfCell / 2;
        float st = Stagger(4 + i);
        PushFade(st, 0, 8.f * (1 - st), 0.8f + 0.2f * st, cx, top + 18);
        if (ID2D1Bitmap* icon = ShelfIcon(static_cast<size_t>(i)))
            rt_->DrawBitmap(icon, D2D1::RectF(cx - 14, top + 4, cx + 14, top + 32));
        const std::wstring& p = shelf_[static_cast<size_t>(i)].path;
        size_t slash = p.find_last_of(L'\\');
        std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
        Text(name.c_str(), fSmallCenter_.Get(),
             D2D1::RectF(cx - kShelfCell / 2 + 2, top + 34, cx + kShelfCell / 2 - 2, top + 50), kGray);
        PopFade();
    }
    if (static_cast<int>(shelf_.size()) > shown) {
        wchar_t more[16];
        swprintf_s(more, L"+%d", static_cast<int>(shelf_.size()) - shown);
        float x = 18 + shown * kShelfCell;
        Text(more, fAlert_.Get(), D2D1::RectF(x, top + 4, x + 40, top + 32), kGray);
    }
    Text(kGlyphClose, fIconSmall_.Get(), D2D1::RectF(w - 40, top + 4, w - 16, top + 32), kGray);
}

// ---------------------------------------------------------------------------
// Ses göstergesi, kulaklık pili, renk seçici
// ---------------------------------------------------------------------------

void Island::OnVolume(int percent, bool muted) {
    int before = volume_;
    bool wasMuted = muted_;
    volume_ = percent;
    muted_ = muted;
    if (sleepFading_ || Now() < volQuietUntil_) return;  // uyku zamanlayıcısı sesi kısıyor
    if (view_ == View::Quick || view_ == View::ExpandedMedia) {  // sürgü zaten gösteriyor
        StartAnimation();
        return;
    }
    if (!volumeOsd_ || before < 0 || Now() - startedAt_ < 3 || (before == percent && wasMuted == muted)) return;
    double now = Now();
    // Sessize alınca kulaklarını kapatır gibi gözlerini yumar; çok açınca irkilir.
    if ((muted || percent == 0) && !(wasMuted || before == 0)) eyes_.SetMood(Eyes::Mood::Sleepy, now, 1.4);
    else if (!muted && percent >= 90 && before < 90) eyes_.SetMood(Eyes::Mood::Surprised, now, 0.9);
    else if (!muted && (wasMuted || before == 0)) eyes_.SetMood(Eyes::Mood::Happy, now, 0.8);
    if (!(alertActive_ && alertKind_ == AlertKind::Volume)) volBar_.Snap(static_cast<float>(before));
    volBar_.target = static_cast<float>(percent);
    ShowAlert(AlertKind::Volume, percent, 1500);
}

void Island::DrawVolume(float w, float h) {
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 30, h / 2, 0.68f, 0.f);
    bool silent = muted_ || alertValue_ == 0;
    Text(silent ? kGlyphMute : kGlyphVolume, fIcon_.Get(), D2D1::RectF(54, 0, 80, h), kWhite);
    float x0 = 88, x1 = w - 58, cy = h / 2;
    D2D1_ROUNDED_RECT track{D2D1::RectF(x0, cy - 3, x1, cy + 3), 3, 3};
    brush_->SetColor(Rgba(1, 1, 1, 0.18f));
    rt_->FillRoundedRectangle(track, brush_.Get());
    if (!silent) {
        float fx = x0 + (x1 - x0) * std::clamp(volBar_.value, 0.f, 100.f) / 100.f;
        D2D1_ROUNDED_RECT fill{D2D1::RectF(x0, cy - 3, std::max(x0 + 6, fx), cy + 3), 3, 3};
        brush_->SetColor(kWhite);
        rt_->FillRoundedRectangle(fill, brush_.Get());
    }
    wchar_t buf[16];
    swprintf_s(buf, L"%%%d", silent ? 0 : alertValue_);
    DrawRolling(rollVolume_, buf, fAlertRight_.Get(), D2D1::RectF(x1, 0, w - 16, h), silent ? kGray : kWhite);
}

void Island::OnBtBattery(int level, const std::wstring& name) {
    bool connectedNow = level != 255, wasConnected = btLevel_ != 255;
    btLevel_ = level;
    if (!connectedNow) {
        btLowShown_ = false;
        return;
    }
    btName_ = name.empty() ? L"Kulaklık" : name;
    alertLabel_ = btName_;
    bool quiet = Now() - startedAt_ < 10;  // açılışta zaten bağlıysa araya girme
    if (!wasConnected && !quiet) {
        ShowAlert(AlertKind::Headphones, level, 3500);
    } else if (level <= 15 && !btLowShown_) {
        btLowShown_ = true;
        alertLabel_ = btName_ + L" · pil azaldı";
        ShowAlert(AlertKind::HeadphonesLow, level, 4000);
    } else if (level > 25) {
        btLowShown_ = false;
    }
}

namespace {
D2D1_COLOR_F ColorOf(COLORREF c) {
    return Rgba(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
}
D2D1_COLOR_F EyeTint(COLORREF c) {
    // Siyah zeminde kaybolmasın: koyu renkleri biraz aç.
    D2D1_COLOR_F k = ColorOf(c);
    float lum = 0.2126f * k.r + 0.7152f * k.g + 0.0722f * k.b;
    if (lum < 0.35f) {
        float t = (0.35f - lum) / 0.35f * 0.5f;
        k.r += (1 - k.r) * t; k.g += (1 - k.g) * t; k.b += (1 - k.b) * t;
    }
    return k;
}
std::wstring Hex(COLORREF c) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}
}  // namespace

LRESULT CALLBACK Island::PickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Island* self = reinterpret_cast<Island*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Island*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && self->picker_ == hwnd) {
        switch (msg) {
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
        case WM_MOUSEMOVE: self->OnPickerMove(); return 0;
        case WM_LBUTTONDOWN: self->EndPicker(true); return 0;
        case WM_RBUTTONDOWN: self->EndPicker(false); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) self->EndPicker(false);
            if (wp == VK_RETURN || wp == VK_SPACE) self->EndPicker(true);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;
        case WM_CLOSE: self->EndPicker(false); return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Island::StartPicker() {
    if (picker_ || ink_) return;
    if (commandMode_) CloseCommand(false);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = PickerProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"GutiColorPicker";
        RegisterClassExW(&wc);
        registered = true;
    }
    if (userHidden_) ToggleUserHidden();
    // Tüm ekranları kaplayan, neredeyse görünmez bir katman: tıklamayı ve imleci o alır.
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    picker_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"GutiColorPicker", L"",
                              WS_POPUP, vx, vy, vw, vh, nullptr, nullptr, instance_, this);
    if (!picker_) return;
    SetLayeredWindowAttributes(picker_, 0, 1, LWA_ALPHA);
    ShowWindow(picker_, SW_SHOW);
    SetForegroundWindow(picker_);
    SetFocus(picker_);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    eyes_.SetMood(Eyes::Mood::Curious, Now(), 1.0);
    Sounds::Play(Sounds::Sound::Open);
    OnPickerMove();
    Update();
}

void Island::OnPickerMove() {
    POINT pt;
    GetCursorPos(&pt);
    HDC dc = GetDC(nullptr);
    COLORREF c = GetPixel(dc, pt.x, pt.y);
    ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID || c == pickColor_) return;
    pickColor_ = c;
    D2D1_COLOR_F tint = EyeTint(c);
    eyes_.SetTint(&tint);  // gözleri bakılan renge bürünür
    if (!animating_) Render();
}

void Island::EndPicker(bool pick) {
    if (!picker_) return;
    HWND w = picker_;
    picker_ = nullptr;
    DestroyWindow(w);
    if (pickForEyes_) {
        // Guti'nin yeni göz rengi ekrandan seçildi.
        pickForEyes_ = false;
        eyes_.SetTint(nullptr);
        if (pick) SetEyeMode(1, pickColor_);
        Update();
        return;
    }
    if (pick) {
        std::wstring hex = Hex(pickColor_);
        CopyToClipboard(hex);
        alertLabel_ = hex + L" kopyalandı";
        ShowAlert(AlertKind::Copied, 0, 2500);
        SetTimer(hwnd_, kTimerTint, 3000, nullptr);  // biraz renkli kalsın
    } else {
        eyes_.SetTint(nullptr);
        Update();
    }
}

void Island::DrawPicker(float w, float h) {
    D2D1_COLOR_F c = ColorOf(pickColor_);
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(24, h / 2), 11, 11), brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.6f));
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(24, h / 2), 11, 11), brush_.Get(), 1.2f);
    Text(Hex(pickColor_).c_str(), fAlert_.Get(), D2D1::RectF(44, 0, w - 70, h), kWhite);
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), w - 36, h / 2, 0.68f, 0.f);
}

// ---------------------------------------------------------------------------
// Komut çubuğu
// ---------------------------------------------------------------------------

int Island::VisibleClips() const {
    return input_.empty() ? static_cast<int>(std::min<size_t>(clips_.size(), 5)) : 0;
}

float Island::CommandHeight() const {
    int n = VisibleClips();
    return n ? kCommandBar + 4.f + n * kCommandRow + 8.f : kCommandBar;
}

void Island::OpenCommand() {
    if (commandMode_) return;
    HWND keep = prevForeground_;  // nottan geçiliyorsa asıl uygulamayı unutma
    CloseNote(false);
    HWND fg = GetForegroundWindow();
    prevForeground_ = fg != hwnd_ ? fg : keep;
    commandMode_ = true;
    input_.clear();
    cmdResult_ = {};
    clipSel_ = 0;
    Update();
    Render();
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    eyes_.SetMood(Eyes::Mood::Curious, Now(), 0.8);  // "evet?"
    Sounds::Play(Sounds::Sound::Open);
    lastActivity_ = Now();
    StartAnimation();
}

void Island::CloseCommand(bool restoreFocus) {
    if (!commandMode_) return;
    commandMode_ = false;
    Update();
    if (restoreFocus && prevForeground_ && IsWindow(prevForeground_)) SetForegroundWindow(prevForeground_);
    prevForeground_ = nullptr;
}

void Island::RefreshCommandResult() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    auto before = cmdResult_.kind;
    cmdResult_ = Command::Evaluate(input_, st.wHour, st.wMinute, st.wSecond);
    // "fav youtube.com" favorilere ekler, "isim Pıtır" Guti'nin adını değiştirir.
    std::wstring low = input_.substr(0, 8);
    if (!low.empty()) CharLowerBuffW(low.data(), static_cast<DWORD>(low.size()));
    cmdSpecial_ = low.rfind(L"fav ", 0) == 0 ? 1 : (low.rfind(L"isim ", 0) == 0 ? 2 : (low.rfind(L"şehir ", 0) == 0 ? 3 : 0));
    if (cmdSpecial_) {
        cmdResult_ = {};
        cmdResult_.kind = Command::Result::Kind::Value;
        cmdResult_.display = cmdSpecial_ == 1 ? L"+ favori" : (cmdSpecial_ == 2 ? L"\u2665 isim" : L"\u2600 şehir");
    }
    double now = Now();
    if (cmdResult_.divideByZero) {
        eyes_.SetMood(Eyes::Mood::Dizzy, now, 1.2);  // sıfıra bölünce başı döner
    } else if (cmdResult_.kind != before && cmdResult_.kind != Command::Result::Kind::None &&
               cmdResult_.kind != Command::Result::Kind::Error) {
        eyes_.SetMood(Eyes::Mood::Happy, now, 0.6);  // anladı!
    }
    lastActivity_ = now;
    Update();
    if (!animating_) Render();
}

void Island::OnCommandChar(wchar_t c) {
    switch (c) {
    case 27: CloseCommand(true); return;              // Esc
    case 13: AcceptCommand(); return;                 // Enter
    case 8: if (!input_.empty()) input_.pop_back(); break;  // Geri sil
    case 127:                                         // Ctrl+Geri sil: kelime sil
        while (!input_.empty() && input_.back() == L' ') input_.pop_back();
        while (!input_.empty() && input_.back() != L' ') input_.pop_back();
        break;
    case 22:                                          // Ctrl+V
        if (!clips_.empty()) {
            std::wstring t = clips_[0];
            for (auto& ch : t) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
            input_ += t.substr(0, 200);
        }
        break;
    default:
        if (c < 32 || input_.size() >= 200) return;
        input_ += c;
        break;
    }
    RefreshCommandResult();
}

void Island::OnCommandKey(UINT vk) {
    int n = VisibleClips();
    if (!n) return;
    if (vk == VK_UP) clipSel_ = (clipSel_ + n - 1) % n;
    else if (vk == VK_DOWN) clipSel_ = (clipSel_ + 1) % n;
    else return;
    StartAnimation();
}

void Island::AcceptCommand() {
    using K = Command::Result::Kind;
    double now = Now();
    if (input_.empty()) {
        if (clipSel_ < VisibleClips()) {
            CopyToClipboard(clips_[clipSel_]);
            eyes_.SetMood(Eyes::Mood::Wink, now, 1.0);
            Sounds::Play(Sounds::Sound::Pet);
            CloseCommand(true);
        } else {
            CloseCommand(true);
        }
        return;
    }
    if (cmdSpecial_) {
        int special = cmdSpecial_;
        std::wstring arg = input_.substr(input_.find(L' ') + 1);
        cmdSpecial_ = 0;
        CloseCommand(true);
        if (special == 2) {
            SetName(arg);
        } else if (special == 3) {
            SetCity(arg);
        } else if (!AddFavorite(arg)) {
            eyes_.SetMood(Eyes::Mood::Curious, now, 1.0);  // bunu açamam
            shake_.velocity += 160.f;
            StartAnimation();
        }
        return;
    }
    switch (cmdResult_.kind) {
    case K::Value:
        CopyToClipboard(cmdResult_.copy);
        CloseCommand(true);
        MoodWithSound(Eyes::Mood::Happy, 1.2);
        height_.velocity += 70.f;
        break;
    case K::Reminder: {
        reminders_.push_back({WallSeconds() + static_cast<ULONGLONG>(cmdResult_.reminderSeconds + 0.5),
                              cmdResult_.reminderText});
        SaveReminders();
        alertLabel_ = cmdResult_.display.substr(2) + L": " + cmdResult_.reminderText;
        CloseCommand(true);
        ShowAlert(AlertKind::ReminderSet, 0, 2500);
        break;
    }
    default:
        // Anlamadı: başını hafifçe sallar.
        eyes_.SetMood(Eyes::Mood::Curious, now, 1.0);
        shake_.velocity += 160.f;
        StartAnimation();
        break;
    }
}

void Island::CaptureClipboardText() {
    // Şifre yöneticileri bu biçimlerle "geçmişe ekleme" der: saygı duy.
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    static const UINT viewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (IsClipboardFormatAvailable(exclude) || IsClipboardFormatAvailable(viewerIgnore)) return;
    std::wstring text;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(hwnd_)) {
            bool allowed = true;
            if (HANDLE hh = GetClipboardData(history)) {
                if (auto* p = static_cast<const DWORD*>(GlobalLock(hh))) {
                    allowed = *p != 0;
                    GlobalUnlock(hh);
                }
            }
            if (allowed) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        text.assign(p, wcsnlen(p, 4000));
                        GlobalUnlock(h);
                    }
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(15);
    }
    size_t a = text.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return;
    auto it = std::find(clips_.begin(), clips_.end(), text);
    if (it != clips_.end()) clips_.erase(it);
    clips_.insert(clips_.begin(), text);
    if (clips_.size() > 5) clips_.resize(5);
}

void Island::CopyToClipboard(const std::wstring& text) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!OpenClipboard(hwnd_)) {
            Sleep(15);
            continue;
        }
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* p = GlobalLock(g)) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(g);
                if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
            } else {
                GlobalFree(g);
            }
        }
        CloseClipboard();
        return;
    }
}

void Island::CheckReminders() {
    if (reminders_.empty()) return;
    ULONGLONG now = WallSeconds();
    for (size_t i = 0; i < reminders_.size(); ++i) {
        if (reminders_[i].at > now) continue;
        alertLabel_ = reminders_[i].text;
        reminders_.erase(reminders_.begin() + static_cast<ptrdiff_t>(i));
        SaveReminders();
        if (userHidden_) ToggleUserHidden();  // hatırlatma kaçmasın
        ShowAlert(AlertKind::Reminder, 0, 8000);
        return;  // aynı anda birden fazlaysa sıradaki bir sonraki saniyede
    }
}

void Island::LoadReminders() {
    // "zaman|metin" satırları (REG_MULTI_SZ). Bir saatten eski kaçırılanlar atılır.
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Reminders", RRF_RT_REG_MULTI_SZ, nullptr,
                     nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Reminders", RRF_RT_REG_MULTI_SZ, nullptr,
                     buf.data(), &size) != ERROR_SUCCESS)
        return;
    ULONGLONG now = WallSeconds();
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t bar = line.find(L'|');
        if (bar == std::wstring::npos) continue;
        ULONGLONG at = wcstoull(line.substr(0, bar).c_str(), nullptr, 10);
        if (at + 3600 < now) continue;
        reminders_.push_back({at, line.substr(bar + 1)});
    }
}

void Island::SaveReminders() {
    std::wstring data;
    for (const auto& r : reminders_) {
        data += std::to_wstring(r.at) + L"|" + r.text;
        data.push_back(L'\0');
    }
    data.push_back(L'\0');
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Reminders", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                       static_cast<DWORD>(data.size() * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

void Island::DrawCommand(float w, float h) {
    (void)h;
    const float bar = kCommandBar;
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 32, bar / 2, 0.8f, 0.f);

    const float left = 62.f, right = w - 150.f;
    if (input_.empty()) {
        Text(clips_.empty() ? L"Yaz: 12*4 · 10 inç cm · 30 dk çamaşır"
                            : L"Yaz ya da panodan seç (↑ ↓, Enter)",
             fArtist_.Get(), D2D1::RectF(left, 0, w - 18, bar), kGray);
        brush_->SetColor(WithAlpha(kWhite, 0.85f));
        rt_->FillRectangle(D2D1::RectF(left, bar / 2 - 9, left + 1.6f, bar / 2 + 9), brush_.Get());
    } else {
        // Yazı uzarsa sona kaydır: imleç hep görünür.
        ComPtr<IDWriteTextLayout> layout;
        dwrite_->CreateTextLayout(input_.c_str(), static_cast<UINT32>(input_.size()), fTitle_.Get(),
                                  4000.f, bar, &layout);
        if (layout) {
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            float tw = m.widthIncludingTrailingWhitespace;
            float x = tw > right - left - 4 ? right - 4 - tw : left;
            rt_->PushAxisAlignedClip(D2D1::RectF(left, 0, right, bar), D2D1_ANTIALIAS_MODE_ALIASED);
            brush_->SetColor(kWhite);
            rt_->DrawTextLayout(D2D1::Point2F(x, 0), layout.Get(), brush_.Get());
            rt_->PopAxisAlignedClip();
            brush_->SetColor(WithAlpha(kWhite, 0.85f));
            float cx = std::min(right - 2, x + tw + 1.5f);
            rt_->FillRectangle(D2D1::RectF(cx, bar / 2 - 9, cx + 1.6f, bar / 2 + 9), brush_.Get());
        }
        D2D1_COLOR_F c = kGray;
        switch (cmdResult_.kind) {
        case Command::Result::Kind::Value: c = accent_; break;
        case Command::Result::Kind::Reminder: c = kOrange; break;
        default: break;
        }
        Text(cmdResult_.display.c_str(), fAlertRight_.Get(), D2D1::RectF(right, 0, w - 18, bar), c);
    }

    int n = VisibleClips();
    if (!n) return;
    brush_->SetColor(Rgba(1, 1, 1, 0.08f));
    rt_->FillRectangle(D2D1::RectF(18, bar, w - 18, bar + 1), brush_.Get());
    // Seçim kutusu satırdan satıra süzülür.
    selY_.target = bar + 4 + clipSel_ * kCommandRow;
    if (std::fabs(selY_.target - selY_.value) > 0.3f && !drawingOld_) StartAnimation();
    float sy = std::clamp(selY_.value, bar + 4, bar + 4 + (n - 1) * kCommandRow);
    brush_->SetColor(Rgba(1, 1, 1, 0.10f));
    D2D1_ROUNDED_RECT sel{D2D1::RectF(10, sy + 1, w - 10, sy + kCommandRow - 1), 9, 9};
    rt_->FillRoundedRectangle(sel, brush_.Get());
    for (int i = 0; i < n; ++i) {
        float y = bar + 4 + i * kCommandRow;
        float st = Stagger(1 + i);  // satırlar sırayla kayarak gelir
        PushFade(st, 0, -8.f * (1 - st));
        std::wstring t = clips_[i].substr(0, 160);
        for (auto& ch : t) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        Text(t.c_str(), fArtist_.Get(), D2D1::RectF(20, y, w - 20, y + kCommandRow),
             i == clipSel_ ? kWhite : kGray);
        PopFade();
    }
}

void Island::OnLookTimer() {
    if (hidden_) return;
    double now = Now();
    POINT pt;
    GetCursorPos(&pt);
    float cx = (monitor_.left + monitor_.right) / 2.f;
    float cy = monitor_.top + (kTopMargin + height_.value / 2.f) * scale_;
    float dx = pt.x - cx, dy = pt.y - cy;
    float reach = 520.f * scale_;
    bool isNear = dx * dx + dy * dy < reach * reach;

    static POINT last{};
    bool moved = pt.x != last.x || pt.y != last.y;
    last = pt;

    if (isNear && moved) {
        if (eyes_.CurrentMood() == Eyes::Mood::Sleepy) {
            eyes_.SetMood(Eyes::Mood::Surprised, now, 0.7);  // uyandı!
        }
        lastActivity_ = now;
    }
    if (snapshot_.playing) lastActivity_ = now;

    // Kimse yoksa uyur: gündüz 90 sn, gece yarısından sonra 30 sn.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    double sleepAfter = lt.wHour < 6 ? 30 : 90;
    if (coffee_ && eyes_.CurrentMood() == Eyes::Mood::Sleepy) eyes_.SetMood(Eyes::Mood::Normal, now, 0);
    if (!coffee_ && now - lastActivity_ > sleepAfter && eyes_.CurrentMood() == Eyes::Mood::Normal) {
        eyes_.SetMood(Eyes::Mood::Sleepy, now, 0);
    }

    // Fare adanın üstünde biraz durursa açılır; hızlıca geçip tıklayınca açılmaz.
    float halfW = width_.value * scale_ / 2.f, top = monitor_.top + kTopMargin * scale_;
    float pad = (hover_ ? 10.f : 0.f) * scale_;
    bool over = pt.x >= cx - halfW - pad && pt.x <= cx + halfW + pad && pt.y >= monitor_.top &&
                pt.y <= top + height_.value * scale_ + pad;
    bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);

    // Dosya rafı: başka yerde basılıp adaya doğru sürükleniyorsa ada bırakmaya hazırlanır.
    // (Adanın üstünde basılan tık ise alttaki sekmeye aittir, ona dokunma.)
    bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (leftDown && !lastLeftDown_) downStartedOver_ = over;
    lastLeftDown_ = leftDown;
    float dropPad = 36.f * scale_;
    bool nearDrop = pt.x >= cx - halfW - dropPad && pt.x <= cx + halfW + dropPad && pt.y >= monitor_.top &&
                    pt.y <= top + height_.value * scale_ + dropPad;
    bool arm = leftDown && !downStartedOver_ && nearDrop && !Typing() && !ink_ && sliderDrag_ < 0;
    if (arm != dropArmed_ && !dropHover_) {
        dropArmed_ = arm;
        Update();
    }
    // Fare sadece üstünden geçiyorsa (sekmelere gidiyorsa) açılma; durup bekleyince aç.
    bool compact = view_ == View::Idle || view_ == View::Media || view_ == View::Timer;
    if (over && !hover_ && moved) lastMoveOver_ = now;
    float ghostWant = (over && !hover_ && compact && now - lastMoveOver_ < 0.25) ? 0.2f : 1.f;
    if (ghost_.target != ghostWant) {
        ghost_.target = ghostWant;
        StartAnimation();
    }
    if (!hover_) {
        if (over && !buttonDown) {
            float mx = static_cast<float>(pt.x - hoverAnchor_.x), my = static_cast<float>(pt.y - hoverAnchor_.y);
            bool drifted = mx * mx + my * my > (7.f * scale_) * (7.f * scale_);
            if (hoverSince_ == 0) {
                hoverSince_ = now;
                hoverAnchor_ = pt;
                if (eyes_.CurrentMood() == Eyes::Mood::Normal || eyes_.CurrentMood() == Eyes::Mood::Sleepy)
                    eyes_.SetMood(Eyes::Mood::Surprised, now, 0.45);
            } else if (drifted) {
                hoverSince_ = now;  // hâlâ hareket ediyor: beklemeyi baştan say
                hoverAnchor_ = pt;
            } else if (now - hoverSince_ >= (slim_ ? 0.55 : 0.45)) {
                SetHover(true);
                if (!snapshot_.hasSession) eyes_.SetMood(Eyes::Mood::Cute, now, 1.2);  // "merhaba!"
                hoverSince_ = 0;
                outsideSince_ = 0;
                Update();
            }
        } else {
            hoverSince_ = 0;
        }
    } else {
        if (over || sliderDrag_ >= 0) {
            outsideSince_ = 0;
        } else if (outsideSince_ == 0) {
            outsideSince_ = now;
        } else if (now - outsideSince_ >= 0.35) {
            SetHover(false);
            outsideSince_ = 0;
            Update();
        }
    }

    if (commandMode_) eyes_.LookAt(0.7f, 0.15f);  // yazdıklarına bakar
    else if (noteMode_) eyes_.LookAt(0.35f, 0.7f);  // nota bakar
    else eyes_.LookAt(dx / (700.f * scale_), dy / (380.f * scale_));
    if (eyes_.Tick(now) && ViewShowsEyes()) StartAnimation();
}

// ---------------------------------------------------------------------------
// Tepsi simgesi
// ---------------------------------------------------------------------------

void Island::UpdatePerf() {
    // Kendi işlemci kullanımımız: son saniyelerin yumuşatılmış ortalaması (tüm çekirdeklere göre).
    FILETIME c, e, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return;
    GetSystemTimeAsFileTime(&now);
    auto q = [](const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    ULONGLONG proc = q(k) + q(u), wall = q(now);
    if (lastWallTime_ && wall > lastWallTime_) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        float pct = 100.f * static_cast<float>(proc - lastProcTime_) /
                    static_cast<float>((wall - lastWallTime_) * std::max<DWORD>(1, si.dwNumberOfProcessors));
        cpuPct_ += (pct - cpuPct_) * 0.2f;
    }
    lastProcTime_ = proc;
    lastWallTime_ = wall;
}

void Island::ToggleDock() {
    if (dock_.Enabled()) {
        dock_.Disable();
        WriteSetting(L"Dock", 0);
    } else if (dock_.Enable(instance_, d2d_.Get(), dwrite_.Get())) {
        WriteSetting(L"Dock", 1);
        ApplyEyeColor();
    }
}

void Island::AddTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    swprintf_s(nid.szTip, L"%s · Dynamis Island", name_.c_str());
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Island::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    wchar_t title[128];
    swprintf_s(title, L"%s  v%d   ·   dostluk %d/100", name_.c_str(), Updater::kCurrentVersion,
               companion_.Affection());
    if (btLevel_ != 255) {
        wchar_t extra[48];
        swprintf_s(extra, L"   ·   kulaklık %%%d", btLevel_);
        wcscat_s(title, extra);
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title);
    {
        PROCESS_MEMORY_COUNTERS_EX pmc{sizeof(pmc)};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        wchar_t perf[128];
        swprintf_s(perf, L"RAM %.0f MB (özel %.0f MB)  ·  CPU %%%.1f  ·  %s",
                   pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0, cpuPct_,
                   gpu_ ? L"ekran kartı" : L"yazılım çizimi");
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, perf);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdToggle,
                userHidden_ ? L"Adayı göster\tCtrl+Alt+D" : L"Adayı gizle\tCtrl+Alt+D");

    HMENU settings = CreatePopupMenu();
    AppendMenuW(settings, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), kCmdAutostart,
                L"Windows ile başlat");
    wchar_t updateLabel[64];
    if (availableVersion_ > Updater::kCurrentVersion)
        swprintf_s(updateLabel, L"Güncelleştir (v%d hazır)", availableVersion_);
    else
        wcscpy_s(updateLabel, L"Güncelleştir");
    AppendMenuW(settings, MF_STRING | (Sounds::Enabled() ? MF_CHECKED : 0), kCmdSounds, L"Sesler");
    AppendMenuW(settings, MF_STRING | (volumeOsd_ ? MF_CHECKED : 0), kCmdVolumeOsd, L"Ses düzeyi göstergesi");
    AppendMenuW(settings, MF_STRING | (homeOn_ ? MF_CHECKED : 0), kCmdHome, L"Ana sayfa");
    AppendMenuW(settings, MF_STRING | (albumTint_ ? MF_CHECKED : 0), kCmdAlbumTint, L"Albüm rengi");
    AppendMenuW(settings, MF_STRING | (albumGlow_ ? MF_CHECKED : 0), kCmdAlbumGlow, L"Albüm ışığı (kapalı ada)");
    AppendMenuW(settings, MF_STRING | (accessories_ ? MF_CHECKED : 0), kCmdAccessories, L"Aksesuarlar");
    AppendMenuW(settings, MF_STRING | (weatherOn_ ? MF_CHECKED : 0), kCmdWeather, L"Hava durumu");
    {
        wchar_t cityLabel[128];
        std::wstring shown = !city_.empty() ? city_ : (weather_.ok && !weather_.city.empty() ? weather_.city + L" (otomatik)" : L"otomatik");
        swprintf_s(cityLabel, L"Şehir: %s (değiştir)…", shown.c_str());
        AppendMenuW(settings, MF_STRING | (weatherOn_ ? 0 : MF_GRAYED), kCmdCity, cityLabel);
    }
    AppendMenuW(settings, MF_STRING | (lyricsOn_ ? MF_CHECKED : 0), kCmdLyrics, L"Şarkı sözleri (internetten)");
    AppendMenuW(settings, MF_STRING | (lyricsPill_ ? MF_CHECKED : 0) | (lyricsOn_ ? 0 : MF_GRAYED), kCmdLyricsPill,
                L"Kapalı adada şarkı sözü");
    {
        HMENU eyesMenu = CreatePopupMenu();
        for (int i = 0; i < static_cast<int>(std::size(kEyePresets)); ++i) {
            bool on = i == 0 ? eyeMode_ == 0 : (eyeMode_ == 1 && eyeRgb_ == kEyePresets[i].rgb);
            AppendMenuW(eyesMenu, MF_STRING | (on ? MF_CHECKED : 0), kCmdEyeWhite + i, kEyePresets[i].name);
        }
        AppendMenuW(eyesMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(eyesMenu, MF_STRING | (eyeMode_ == 2 ? MF_CHECKED : 0), kCmdEyeAccent, L"Windows vurgu rengine uy");
        AppendMenuW(eyesMenu, MF_STRING, kCmdEyePick, L"Ekrandan seç…");
        AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(eyesMenu), L"Göz rengi");
    }
    {
        wchar_t renameLabel[64];
        swprintf_s(renameLabel, L"Adını değiştir (%s)…", name_.c_str());
        AppendMenuW(settings, MF_STRING, kCmdRename, renameLabel);
    }
    {
        int mode = NotifyMirror::Mode();
        const wchar_t* how = mode == 1 ? L"" : (mode == 2 ? L"  (pencere izleme)" : (mode == 3 ? L"  (çalışmıyor)" : L""));
        wchar_t label[96];
        swprintf_s(label, L"Bildirim aynası%s", how);
        AppendMenuW(settings, MF_STRING | (notifyOn_ ? MF_CHECKED : 0), kCmdNotify, label);
    }
    AppendMenuW(settings, MF_STRING, kCmdLikes, L"Beğenilen şarkılar…");
    AppendMenuW(settings, MF_STRING, kCmdUpdate, updateLabel);
    bool focusing = companion_.Kind() == Companion::TimerKind::Focus ||
                    companion_.Kind() == Companion::TimerKind::Break;
    wchar_t focusLabel[64];
    swprintf_s(focusLabel, L"Odak modu başlat (%d dk)", Companion::kFocusMinutes);
    AppendMenuW(menu, MF_STRING, kCmdCommand, L"Hesap · çevir · hatırlat · pano\tCtrl+Alt+Boşluk");
    AppendMenuW(menu, MF_STRING, kCmdShot, L"Ekran görüntüsü al\tCtrl+Alt+S");
    AppendMenuW(menu, MF_STRING, kCmdPicker, L"Ekrandan renk seç\tCtrl+Alt+C");
    AppendMenuW(menu, MF_STRING, kCmdOcr, L"Ekrandan yazı kopyala\tCtrl+Alt+O");
    AppendMenuW(menu, MF_STRING, kCmdNote, L"Not\tCtrl+Alt+N");
    AppendMenuW(menu, MF_STRING | (ink_ ? MF_CHECKED : 0), kCmdInk, L"Ekrana çiz\tCtrl+Alt+P");
    AppendMenuW(menu, MF_STRING | (coffee_ ? MF_CHECKED : 0), kCmdCoffee, L"Uyanık tut (kahve)");
    {
        wchar_t sleepLabel[64];
        if (sleepEnd_ > 0)
            swprintf_s(sleepLabel, L"Uyku zamanlayıcısı: %d dk kaldı (değiştir)",
                       static_cast<int>(std::ceil(std::max(0.0, sleepEnd_ - Now()) / 60.0)));
        else
            wcscpy_s(sleepLabel, L"Müzik uyku zamanlayıcısı (15 dk)");
        AppendMenuW(menu, MF_STRING | (sleepEnd_ > 0 ? MF_CHECKED : 0), kCmdSleep, sleepLabel);
    }
    if (!shelf_.empty()) AppendMenuW(menu, MF_STRING, kCmdClearShelf, L"Rafı boşalt");
    AppendMenuW(menu, MF_STRING, kCmdFocus, focusing ? L"Odak modunu bitir" : focusLabel);
    if (companion_.Kind() == Companion::TimerKind::Quick)
        AppendMenuW(menu, MF_STRING, kCmdCancelTimer, L"Zamanlayıcıyı iptal et");
    AppendMenuW(menu, MF_STRING | (dock_.Enabled() ? MF_CHECKED : 0), kCmdDock,
                L"Guti Dock (yeni taskbar)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"Ayarlar");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Çıkış");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0,
                              hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == kCmdAutostart) SetAutostart(!IsAutostartEnabled());
    if (cmd == kCmdToggle) ToggleUserHidden();
    if (cmd == kCmdDock) ToggleDock();
    if (cmd == kCmdSounds) ToggleSounds();
    if (cmd == kCmdFocus) ToggleFocus();
    if (cmd == kCmdOcr) StartOcr();
    if (cmd == kCmdNote) OpenNote();
    if (cmd == kCmdInk) {
        if (ink_) EndInk();
        else StartInk();
    }
    if (cmd == kCmdCoffee) SetCoffee(!coffee_);
    if (cmd == kCmdCancelTimer) {
        companion_.CancelTimer();
        Update();
    }
    if (cmd == kCmdUpdate) StartUpdate();
    if (cmd == kCmdShot) TakeScreenshot();
    if (cmd == kCmdPicker) StartPicker();
    if (cmd == kCmdVolumeOsd) {
        volumeOsd_ = !volumeOsd_;
        WriteSetting(L"VolumeOsd", volumeOsd_ ? 1 : 0);
    }
    if (cmd == kCmdClearShelf) {
        shelf_.clear();
        Update();
    }
    if (cmd == kCmdCommand) {
        if (userHidden_) ToggleUserHidden();
        OpenCommand();
    }
    if (cmd == kCmdNotify) {
        notifyOn_ = !notifyOn_;
        WriteSetting(L"NotifyMirror", notifyOn_ ? 1 : 0);
        NotifyMirror::SetEnabled(notifyOn_);
    }
    if (cmd == kCmdLikes) {
        std::wstring path = Net::DataDir() + L"begeniler.txt";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) Net::WriteFile(path, "");
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (cmd == kCmdHome) {
        homeOn_ = !homeOn_;
        WriteSetting(L"Home", homeOn_ ? 1 : 0);
        Update();
    }
    if (cmd == kCmdAlbumGlow) {
        albumGlow_ = !albumGlow_;
        WriteSetting(L"AlbumGlow", albumGlow_ ? 1 : 0);
        StartAnimation();
    }
    if (cmd == kCmdAccessories) {
        accessories_ = !accessories_;
        WriteSetting(L"Accessories", accessories_ ? 1 : 0);
        StartAnimation();
    }
    if (cmd == kCmdWeather) {
        weatherOn_ = !weatherOn_;
        WriteSetting(L"Weather", weatherOn_ ? 1 : 0);
        if (weatherOn_) RefreshWeather();
        else weather_ = {};
        StartAnimation();
    }
    if (cmd == kCmdCity) {
        if (userHidden_) ToggleUserHidden();
        OpenCommand();
        input_ = L"şehir ";
        RefreshCommandResult();
    }
    if (cmd == kCmdAlbumTint) {
        albumTint_ = !albumTint_;
        WriteSetting(L"AlbumTint", albumTint_ ? 1 : 0);
        StartAnimation();
    }
    if (cmd == kCmdLyrics) {
        lyricsOn_ = !lyricsOn_;
        WriteSetting(L"Lyrics", lyricsOn_ ? 1 : 0);
        lyricsKey_.clear();  // açıldıysa şimdiki şarkı için hemen ara
        if (lyricsOn_) OnMediaForLyrics();
        else lyrics_.clear();
        Update();
    }
    if (cmd == kCmdLyricsPill) {
        lyricsPill_ = !lyricsPill_;
        WriteSetting(L"LyricsPill", lyricsPill_ ? 1 : 0);
        Update();
    }
    if (cmd == kCmdSleep) CycleSleep();
    if (cmd >= kCmdEyeWhite && cmd < kCmdEyeWhite + std::size(kEyePresets)) {
        size_t i = cmd - kCmdEyeWhite;
        SetEyeMode(i == 0 ? 0 : 1, kEyePresets[i].rgb);
    }
    if (cmd == kCmdEyeAccent) SetEyeMode(2, eyeRgb_);
    if (cmd == kCmdEyePick) {
        pickForEyes_ = true;
        StartPicker();
        if (!picker_) pickForEyes_ = false;
    }
    if (cmd == kCmdRename) {
        if (userHidden_) ToggleUserHidden();
        OpenCommand();
        input_ = L"isim ";
        RefreshCommandResult();
    }
    if (cmd == kCmdExit) DestroyWindow(hwnd_);
}

bool Island::IsAutostartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool exists = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void Island::SetAutostart(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------------------
// Pencere prosedürü
// ---------------------------------------------------------------------------

LRESULT CALLBACK Island::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Island* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<Island*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Island*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->WndProc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Island::WndProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        AddTrayIcon();  // Explorer yeniden başladı
        return 0;
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // tıklayınca açık uygulamanın odağını çalma

    case WM_LBUTTONDOWN:
        swipeX0_ = -1;
        if (ToolsShown(view_)) {
            float y = GET_Y_LPARAM(lp) / scale_;
            if (y >= ToolsTop() && y <= ToolsTop() + kToolsH) {
                swipeX0_ = GET_X_LPARAM(lp) / scale_;  // basılı tutup yana çekince sayfa değişir
                swipeMoved_ = false;
            }
        }
        if (view_ == View::ExpandedMedia) {
            float x = GET_X_LPARAM(lp) / scale_ - kPad, y = GET_Y_LPARAM(lp) / scale_;
            int hit = MediaHit(x, y);
            if (hit == 20 || hit == 21) {
                sliderDrag_ = hit == 20 ? 2 : 3;
                seekPreview_ = snapshot_.CurrentPosition();
                SetCapture(hwnd_);
                MediaSlider(sliderDrag_, x);
                return 0;
            }
        }
        if (view_ == View::Quick) {
            float x = GET_X_LPARAM(lp) / scale_ - kPad, y = GET_Y_LPARAM(lp) / scale_;
            int hit = QuickHit(x, y);
            if (hit == 10 || hit == 11) {
                // Sürgüyü tut ve kaydır: fare adadan çıksa da bırakana kadar takip edilir.
                sliderDrag_ = hit - 10;
                SetCapture(hwnd_);
                QuickSlider(sliderDrag_, x);
                return 0;
            }
        }
        if (IsFileCard()) {
            // Basılı tutup çekersen dosya sürüklenir; sadece tıklarsan açılır.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &pt);
            swallowUpUntil_ = Now() + 1.0;
            if (DragDetect(hwnd_, pt)) StartCardDrag();
            else OpenCard();
            return 0;
        }
        if (int b = ButtonAt(GET_X_LPARAM(lp) / scale_ - kPad, GET_Y_LPARAM(lp) / scale_); b >= 0) {
            pressBtn_ = b;
            btnScale_[b].target = 0.8f;  // basınca hafifçe çöker
            StartAnimation();
            return 0;
        }
        if (int hit = ShelfHit(GET_X_LPARAM(lp) / scale_ - kPad, GET_Y_LPARAM(lp) / scale_); hit != -1) {
            swallowUpUntil_ = Now() + 1.0;
            if (hit == -2) {
                shelf_.clear();  // rafı boşalt
                MoodWithSound(Eyes::Mood::Wink, 0.8);
                Update();
                return 0;
            }
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &pt);
            std::wstring path = shelf_[static_cast<size_t>(hit)].path;
            if (DragDetect(hwnd_, pt)) {
                // Raftan çekip bırakınca dosya raftan iner.
                if (DragFiles({path})) {
                    auto it = std::find_if(shelf_.begin(), shelf_.end(),
                                           [&](const ShelfItem& s) { return s.path == path; });
                    if (it != shelf_.end()) shelf_.erase(it);
                }
            } else {
                AllowSetForegroundWindow(ASFW_ANY);
                ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            Update();
        }
        return 0;

    case WM_LBUTTONUP:
        if (sliderDrag_ >= 0) {
            if (sliderDrag_ == 2 && snapshot_.durationSec > 0) {
                // Bırakınca şarkı oraya sarılır; yeni konum gelene kadar çubuk yerinde dursun.
                media_.Seek(seekPreview_);
                snapshot_.positionSec = seekPreview_;
                snapshot_.stamp = std::chrono::steady_clock::now();
                eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.6);
            }
            sliderDrag_ = -1;
            ReleaseCapture();
            StartAnimation();
            return 0;
        }
        if (IsFileCard() || Now() < swallowUpUntil_) {
            swallowUpUntil_ = 0;
            return 0;
        }
        if (swipeX0_ >= 0) {
            float dx = GET_X_LPARAM(lp) / scale_ - swipeX0_;
            bool moved = swipeMoved_;
            swipeX0_ = -1;
            if (moved) {
                if (GetCapture() == hwnd_) ReleaseCapture();
                if (std::fabs(dx) > 28.f) SetToolPage(toolPage_ + (dx < 0 ? 1 : -1));
                else SetToolPage(toolPage_);  // az çekildi: yerine yaylanır
                return 0;
            }
        }
        if (pressBtn_ >= 0) {
            btnScale_[pressBtn_].target = 1.f;
            btnScale_[pressBtn_].velocity += 3.f;  // bırakınca yaylanarak geri gelir
            pressBtn_ = -1;
            StartAnimation();
        }
        OnClick(GET_X_LPARAM(lp) / scale_ - kPad, GET_Y_LPARAM(lp) / scale_);
        return 0;

    case WM_MOUSEMOVE:
        if (swipeX0_ >= 0 && (wp & MK_LBUTTON)) {
            float dx = GET_X_LPARAM(lp) / scale_ - swipeX0_;
            if (!swipeMoved_ && std::fabs(dx) > 6.f) {
                swipeMoved_ = true;
                SetCapture(hwnd_);
            }
            if (swipeMoved_) {
                // Parmağı takip eder; bırakınca en yakın sayfaya oturur.
                pageX_.target = static_cast<float>(toolPage_) - dx / 380.f;
                StartAnimation();
                return 0;
            }
        }
        OnMouseMove(GET_X_LPARAM(lp) / scale_ - kPad, GET_Y_LPARAM(lp) / scale_);
        return 0;

    case WM_RBUTTONUP:
        // Favori kutucuğuna sağ tık: favoriden çıkar.
        if (ToolsShown(view_)) {
            int t = ToolHit(GET_X_LPARAM(lp) / scale_ - kPad, GET_Y_LPARAM(lp) / scale_);
            if (t >= kFavHit && t < kFavHit + kFavMax) RemoveFavorite(t - kFavHit);
            int hh = HomeHit(GET_X_LPARAM(lp) / scale_ - kPad, GET_Y_LPARAM(lp) / scale_);
            if (hh >= 50 && hh < 54) RemoveFavorite(hh - 50);
        }
        return 0;

    case Lyrics::WM_LYRICS:
        OnLyrics(reinterpret_cast<Lyrics::Result*>(lp));
        return 0;

    case WM_FAVICON: {
        std::unique_ptr<std::wstring> domain(reinterpret_cast<std::wstring*>(lp));
        if (domain) OnFavicon(*domain);
        return 0;
    }

    case NotifyMirror::WM_TOAST: {
        std::unique_ptr<NotifyMirror::Toast> t(reinterpret_cast<NotifyMirror::Toast*>(lp));
        if (!t || !notifyOn_ || fullscreen_ || userHidden_ || Typing() || ink_ || picker_) return 0;
        if (companion_.Kind() == Companion::TimerKind::Focus) {
            eyes_.SetMood(Eyes::Mood::Glance, Now(), 1.0);  // odaktayken rahatsız etmez, sadece bakar
            StartAnimation();
            return 0;
        }
        std::wstring label = t->app.empty() || t->app == t->title ? t->title : t->app + L": " + t->title;
        if (!t->body.empty()) label += L" \u2014 " + t->body;
        for (auto& c : label)
            if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
        alertLabel_ = label;
        ShowAlert(AlertKind::Notification, 0, 4500);
        return 0;
    }

    case Weather::WM_WEATHER:
        OnWeather(reinterpret_cast<Weather::Info*>(lp));
        return 0;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
        if (eyeMode_ == 2) ApplyEyeColor();  // Windows vurgu rengi değişti
        return 0;

    case WM_CAPTURECHANGED:
        if (sliderDrag_ >= 0 && reinterpret_cast<HWND>(lp) != hwnd_) {
            sliderDrag_ = -1;
            StartAnimation();
        }
        return 0;

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        hoverBtn_ = -1;
        hoverTool_ = -1;
        mediaHover_ = -1;
        for (auto& g : btnGlow_) g.target = 0.f;
        for (auto& g : toolGlow_) g.target = 0.f;
        for (auto& g : favGlow_) g.target = 0.f;
        if (pressBtn_ >= 0) btnScale_[pressBtn_].target = 1.f;
        pressBtn_ = -1;
        StartAnimation();
        return 0;

    case kFrameMsg:
        framePending_ = false;
        OnAnimationTick();
        return 0;

    case WM_CHAR:
        if (commandMode_) OnCommandChar(static_cast<wchar_t>(wp));
        else if (noteMode_) OnNoteChar(static_cast<wchar_t>(wp));
        return 0;

    case WM_KEYDOWN:
        if (commandMode_) OnCommandKey(static_cast<UINT>(wp));
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {  // başka yere tıklandı
            CloseCommand(false);
            CloseNote(false);
        }
        break;

    case VolumeWatch::WM_VOLUME_CHANGED:
        OnVolume(static_cast<int>(wp), lp != 0);
        return 0;

    case VolumeWatch::WM_VOLUME_DEVICE:
        VolumeWatch::Rebind();
        return 0;

    case BtBattery::WM_BT_BATTERY: {
        std::unique_ptr<std::wstring> name(reinterpret_cast<std::wstring*>(lp));
        OnBtBattery(static_cast<int>(wp), name ? *name : std::wstring());
        return 0;
    }

    case Ocr::WM_OCR_DONE: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        OnOcrDone(static_cast<int>(static_cast<INT_PTR>(wp)), text ? *text : std::wstring());
        return 0;
    }

    case Radios::WM_RADIOS:
        radios_ = static_cast<int>(wp);
        StartAnimation();
        return 0;

    case Brightness::WM_BRIGHTNESS:
        if (wp == 255) {
            if (brightness_ < 0) brightness_ = 255;  // okunamadı: desteklenmiyor (tek bir yazma hatası değil)
        } else if (sliderDrag_ != 0 && Now() - brightSetAt_ > 1.0) {
            brightness_ = static_cast<int>(wp);
        }
        StartAnimation();
        return 0;

    case WM_CLIPBOARDUPDATE:
        OnClipboardUpdate();
        return 0;

    case DownloadWatch::WM_DOWNLOAD_DONE: {
        std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lp));
        if (path) OnDownload(*path);
        return 0;
    }

    case WM_TIMER:
        switch (wp) {
        case kTimerLook: OnLookTimer(); break;
        case kTimerUpdateCheck:
            SetTimer(hwnd_, kTimerUpdateCheck, 6 * 3600 * 1000, nullptr);
            Updater::CheckAsync(hwnd_, false);
            break;
        case kTimerHover:
            KillTimer(hwnd_, kTimerHover);
            hoverPending_ = false;
            hover_ = true;
            Update();
            break;
        case kTimerCollapse:
            KillTimer(hwnd_, kTimerCollapse);
            hover_ = false;
            Update();
            break;
        case kTimerTint:
            KillTimer(hwnd_, kTimerTint);
            eyes_.SetTint(nullptr);
            StartAnimation();
            break;
        case kTimerLyrics: StartLyricsFetch(); break;
        case kTimerWeather: RefreshWeather(); break;
        case kTimerSleepRestore:
            KillTimer(hwnd_, kTimerSleepRestore);
            if (sleepRestoreVol_ >= 0) {
                volQuietUntil_ = Now() + 1.5;
                VolumeWatch::Set(sleepRestoreVol_);
            }
            sleepRestoreVol_ = -1;
            sleepFading_ = false;
            break;
        case kTimerAlert:
            KillTimer(hwnd_, kTimerAlert);
            if (IsFileCard() && hover_) {
                SetTimer(hwnd_, kTimerAlert, 1500, nullptr);  // üstündeyken bekle
                break;
            }
            alertActive_ = false;
            Update();
            break;
        case kTimerTick: {
            CheckFullscreen();
            if (CheckSlim()) Update();
            OnCompanionTick();
            OnSleepTick();
            OnDiaryTick();
            OnWeatherMood(Now());
            if (healthTick_ % 10 == 0) CheckAccessoryUnlocks();
            OnPrivacyTick();
            CheckReminders();
            UpdatePerf();
            if (++healthTick_ % 2 == 0) OnHealthTick();
            if (!hidden_) {
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                SYSTEMTIME st;
                GetLocalTime(&st);
                bool showsClock = view_ == View::Idle || view_ == View::ExpandedIdle;
                bool showsProgress = view_ == View::ExpandedMedia || view_ == View::Timer ||
                                     (view_ == View::ExpandedIdle && companion_.TimerActive());
                if ((showsClock && st.wMinute != lastMinute_) || showsProgress) {
                    if (!animating_) Render();
                }
                lastMinute_ = st.wMinute;
            }
            break;
        }
        }
        return 0;

    case MediaService::WM_MEDIA_CHANGED:
        snapshot_ = media_.Snapshot();
        if (snapshot_.artVersion != artVersion_) {
            artVersion_ = snapshot_.artVersion;
            artBitmap_.Reset();
            SetAccent(AccentFromArt(snapshot_.art.Get()));
        }
        OnMediaForLyrics();
        if (snapshot_.title != lastTitle_) {
            if (!lastTitle_.empty() || !snapshot_.title.empty()) {
                eyes_.SetMood(Eyes::Mood::Surprised, Now(), 0.6);  // yeni şarkı!
                lastActivity_ = Now();
            }
            lastTitle_ = snapshot_.title;
        } else if (snapshot_.hasSession && snapshot_.playing != lastPlaying_) {
            // Müzik başlayınca sevinir, durunca biraz üzülür.
            eyes_.SetMood(snapshot_.playing ? Eyes::Mood::Happy : Eyes::Mood::Sad, Now(),
                          snapshot_.playing ? 1.4 : 1.6);
            lastActivity_ = Now();
        }
        lastPlaying_ = snapshot_.playing;
        Update();
        return 0;

    case WM_POWERBROADCAST:
        if (wp == PBT_APMPOWERSTATUSCHANGE) OnPowerChanged(false);
        return TRUE;

    case WM_DPICHANGED:
        UpdateDpi(HIWORD(wp));
        Render();
        return 0;

    case WM_DISPLAYCHANGE:
        UpdateMonitor();
        Render();
        return 0;

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) {
            UpdateMonitor();
            Render();
        }
        return 0;

    case kTrayMsg:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
        if (LOWORD(lp) == WM_LBUTTONUP) ToggleUserHidden();
        return 0;

    case WM_MOUSEHWHEEL:
        // Dokunmatik yüzeyde yana kaydırma: araç sayfaları
        if (ToolsShown(view_)) PageWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;

    case WM_MOUSEWHEEL: {
        // Açık adanın üstünde teker: zamanlayıcı süresi (Ctrl ile 5'er dakika).
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (notches == 0) notches = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        if (commandMode_) {
            OnCommandKey(notches > 0 ? VK_UP : VK_DOWN);
            return 0;
        }
        {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            float x = pt.x / scale_ - kPad, y = pt.y / scale_;
            if (ToolsShown(view_) && y >= ToolsTop() && y <= ToolsTop() + kToolsH) {
                PageWheel(-GET_WHEEL_DELTA_WPARAM(wp));  // araç sırasında teker: sayfa çevir
                return 0;
            }
            if (view_ == View::Quick) {
                int hit = QuickHit(x, y);
                if (hit == 10 && brightness_ >= 0 && brightness_ <= 100) {
                    brightness_ = std::clamp(brightness_ + notches * 5, 0, 100);
                    brightSetAt_ = Now();
                    Brightness::Set(hwnd_, brightness_);
                } else if (hit == 11 && volume_ >= 0) {
                    volume_ = std::clamp(volume_ + notches * 2, 0, 100);
                    muted_ = false;
                    VolumeWatch::Set(volume_);
                }
                StartAnimation();
                return 0;
            }
            if (view_ == View::Note || view_ == View::Ink) return 0;
            if (view_ == View::ExpandedMedia) {
                int hit = MediaHit(x, y);
                if (hit == 20 && snapshot_.durationSec > 0) {  // teker: 5 sn ileri / geri
                    double p = std::clamp(snapshot_.CurrentPosition() + notches * 5.0, 0.0, snapshot_.durationSec);
                    media_.Seek(p);
                    snapshot_.positionSec = p;
                    snapshot_.stamp = std::chrono::steady_clock::now();
                    StartAnimation();
                    return 0;
                }
                if (hit == 21 && volume_ >= 0) {
                    volume_ = std::clamp(volume_ + notches * 2, 0, 100);
                    muted_ = false;
                    VolumeWatch::Set(volume_);
                    StartAnimation();
                    return 0;
                }
            }
        }
        int step = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) ? 5 : 1;
        companion_.AdjustPending(notches * step, Now());
        eyes_.SetMood(Eyes::Mood::Curious, Now(), 1.0);
        Sounds::Play(Sounds::Sound::Close);
        Update();
        if (!animating_) Render();
        return 0;
    }

    case WM_HOTKEY:
        if (wp == kHotkeyToggle) ToggleUserHidden();
        if (wp == kHotkeyShot) TakeScreenshot();
        if (wp == kHotkeyPicker) StartPicker();
        if (wp == kHotkeyOcr) StartOcr();
        if (wp == kHotkeyNote) {
            if (noteMode_) CloseNote(true);
            else OpenNote();
        }
        if (wp == kHotkeyInk) {
            if (ink_) EndInk();
            else StartInk();
        }
        if (wp == kHotkeyCommand) {
            if (commandMode_) {
                CloseCommand(true);
            } else {
                if (userHidden_) ToggleUserHidden();
                OpenCommand();
            }
        }
        return 0;

    case Updater::WM_UPDATE_RESULT: {
        auto result = static_cast<Updater::Result>(wp);
        int version = static_cast<int>(lp);
        bool userAsked = alertActive_ && alertKind_ == AlertKind::Updating;
        switch (result) {
        case Updater::Result::Installed:
            if (Updater::RelaunchUpdated()) {
                DestroyWindow(hwnd_);  // yeni sürüm açılıyor
            } else {
                ShowAlert(AlertKind::UpdateFailed, 8, 5000);
            }
            break;
        case Updater::Result::Available:
            if (userAsked) {
                // Arka plandaki kontrol, kullanıcının isteğiyle çakıştı: şimdi kur.
                availableVersion_ = version;
                Updater::CheckAsync(hwnd_, true);
            } else if (version != availableVersion_) {
                availableVersion_ = version;
                ShowAlert(AlertKind::UpdateAvailable, version, 4500);
            }
            break;
        case Updater::Result::UpToDate:
            if (userAsked) ShowAlert(AlertKind::UpToDate, Updater::kCurrentVersion, 3000);
            break;
        case Updater::Result::Failed:
            if (userAsked) ShowAlert(AlertKind::UpdateFailed, version, 5000);  // version = hata adımı
            break;
        }
        return 0;
    }

    case WM_ENDSESSION:
        if (wp) dock_.Disable();  // oturum kapanıyor: Windows taskbar'ı ayarını geri koy
        return 0;
    case WM_DESTROY: {
        dock_.Disable();
        companion_.Save();
        SaveDiary();
        if (sleepFading_ && sleepRestoreVol_ >= 0) VolumeWatch::Set(sleepRestoreVol_);
        if (noteMode_) SaveNote();
        if (ink_) EndInk();
        if (coffee_) SetThreadExecutionState(ES_CONTINUOUS);
        RemoveClipboardFormatListener(hwnd_);
        UnregisterHotKey(hwnd_, kHotkeyToggle);
        UnregisterHotKey(hwnd_, kHotkeyCommand);
        UnregisterHotKey(hwnd_, kHotkeyShot);
        UnregisterHotKey(hwnd_, kHotkeyPicker);
        UnregisterHotKey(hwnd_, kHotkeyOcr);
        UnregisterHotKey(hwnd_, kHotkeyNote);
        UnregisterHotKey(hwnd_, kHotkeyInk);
        RevokeDragDrop(hwnd_);
        if (dropTarget_) dropTarget_->Release();
        dropTarget_ = nullptr;
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
