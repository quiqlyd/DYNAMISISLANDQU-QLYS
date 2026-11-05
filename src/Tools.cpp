// Araç çentiği: açık adanın altındaki araç kutucukları ve arkalarındaki özellikler.
// Yazı tanıma, kahve (uyanık tut), not, ekrana çizim ve hızlı ayarlar kartı.
#include "Island.h"
#include "IslandUi.h"
#include "Sounds.h"
#include "Sys.h"
#include "Updater.h"

#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace isl;

namespace {

struct ToolInfo {
    const wchar_t* glyph;  // nullptr: "Aa" yazısı
    const wchar_t* label;
};
// Island::Tool sırasıyla aynı.
const ToolInfo kToolInfo[] = {
    {nullptr, L"Yazı al"},       {kGlyphCamera, L"Ekran"},  {kGlyphColor, L"Renk"},
    {kGlyphCafe, L"Kahve"},      {kGlyphNote, L"Not"},      {kGlyphPen, L"Çiz"},
    {kGlyphSettings, L"Ayarlar"}, {kGlyphTimer, L"Odak"},   {kGlyphCalc, L"Hesap"},
    {kGlyphVolume, L"Sesler"},   {kGlyphSync, L"Güncelle"},
};
static_assert(sizeof(kToolInfo) / sizeof(kToolInfo[0]) == static_cast<size_t>(Island::kToolCount));

constexpr int kPages = (Island::kToolCount + kToolCols - 1) / kToolCols;
constexpr float kRowW = kToolCols * kTile + (kToolCols - 1) * kTileGap;
constexpr size_t kNoteMax = 600;

// Kalem renkleri: kırmızı, sarı, yeşil, mavi, beyaz
const D2D1_COLOR_F kInkColors[] = {
    Rgba(1.f, 0.30f, 0.32f), Rgba(1.f, 0.83f, 0.22f), Rgba(0.30f, 0.86f, 0.45f),
    Rgba(0.38f, 0.62f, 1.f), Rgba(1.f, 1.f, 1.f),
};
constexpr int kInkColorCount = 5;

// Hızlı ayarlar kartının yerleşimi
constexpr float kQTileY = 14.f, kQTileH = 56.f, kQBrightY = 96.f, kQVolumeY = 130.f, kQBottomY = 152.f;
constexpr float kQTrackX0 = 54.f;
float QTileW(float w) { return (w - 36.f - 3 * 8.f) / 4.f; }
float QTrackX1(float w) { return w - 66.f; }

}  // namespace

// ---------------------------------------------------------------------------
// Araç ızgarası
// ---------------------------------------------------------------------------

float Island::ToolsTop() const {
    return view_ == View::ExpandedMedia ? kExpandedMedia.h : kExpandedIdle.h;
}

float Island::ToolsX0(float w) const { return (w - kRowW) / 2.f; }

int Island::ToolHit(float x, float y) const {
    if (!ToolsShown(view_)) return -1;
    float top = ToolsTop(), y0 = top + 4.f, w = viewW_;
    if (y >= y0 && y <= y0 + kTile) {
        float rel = x - ToolsX0(w);
        if (rel < 0) return -1;
        int col = static_cast<int>(rel / (kTile + kTileGap));
        if (col >= kToolCols || rel - col * (kTile + kTileGap) > kTile) return -1;
        int t = toolPage_ * kToolCols + col;
        return t < kToolCount ? t : -1;
    }
    if (y > y0 + kTile && y <= top + kToolsH && std::fabs(x - w / 2) < 26.f) return x < w / 2 ? -10 : -11;
    return -1;
}

bool Island::ToolActive(Tool t) const {
    switch (t) {
    case Tool::Coffee: return coffee_;
    case Tool::Ink: return ink_ != nullptr;
    case Tool::Sounds: return Sounds::Enabled();
    case Tool::Focus:
        return companion_.Kind() == Companion::TimerKind::Focus || companion_.Kind() == Companion::TimerKind::Break;
    default: return false;
    }
}

