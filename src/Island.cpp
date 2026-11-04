#include "Island.h"
#include "Updater.h"

#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <algorithm>
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

constexpr UINT kTrayMsg = WM_APP + 2;
constexpr UINT kTrayId = 1;
constexpr UINT kCmdAutostart = 100;
constexpr UINT kCmdExit = 101;
constexpr UINT kCmdToggle = 102;
constexpr UINT kCmdUpdate = 103;
constexpr wchar_t kSettingsKey[] = L"Software\\DynamisIsland";

constexpr float kTopMargin = 4.f;
constexpr float kMaxW = 420.f, kMaxH = 180.f;

// Görünüm boyutları (DIP)
struct Size3 { float w, h, r; };
constexpr Size3 kIdle{78, 22, 11};
constexpr Size3 kMedia{236, 34, 17};
constexpr Size3 kExpandedMedia{400, 164, 40};
constexpr Size3 kExpandedIdle{320, 96, 36};
constexpr Size3 kAlert{300, 40, 20};

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

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DynamisIsland";

D2D1_COLOR_F Rgba(float r, float g, float b, float a = 1.f) { return D2D1::ColorF(r, g, b, a); }
D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

const D2D1_COLOR_F kWhite = Rgba(1, 1, 1);
const D2D1_COLOR_F kGray = Rgba(1, 1, 1, 0.55f);
const D2D1_COLOR_F kGreen = Rgba(0.30f, 0.85f, 0.42f);
const D2D1_COLOR_F kRed = Rgba(1.f, 0.30f, 0.27f);

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
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic_))))
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

    media_.Start(hwnd_, wic_.Get());
    OnPowerChanged(true);
    AddTrayIcon();

    SetTimer(hwnd_, kTimerTick, 1000, nullptr);
    SetTimer(hwnd_, kTimerLook, 100, nullptr);
    SetTimer(hwnd_, kTimerUpdateCheck, 20 * 1000, nullptr);  // açılıştan 20 sn sonra, sonra 6 saatte bir
    RegisterHotKey(hwnd_, kHotkeyToggle, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
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
    if (alertActive_) return View::Alert;
    if (hover_) return snapshot_.hasSession ? View::ExpandedMedia : View::ExpandedIdle;
    return snapshot_.hasSession ? View::Media : View::Idle;
}

void Island::SetTargets(View v) {
    Size3 s = kIdle;
    switch (v) {
    case View::Idle: s = kIdle; break;
    case View::Media: s = kMedia; break;
    case View::ExpandedMedia: s = kExpandedMedia; break;
    case View::ExpandedIdle: s = kExpandedIdle; break;
    case View::Alert: s = kAlert; break;
    }
    width_.target = s.w;
    height_.target = s.h;
    radius_.target = s.r;
}

void Island::Update() {
    View v = TargetView();
    if (v != view_) {
        view_ = v;
        contentAlpha_ = 0.f;
        SetTargets(v);
        // Küçük hâlde tıklamalar alttaki pencereye (ör. tarayıcı sekmeleri) geçer.
        bool interactive = v == View::ExpandedMedia || v == View::ExpandedIdle;
        LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        LONG_PTR want = interactive ? (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT))
                                    : (ex | WS_EX_TRANSPARENT);
        if (want != ex) SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, want);
    }
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
    if (kind == AlertKind::Charging) eyes_.SetMood(Eyes::Mood::Love, now, 4.5);
    if (kind == AlertKind::Unplugged) eyes_.SetMood(Eyes::Mood::Sad, now, 2.5);
    if (kind == AlertKind::LowBattery) eyes_.SetMood(Eyes::Mood::Sad, now, 4.5);
    if (kind == AlertKind::UpToDate || kind == AlertKind::UpdateAvailable)
        eyes_.SetMood(Eyes::Mood::Happy, now, 4.0);
    if (kind == AlertKind::UpdateFailed) eyes_.SetMood(Eyes::Mood::Dizzy, now, 2.0);
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
    float bob = snapshot_.playing ? static_cast<float>(std::sin(time_ * 7.0)) * 1.6f : 0.f;
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
    Text(FormatDate(st).c_str(), fDate_.Get(), D2D1::RectF(28, 56, w - 150, 80), kGray);
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

