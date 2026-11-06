// 2. parti: ana sayfa (müzik + odak + favoriler tek ekranda), hava durumu,
// kapalı hapta albüm ışığı ve Guti'nin aksesuarları.
#include "Island.h"
#include "IslandUi.h"
#include "Net.h"
#include "Sounds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using Microsoft::WRL::ComPtr;
using namespace isl;

namespace {

// Ana sayfa yerleşimi (kHome: 400 x 132)
constexpr float kHPad = 14.f, kHTop = 14.f, kHBottom = 118.f;
// Hepsi 400 genişliğe sığar: 14 | sol 134 | 8 | orta 108 | 8 | favoriler 2x54+6 | 14
constexpr float kLeftX0 = 14.f, kLeftX1 = 148.f;
constexpr float kMidX0 = 156.f, kMidX1 = 264.f;
constexpr float kFavX0 = 272.f, kFavTileW = 54.f, kFavTileH = 49.f, kFavGap = 6.f;
constexpr float kPlayX = 124.f, kPlayY = 46.f, kPlayR = 16.f;
constexpr int kHomeFavs = 4;

enum HomeHitCode { kHitNone = -1, kHitMusic = 1, kHitPlay = 2, kHitEyes = 3, kHitFocus = 4, kHitFav = 50 };

D2D1_RECT_F FavRect(int i) {
    float x = kFavX0 + (i % 2) * (kFavTileW + kFavGap), y = kHTop + (i / 2) * (kFavTileH + kFavGap);
    return D2D1::RectF(x, y, x + kFavTileW, y + kFavTileH);
}

bool In(D2D1_RECT_F r, float x, float y) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

// Aksesuar eşikleri: dostluk arttıkça açılır.
constexpr int kAccThresholds[] = {20, 40, 60, 80};
const wchar_t* const kAccNames[] = {L"kulaklık", L"atkı", L"uyku başlığı", L"güneş gözlüğü"};

}  // namespace

float Island::CardH(View v) const {
    if (v == View::ExpandedMedia) return MediaCardH();
    if (v == View::Home) return kHome.h;
    return kExpandedIdle.h;
}

// ---------------------------------------------------------------------------
// Ana sayfa
// ---------------------------------------------------------------------------