void Island::DrawTools(View v, float w) {
    float top = v == View::ExpandedMedia ? kExpandedMedia.h : kExpandedIdle.h;
    float y0 = top + 4.f, x0 = ToolsX0(w);
    brush_->SetColor(Rgba(1, 1, 1, 0.08f));
    rt_->FillRectangle(D2D1::RectF(18, top - 2, w - 18, top - 1), brush_.Get());

    // Sayfalar yan yana durur; sayfa değişince yayla kayarak gelir.
    float page = drawingOld_ ? static_cast<float>(toolPage_) : pageX_.value;
    rt_->PushAxisAlignedClip(D2D1::RectF(8, top, w - 8, y0 + kTile + 2), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int p = 0; p < kPages; ++p) {
        float off = (p - page) * (kRowW + 40.f);
        if (std::fabs(off) >= w) continue;
        for (int i = 0; i < kToolCols; ++i) {
            int t = p * kToolCols + i;
            if (t >= kToolCount) break;
            float x = x0 + i * (kTile + kTileGap) + off;
            float st = Stagger(3 + i);
            PushFade(st, 0, 8.f * (1 - st), 0.85f + 0.15f * st, x + kTile / 2, y0 + kTile / 2);
            float glow = std::clamp(toolGlow_[t].value, 0.f, 1.f);
            bool active = ToolActive(static_cast<Tool>(t));
            // Üstüne gelince hafifçe kabarır.
            PushFade(1.f, 0, -1.5f * glow, 1.f + 0.04f * glow, x + kTile / 2, y0 + kTile / 2);
            D2D1_ROUNDED_RECT rr{D2D1::RectF(x, y0, x + kTile, y0 + kTile), 14, 14};
            brush_->SetColor(active ? WithAlpha(accent_, 0.26f + 0.12f * glow) : Rgba(1, 1, 1, 0.07f + 0.08f * glow));
            rt_->FillRoundedRectangle(rr, brush_.Get());
            D2D1_COLOR_F ic = active ? accent_ : kWhite;
            const ToolInfo& info = kToolInfo[t];
            if (!info.glyph) {
                Text(L"Aa", fAa_.Get(), D2D1::RectF(x, y0 + 4, x + kTile, y0 + 31), ic);
            } else {
                const wchar_t* g = info.glyph;
                if (static_cast<Tool>(t) == Tool::Sounds && !Sounds::Enabled()) g = kGlyphMute;
                Text(g, fIcon_.Get(), D2D1::RectF(x, y0 + 5, x + kTile, y0 + 31), ic);
            }
            Text(info.label, fSmallCenter_.Get(), D2D1::RectF(x + 2, y0 + 31, x + kTile - 2, y0 + 46),
                 active ? kWhite : kGray);
            if (static_cast<Tool>(t) == Tool::Note && !note_.empty()) {
                brush_->SetColor(kOrange);  // kayıtlı not var
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + kTile - 9, y0 + 9), 2.6f, 2.6f), brush_.Get());
            }
            PopFade();
            PopFade();
        }
    }
    rt_->PopAxisAlignedClip();

    // Sayfa noktaları: etkin olan uzar.
    float cy = y0 + kTile + 11.f;
    for (int p = 0; p < kPages; ++p) {
        float a = 1.f - std::min(1.f, std::fabs(page - p));
        float dw = 6.f + 8.f * a;
        float cx = w / 2 + (p - (kPages - 1) / 2.f) * 16.f;
        brush_->SetColor(Rgba(1, 1, 1, 0.25f + 0.6f * a));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(cx - dw / 2, cy - 3, cx + dw / 2, cy + 3), 3, 3},
                                  brush_.Get());
    }
}

void Island::SetToolPage(int page) {
    page = std::clamp(page, 0, kPages - 1);
    if (page == toolPage_) {
        pageX_.velocity += (page == 0 ? 2.f : -2.f);  // kenarda: hafif esneyip geri gelir
    } else {
        toolPage_ = page;
        Sounds::Play(Sounds::Sound::Close);
    }
    pageX_.target = static_cast<float>(toolPage_);
    hoverTool_ = -1;
    for (auto& g : toolGlow_) g.target = 0.f;
    StartAnimation();
}

void Island::RunTool(Tool t) {
    lastActivity_ = Now();
    switch (t) {
    case Tool::Ocr: StartOcr(); break;
    case Tool::Shot:
        hover_ = false;
        Update();
        TakeScreenshot();
        break;
    case Tool::Picker:
        hover_ = false;
        StartPicker();
        break;
    case Tool::Coffee: SetCoffee(!coffee_); break;
    case Tool::Note: OpenNote(); break;
    case Tool::Ink: StartInk(); break;
    case Tool::Quick: OpenQuick(); break;
    case Tool::Focus: ToggleFocus(); break;
    case Tool::Calc: OpenCommand(); break;
    case Tool::Sounds: ToggleSounds(); break;
    case Tool::Update: StartUpdate(); break;
    case Tool::Count: break;
    }
    StartAnimation();
}

void Island::ToggleFocus() {
    if (companion_.Kind() == Companion::TimerKind::Focus || companion_.Kind() == Companion::TimerKind::Break) {
        companion_.CancelTimer();
    } else {
        companion_.StartTimer(Companion::TimerKind::Focus, Companion::kFocusMinutes, Now());
        eyes_.SetMood(Eyes::Mood::Focused, Now(), 0);
        Sounds::Play(Sounds::Sound::Pet);
    }
    Update();
}

void Island::ToggleSounds() {
    Sounds::SetEnabled(!Sounds::Enabled());
    WriteSetting(L"Sounds", Sounds::Enabled() ? 1 : 0);
    Sounds::Play(Sounds::Sound::Pet);
    if (!Sounds::Enabled()) eyes_.SetMood(Eyes::Mood::Sleepy, Now(), 0.8);  // "şşş"
    StartAnimation();
}

void Island::StartUpdate() {
    if (userHidden_) ToggleUserHidden();  // ilerlemeyi adada göster
    hover_ = false;
    ShowAlert(AlertKind::Updating, availableVersion_, 0);
    Updater::CheckAsync(hwnd_, true);
}

// ---------------------------------------------------------------------------
// Yazı tanıma: Windows'un alan seçicisiyle seç, Guti okusun, yazı panoya gelsin
// ---------------------------------------------------------------------------