void Island::DrawAlert(float w, float h) {
    if (alertKind_ >= AlertKind::UpdateAvailable) {
        const wchar_t* glyph = kGlyphSync;
        const wchar_t* label = L"";
        D2D1_COLOR_F color = accent_;
        switch (alertKind_) {
        case AlertKind::UpdateAvailable: label = L"Yeni sürüm var"; color = kGreen; break;
        case AlertKind::Updating: label = L"Güncelleniyor..."; break;
        case AlertKind::UpToDate: label = L"En güncel sürüm"; glyph = kGlyphCheck; color = kGreen; break;
        case AlertKind::UpdateFailed: label = L"Güncelleme başarısız"; glyph = kGlyphError; color = kRed; break;
        default: break;
        }
        Text(glyph, fIcon_.Get(), D2D1::RectF(12, 0, 40, h), color);
        Text(label, fAlert_.Get(), D2D1::RectF(46, 0, w - 70, h), kWhite);
        wchar_t buf[16];
        swprintf_s(buf, L"v%d", alertValue_);
        if (alertValue_ > 0) Text(buf, fAlertRight_.Get(), D2D1::RectF(w - 80, 0, w - 18, h), color);
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
    if (view_ == View::ExpandedIdle) {
        Pet();
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
    return view_ == View::Idle || view_ == View::Media || view_ == View::ExpandedIdle;
}

void Island::Pet() {
    // Tıklama serisine göre duygu: sevilmek hoşuna gider, abartınca kızar.
    double now = Now();
    lastActivity_ = now;
    clickStreak_ = (now - lastClick_ < 0.7) ? clickStreak_ + 1 : 1;
    lastClick_ = now;

    using M = Eyes::Mood;
    if (clickStreak_ >= 8) {
        eyes_.SetMood(M::Dizzy, now, 2.0);  // başı döndü
        shake_.velocity += 420.f;
        clickStreak_ = 0;
    } else if (clickStreak_ >= 5) {
        eyes_.SetMood(M::Angry, now, 2.2);  // "yeter!"
        shake_.velocity += 300.f;
    } else if (clickStreak_ == 4) {
        eyes_.SetMood(M::Love, now, 2.6);
        height_.velocity += 60.f;
    } else if (clickStreak_ == 3) {
        eyes_.SetMood(M::Laugh, now, 1.8);  // gıdıklandı
        height_.velocity += 80.f;
    } else {
        eyes_.SetMood(M::Happy, now, 1.4);
        height_.velocity += 70.f;  // sevinçle zıplar
        width_.velocity += 50.f;
    }
    StartAnimation();
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
    if (!hover_) {
        if (over && !buttonDown) {
            if (hoverSince_ == 0) {
                hoverSince_ = now;
                if (eyes_.CurrentMood() == Eyes::Mood::Normal || eyes_.CurrentMood() == Eyes::Mood::Sleepy)
                    eyes_.SetMood(Eyes::Mood::Surprised, now, 0.45);
            } else if (now - hoverSince_ >= 0.45) {
                hover_ = true;
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
            hover_ = false;
            outsideSince_ = 0;
            Update();
        }
    }

    eyes_.LookAt(dx / (700.f * scale_), dy / (380.f * scale_));
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
    swprintf_s(title, L"Dynamis Island  v%d", Updater::kCurrentVersion);
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
    AppendMenuW(settings, MF_STRING, kCmdUpdate, updateLabel);
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
    if (cmd == kCmdUpdate) {
        if (userHidden_) ToggleUserHidden();  // ilerlemeyi adada göster
        ShowAlert(AlertKind::Updating, availableVersion_, 0);
        Updater::CheckAsync(hwnd_, true);
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

    case WM_LBUTTONUP:
        OnClick(GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_);
        return 0;

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
            alertActive_ = false;
            Update();
            break;
        case kTimerTick: {
            CheckFullscreen();
            if (!hidden_) {
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                SYSTEMTIME st;
                GetLocalTime(&st);
                bool showsClock = view_ == View::Idle || view_ == View::ExpandedIdle;
                bool showsProgress = view_ == View::ExpandedMedia;
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

    case WM_HOTKEY:
        if (wp == kHotkeyToggle) ToggleUserHidden();
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
                ShowAlert(AlertKind::UpdateFailed, version, 3500);
            }
            break;
        case Updater::Result::Available:
            if (version != availableVersion_) {
                availableVersion_ = version;
                ShowAlert(AlertKind::UpdateAvailable, version, 4500);
            }
            break;
        case Updater::Result::UpToDate:
            if (userAsked) ShowAlert(AlertKind::UpToDate, Updater::kCurrentVersion, 3000);
            break;
        case Updater::Result::Failed:
            if (userAsked) ShowAlert(AlertKind::UpdateFailed, 0, 3500);
            break;
        }
        return 0;
    }

    case WM_DESTROY: {
        UnregisterHotKey(hwnd_, kHotkeyToggle);
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