void Island::DrawHome(float w, float h) {
    (void)w;
    (void)h;
    double now = Now();
    float s0 = Stagger(0), s1 = Stagger(1), s2 = Stagger(2);

    // Sol kart: müzik (yoksa saat)
    PushFade(s0, 0, 6.f * (1 - s0), 0.96f + 0.04f * s0, (kLeftX0 + kLeftX1) / 2, 66);
    {
        D2D1_ROUNDED_RECT card{D2D1::RectF(kLeftX0, kHTop, kLeftX1, kHBottom), 20, 20};
        bool hot = homeHover_ == kHitMusic || homeHover_ == kHitPlay;
        if (snapshot_.hasSession && albumTint_ && snapshot_.art) {
            D2D1_COLOR_F c = CardAccent();
            D2D1_COLOR_F top = Rgba(c.r * 0.75f, c.g * 0.75f, c.b * 0.75f, 0.62f);
            D2D1_GRADIENT_STOP stops[2] = {{0.f, top}, {1.f, WithAlpha(top, 0.22f)}};
            ComPtr<ID2D1GradientStopCollection> coll;
            ComPtr<ID2D1LinearGradientBrush> g;
            if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &coll)) &&
                SUCCEEDED(rt_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, kHTop), D2D1::Point2F(0, kHBottom)),
                    coll.Get(), &g)))
                rt_->FillRoundedRectangle(card, g.Get());
        } else {
            brush_->SetColor(Rgba(1, 1, 1, 0.07f));
            rt_->FillRoundedRectangle(card, brush_.Get());
        }
        if (hot) {
            brush_->SetColor(Rgba(1, 1, 1, 0.05f));
            rt_->FillRoundedRectangle(card, brush_.Get());
        }
        if (snapshot_.hasSession) {
            DrawArt(D2D1::RectF(kLeftX0 + 12, kHTop + 12, kLeftX0 + 56, kHTop + 56), 11.f);
            // Oynat / duraklat
            float glow = homeHover_ == kHitPlay ? 1.f : 0.f;
            brush_->SetColor(Rgba(1, 1, 1, 0.9f + 0.1f * glow));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kPlayX, kPlayY), kPlayR, kPlayR), brush_.Get());
            Text(snapshot_.playing ? kGlyphPause : kGlyphPlay, fIcon_.Get(),
                 D2D1::RectF(kPlayX - kPlayR, kPlayY - kPlayR, kPlayX + kPlayR, kPlayY + kPlayR), Rgba(0, 0, 0, 0.9f));
            const wchar_t* title = snapshot_.title.empty() ? L"Bilinmeyen parça" : snapshot_.title.c_str();
            Text(title, fAlert_.Get(), D2D1::RectF(kLeftX0 + 12, kHTop + 62, kLeftX1 - 10, kHTop + 80), kWhite);
            // Söz varsa sanatçının yerinde şimdiki satır
            std::wstring sub = snapshot_.artist;
            if (HasLyrics() && lyricLine_ >= 0 && !lyrics_[static_cast<size_t>(lyricLine_)].text.empty())
                sub = lyrics_[static_cast<size_t>(lyricLine_)].text;
            Text(sub.c_str(), fDate_.Get(), D2D1::RectF(kLeftX0 + 12, kHTop + 80, kLeftX1 - 10, kHTop + 96), kGray);
        } else {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t clock[16];
            swprintf_s(clock, L"%02d:%02d", st.wHour, st.wMinute);
            DrawRolling(rollClock_, clock, fBigClock_.Get(), D2D1::RectF(kLeftX0 + 12, kHTop + 6, kLeftX1, kHTop + 52),
                        kWhite);
            wchar_t date[64]{};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMMM dddd", date, 64, nullptr);
            Text(date, fDate_.Get(), D2D1::RectF(kLeftX0 + 14, kHTop + 54, kLeftX1 - 8, kHTop + 72), kGray);
            if (weather_.ok && weatherOn_)
                Text(Weather::Short(weather_).c_str(), fDate_.Get(),
                     D2D1::RectF(kLeftX0 + 14, kHTop + 74, kLeftX1 - 8, kHTop + 92), kWhite);
        }
    }
    PopFade();

    // Orta kart: Guti ve odak
    PushFade(s1, 0, 6.f * (1 - s1), 0.96f + 0.04f * s1, (kMidX0 + kMidX1) / 2, 66);
    {
        D2D1_ROUNDED_RECT card{D2D1::RectF(kMidX0, kHTop, kMidX1, kHBottom), 20, 20};
        bool focus = companion_.TimerActive();
        if (focus) {
            D2D1_COLOR_F c = TimerColor();
            D2D1_GRADIENT_STOP stops[2] = {{0.f, WithAlpha(c, 0.05f)}, {1.f, WithAlpha(c, 0.30f)}};
            ComPtr<ID2D1GradientStopCollection> coll;
            ComPtr<ID2D1LinearGradientBrush> g;
            if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &coll)) &&
                SUCCEEDED(rt_->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, kHTop), D2D1::Point2F(0, kHBottom)),
                    coll.Get(), &g)))
                rt_->FillRoundedRectangle(card, g.Get());
        } else {
            brush_->SetColor(Rgba(1, 1, 1, homeHover_ == kHitFocus ? 0.11f : 0.07f));
            rt_->FillRoundedRectangle(card, brush_.Get());
        }
        float cx = (kMidX0 + kMidX1) / 2;
        float breath = 1.f + 0.018f * static_cast<float>(std::sin(time_ * 1.9));
        float bob = -0.8f * (breath - 1.f) * 40.f + 1.4f * std::sin(nodPulse_ * 3.14159f);
        DrawGuti(cx, kHTop + 30, 1.25f * breath, bob);
        wchar_t idleTimer[16];
        swprintf_s(idleTimer, L"%d:00", Companion::kFocusMinutes);
        if (focus) {
            int sec = static_cast<int>(companion_.Remaining(now) + 0.999);
            if (sec >= 3600) swprintf_s(idleTimer, L"%d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);
            else swprintf_s(idleTimer, L"%d:%02d", sec / 60, sec % 60);
        }
        std::wstring timer = idleTimer;
        if (fHomeTimer_)
            DrawRolling(rollTimer_, timer, fHomeTimer_.Get(), D2D1::RectF(kMidX0, kHTop + 52, kMidX1, kHTop + 80),
                        focus ? TimerColor() : kWhite);
        // Alt satır: hava ve pil
        const wchar_t* kind = companion_.Kind() == Companion::TimerKind::Break   ? L"mola"
                              : companion_.Kind() == Companion::TimerKind::Focus ? L"odak"
                                                                                 : L"zamanlayıcı";
        std::wstring sub = focus ? kind : L"odak · tıkla";
        if (!focus) {
            std::wstring extra;
            if (weather_.ok && weatherOn_) extra = Weather::Short(weather_);
            SYSTEM_POWER_STATUS ps{};
            if (GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryLifePercent <= 100) {
                wchar_t b[16];
                swprintf_s(b, L"%%%d", ps.BatteryLifePercent);
                extra += (extra.empty() ? L"" : L"  ·  ") + std::wstring(b);
            }
            if (!extra.empty()) sub = extra;
        }
        Text(sub.c_str(), fSmallCenter_.Get(), D2D1::RectF(kMidX0 + 4, kHTop + 82, kMidX1 - 4, kHTop + 98), kGray);
    }
    PopFade();

    // Sağ: dört favori
    PushFade(s2, 0, 6.f * (1 - s2));
    {
        int n = std::min(static_cast<int>(favs_.size()), kHomeFavs);
        int tiles = std::min(n + 1, kHomeFavs);
        for (int i = 0; i < tiles; ++i) {
            D2D1_RECT_F r = FavRect(i);
            float glow = std::clamp(favGlow_[i].value, 0.f, 1.f);
            float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            PushFade(1.f, 0, -1.5f * glow, 1.f + 0.05f * glow, cx, cy);
            brush_->SetColor(Rgba(1, 1, 1, 0.07f + 0.08f * glow));
            rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, 14, 14}, brush_.Get());
            if (i < n) {
                if (ID2D1Bitmap* icon = FavoriteIcon(static_cast<size_t>(i))) {
                    rt_->DrawBitmap(icon, D2D1::RectF(cx - 11, cy - 16, cx + 11, cy + 6), 1.f,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                } else {
                    const std::wstring& label = favs_[static_cast<size_t>(i)].label;
                    wchar_t letter[2] = {label.empty() ? L'?' : label[0], 0};
                    brush_->SetColor(Rgba(1, 1, 1, 0.18f));
                    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - 5), 11, 11), brush_.Get());
                    Text(letter, fAa_.Get(), D2D1::RectF(cx - 12, cy - 17, cx + 12, cy + 7), kWhite);
                }
                Text(favs_[static_cast<size_t>(i)].label.c_str(), fSmallCenter_.Get(),
                     D2D1::RectF(r.left + 2, cy + 7, r.right - 2, r.bottom - 1), kGray);
            } else {
                brush_->SetColor(Rgba(1, 1, 1, 0.7f));
                rt_->DrawLine(D2D1::Point2F(cx - 7, cy), D2D1::Point2F(cx + 7, cy), brush_.Get(), 2.f, roundStroke_.Get());
                rt_->DrawLine(D2D1::Point2F(cx, cy - 7), D2D1::Point2F(cx, cy + 7), brush_.Get(), 2.f, roundStroke_.Get());
            }
            PopFade();
        }
    }
    PopFade();
    (void)now;
}