void Island::StartOcr() {
    if (ink_) return;  // çizim tuvali ekranı tutuyor
    CloseCommand(false);
    CloseNote(false);
    if (userHidden_) ToggleUserHidden();
    ocrPending_ = true;
    ocrAskedAt_ = Now();
    hover_ = false;
    quickOpen_ = false;
    eyes_.SetMood(Eyes::Mood::Curious, Now(), 2.0);  // "hangisini okuyayım?"
    Sounds::Play(Sounds::Sound::Open);
    Update();
    ShellExecuteW(nullptr, L"open", L"ms-screenclip:", nullptr, nullptr, SW_SHOWNORMAL);
}

ComPtr<IWICBitmap> Island::ClipboardBitmap() {
    ComPtr<IWICBitmap> bmp;
    IWICImagingFactory* wic = Wic();
    if (!wic || !OpenClipboard(hwnd_)) return bmp;
    if (auto hb = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)))
        wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &bmp);
    CloseClipboard();
    return bmp;
}

bool Island::RunOcr(IWICBitmapSource* src) {
    IWICImagingFactory* wic = Wic();
    if (!wic || !src) return false;
    UINT w = 0, h = 0;
    if (FAILED(src->GetSize(&w, &h)) || w < 4 || h < 4) return false;
    // Küçük seçimler iki kat büyütülünce daha iyi okunur; çok büyükler belleği şişirmesin.
    UINT mx = std::max(w, h);
    float k = mx < 1000 ? 2.f : (mx > 2600 ? 2600.f / mx : 1.f);
    UINT tw = std::max(1u, static_cast<UINT>(w * k)), th = std::max(1u, static_cast<UINT>(h * k));
    ComPtr<IWICBitmapSource> cur = src;
    ComPtr<IWICBitmapScaler> scaler;
    if (k != 1.f) {
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(src, tw, th, WICBitmapInterpolationModeFant)))
            return false;
        cur = scaler;
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(cur.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    std::vector<BYTE> px(static_cast<size_t>(tw) * th * 4);
    if (FAILED(conv->CopyPixels(nullptr, tw * 4, static_cast<UINT>(px.size()), px.data()))) return false;
    Ocr::Recognize(hwnd_, std::move(px), tw, th);
    alertLabel_ = L"Okuyorum...";
    ShowAlert(AlertKind::Reading, 0, 15000);
    return true;
}

void Island::OnOcrDone(int code, const std::wstring& text) {
    if (code > 0 && !text.empty()) {
        CopyToClipboard(text);
        wchar_t buf[64];
        swprintf_s(buf, code == 1 ? L"Yazı kopyalandı" : L"%d satır yazı kopyalandı", code);
        alertLabel_ = buf;
        ShowAlert(AlertKind::Copied, 0, 2800);
        height_.velocity += 90.f;  // "buldum!"
        return;
    }
    alertLabel_ = code == -1 ? L"Yazı tanıma dili yüklü değil" : (code == 0 ? L"Burada yazı bulamadım" : L"Yazı okunamadı");
    ShowAlert(AlertKind::OcrFailed, 0, 3200);
}

// ---------------------------------------------------------------------------
// Kahve: bilgisayar ve ekran uyumaz; Guti de gözlerini açık tutar
// ---------------------------------------------------------------------------

void Island::SetCoffee(bool on) {
    coffee_ = on;
    SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) : ES_CONTINUOUS);
    if (on) {
        MoodWithSound(Eyes::Mood::Surprised, 1.4);  // kahveyi içti, gözler fal taşı
        height_.velocity += 110.f;
    } else {
        MoodWithSound(Eyes::Mood::Yawn, 1.6);
    }
    lastActivity_ = Now();
    Update();
}

void Island::DrawCup(float x, float cy, float alpha) {
    // Minik kahve fincanı + buhar
    brush_->SetColor(WithAlpha(kOrange, alpha));
    rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, cy - 1.5f, x + 6.f, cy + 4.f), 1.5f, 1.5f},
                              brush_.Get());
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + 6.8f, cy + 1.2f), 1.6f, 1.6f), brush_.Get(), 1.f);
    brush_->SetColor(Rgba(1, 1, 1, 0.45f * alpha));
    float wob = static_cast<float>(std::sin(time_ * 3.0)) * 0.6f;
    rt_->DrawLine(D2D1::Point2F(x + 2.f + wob, cy - 3.f), D2D1::Point2F(x + 2.5f - wob, cy - 6.5f), brush_.Get(),
                  1.f, roundStroke_.Get());
    rt_->DrawLine(D2D1::Point2F(x + 4.5f - wob, cy - 3.f), D2D1::Point2F(x + 4.f + wob, cy - 6.f), brush_.Get(),
                  1.f, roundStroke_.Get());
}

// ---------------------------------------------------------------------------
// Not: tek bir yapışkan not; Guti notu tutarken yanında minik bir kalem olur
// ---------------------------------------------------------------------------

void Island::LoadNote() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Note", RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size < sizeof(wchar_t))
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Note", RRF_RT_REG_SZ, nullptr, buf.data(), &size) ==
        ERROR_SUCCESS)
        note_ = buf.data();
    if (note_.size() > kNoteMax) note_.resize(kNoteMax);
}

