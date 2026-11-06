#include "Snap.h"

#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT_PTR kTimer = 1;
constexpr double kMoveDur = 0.26;  // pencerenin kayma süresi
constexpr double kFadeIn = 0.10, kHoldUntil = 0.42, kFadeOut = 0.22;  // cam önizleme

double Now() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

bool LightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

bool Snappable(HWND w) {
    if (!w || !IsWindowVisible(w) || IsIconic(w)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;  // ada, dock vb.
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    LONG style = GetWindowLongW(w, GWL_STYLE);
    if (style & WS_CHILD) return false;
    return (style & WS_THICKFRAME) != 0;  // boyutu değiştirilemeyen pencereler (iletişim kutuları) olduğu gibi kalır
}

// Görünen çerçeve: Windows 10/11'de GetWindowRect görünmez boyutlandırma kenarını da içerir.
RECT VisibleRect(HWND w) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(w, &r);
    return r;
}

RECT FrameMargins(HWND w) {
    RECT wr{}, vr = VisibleRect(w);
    GetWindowRect(w, &wr);
    return {vr.left - wr.left, vr.top - wr.top, wr.right - vr.right, wr.bottom - vr.bottom};
}

struct Frac {
    float x0, y0, x1, y1;
};

Frac Fraction(Snapper::Action a) {
    using A = Snapper::Action;
    switch (a) {
    case A::Left: return {0, 0, 0.5f, 1};
    case A::Right: return {0.5f, 0, 1, 1};
    case A::ThirdL: return {0, 0, 1 / 3.f, 1};
    case A::ThirdC: return {1 / 3.f, 0, 2 / 3.f, 1};
    case A::ThirdR: return {2 / 3.f, 0, 1, 1};
    case A::TopLeft: return {0, 0, 0.5f, 0.5f};
    case A::TopRight: return {0.5f, 0, 1, 0.5f};
    case A::BottomLeft: return {0, 0.5f, 0.5f, 1};
    case A::BottomRight: return {0.5f, 0.5f, 1, 1};
    default: return {0, 0, 1, 1};
    }
}
}  // namespace

const wchar_t* Snapper::Label(Action a) {
    switch (a) {
    case Action::Left: return L"Sol yarı";
    case Action::Right: return L"Sağ yarı";
    case Action::ThirdL: return L"Sol üçte bir";
    case Action::ThirdC: return L"Orta üçte bir";
    case Action::ThirdR: return L"Sağ üçte bir";
    case Action::TopLeft: return L"Sol üst köşe";
    case Action::TopRight: return L"Sağ üst köşe";
    case Action::BottomLeft: return L"Sol alt köşe";
    case Action::BottomRight: return L"Sağ alt köşe";
    case Action::Full: return L"Tam ekran";
    case Action::Back: return L"Eski boyut";
    default: return L"";
    }
}

const wchar_t* Snapper::Key(Action a) {
    switch (a) {
    case Action::Left: return L"←";
    case Action::Right: return L"→";
    case Action::ThirdL: return L"F";
    case Action::ThirdC: return L"G";
    case Action::ThirdR: return L"H";
    case Action::TopLeft: return L"U";
    case Action::TopRight: return L"I";
    case Action::BottomLeft: return L"J";
    case Action::BottomRight: return L"K";
    case Action::Full: return L"↵";
    case Action::Back: return L"⌫";
    default: return L"";
    }
}

void Snapper::LookDir(Action a, float& x, float& y) {
    Frac f = Fraction(a);
    x = (f.x0 + f.x1) - 1.f;  // pencerenin ortası: -1 sol, 1 sağ
    y = (f.y0 + f.y1) - 1.f;
    if (a == Action::Full || a == Action::Back) x = 0, y = 0.6f;
}

bool Snapper::Init(HINSTANCE instance, ID2D1Factory* d2d, IDWriteFactory* dwrite) {
    instance_ = instance;
    d2d_ = d2d;
    dwrite_ = dwrite;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GutiSnapGlass";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"GutiSnapLabel";
    RegisterClassExW(&wc);

    return true;
}