int Island::HomeHit(float x, float y) const {
    if (view_ != View::Home) return kHitNone;
    if (snapshot_.hasSession) {
        float dx = x - kPlayX, dy = y - kPlayY;
        if (dx * dx + dy * dy <= (kPlayR + 4) * (kPlayR + 4)) return kHitPlay;
    }
    if (In(D2D1::RectF(kLeftX0, kHTop, kLeftX1, kHBottom), x, y)) return kHitMusic;
    if (In(D2D1::RectF(kMidX0, kHTop, kMidX1, kHTop + 50), x, y)) return kHitEyes;
    if (In(D2D1::RectF(kMidX0, kHTop + 50, kMidX1, kHBottom), x, y)) return kHitFocus;
    int n = std::min(static_cast<int>(favs_.size()), kHomeFavs);
    int tiles = std::min(n + 1, kHomeFavs);
    for (int i = 0; i < tiles; ++i)
        if (In(FavRect(i), x, y)) return kHitFav + i;
    return kHitNone;
}

void Island::HomeClick(int hit) {
    lastActivity_ = Now();
    int n = std::min(static_cast<int>(favs_.size()), kHomeFavs);
    switch (hit) {
    case kHitPlay: media_.Send(MediaCommand::PlayPause); break;
    case kHitMusic:
        if (snapshot_.hasSession) {
            homeMedia_ = true;  // büyük müzik kartı (kapağa tıklayınca geri döner)
            Sounds::Play(Sounds::Sound::Open);
            Update();
        } else {
            OpenCommand();
        }
        break;
    case kHitEyes: Pet(); break;
    case kHitFocus: ToggleFocus(); break;
    default:
        if (hit >= kHitFav && hit < kHitFav + n) OpenFavorite(hit - kHitFav);
        else if (hit >= kHitFav && hit < kHitFav + kHomeFavs) AddFavoriteFromClipboard();
        break;
    }
    StartAnimation();
}