void Island::SaveNote() {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Note", REG_SZ, note_.c_str(),
                    static_cast<DWORD>((note_.size() + 1) * sizeof(wchar_t)));
}

void Island::OpenNote() {
    if (noteMode_) return;
    HWND keep = prevForeground_;  // komut çubuğundan geçiliyorsa asıl uygulamayı unutma
    CloseCommand(false);
    if (userHidden_) ToggleUserHidden();
    HWND fg = GetForegroundWindow();
    prevForeground_ = fg != hwnd_ ? fg : keep;
    noteMode_ = true;
    hover_ = false;
    quickOpen_ = false;
    Update();
    Render();
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    MoodWithSound(Eyes::Mood::Happy, 1.0);  // kalemini kaptı
    Sounds::Play(Sounds::Sound::Open);
    lastActivity_ = Now();
}

void Island::CloseNote(bool restoreFocus) {
    if (!noteMode_) return;
    noteMode_ = false;
    while (!note_.empty() && (note_.back() == L'\n' || note_.back() == L' ')) note_.pop_back();
    SaveNote();
    if (!note_.empty()) eyes_.SetMood(Eyes::Mood::Wink, Now(), 1.0);  // "sakladım"
    Update();
    if (restoreFocus && prevForeground_ && IsWindow(prevForeground_)) SetForegroundWindow(prevForeground_);
    prevForeground_ = nullptr;
}

void Island::OnNoteChar(wchar_t c) {
    switch (c) {
    case 27: CloseNote(true); return;  // Esc
    case 13:                           // Enter: yeni satır
        if (note_.size() < kNoteMax) note_ += L'\n';
        break;
    case 8:
        if (!note_.empty()) note_.pop_back();
        if (!note_.empty() && IS_HIGH_SURROGATE(note_.back())) note_.pop_back();  // emoji yarım kalmasın
        break;
    case 127:  // Ctrl+Geri sil: kelime sil
        while (!note_.empty() && (note_.back() == L' ' || note_.back() == L'\n')) note_.pop_back();
        while (!note_.empty() && note_.back() != L' ' && note_.back() != L'\n') note_.pop_back();
        break;
    case 22: {  // Ctrl+V: panodaki güncel yazı
        std::wstring paste;
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(hwnd_)) {
            if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(hd))) {
                    paste.assign(p, wcsnlen(p, kNoteMax));
                    GlobalUnlock(hd);
                }
            }
            CloseClipboard();
        }
        {
            for (wchar_t ch : paste) {
                if (note_.size() >= kNoteMax) break;
                if (ch == L'\r') continue;
                note_ += ch == L'\t' ? L' ' : ch;
            }
        }
        break;
    }
    default:
        if (c < 32 || note_.size() >= kNoteMax) return;
        note_ += c;
        break;
    }
    lastActivity_ = Now();
    StartAnimation();
}

void Island::DrawNote(float w, float h) {
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 30, 22, 0.72f, 0.f);
    // Guti'nin kalemi
    brush_->SetColor(kOrange);
    rt_->DrawLine(D2D1::Point2F(45, 28), D2D1::Point2F(52, 18), brush_.Get(), 2.2f, roundStroke_.Get());
    brush_->SetColor(kWhite);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(44.6f, 28.6f), 1.1f, 1.1f), brush_.Get());

    Text(L"Not", fTiny_.Get(), D2D1::RectF(60, 8, 160, 36), kWhite);
    Text(L"Esc: kaydet", fSmallRight_.Get(), D2D1::RectF(w - 200, 8, w - 50, 36), kGray);
    Text(kGlyphDelete, fIconSmall_.Get(), D2D1::RectF(w - 44, 8, w - 18, 36), kGray);

    D2D1_RECT_F area = D2D1::RectF(22, 42, w - 22, h - 14);
    float aw = area.right - area.left, ah = area.bottom - area.top;
    if (note_.empty()) {
        Text(L"Aklındakini yaz... (Enter: yeni satır)", fNote_.Get(), area, kGray);
        brush_->SetColor(WithAlpha(kWhite, 0.85f));
        rt_->FillRectangle(D2D1::RectF(area.left - 3, area.top + 1, area.left - 1.4f, area.top + 19), brush_.Get());
        return;
    }
    ComPtr<IDWriteTextLayout> layout;
    dwrite_->CreateTextLayout(note_.c_str(), static_cast<UINT32>(note_.size()), fNote_.Get(), aw, 4000.f, &layout);
    if (!layout) return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    float oy = std::min(0.f, ah - m.height);  // uzarsa son satırlar görünür
    rt_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    brush_->SetColor(kWhite);
    rt_->DrawTextLayout(D2D1::Point2F(area.left, area.top + oy), layout.Get(), brush_.Get());
    FLOAT cx = 0, cy = 0;
    DWRITE_HIT_TEST_METRICS hm{};
    layout->HitTestTextPosition(static_cast<UINT32>(note_.size()), FALSE, &cx, &cy, &hm);
    float lineH = hm.height > 4 ? hm.height : 18.f;
    brush_->SetColor(WithAlpha(kWhite, 0.85f));
    float x = area.left + cx + 1.f, y = area.top + oy + cy;
    rt_->FillRectangle(D2D1::RectF(x, y + 1, x + 1.6f, y + lineH - 1), brush_.Get());
    rt_->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// Ekrana çizim: ekranı kaplayan saydam bir tuval; kalem paleti adada
