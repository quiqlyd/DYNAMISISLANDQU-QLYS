#include "Island.h"
#include "Sounds.h"
#include "Updater.h"

#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlguid.h>
#include <algorithm>
#include <memory>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT_PTR kTimerAnim = 1;
constexpr UINT_PTR kTimerTick = 2;
constexpr UINT_PTR kTimerHover = 3;
constexpr UINT_PTR kTimerCollapse = 4;
constexpr UINT_PTR kTimerAlert = 5;
constexpr UINT_PTR kTimerLook = 6;
constexpr UINT_PTR kTimerUpdateCheck = 7;
constexpr int kHotkeyToggle = 1;
constexpr int kHotkeyCommand = 2;
constexpr int kHotkeyShot = 3;

constexpr UINT kTrayMsg = WM_APP + 2;
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
constexpr wchar_t kSettingsKey[] = L"Software\\DynamisIsland";

constexpr float kTopMargin = 4.f;
constexpr float kMaxW = 420.f, kMaxH = 230.f;

// Görünüm boyutları (DIP)
struct Size3 { float w, h, r; };
constexpr Size3 kIdle{78, 22, 11};
constexpr Size3 kMedia{236, 34, 17};
constexpr Size3 kExpandedMedia{400, 164, 40};
constexpr Size3 kExpandedIdle{320, 96, 36};
constexpr Size3 kAlert{300, 40, 20};
constexpr Size3 kTimer{128, 24, 12};
constexpr Size3 kDownload{340, 46, 23};
constexpr Size3 kShot{360, 92, 28};
constexpr Size3 kDrop{260, 46, 23};
constexpr float kShelfH = 58.f, kShelfCell = 58.f;
constexpr int kShelfMax = 8, kShelfShown = 5;
constexpr float kCommandW = 420.f, kCommandRow = 26.f, kCommandBar = 48.f;

// Segoe Fluent Icons
constexpr wchar_t kGlyphPrev[] = L"\xE892";
constexpr wchar_t kGlyphNext[] = L"\xE893";
constexpr wchar_t kGlyphPlay[] = L"\xE768";
constexpr wchar_t kGlyphPause[] = L"\xE769";
constexpr wchar_t kGlyphMusic[] = L"\xE8D6";
constexpr wchar_t kGlyphBolt[] = L"\xE945";
constexpr wchar_t kGlyphSync[] = L"\xE895";
constexpr wchar_t kGlyphCheck[] = L"\xE73E";
constexpr wchar_t kGlyphError[] = L"\xE783";
constexpr wchar_t kGlyphTimer[] = L"\xE916";
constexpr wchar_t kGlyphDownload[] = L"\xE896";
constexpr wchar_t kGlyphWarning[] = L"\xE7BA";
constexpr wchar_t kGlyphGlobe[] = L"\xE774";
constexpr wchar_t kGlyphClose[] = L"\xE711";

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DynamisIsland";

D2D1_COLOR_F Rgba(float r, float g, float b, float a = 1.f) { return D2D1::ColorF(r, g, b, a); }
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

const D2D1_COLOR_F kWhite = Rgba(1, 1, 1);
const D2D1_COLOR_F kGray = Rgba(1, 1, 1, 0.55f);
const D2D1_COLOR_F kGreen = Rgba(0.30f, 0.85f, 0.42f);
const D2D1_COLOR_F kRed = Rgba(1.f, 0.30f, 0.27f);
const D2D1_COLOR_F kOrange = Rgba(1.f, 0.62f, 0.25f);

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

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

void WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
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
        *effect = hasFiles_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;  // kopya: kaynak dosya silinmez
        if (hasFiles_) island_->OnDropEnter();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        *effect = hasFiles_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (hasFiles_) island_->OnDropLeave();
        hasFiles_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        *effect = DROPEFFECT_NONE;
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
    Island* island_;
    ULONG refs_ = 1;
    bool hasFiles_ = false;
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

    SetTimer(hwnd_, kTimerTick, 1000, nullptr);
    SetTimer(hwnd_, kTimerLook, 100, nullptr);
    SetTimer(hwnd_, kTimerUpdateCheck, 20 * 1000, nullptr);  // açılıştan 20 sn sonra, sonra 6 saatte bir
    RegisterHotKey(hwnd_, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
    RegisterHotKey(hwnd_, kHotkeyCommand, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    RegisterHotKey(hwnd_, kHotkeyShot, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
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
    int needW = static_cast<int>(std::ceil(kMaxW * newScale)) + 2;
    int needH = static_cast<int>(std::ceil(kMaxH * newScale)) + 2;
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
    if (dropHover_) return View::Drop;
    if (alertActive_) return View::Alert;
    bool timer = companion_.TimerActive() || companion_.Pending();
    if (hover_) {
        if (companion_.Pending()) return View::ExpandedIdle;  // teker çevrilirken süreyi göster
        return snapshot_.hasSession ? View::ExpandedMedia : View::ExpandedIdle;
    }
    if (timer) return View::Timer;
    return snapshot_.hasSession ? View::Media : View::Idle;
}

void Island::SetTargets(View v) {
    Size3 s = kIdle;
    switch (v) {
    case View::Idle: s = kIdle; break;
    case View::Media: s = kMedia; break;
    case View::ExpandedMedia: s = kExpandedMedia; break;
    case View::ExpandedIdle: s = kExpandedIdle; break;
    case View::Alert:
        s = !IsFileCard() ? kAlert : (alertKind_ == AlertKind::Screenshot ? kShot : kDownload);
        break;
    case View::Timer: s = kTimer; break;
    case View::Command: s = {kCommandW, CommandHeight(), 24}; break;
    case View::Drop: s = kDrop; break;
    }
    if ((v == View::ExpandedIdle || v == View::ExpandedMedia) && ShelfVisible()) s.h += kShelfH;
    width_.target = s.w;
    height_.target = s.h;
    radius_.target = s.r;
}

void Island::Update() {
    View v = TargetView();
    if (v != view_) {
        view_ = v;
        contentAlpha_ = 0.f;
    }
    SetTargets(v);
    // Küçük hâlde tıklamalar alttaki pencereye (ör. tarayıcı sekmeleri) geçer.
    // İndirme kartı ise tıklanır ve sürüklenir.
    bool interactive = v == View::ExpandedMedia || v == View::ExpandedIdle || IsFileCard() ||
                       v == View::Command || v == View::Drop || dropArmed_;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    LONG_PTR want = interactive ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
                                : (ex | WS_EX_TRANSPARENT);
    // Komut çubuğunda klavye odağı alabilmek için "etkinleşmez" bayrağı kalkar.
    want = commandMode_ ? (want & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE)) : (want | WS_EX_NOACTIVATE);
    if (want != ex) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, want);
    StartAnimation();
}

bool Island::NeedsContinuousFrames() const {
    bool mediaVisible = view_ == View::Media || view_ == View::ExpandedMedia;
    return mediaVisible && snapshot_.playing;
}