void Island::SyncFavGlow() {
    for (int i = 0; i <= kFavMax; ++i) {
        bool on = kFavHit + i == hoverTool_ || (i < kHomeFavs && homeHover_ == kHitFav + i);
        favGlow_[i].target = on ? 1.f : 0.f;
    }
    StartAnimation();
}

// ---------------------------------------------------------------------------
// Kapalı hapta albüm ışığı: hapın kenarı kapak renginde, vuruşla atar
// ---------------------------------------------------------------------------

void Island::DrawPillGlow(float w, float h, float r) {
    if (!albumGlow_ || !snapshot_.playing || !snapshot_.art || view_ != View::Media) return;
    float fade = trans_ < 1.f ? EaseOut(trans_) : 1.f;
    float pulse = level_ >= 0 ? beatPulse_ : 0.5f + 0.5f * static_cast<float>(std::sin(time_ * 2.2));
    float a = (0.22f + 0.5f * pulse) * fade;
    D2D1_COLOR_F c = CardAccent();
    const float alphas[3] = {0.7f, 0.35f, 0.15f};
    for (int i = 0; i < 3; ++i) {
        float d = 0.8f + i * 1.6f;
        brush_->SetColor(WithAlpha(c, a * alphas[i]));
        D2D1_ROUNDED_RECT rr{D2D1::RectF(-d, -d, w + d, h + d), r + d, r + d};
        rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.6f);
    }
}

// ---------------------------------------------------------------------------
// Aksesuarlar
// ---------------------------------------------------------------------------

void Island::DrawGuti(float cx, float cy, float u, float bob) {
    eyes_.Draw(rt_.Get(), d2d_.Get(), brush_.Get(), roundStroke_.Get(), cx, cy, u, bob);
    DrawAccessories(cx, cy, u, bob);
}