// ---------------------------------------------------------------------------

LRESULT CALLBACK Island::InkProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Island* self = reinterpret_cast<Island*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Island*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && self->ink_ == hwnd) {
        switch (msg) {
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
        case WM_MOUSEACTIVATE:
            // Tıklayınca öne çıkmasın: palet (ada) hep tuvalin üstünde kalmalı.
            SetWindowPos(self->hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return MA_NOACTIVATE;
        case WM_ACTIVATE:
            SetWindowPos(self->hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            self->inkDown_ = true;
            self->InkPoint(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, true);
            return 0;
        case WM_MOUSEMOVE:
            if (self->inkDown_ && (wp & MK_LBUTTON)) self->InkPoint(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, false);
            return 0;
        case WM_LBUTTONUP:
            self->inkDown_ = false;
            ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED: self->inkDown_ = false; return 0;
        case WM_RBUTTONUP: self->EndInk(); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                self->EndInk();
            } else if (wp == 'Z' && GetKeyState(VK_CONTROL) < 0) {
                self->InkUndo();
            } else if (wp == VK_DELETE || wp == VK_BACK) {
                self->InkClear();
            } else if (wp >= '1' && wp < '1' + kInkColorCount) {
                self->SetInkColor(static_cast<int>(wp - '1'));
            }
            return 0;
        case WM_CLOSE: self->EndInk(); return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Island::StartInk() {
    if (ink_) return;
    CloseCommand(false);
    CloseNote(false);
    if (picker_) EndPicker(false);
    if (userHidden_) ToggleUserHidden();
    IWICImagingFactory* wic = Wic();
    if (!wic) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = InkProc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = L"GutiInk";
        RegisterClassExW(&wc);
        registered = true;
    }
    inkRect_ = monitor_;
    inkW_ = inkRect_.right - inkRect_.left;
    inkH_ = inkRect_.bottom - inkRect_.top;
    if (inkW_ <= 0 || inkH_ <= 0) return;

    auto fail = [this] {
        inkRt_.Reset();
        inkBrush_.Reset();
        inkWic_.Reset();
        if (inkDC_) {
            SelectObject(inkDC_, inkOld_);
            DeleteDC(inkDC_);
        }
        if (inkDib_) DeleteObject(inkDib_);
        inkDC_ = nullptr;
        inkDib_ = nullptr;
        inkBits_ = nullptr;
    };
    // Kalıcı tuval (WIC) + ekrana aktarmak için GDI yüzeyi. Sadece çizerken bellekte durur.
    auto rtProps = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(wic->CreateBitmap(static_cast<UINT>(inkW_), static_cast<UINT>(inkH_), GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &inkWic_)) ||
        FAILED(d2d_->CreateWicBitmapRenderTarget(inkWic_.Get(), rtProps, &inkRt_)) ||
        FAILED(inkRt_->CreateSolidColorBrush(kWhite, &inkBrush_))) {
        fail();
        return;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = inkW_;
    bi.bmiHeader.biHeight = -inkH_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    inkDC_ = CreateCompatibleDC(nullptr);
    inkDib_ = CreateDIBSection(inkDC_, &bi, DIB_RGB_COLORS, &inkBits_, nullptr, 0);
    if (!inkDC_ || !inkDib_ || !inkBits_) {
        fail();
        return;
    }
    inkOld_ = SelectObject(inkDC_, inkDib_);

    ink_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"GutiInk", L"", WS_POPUP,
                           inkRect_.left, inkRect_.top, inkW_, inkH_, nullptr, nullptr, instance_, this);
    if (!ink_) {
        fail();
        return;
    }
    strokes_.clear();
    RedrawInk();
    ShowWindow(ink_, SW_SHOW);
    SetForegroundWindow(ink_);
    SetFocus(ink_);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // palet üstte
    hover_ = false;
    quickOpen_ = false;
    eyes_.SetTint(&kInkColors[inkColor_]);  // gözler kalemin renginde
    MoodWithSound(Eyes::Mood::Cute, 1.6);
    Update();
}

void Island::EndInk() {
    if (!ink_) return;
    HWND w = ink_;
    ink_ = nullptr;
    inkDown_ = false;
    DestroyWindow(w);
    inkRt_.Reset();
    inkBrush_.Reset();
    inkWic_.Reset();
    if (inkDC_) {
        SelectObject(inkDC_, inkOld_);
        DeleteDC(inkDC_);
    }
    if (inkDib_) DeleteObject(inkDib_);
    inkDC_ = nullptr;
    inkDib_ = nullptr;
    inkBits_ = nullptr;
    strokes_.clear();
    strokes_.shrink_to_fit();
    eyes_.SetTint(nullptr);
    MoodWithSound(Eyes::Mood::Happy, 1.0);
    Update();
}

void Island::InkPoint(POINT pt, bool begin) {
    if (!inkRt_) return;
    D2D1_POINT_2F p{static_cast<float>(pt.x), static_cast<float>(pt.y)};
    float width = 4.5f * scale_;
    D2D1_POINT_2F a = p;
    if (begin || strokes_.empty()) {
        strokes_.push_back({kInkColors[inkColor_], {p}});
    } else {
        Stroke& s = strokes_.back();
        a = s.pts.back();
        float dx = p.x - a.x, dy = p.y - a.y;
        if (dx * dx + dy * dy < 1.f) return;
        s.pts.push_back(p);
    }
    inkRt_->BeginDraw();
    inkBrush_->SetColor(strokes_.back().color);
    if (begin) inkRt_->FillEllipse(D2D1::Ellipse(p, width / 2, width / 2), inkBrush_.Get());
    else inkRt_->DrawLine(a, p, inkBrush_.Get(), width, roundStroke_.Get());
    inkRt_->EndDraw();
    int m = static_cast<int>(width) + 3;
    RECT dirty{static_cast<LONG>(std::min(a.x, p.x)) - m, static_cast<LONG>(std::min(a.y, p.y)) - m,
               static_cast<LONG>(std::max(a.x, p.x)) + m, static_cast<LONG>(std::max(a.y, p.y)) + m};
    PresentInk(&dirty);
}

void Island::RedrawInk() {
    if (!inkRt_) return;
    float width = 4.5f * scale_;
    inkRt_->BeginDraw();
    // Neredeyse görünmez zemin: tamamen saydam pikseller tıklamayı alttaki pencereye geçirirdi.
    inkRt_->Clear(D2D1::ColorF(0, 0, 0, 1.f / 255.f));
    for (const auto& s : strokes_) {
        inkBrush_->SetColor(s.color);
        if (s.pts.size() == 1) inkRt_->FillEllipse(D2D1::Ellipse(s.pts[0], width / 2, width / 2), inkBrush_.Get());
        for (size_t i = 1; i < s.pts.size(); ++i)
            inkRt_->DrawLine(s.pts[i - 1], s.pts[i], inkBrush_.Get(), width, roundStroke_.Get());
    }
    inkRt_->EndDraw();
    PresentInk(nullptr);
}

void Island::PresentInk(const RECT* dirty) {
    if (!ink_ || !inkWic_ || !inkBits_) return;
    RECT r = dirty ? *dirty : RECT{0, 0, inkW_, inkH_};
    r.left = std::max<LONG>(0, r.left);
    r.top = std::max<LONG>(0, r.top);
    r.right = std::min<LONG>(inkW_, r.right);
    r.bottom = std::min<LONG>(inkH_, r.bottom);
    if (r.right <= r.left || r.bottom <= r.top) return;
    // Sadece değişen kısmı GDI yüzeyine kopyala ve pencereye o kadarını bildir.
    WICRect wr{r.left, r.top, r.right - r.left, r.bottom - r.top};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(inkWic_->Lock(&wr, WICBitmapLockRead, &lock))) return;
    UINT stride = 0, size = 0;
    BYTE* src = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &src);
    if (src) {
        auto* dst = static_cast<BYTE*>(inkBits_);
        size_t rowBytes = static_cast<size_t>(wr.Width) * 4;
        for (int y = 0; y < wr.Height; ++y)
            std::memcpy(dst + (static_cast<size_t>(r.top + y) * inkW_ + r.left) * 4, src + static_cast<size_t>(y) * stride,
                        rowBytes);
    }
    lock.Reset();
    POINT dst{inkRect_.left, inkRect_.top}, srcPt{0, 0};
    SIZE sz{inkW_, inkH_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info{sizeof(info)};
    info.hdcSrc = inkDC_;
    info.pptDst = &dst;
    info.psize = &sz;
    info.pptSrc = &srcPt;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = dirty ? &r : nullptr;
    if (!UpdateLayeredWindowIndirect(ink_, &info) && dirty) {
        info.prcDirty = nullptr;  // eski sürücülerde kirli alan desteklenmeyebilir
        UpdateLayeredWindowIndirect(ink_, &info);
    }
}