void Island::StartAnimation() {
    if (hidden_) return;
    if (!animating_) QueryPerformanceCounter(&lastTick_);
    animating_ = true;
    if (animInterval_ != 16) {
        animInterval_ = 16;
        SetTimer(hwnd_, kTimerAnim, animInterval_, nullptr);
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

    bool moving = false;
    // Küçük adımlarla entegre et: yay kararlı kalsın.
    for (float left = dt; left > 0; left -= 1.f / 240.f) {
        float step = std::min(left, 1.f / 240.f);
        moving |= width_.Step(step);
        moving |= height_.Step(step);
        moving |= radius_.Step(step);
        moving |= shake_.Step(step);
    }
    {
        double now = Now();
        for (float left = dt; left > 0; left -= 1.f / 240.f)
            moving |= eyes_.Step(now, std::min(left, 1.f / 240.f));
    }
    if (contentAlpha_ < 1.f) {
        // İçerik, şekil hedefe yaklaşınca belirir.
        float progress = 1.f - std::min(1.f, std::fabs(width_.target - width_.value) / 60.f);
        contentAlpha_ = std::min(1.f, std::max(contentAlpha_ + dt * 4.f, progress * 0.6f));
        moving = true;
    }

    Render();

    UINT want = moving ? 16 : (NeedsContinuousFrames() ? 40 : 0);
    if (want == 0) {
        KillTimer(hwnd_, kTimerAnim);
        animating_ = false;
        animInterval_ = 0;
    } else if (want != animInterval_) {
        animInterval_ = want;
        SetTimer(hwnd_, kTimerAnim, animInterval_, nullptr);
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
        if (!fClock_) return false;
    }
    if (!roundStroke_) {
        auto sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                              D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d_->CreateStrokeStyle(&sp, nullptr, 0, &roundStroke_);
    }
    if (!rt_) {
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,  // D3D aygıtı açmaz: onlarca MB RAM tasarrufu
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f * scale_, 96.f * scale_);
        if (FAILED(d2d_->CreateDCRenderTarget(&props, &rt_))) return false;
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

void Island::DiscardDeviceResources() {
    brush_.Reset();
    shotBitmap_.Reset();
    for (auto& it : shelf_) it.icon.Reset();
    artBitmap_.Reset();
    rt_.Reset();
}

void Island::Render() {
    if (!memDC_ || !EnsureResources()) return;

    float w = std::clamp(width_.value, 20.f, kMaxW);
    float h = std::clamp(height_.value, 20.f, kMaxH);
    float r = std::clamp(radius_.value, 0.f, std::min(w, h) / 2.f);
    int pxW = std::min(dibW_, static_cast<int>(std::ceil(w * scale_)));
    int pxH = std::min(dibH_, static_cast<int>(std::ceil(h * scale_)));

    RECT bind{0, 0, pxW, pxH};
    if (FAILED(rt_->BindDC(memDC_, &bind))) return;
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));

    D2D1_ROUNDED_RECT pill{D2D1::RectF(0, 0, w, h), r, r};
    brush_->SetColor(Rgba(0, 0, 0, 1));
    rt_->FillRoundedRectangle(pill, brush_.Get());
    // İnce kenar: koyu duvar kağıtlarında hap kaybolmasın.
    D2D1_ROUNDED_RECT edge{D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r - 0.5f, r - 0.5f};
    brush_->SetColor(Rgba(1, 1, 1, 0.07f));
    rt_->DrawRoundedRectangle(edge, brush_.Get(), 1.f);

    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    d2d_->CreateRoundedRectangleGeometry(pill, &clip);
    ComPtr<ID2D1Layer> layer;
    rt_->CreateLayer(&layer);
    if (layer && clip) {
        rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip.Get(),
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), contentAlpha_),
                       layer.Get());
    }
    switch (view_) {
    case View::Idle: DrawIdle(w, h); break;
    case View::Media: DrawCompactMedia(w, h); break;
    case View::ExpandedMedia: DrawExpandedMedia(w, h); break;
    case View::ExpandedIdle: DrawExpandedIdle(w, h); break;
    case View::Alert: DrawAlert(w, h); break;
    case View::Timer: DrawTimer(w, h); break;
    case View::Command: DrawCommand(w, h); break;
    case View::Drop: DrawDrop(w, h); break;
    }
    if ((view_ == View::ExpandedIdle || view_ == View::ExpandedMedia) && ShelfVisible()) DrawShelf(w);
    if (view_ == View::Idle && !shelf_.empty()) {
        // Rafta dosya var: solda minik kâğıt yığını
        brush_->SetColor(Rgba(1, 1, 1, 0.5f));
        D2D1_ROUNDED_RECT a{D2D1::RectF(7, h / 2 - 4, 13, h / 2 + 3), 1.2f, 1.2f};
        rt_->DrawRoundedRectangle(a, brush_.Get(), 1.f);
        D2D1_ROUNDED_RECT b{D2D1::RectF(9, h / 2 - 2, 15, h / 2 + 5), 1.2f, 1.2f};
        rt_->FillRoundedRectangle(b, brush_.Get());
    }
    // Kamera / mikrofon açıkken küçük renkli nokta (yeşil kamera, turuncu mikrofon).
    switch (view_) {
    case View::Idle: DrawPrivacyDots(w - 9.f, h / 2); break;
    case View::Media: DrawPrivacyDots(w - 44.f, h / 2); break;
    case View::Timer: DrawPrivacyDots(w - 6.5f, 6.5f); break;
    case View::ExpandedIdle:
    case View::ExpandedMedia: DrawPrivacyDots(w / 2 + 6.f, 9.f); break;
    case View::Alert:
    case View::Command:
    case View::Drop: break;
    }
    if (layer && clip) rt_->PopLayer();

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        return;
    }

    POINT dst{(monitor_.left + monitor_.right) / 2 - pxW / 2 +
                  static_cast<int>(std::lround(shake_.value * scale_)),
              monitor_.top + static_cast<int>(std::lround(kTopMargin * scale_))};
    SIZE size{pxW, pxH};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &dst, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
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
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), w / 2, h / 2, 0.72f, 0.f);
}