void Island::DrawAccessories(float cx, float cy, float u, float bob) {
    if (!accessories_) return;
    int aff = companion_.Affection();
    bool sleepy = eyes_.CurrentMood() == Eyes::Mood::Sleepy;
    bool wet = weather_.ok && (Weather::IsRain(weather_.code) || Weather::IsStorm(weather_.code));
    bool phones = snapshot_.playing && aff >= kAccThresholds[0];
    bool scarf = weather_.ok && weather_.temp <= 8 && aff >= kAccThresholds[1];
    bool cap = sleepy && aff >= kAccThresholds[2];
    bool glasses = !sleepy && weather_.ok && Weather::IsClear(weather_.code) && weather_.day && weather_.temp >= 26 &&
                   aff >= kAccThresholds[3];
    bool umbrella = wet && !phones && !sleepy;
    float ey = cy + bob;

    if (scarf) {
        float y0 = ey + 8.5f * u;
        brush_->SetColor(Rgba(0.92f, 0.30f, 0.33f));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(cx - 15 * u, y0, cx + 15 * u, y0 + 4 * u), 2 * u, 2 * u},
                                  brush_.Get());
        rt_->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1::RectF(cx + 7 * u, y0 + 2 * u, cx + 11 * u, y0 + 9 * u), 1.6f * u, 1.6f * u},
            brush_.Get());
        brush_->SetColor(Rgba(1, 1, 1, 0.65f));
        for (float sx : {-8.f, 0.f, 8.f})
            rt_->DrawLine(D2D1::Point2F(cx + sx * u, y0 + 0.8f * u), D2D1::Point2F(cx + sx * u, y0 + 3.2f * u),
                          brush_.Get(), 1.2f * u);
    }
    if (glasses) {
        for (int side = -1; side <= 1; side += 2) {
            float ex = cx + side * 11.5f * u;
            D2D1_ROUNDED_RECT lens{D2D1::RectF(ex - 6.5f * u, ey - 5 * u, ex + 6.5f * u, ey + 4.5f * u), 3 * u, 3 * u};
            brush_->SetColor(Rgba(0.04f, 0.04f, 0.07f));
            rt_->FillRoundedRectangle(lens, brush_.Get());
            brush_->SetColor(Rgba(1, 1, 1, 0.85f));
            rt_->DrawRoundedRectangle(lens, brush_.Get(), 1.1f * u);
            brush_->SetColor(Rgba(1, 1, 1, 0.5f));
            rt_->DrawLine(D2D1::Point2F(ex - 3.5f * u, ey - 2.5f * u), D2D1::Point2F(ex - 1.5f * u, ey - 3.6f * u),
                          brush_.Get(), 1.f * u, roundStroke_.Get());
        }
        brush_->SetColor(Rgba(1, 1, 1, 0.85f));
        rt_->DrawLine(D2D1::Point2F(cx - 5 * u, ey - 2.5f * u), D2D1::Point2F(cx + 5 * u, ey - 2.5f * u), brush_.Get(),
                      1.1f * u);
    }
    if (cap) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2d_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            D2D1_POINT_2F a{cx - 8 * u, ey - 6 * u}, b{cx + 17 * u, ey - 4.5f * u}, tip{cx + 22 * u, ey + 3 * u};
            sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - 2 * u, ey - 20 * u),
                                                D2D1::Point2F(cx + 18 * u, ey - 18 * u), tip));
            sink->AddLine(b);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            brush_->SetColor(Rgba(0.36f, 0.52f, 0.98f));
            rt_->FillGeometry(path.Get(), brush_.Get());
            brush_->SetColor(Rgba(1, 1, 1, 0.95f));
            rt_->DrawLine(a, b, brush_.Get(), 2.6f * u, roundStroke_.Get());
            rt_->FillEllipse(D2D1::Ellipse(tip, 2.8f * u, 2.8f * u), brush_.Get());
        }
    }
    if (phones) {
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        float side = 19.f * u;
        if (SUCCEEDED(d2d_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(cx - side, ey - 1 * u), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - side, ey - 17 * u),
                                                D2D1::Point2F(cx + side, ey - 17 * u),
                                                D2D1::Point2F(cx + side, ey - 1 * u)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            brush_->SetColor(Rgba(0.86f, 0.88f, 0.92f, 0.95f));
            rt_->DrawGeometry(path.Get(), brush_.Get(), 2.f * u, roundStroke_.Get());
        }
        brush_->SetColor(albumTint_ && snapshot_.art ? CardAccent() : Rgba(0.86f, 0.88f, 0.92f));
        for (int s = -1; s <= 1; s += 2) {
            float x = cx + s * side;
            rt_->FillRoundedRectangle(
                D2D1_ROUNDED_RECT{D2D1::RectF(x - 2.8f * u, ey - 4.8f * u, x + 2.8f * u, ey + 4.8f * u), 2.2f * u, 2.2f * u},
                brush_.Get());
        }
    }
    if (umbrella) {
        float ux = cx + 25 * u, uy = ey - 5 * u;
        ComPtr<ID2D1PathGeometry> path;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(d2d_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(ux - 7 * u, uy), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(ux + 7 * u, uy), D2D1::SizeF(7 * u, 6 * u), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            brush_->SetColor(Rgba(0.42f, 0.72f, 1.f));
            rt_->FillGeometry(path.Get(), brush_.Get());
        }
        brush_->SetColor(Rgba(0.86f, 0.88f, 0.92f));
        rt_->DrawLine(D2D1::Point2F(ux, uy), D2D1::Point2F(ux, uy + 9 * u), brush_.Get(), 1.3f * u, roundStroke_.Get());
        rt_->DrawLine(D2D1::Point2F(ux, uy + 9 * u), D2D1::Point2F(ux - 2.5f * u, uy + 9 * u), brush_.Get(), 1.3f * u,
                      roundStroke_.Get());
    }
}