void Island::InkUndo() {
    if (strokes_.empty()) {
        shake_.velocity += 140.f;  // geri alacak bir şey yok
        StartAnimation();
        return;
    }
    strokes_.pop_back();
    RedrawInk();
    eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.6);
    StartAnimation();
}

void Island::SetInkColor(int i) {
    if (i < 0 || i >= kInkColorCount) return;
    inkColor_ = i;
    eyes_.SetTint(&kInkColors[i]);  // Guti'nin gözleri yeni kalemin rengine bürünür
    Sounds::Play(Sounds::Sound::Pet);
    StartAnimation();
}

void Island::InkClear() {
    strokes_.clear();
    RedrawInk();
    MoodWithSound(Eyes::Mood::Happy, 0.8);  // tertemiz
}

int Island::InkBarHit(float x, float y) const {
    if (view_ != View::Ink || y < 0 || y > kInk.h) return -1;
    float w = viewW_;
    for (int i = 0; i < kInkColorCount; ++i)
        if (std::fabs(x - (62.f + i * 24.f)) < 11.f) return i;
    if (std::fabs(x - (w - 88.f)) < 14.f) return 10;
    if (std::fabs(x - (w - 60.f)) < 14.f) return 11;
    if (std::fabs(x - (w - 32.f)) < 14.f) return 12;
    return -1;
}