void Island::DrawCompactMedia(float w, float h) {
    float pad = 6.f, side = h - pad * 2;
    DrawArt(D2D1::RectF(pad, pad, pad + side, pad + side), 7.f);
    DrawBars(w - 14.f, h / 2, 14.f, 3.f);
    // Müzik çalarken gözler ritimle hafifçe sallanır.
    float bob = 0.f;
    if (snapshot_.playing) {
        bob = level_ >= 0 ? -beatPulse_ * 2.4f  // vuruşta hop
                          : static_cast<float>(std::sin(time_ * 7.0)) * 1.6f;
    }
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), w / 2, h / 2, 0.85f, bob);
}

void Island::DrawExpandedMedia(float w, float h) {
    const float pad = 18.f, art = 64.f;
    DrawArt(D2D1::RectF(pad, pad, pad + art, pad + art), 14.f);

    float tx = pad + art + 14.f, tr = w - pad - 30.f;
    const wchar_t* title = snapshot_.title.empty() ? L"Bilinmeyen parça" : snapshot_.title.c_str();
    Text(title, fTitle_.Get(), D2D1::RectF(tx, pad + 6, tr, pad + 30), kWhite);
    Text(snapshot_.artist.c_str(), fArtist_.Get(), D2D1::RectF(tx, pad + 32, tr, pad + 54), kGray);
    DrawBars(w - pad, pad + 18, 16.f, 3.f);

    // İlerleme çubuğu
    float py = pad + art + 14.f;
    double dur = snapshot_.durationSec, pos = snapshot_.CurrentPosition();
    D2D1_ROUNDED_RECT track{D2D1::RectF(pad, py, w - pad, py + 4), 2, 2};
    brush_->SetColor(Rgba(1, 1, 1, 0.18f));
    rt_->FillRoundedRectangle(track, brush_.Get());
    if (dur > 0) {
        float fx = pad + (w - pad * 2) * static_cast<float>(std::clamp(pos / dur, 0.0, 1.0));
        D2D1_ROUNDED_RECT fill{D2D1::RectF(pad, py, std::max(pad + 4, fx), py + 4), 2, 2};
        brush_->SetColor(WithAlpha(kWhite, 0.9f));
        rt_->FillRoundedRectangle(fill, brush_.Get());
        Text(FormatDuration(pos).c_str(), fSmall_.Get(), D2D1::RectF(pad, py + 6, w / 2, py + 20), kGray);
        Text((L"-" + FormatDuration(std::max(0.0, dur - pos))).c_str(), fSmallRight_.Get(),
                 D2D1::RectF(w / 2, py + 6, w - pad, py + 20), kGray);
    }

    // Kontroller
    float cy = py + 42.f, cx = w / 2;
    Text(kGlyphPrev, fIcon_.Get(), D2D1::RectF(cx - 86, cy - 20, cx - 46, cy + 20), kWhite);
    Text(snapshot_.playing ? kGlyphPause : kGlyphPlay, fIconBig_.Get(),
             D2D1::RectF(cx - 22, cy - 22, cx + 22, cy + 22), kWhite);
    Text(kGlyphNext, fIcon_.Get(), D2D1::RectF(cx + 46, cy - 20, cx + 86, cy + 20), kWhite);
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
    Text(FormatClock(st).c_str(), fBigClock_.Get(), D2D1::RectF(26, 10, w - 150, 56), kWhite);
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
    } else
        Text(FormatDate(st).c_str(), fDate_.Get(), D2D1::RectF(28, 56, w - 150, 80), kGray);
    h = kExpandedIdle.h;  // raf açıksa alt kısım rafındır
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), w - 128, h / 2, 1.6f, 0.f);

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
    Text(t.c_str(), fTinyRight_.Get(), D2D1::RectF(44, 0, w - 12, h), TimerColor());
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
        case AlertKind::ReminderSet: label = alertLabel_.c_str(); glyph = kGlyphCheck; color = kGreen; break;
        default: break;
        }
        Text(glyph, fIcon_.Get(), D2D1::RectF(12, 0, 40, h), color);
        Text(label, fAlert_.Get(), D2D1::RectF(46, 0, w - 70, h), kWhite);
        wchar_t buf[16];
        bool percent = alertKind_ == AlertKind::CpuBusy || alertKind_ == AlertKind::MemoryFull;
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
    if (view_ == View::ExpandedMedia) {
        const float pad = 18.f, art = 64.f;
        float cy = pad + art + 14.f + 42.f, cx = width_.value / 2;
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
           view_ == View::Timer || view_ == View::Command;
}