void Island::CheckAccessoryUnlocks() {
    int aff = companion_.Affection();
    int level = 0;
    for (int t : kAccThresholds)
        if (aff >= t) ++level;
    DWORD stored = ReadSetting(L"AccLevel", 0xFFFFFFFF);
    if (stored == 0xFFFFFFFF || static_cast<int>(stored) > level) {
        WriteSetting(L"AccLevel", static_cast<DWORD>(level));  // ilk açılış: kutlama yok
        return;
    }
    if (level > static_cast<int>(stored)) {
        WriteSetting(L"AccLevel", static_cast<DWORD>(level));
        if (!accessories_ || fullscreen_ || userHidden_) return;
        alertLabel_ = std::wstring(L"Yeni aksesuar: ") + kAccNames[level - 1];
        ShowAlert(AlertKind::Copied, 0, 3500);
        MoodWithSound(Eyes::Mood::Love, 2.2);
        height_.velocity += 120.f;
    }
}

// ---------------------------------------------------------------------------
// Hava durumu
// ---------------------------------------------------------------------------

void Island::RefreshWeather() {
    SetTimer(hwnd_, kTimerWeather, 30 * 60 * 1000, nullptr);
    if (weatherOn_) Weather::Refresh(hwnd_, city_);
}

void Island::OnWeather(Weather::Info* raw) {
    std::unique_ptr<Weather::Info> info(raw);
    if (!info || !info->ok) return;  // eskisi kalsın
    bool first = !weather_.ok;
    bool wasWet = weather_.ok && (Weather::IsRain(weather_.code) || Weather::IsStorm(weather_.code));
    weather_ = *info;
    double now = Now();
    using M = Eyes::Mood;
    if (eyes_.CurrentMood() == M::Normal && !fullscreen_) {
        if (Weather::IsStorm(weather_.code)) eyes_.SetMood(M::Surprised, now, 1.0);  // gök gürültüsü!
        else if (Weather::IsRain(weather_.code) && !wasWet && !first) eyes_.SetMood(M::Sad, now, 1.2);
    }
    StartAnimation();
}

void Island::OnWeatherMood(double now) {
    // Her saniye: soğukta arada bir titrer, sıcakta terler.
    if (!weather_.ok || !weatherOn_ || fullscreen_ || hidden_) return;
    using M = Eyes::Mood;
    if (eyes_.CurrentMood() != M::Normal) return;
    if ((weather_.temp <= 2 || Weather::IsSnow(weather_.code)) && now - lastShiver_ > 120) {
        lastShiver_ = now;
        shake_.velocity += 90.f;  // brrr
        StartAnimation();
    } else if (weather_.temp >= 31 && now - lastHeat_ > 20 * 60) {
        lastHeat_ = now;
        eyes_.SetMood(M::Sweaty, now, 2.5);
        StartAnimation();
    }
}

void Island::SetCity(std::wstring city) {
    size_t a = city.find_first_not_of(L" \t"), b = city.find_last_not_of(L" \t");
    city = a == std::wstring::npos ? std::wstring() : city.substr(a, b - a + 1);
    if (city.size() > 60) city.resize(60);
    city_ = city;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, L"City", 0, REG_SZ, reinterpret_cast<const BYTE*>(city_.c_str()),
                       static_cast<DWORD>((city_.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    weather_ = {};
    eyes_.SetMood(Eyes::Mood::Curious, Now(), 1.0);  // gökyüzüne bakar
    RefreshWeather();
    StartAnimation();
}