void Island::DrawInkBar(float w, float h) {
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), 28, h / 2, 0.62f, 0.f);
    for (int i = 0; i < kInkColorCount; ++i) {
        float cx = 62.f + i * 24.f, cy = h / 2;
        brush_->SetColor(kInkColors[i]);
        bool sel = i == inkColor_;
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), sel ? 7.5f : 6.f, sel ? 7.5f : 6.f), brush_.Get());
        if (sel) {
            brush_->SetColor(Rgba(1, 1, 1, 0.85f));
            rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 10.5f, 10.5f), brush_.Get(), 1.5f);
        }
    }
    Text(kGlyphUndo, fIconSmall_.Get(), D2D1::RectF(w - 102, 0, w - 74, h), strokes_.empty() ? kGray : kWhite);
    Text(kGlyphDelete, fIconSmall_.Get(), D2D1::RectF(w - 74, 0, w - 46, h), strokes_.empty() ? kGray : kWhite);
    Text(kGlyphClose, fIconSmall_.Get(), D2D1::RectF(w - 46, 0, w - 18, h), kWhite);
}

// ---------------------------------------------------------------------------
// Hızlı ayarlar kartı
// ---------------------------------------------------------------------------

void Island::OpenQuick() {
    quickOpen_ = true;
    nightLight_ = NightLight::State();
    Radios::Refresh(hwnd_);
    Brightness::Refresh(hwnd_);
    int v = 0;
    bool m = false;
    if (VolumeWatch::Get(v, m)) {
        volume_ = v;
        muted_ = m;
    }
    eyes_.SetMood(Eyes::Mood::Curious, Now(), 0.8);
    Sounds::Play(Sounds::Sound::Open);
    Update();
}

int Island::QuickHit(float x, float y) const {
    if (view_ != View::Quick) return -1;
    float w = viewW_, tw = QTileW(w);
    if (y >= kQTileY && y <= kQTileY + kQTileH) {
        for (int i = 0; i < 4; ++i) {
            float tx = 18.f + i * (tw + 8.f);
            if (x >= tx && x <= tx + tw) return i;
        }
        return -1;
    }
    if (x >= kQTrackX0 - 10.f && x <= QTrackX1(w) + 10.f) {
        if (std::fabs(y - kQBrightY) < 14.f) return 10;
        if (std::fabs(y - kQVolumeY) < 14.f) return 11;
    }
    if (y >= kQBottomY - 2.f && y <= kQBottomY + 36.f) {
        if (x < 130.f) return 20;
        if (x > w - 170.f) return 21;
    }
    return -1;
}

void Island::QuickSlider(int which, float x) {
    float x0 = kQTrackX0, x1 = QTrackX1(viewW_);
    int pct = static_cast<int>(std::lround(std::clamp((x - x0) / (x1 - x0), 0.f, 1.f) * 100.f));
    if (which == 0) {
        if (brightness_ < 0 || brightness_ > 100 || pct == brightness_) return;
        brightness_ = pct;
        brightSetAt_ = Now();
        Brightness::Set(hwnd_, pct);
    } else {
        if (pct == volume_ && !muted_) return;
        volume_ = pct;
        muted_ = false;
        VolumeWatch::Set(pct);
    }
    lastActivity_ = Now();
    StartAnimation();
}

void Island::QuickClick(int hit) {
    double now = Now();
    switch (hit) {
    case 0:
    case 1: {
        bool wifi = hit == 0;
        int has = wifi ? Radios::kWifi : Radios::kBt, on = wifi ? Radios::kWifiOn : Radios::kBtOn;
        if (radios_ < 0 || !(radios_ & has)) {
            shake_.velocity += 160.f;  // bu bilgisayarda yok
            break;
        }
        radios_ ^= on;  // hemen göster; gerçek durum birazdan gelir
        Radios::Toggle(hwnd_, wifi);
        if (radios_ & on) MoodWithSound(Eyes::Mood::Happy, 1.0);
        else eyes_.SetMood(Eyes::Mood::Sad, now, 1.0);
        break;
    }
    case 2:
        if (NightLight::Toggle()) {
            nightLight_ = NightLight::State();
            if (nightLight_ == 1) eyes_.SetMood(Eyes::Mood::Sleepy, now, 1.2);  // sıcak ışık: göz kapakları ağırlaşır
            else MoodWithSound(Eyes::Mood::Happy, 0.8);
        } else {
            // Bu Windows'ta kayıt biçimi tanınmadı: dokunma, Ayarlar'ı aç.
            AllowSetForegroundWindow(ASFW_ANY);
            ShellExecuteW(nullptr, L"open", L"ms-settings:nightlight", nullptr, nullptr, SW_SHOWNORMAL);
            hover_ = false;
        }
        break;
    case 3: SetCoffee(!coffee_); break;
    case 20:
        quickOpen_ = false;
        Sounds::Play(Sounds::Sound::Close);
        break;
    case 21:
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr, SW_SHOWNORMAL);
        hover_ = false;
        break;
    default: return;
    }
    Update();
    StartAnimation();
}