void Island::Pet() {
    // Tıklama serisine göre duygu: sevilmek hoşuna gider, abartınca kızar.
    double now = Now();
    lastActivity_ = now;
    clickStreak_ = (now - lastClick_ < 0.7) ? clickStreak_ + 1 : 1;
    lastClick_ = now;
    companion_.OnPet(now);
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
    ComPtr<IShellItemArray> items;
    ComPtr<IDataObject> data;
    if (!pidls.empty() &&
        SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(pidls.size()), pidls.data(), &items)) &&
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
    // Windows'un kendi alan seçicisi; görüntü panoya gelince Guti kartı gösterir.
    ShellExecuteW(nullptr, L"open", L"ms-screenclip:", nullptr, nullptr, SW_SHOWNORMAL);
}

bool Island::SaveClipboardImage() {
    IWICImagingFactory* wic = Wic();
    if (!wic) return false;
    ComPtr<IWICBitmap> full;
    if (!OpenClipboard(hwnd_)) return false;
    if (auto hb = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
        wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &full);
    CloseClipboard();
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
    float tx = x0 + boxW + 14;
    Text(L"Ekran görüntüsü", fAlert_.Get(), D2D1::RectF(tx, h / 2 - 22, w - 20, h / 2 + 2), kWhite);
    Text(L"sürükle bırak · tıkla: aç", fSmall_.Get(), D2D1::RectF(tx, h / 2 + 2, w - 20, h / 2 + 20), kGray);
}

void Island::DrawDrop(float w, float h) {
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 34, h / 2, 0.85f, 0.f);
    Text(L"Bırak, Guti tutsun", fAlert_.Get(), D2D1::RectF(66, 0, w - 16, h), kWhite);
}

void Island::OnDropEnter() {
    dropHover_ = true;
    eyes_.SetMood(Eyes::Mood::Cute, Now(), 0);  // kocaman gözlerle bekler
    Sounds::Play(Sounds::Sound::Open);
    Update();
}