// Pencereler ilk kullanımda açılır: özellik hiç kullanılmazsa cam bellekte yer tutmaz.
bool Snapper::CreateWindows() {
    if (label_) return true;
    if (!instance_) return false;
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    glass_ = CreateWindowExW(ex | WS_EX_NOREDIRECTIONBITMAP, L"GutiSnapGlass", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                             nullptr, instance_, this);
    if (glass_ && !glassFx_.Attach(glass_)) {
        DestroyWindow(glass_);
        glass_ = nullptr;  // eski sistem: yalnız yarı saydam katman çizilir
    }
    if (glass_) {
        DWORD corner = 1;  // DONOTROUND: köşeleri cam kırpar
        DwmSetWindowAttribute(glass_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
        COLORREF none = 0xFFFFFFFE;
        DwmSetWindowAttribute(glass_, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof(none));
    }
    label_ = CreateWindowExW(ex | WS_EX_LAYERED, L"GutiSnapLabel", L"", WS_POPUP, 0, 0, 1, 1, glass_, nullptr,
                             instance_, this);
    return label_ != nullptr;
}

void Snapper::Shutdown() {
    if (label_) DestroyWindow(label_);
    glassFx_.Detach();
    if (glass_) DestroyWindow(glass_);
    label_ = glass_ = nullptr;
}

LRESULT CALLBACK Snapper::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Snapper*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (self && wp == kTimer) self->Tick();
        return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Snapper::Run(Action a) {
    if (!CreateWindows()) return false;
    for (auto it = original_.begin(); it != original_.end();)  // kapanmış pencereleri unut
        it = IsWindow(it->first) ? std::next(it) : original_.erase(it);
    for (auto it = placed_.begin(); it != placed_.end();) it = IsWindow(it->first) ? std::next(it) : placed_.erase(it);

    HWND w = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!Snappable(w)) return false;

    RECT start = VisibleRect(w);
    RECT target{};
    if (a == Action::Back) {
        auto it = original_.find(w);
        if (it == original_.end()) return false;
        target = it->second;
        original_.erase(it);
        placed_.erase(w);
    }
    if (IsZoomed(w)) {
        // Büyütülmüş pencere önce normal hale gelir, animasyon yine tam ekrandan başlar.
        ShowWindow(w, SW_RESTORE);
        if (a != Action::Back && original_.find(w) == original_.end()) original_[w] = VisibleRect(w);
    } else if (a != Action::Back) {
        // Kullanıcı pencereyi bizden sonra elle taşıdıysa "geri" yeni yerine dönsün.
        auto p = placed_.find(w);
        if (p == placed_.end() || !EqualRect(&p->second, &start)) original_[w] = start;
    }

    HMONITOR mon = MonitorFromRect(&start, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForWindow(w);
    if (!dpi) dpi = 96;
    if (a != Action::Back) {
        RECT work = mi.rcWork;
        if (a == Action::Full) {
            target = work;
        } else {
            int g = static_cast<int>(std::lround(6.0 * dpi / 96.0));  // pencereler arası boşluk
            RECT in{work.left + g, work.top + g, work.right - g, work.bottom - g};
            float W = static_cast<float>(in.right - in.left + g), H = static_cast<float>(in.bottom - in.top + g);
            Frac f = Fraction(a);
            target.left = in.left + std::lround(f.x0 * W);
            target.top = in.top + std::lround(f.y0 * H);
            target.right = in.left + std::lround(f.x1 * W) - g;
            target.bottom = in.top + std::lround(f.y1 * H) - g;
        }
        placed_[w] = target;
    }

    win_ = w;
    margin_ = FrameMargins(w);
    from_ = start;
    to_ = target;
    startAt_ = Now();
    moving_ = true;
    ShowPreview(target, Label(a), dpi);
    SetTimer(label_, kTimer, 10, nullptr);
    Tick();
    return true;
}

void Snapper::Tick() {
    double t = Now() - startAt_;
    if (moving_) {
        if (!IsWindow(win_)) {
            moving_ = false;
        } else {
            double p = std::min(1.0, t / kMoveDur);
            double e = 1.0 - std::pow(1.0 - p, 3.0);  // yumuşak yavaşlayarak durur
            auto lerp = [e](LONG a, LONG b) { return static_cast<int>(std::lround(a + (b - a) * e)); };
            int l = lerp(from_.left, to_.left), tp = lerp(from_.top, to_.top);
            int r = lerp(from_.right, to_.right), b = lerp(from_.bottom, to_.bottom);
            // Async: yanıt vermeyen bir uygulama adayı kilitlemesin.
            SetWindowPos(win_, nullptr, l - margin_.left, tp - margin_.top, (r - l) + margin_.left + margin_.right,
                         (b - tp) + margin_.top + margin_.bottom,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
            if (p >= 1.0) moving_ = false;
        }
    }
    if (previewShown_) {
        float a = t < kFadeIn ? static_cast<float>(t / kFadeIn)
                  : t < kHoldUntil ? 1.f
                                   : 1.f - static_cast<float>((t - kHoldUntil) / kFadeOut);
        if (a <= 0.f) HidePreview();
        else SetPreviewAlpha(a);
    }
    if (!moving_ && !previewShown_) KillTimer(label_, kTimer);
}

void Snapper::ShowPreview(const RECT& r, const wchar_t* label, UINT dpi) {
    float scale = dpi / 96.f;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (glass_) {
        glassFx_.Update(w, h, 8.f * scale, LightTheme());
        glassFx_.SetOpacity(0.f);
        SetWindowPos(glass_, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    SetWindowPos(label_, HWND_TOPMOST, r.left, r.top, w, h, SWP_NOACTIVATE);
    if (!DrawLabel(w, h, label, scale)) {
        if (glass_) ShowWindow(glass_, SW_HIDE);
        return;
    }
    ShowWindow(label_, SW_SHOWNOACTIVATE);
    previewShown_ = true;
}

void Snapper::SetPreviewAlpha(float a) {
    a = std::clamp(a, 0.f, 1.f);
    if (glass_) glassFx_.SetOpacity(a);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, static_cast<BYTE>(a * 255.f), AC_SRC_ALPHA};
    UpdateLayeredWindow(label_, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

void Snapper::HidePreview() {
    previewShown_ = false;
    ShowWindow(label_, SW_HIDE);
    if (glass_) {
        glassFx_.SetOpacity(0.f);
        ShowWindow(glass_, SW_HIDE);
    }
}

// Önizlemenin üst katmanı: ince kenar, hafif parlaklık ve ortada "Sol yarı" gibi beyaz bir etiket.
bool Snapper::DrawLabel(int w, int h, const wchar_t* label, float scale) {
    if (!d2d_ || !dwrite_) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib) {
        HGDIOBJ old = SelectObject(mem, dib);
        ComPtr<ID2D1DCRenderTarget> rt;
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.f, 96.f);
        RECT rc{0, 0, w, h};
        if (SUCCEEDED(d2d_->CreateDCRenderTarget(&props, &rt)) && SUCCEEDED(rt->BindDC(mem, &rc))) {
            rt->BeginDraw();
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            ComPtr<ID2D1SolidColorBrush> br;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
            float rad = 8.f * scale, s = std::max(1.f, scale);
            D2D1_ROUNDED_RECT body{D2D1::RectF(s / 2, s / 2, w - s / 2, h - s / 2), rad, rad};
            // Cam yoksa (eski Windows) bulanıklık yerine hafif mavi bir dolgu.
            br->SetColor(glass_ ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0.55f, 0.68f, 0.95f, 0.30f));
            rt->FillRoundedRectangle(body, br.Get());
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
            rt->DrawRoundedRectangle(body, br.Get(), s);

            ComPtr<IDWriteTextFormat> fmt;
            if (SUCCEEDED(dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                    13.f * scale, L"tr-tr", &fmt))) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                ComPtr<IDWriteTextLayout> lay;
                UINT32 len = static_cast<UINT32>(wcslen(label));
                if (SUCCEEDED(dwrite_->CreateTextLayout(label, len, fmt.Get(), static_cast<float>(w),
                                                        40.f * scale, &lay))) {
                    DWRITE_TEXT_METRICS m{};
                    lay->GetMetrics(&m);
                    float pw = m.width + 30.f * scale, ph = 30.f * scale;
                    float cx = w / 2.f, cy = h / 2.f;
                    if (pw < w - 8 && ph < h - 8) {
                        D2D1_ROUNDED_RECT shadow{D2D1::RectF(cx - pw / 2, cy - ph / 2 + 2 * scale, cx + pw / 2,
                                                             cy + ph / 2 + 2 * scale),
                                                 ph / 2, ph / 2};
                        br->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
                        rt->FillRoundedRectangle(shadow, br.Get());
                        D2D1_ROUNDED_RECT pill{D2D1::RectF(cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2), ph / 2,
                                               ph / 2};
                        br->SetColor(D2D1::ColorF(1, 1, 1, 0.96f));
                        rt->FillRoundedRectangle(pill, br.Get());
                        br->SetColor(D2D1::ColorF(0.11f, 0.11f, 0.13f, 1));
                        rt->DrawTextLayout(D2D1::Point2F(0, cy - 20.f * scale), lay.Get(), br.Get());
                    }
                }
            }
            ok = SUCCEEDED(rt->EndDraw());
        }
        if (ok) {
            POINT src{0, 0};
            SIZE size{w, h};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 0, AC_SRC_ALPHA};  // ilk kare görünmez, sonra belirir
            ok = UpdateLayeredWindow(label_, screen, nullptr, &size, mem, &src, 0, &bf, ULW_ALPHA) != FALSE;
        }
        SelectObject(mem, old);
        DeleteObject(dib);  // pencere içeriği DWM'de kalır; büyük bitmap bellekte tutulmaz
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}