void Island::DrawQuick(float w, float h) {
    (void)h;
    float tw = QTileW(w);
    struct QTile {
        const wchar_t* glyph;
        const wchar_t* label;
        const wchar_t* state;
        bool on;
    };
    auto radioState = [&](int has, int on) -> const wchar_t* {
        if (radios_ < 0) return L"...";
        if (!(radios_ & has)) return L"Yok";
        return (radios_ & on) ? L"Açık" : L"Kapalı";
    };
    QTile tiles[4] = {
        {kGlyphWifi, L"Wi-Fi", radioState(Radios::kWifi, Radios::kWifiOn), radios_ > 0 && (radios_ & Radios::kWifiOn)},
        {kGlyphBluetooth, L"Bluetooth", radioState(Radios::kBt, Radios::kBtOn), radios_ > 0 && (radios_ & Radios::kBtOn)},
        {kGlyphMoon, L"Gece ışığı", nightLight_ < 0 ? L"Ayarla" : (nightLight_ ? L"Açık" : L"Kapalı"), nightLight_ == 1},
        {kGlyphCafe, L"Kahve", coffee_ ? L"Uyanık" : L"Kapalı", coffee_},
    };
    for (int i = 0; i < 4; ++i) {
        float x = 18.f + i * (tw + 8.f), y = kQTileY;
        float st = Stagger(i);
        PushFade(st, 0, 6.f * (1 - st), 0.9f + 0.1f * st, x + tw / 2, y + kQTileH / 2);
        brush_->SetColor(tiles[i].on ? WithAlpha(accent_, 0.30f) : Rgba(1, 1, 1, 0.07f));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, y, x + tw, y + kQTileH), 14, 14}, brush_.Get());
        D2D1_COLOR_F ic = tiles[i].on ? accent_ : kWhite;
        Text(tiles[i].glyph, fIcon_.Get(), D2D1::RectF(x + 6, y + 4, x + 32, y + 28), ic);
        Text(tiles[i].label, fTiny_.Get(), D2D1::RectF(x + 10, y + 26, x + tw - 4, y + 42), kWhite);
        Text(tiles[i].state, fSmall_.Get(), D2D1::RectF(x + 10, y + 40, x + tw - 4, y + 54), kGray);
        PopFade();
    }

    auto slider = [&](int idx, float cy, const wchar_t* glyph, int value, const wchar_t* missing) {
        float st = Stagger(4 + idx);
        PushFade(st, 0, 6.f * (1 - st));
        float x0 = kQTrackX0, x1 = QTrackX1(w);
        bool ok = value >= 0 && value <= 100;
        Text(glyph, fIcon_.Get(), D2D1::RectF(18, cy - 13, 44, cy + 13), ok ? kWhite : kGray);
        D2D1_ROUNDED_RECT track{D2D1::RectF(x0, cy - 4, x1, cy + 4), 4, 4};
        brush_->SetColor(Rgba(1, 1, 1, ok ? 0.16f : 0.07f));
        rt_->FillRoundedRectangle(track, brush_.Get());
        if (ok) {
            float fx = x0 + (x1 - x0) * value / 100.f;
            brush_->SetColor(idx == sliderDrag_ ? kWhite : WithAlpha(kWhite, 0.9f));
            rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, cy - 4, std::max(x0 + 8, fx), cy + 4), 4, 4},
                                      brush_.Get());
            float kr = idx == sliderDrag_ ? 8.f : 6.5f;  // tutamak: sürüklerken büyür
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(std::max(x0 + 4, fx), cy), kr, kr), brush_.Get());
            wchar_t buf[16];
            swprintf_s(buf, L"%%%d", value);
            Text(buf, fSmallRight_.Get(), D2D1::RectF(x1 + 6, cy - 10, w - 18, cy + 10), kGray);
        } else if (missing) {
            Text(missing, fSmallCenter_.Get(), D2D1::RectF(x0, cy - 9, x1, cy + 9), kGray);
        }
        PopFade();
    };
    slider(0, kQBrightY, kGlyphBrightness, brightness_ == 255 ? -1 : brightness_,
           brightness_ == 255 ? L"Bu ekranda parlaklık ayarlanamıyor" : nullptr);
    slider(1, kQVolumeY, muted_ ? kGlyphMute : kGlyphVolume, volume_ < 0 ? -1 : (muted_ ? 0 : volume_), nullptr);

    float by = kQBottomY;
    Text(kGlyphBack, fIconSmall_.Get(), D2D1::RectF(18, by, 34, by + 32), kGray);
    Text(L"Araçlar", fSmall_.Get(), D2D1::RectF(38, by, 130, by + 32), kGray);
    Text(L"Tüm ayarlar", fSmallRight_.Get(), D2D1::RectF(w - 170, by, w - 40, by + 32), kGray);
    Text(kGlyphOpen, fIconSmall_.Get(), D2D1::RectF(w - 36, by, w - 18, by + 32), kGray);
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), w / 2, by + 16, 0.6f, 0.f);
}