void Island::OnDropLeave() {
    dropHover_ = false;
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
    return view_ == View::ExpandedMedia ? kExpandedMedia.h : kExpandedIdle.h;
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
        if (ID2D1Bitmap* icon = ShelfIcon(static_cast<size_t>(i)))
            rt_->DrawBitmap(icon, D2D1::RectF(cx - 14, top + 4, cx + 14, top + 32));
        const std::wstring& p = shelf_[static_cast<size_t>(i)].path;
        size_t slash = p.find_last_of(L'\\');
        std::wstring name = slash == std::wstring::npos ? p : p.substr(slash + 1);
        Text(name.c_str(), fSmallCenter_.Get(),
             D2D1::RectF(cx - kShelfCell / 2 + 2, top + 34, cx + kShelfCell / 2 - 2, top + 50), kGray);
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
    HWND fg = GetForegroundWindow();
    prevForeground_ = fg != hwnd_ ? fg : nullptr;
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
    if (!animating_) Render();
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
    for (int i = 0; i < n; ++i) {
        float y = bar + 4 + i * kCommandRow;
        if (i == clipSel_) {
            brush_->SetColor(Rgba(1, 1, 1, 0.10f));
            D2D1_ROUNDED_RECT sel{D2D1::RectF(10, y + 1, w - 10, y + kCommandRow - 1), 9, 9};
            rt_->FillRoundedRectangle(sel, brush_.Get());
        }
        std::wstring t = clips_[i].substr(0, 160);
        for (auto& ch : t) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        Text(t.c_str(), fArtist_.Get(), D2D1::RectF(20, y, w - 20, y + kCommandRow),
             i == clipSel_ ? kWhite : kGray);
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
    if (now - lastActivity_ > sleepAfter && eyes_.CurrentMood() == Eyes::Mood::Normal) {
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
    bool arm = leftDown && !downStartedOver_ && nearDrop && !commandMode_;
    if (arm != dropArmed_ && !dropHover_) {
        dropArmed_ = arm;
        Update();
    }
    if (!hover_) {
        if (over && !buttonDown) {
            if (hoverSince_ == 0) {
                hoverSince_ = now;
                if (eyes_.CurrentMood() == Eyes::Mood::Normal || eyes_.CurrentMood() == Eyes::Mood::Sleepy)
                    eyes_.SetMood(Eyes::Mood::Surprised, now, 0.45);
            } else if (now - hoverSince_ >= 0.45) {
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
        if (over) {
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
    else eyes_.LookAt(dx / (700.f * scale_), dy / (380.f * scale_));
    if (eyes_.Tick(now) && ViewShowsEyes()) StartAnimation();
}

// ---------------------------------------------------------------------------
// Tepsi simgesi
// ---------------------------------------------------------------------------

void Island::AddTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Dynamis Island");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Island::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    wchar_t title[64];
    swprintf_s(title, L"Guti  v%d   ·   dostluk %d/100", Updater::kCurrentVersion,
               companion_.Affection());
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title);
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
    AppendMenuW(settings, MF_STRING, kCmdUpdate, updateLabel);
    bool focusing = companion_.Kind() == Companion::TimerKind::Focus ||
                    companion_.Kind() == Companion::TimerKind::Break;
    wchar_t focusLabel[64];
    swprintf_s(focusLabel, L"Odak modu başlat (%d dk)", Companion::kFocusMinutes);
    AppendMenuW(menu, MF_STRING, kCmdCommand, L"Hesap · çevir · hatırlat · pano\tCtrl+Alt+Boşluk");
    AppendMenuW(menu, MF_STRING, kCmdShot, L"Ekran görüntüsü al\tCtrl+Alt+S");
    if (!shelf_.empty()) AppendMenuW(menu, MF_STRING, kCmdClearShelf, L"Rafı boşalt");
    AppendMenuW(menu, MF_STRING, kCmdFocus, focusing ? L"Odak modunu bitir" : focusLabel);
    if (companion_.Kind() == Companion::TimerKind::Quick)
        AppendMenuW(menu, MF_STRING, kCmdCancelTimer, L"Zamanlayıcıyı iptal et");
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
    if (cmd == kCmdSounds) {
        Sounds::SetEnabled(!Sounds::Enabled());
        WriteSetting(L"Sounds", Sounds::Enabled() ? 1 : 0);
        Sounds::Play(Sounds::Sound::Pet);
    }
    if (cmd == kCmdFocus) {
        if (companion_.Kind() == Companion::TimerKind::Focus ||
            companion_.Kind() == Companion::TimerKind::Break) {
            companion_.CancelTimer();
        } else {
            companion_.StartTimer(Companion::TimerKind::Focus, Companion::kFocusMinutes, Now());
            eyes_.SetMood(Eyes::Mood::Focused, Now(), 0);
            Sounds::Play(Sounds::Sound::Pet);
        }
        Update();
    }
    if (cmd == kCmdCancelTimer) {
        companion_.CancelTimer();
        Update();
    }
    if (cmd == kCmdUpdate) {
        if (userHidden_) ToggleUserHidden();  // ilerlemeyi adada göster
        ShowAlert(AlertKind::Updating, availableVersion_, 0);
        Updater::CheckAsync(hwnd_, true);
    }
    if (cmd == kCmdShot) TakeScreenshot();
    if (cmd == kCmdClearShelf) {
        shelf_.clear();
        Update();
    }
    if (cmd == kCmdCommand) {
        if (userHidden_) ToggleUserHidden();
        OpenCommand();
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
        if (IsFileCard()) {
            // Basılı tutup çekersen dosya sürüklenir; sadece tıklarsan açılır.
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &pt);
            swallowUpUntil_ = Now() + 1.0;
            if (DragDetect(hwnd_, pt)) StartCardDrag();
            else OpenCard();
            return 0;
        }
        if (int hit = ShelfHit(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_); hit != -1) {
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
        if (IsFileCard() || Now() < swallowUpUntil_) {
            swallowUpUntil_ = 0;
            return 0;
        }
        OnClick(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
        return 0;

    case WM_CHAR:
        if (commandMode_) OnCommandChar(static_cast<wchar_t>(wp));
        return 0;

    case WM_KEYDOWN:
        if (commandMode_) OnCommandKey(static_cast<UINT>(wp));
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && commandMode_) CloseCommand(false);  // başka yere tıklandı
        break;

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
        case kTimerAnim: OnAnimationTick(); break;
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
            OnCompanionTick();
            OnPrivacyTick();
            CheckReminders();
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
            accent_ = AccentFromArt(snapshot_.art.Get());
        }
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

    case WM_MOUSEWHEEL: {
        // Açık adanın üstünde teker: zamanlayıcı süresi (Ctrl ile 5'er dakika).
        int notches = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (notches == 0) notches = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1;
        if (commandMode_) {
            OnCommandKey(notches > 0 ? VK_UP : VK_DOWN);
            return 0;
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

    case WM_DESTROY: {
        companion_.Save();
        RemoveClipboardFormatListener(hwnd_);
        UnregisterHotKey(hwnd_, kHotkeyToggle);
        UnregisterHotKey(hwnd_, kHotkeyCommand);
        UnregisterHotKey(hwnd_, kHotkeyShot);
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
