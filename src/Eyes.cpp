#include "Eyes.h"

#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <iterator>

using Microsoft::WRL::ComPtr;

namespace {
// Duygu tablosu. Sıra Mood enum'u ile aynı olmalı.
//                     open  lidT  brow  lidB  happy heart blush tear  wink  spark asym   sweat
const float kRows[][12] = {
    /* Normal    */ {1.00f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Happy     */ {1.00f, 0.00f, 0.00f, 0.00f, 1.0f, 0.0f, 0.3f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Laugh     */ {1.00f, 0.00f, 0.00f, 0.00f, 1.0f, 0.0f, 0.6f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Love      */ {1.00f, 0.00f, 0.00f, 0.00f, 0.0f, 1.0f, 0.8f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Cute      */ {1.18f, 0.00f, 0.00f, 0.22f, 0.0f, 0.0f, 0.9f, 0.0f, 0.0f, 1.0f, 0.00f, 0.0f},
    /* Angry     */ {0.95f, 0.42f, 0.85f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Sad       */ {0.95f, 0.30f, -0.8f, 0.00f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Surprised */ {1.28f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.4f, 0.00f, 0.0f},
    /* Wink      */ {1.00f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.4f, 0.0f, 1.0f, 0.0f, 0.00f, 0.0f},
    /* Curious   */ {1.00f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.00f, 0.0f},
    /* Glance    */ {1.00f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Dizzy     */ {1.00f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Sleepy    */ {0.12f, 0.00f, 0.00f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Yawn      */ {0.30f, 0.15f, -0.4f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Sulk      */ {0.90f, 0.32f, 0.45f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Focused   */ {0.85f, 0.26f, 0.30f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 0.0f},
    /* Sweaty    */ {0.85f, 0.22f, -0.5f, 0.00f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.00f, 1.0f},
};
//                         follow blinks animated
const bool kFlags[][3] = {
    /* Normal    */ {true, true, false},
    /* Happy     */ {true, false, false},
    /* Laugh     */ {false, false, true},
    /* Love      */ {true, false, true},
    /* Cute      */ {true, true, false},
    /* Angry     */ {true, true, false},
    /* Sad       */ {false, true, true},
    /* Surprised */ {true, true, false},
    /* Wink      */ {true, false, false},
    /* Curious   */ {true, true, false},
    /* Glance    */ {false, true, true},
    /* Dizzy     */ {false, false, false},
    /* Sleepy    */ {false, false, false},
    /* Yawn      */ {false, false, false},
    /* Sulk      */ {false, true, false},
    /* Focused   */ {true, true, false},
    /* Sweaty    */ {true, true, true},
};
static_assert(sizeof(kRows) / sizeof(kRows[0]) == static_cast<size_t>(Eyes::Mood::Count));
static_assert(sizeof(kFlags) / sizeof(kFlags[0]) == static_cast<size_t>(Eyes::Mood::Count));

constexpr D2D1_COLOR_F kEye{0.93f, 0.97f, 1.f, 1.f};
constexpr D2D1_COLOR_F kBlack{0.f, 0.f, 0.f, 1.f};
constexpr D2D1_COLOR_F kPink{1.f, 0.42f, 0.58f, 1.f};
constexpr D2D1_COLOR_F kTear{0.45f, 0.75f, 1.f, 1.f};

D2D1_COLOR_F A(D2D1_COLOR_F c, float a) { c.a *= std::clamp(a, 0.f, 1.f); return c; }

void Tune(Spring& s, float k, float d) { s.stiffness = k; s.damping = d; }
}  // namespace

const Eyes::Style& Eyes::StyleOf(Mood m) {
    static Style styles[static_cast<int>(Mood::Count)];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < static_cast<int>(Mood::Count); ++i) {
            const float* r = kRows[i];
            styles[i] = Style{r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
                              r[11], kFlags[i][0], kFlags[i][1], kFlags[i][2]};
        }
        built = true;
    }
    return styles[static_cast<int>(m)];
}

Eyes::Eyes() : rng_(std::random_device{}()) {
    Tune(lookX_, 140, 18);
    Tune(lookY_, 140, 18);
    Tune(open_, 900, 42);  // kırpma hızlı olsun
    for (Spring* s : {&lidTop_, &brow_, &lidBottom_, &happy_, &heart_, &blush_, &tear_, &wink_,
                      &sparkle_, &asym_, &dizzy_, &sweat_})
        Tune(*s, 260, 26);
    Tune(heart_, 380, 20);  // kalpler hafif "pıt" diye çıksın
    open_.Snap(1.f);
}

void Eyes::LookAt(float nx, float ny) {
    wantX_ = std::clamp(nx, -1.f, 1.f);
    wantY_ = std::clamp(ny, -1.f, 1.f);
}

void Eyes::SetMood(Mood mood, double now, double duration) {
    mood_ = mood;
    moodStart_ = now;
    moodUntil_ = duration > 0 ? now + duration : 0;
    blinkEnd_ = 0;
    if (mood == Mood::Love) heart_.velocity += 6.f;  // kalp pıtlaması
    UpdateTargets(now);
}

void Eyes::UpdateTargets(double now) {
    if (moodUntil_ > 0 && now >= moodUntil_) {
        mood_ = Mood::Normal;
        moodUntil_ = 0;
    }
    const Style& s = StyleOf(mood_);

    float open = s.open;
    if (blinkEnd_ > now) open = 0.06f;
    open_.target = open;
    lidTop_.target = s.lidTop;
    brow_.target = s.brow;
    lidBottom_.target = s.lidBottom;
    happy_.target = s.happy;
    heart_.target = s.heart;
    blush_.target = s.blush;
    tear_.target = s.tear;
    wink_.target = s.wink;
    sparkle_.target = s.sparkle;
    asym_.target = s.asym;
    sweat_.target = s.sweat;
    dizzy_.target = mood_ == Mood::Dizzy ? 1.f : 0.f;

    if (mood_ == Mood::Glance) {
        // Sağa sola bakınır: sol - sağ - orta
        double t = now - moodStart_;
        lookX_.target = t < 0.6 ? -1.f : (t < 1.3 ? 1.f : 0.f);
        lookY_.target = -0.3f;
    } else if (mood_ == Mood::Sulk) {
        lookX_.target = 1.f;  // küstü: başını çevirir
        lookY_.target = -0.2f;
    } else if (s.follow) {
        lookX_.target = wantX_;
        lookY_.target = wantY_;
    } else {
        lookX_.target = 0.f;
        lookY_.target = mood_ == Mood::Sleepy || mood_ == Mood::Sad ? 0.5f
                        : mood_ == Mood::Yawn ? -0.4f : 0.f;
    }
}

bool Eyes::Tick(double now) {
    if (nextBlink_ == 0) nextBlink_ = now + 2.0;
    if (nextIdleAct_ == 0) nextIdleAct_ = now + 20.0;

    const Style& s = StyleOf(mood_);
    if (s.blinks && now >= nextBlink_ && blinkEnd_ <= now) {
        blinkEnd_ = now + 0.11;
        std::uniform_real_distribution<double> gap(2.5, 6.0);
        std::uniform_int_distribution<int> coin(0, 4);
        // Ara sıra çift kırpma: daha canlı görünür.
        nextBlink_ = coin(rng_) == 0 ? now + 0.28 : now + gap(rng_);
    }

    // Kendi kendine şirinlik: arada bir göz kırpar, bakınır, meraklanır.
    if (mood_ == Mood::Normal && now >= nextIdleAct_) {
        // Dostluk arttıkça daha sık ve daha sevgi dolu şirinlik yapar.
        std::uniform_int_distribution<int> pick(0, affection_ >= 70 ? 7 : 5);
        switch (pick(rng_)) {
        case 6: SetMood(Mood::Love, now, 1.6); break;
        case 7: SetMood(Mood::Cute, now, 2.2); break;
        case 0: SetMood(Mood::Wink, now, 0.5); break;
        case 1: SetMood(Mood::Glance, now, 1.9); break;
        case 2: SetMood(Mood::Curious, now, 1.8); break;
        case 3: SetMood(Mood::Cute, now, 2.2); break;
        case 4: SetMood(Mood::Happy, now, 1.2); break;
        default: SetMood(Mood::Glance, now, 1.9); break;
        }
        double scale = 1.4 - std::clamp(affection_, 0, 100) / 100.0 * 0.8;  // 1.4x .. 0.6x
        std::uniform_real_distribution<double> gap(25.0 * scale, 60.0 * scale);
        nextIdleAct_ = now + gap(rng_);
    }

    float before[] = {open_.target, lookX_.target, lookY_.target, happy_.target, heart_.target,
                      lidTop_.target, wink_.target, dizzy_.target, asym_.target};
    UpdateTargets(now);
    float after[] = {open_.target, lookX_.target, lookY_.target, happy_.target, heart_.target,
                     lidTop_.target, wink_.target, dizzy_.target, asym_.target};
    for (size_t i = 0; i < std::size(before); ++i)
        if (std::fabs(before[i] - after[i]) > 0.01f) return true;
    return blinkEnd_ > now || StyleOf(mood_).animated;
}

bool Eyes::Step(double now, float dt) {
    UpdateTargets(now);
    animTime_ += dt;
    bool moving = false;
    for (Spring* s : {&lookX_, &lookY_, &open_, &lidTop_, &brow_, &lidBottom_, &happy_, &heart_,
                      &blush_, &tear_, &wink_, &sparkle_, &asym_, &dizzy_, &sweat_})
        moving |= s->Step(dt);
    return moving || blinkEnd_ > now || StyleOf(mood_).animated;
}

namespace {

void FillPath(ID2D1RenderTarget* rt, ID2D1Factory* f, ID2D1Brush* b,
              const D2D1_POINT_2F* pts, int n) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i < n; ++i) sink->AddLine(pts[i]);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->FillGeometry(path.Get(), b);
}

void Heart(ID2D1RenderTarget* rt, ID2D1Factory* f, ID2D1Brush* b, float cx, float cy, float s) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(cx, cy + s * 0.85f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx - s * 1.25f, cy + s * 0.05f),
                                        D2D1::Point2F(cx - s * 0.85f, cy - s * 1.05f),
                                        D2D1::Point2F(cx, cy - s * 0.42f)));
    sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(cx + s * 0.85f, cy - s * 1.05f),
                                        D2D1::Point2F(cx + s * 1.25f, cy + s * 0.05f),
                                        D2D1::Point2F(cx, cy + s * 0.85f)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    rt->FillGeometry(path.Get(), b);
}

void Arc(ID2D1RenderTarget* rt, ID2D1Factory* f, ID2D1Brush* b, ID2D1StrokeStyle* st, float ex,
         float ey, float hw, float rise, float width) {
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    sink->BeginFigure(D2D1::Point2F(ex - hw, ey + rise * 0.4f), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(ex, ey - rise),
                                                          D2D1::Point2F(ex + hw, ey + rise * 0.4f)));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    rt->DrawGeometry(path.Get(), b, width, st);
}

}  // namespace

void Eyes::Draw(ID2D1RenderTarget* rt, ID2D1Factory* f, ID2D1SolidColorBrush* brush,
                ID2D1StrokeStyle* round, float cx, float cy, float u, float bob) const {
    const float eyeW = 9.f * u, eyeH = 14.f * u, spacing = 23.f * u;

    auto c01 = [](float v) { return std::clamp(v, 0.f, 1.f); };
    float happy = c01(happy_.value), heart = std::clamp(heart_.value, 0.f, 1.3f),
          dizzy = c01(dizzy_.value), blush = c01(blush_.value), tear = c01(tear_.value),
          wink = c01(wink_.value), sparkle = c01(sparkle_.value), lidTop = c01(lidTop_.value),
          lidBottom = c01(lidBottom_.value), brow = std::clamp(brow_.value, -1.f, 1.f),
          asym = std::clamp(asym_.value, -1.f, 1.f), open = std::max(0.f, open_.value),
          sweat = c01(sweat_.value);

    // Gülerken kıkır kıkır zıplar.
    if (mood_ == Mood::Laugh) bob += static_cast<float>(std::sin(animTime_ * 28.0)) * 1.4f * u;
    // Aşıkken kalpler küçük küçük atar.
    float beat = mood_ == Mood::Love ? 1.f + 0.08f * static_cast<float>(std::sin(animTime_ * 9.0)) : 1.f;

    for (int side = -1; side <= 1; side += 2) {
        float ex = cx + side * spacing / 2 + lookX_.value * 3.5f * u;
        float ey = cy + lookY_.value * 2.5f * u + bob;
        float sideScale = 1.f - side * asym * 0.18f;  // meraklı: sol büyük, sağ kısık
        float inner = static_cast<float>(-side);      // iç taraf yönü (burna doğru)

        // Yanak kızarması
        if (blush > 0.01f) {
            brush->SetColor(A(kPink, blush * 0.45f));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ex - inner * 4.f * u, ey + eyeH * 0.62f),
                                          4.2f * u, 2.2f * u),
                            brush);
        }

        if (dizzy > 0.5f) {
            // > <
            float s = 4.5f * u, a = (dizzy - 0.5f) * 2.f, d = inner;
            brush->SetColor(A(kEye, a));
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(f->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(ex - d * s * 0.6f, ey - s), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(ex + d * s * 0.6f, ey));
                sink->AddLine(D2D1::Point2F(ex - d * s * 0.6f, ey + s));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                rt->DrawGeometry(path.Get(), brush, 2.4f * u, round);
            }
            continue;
        }

        bool winked = side == 1 && wink > 0.5f;
        float arcAlpha = std::max(happy, winked ? (wink - 0.5f) * 2.f : 0.f);
        float heartAlpha = c01(heart);
        float bodyAlpha = (1.f - happy) * (1.f - dizzy * 2.f) * (1.f - heartAlpha) *
                          (winked ? 1.f - (wink - 0.5f) * 2.f : 1.f);

        if (bodyAlpha > 0.01f) {
            float h = std::max(2.f * u, eyeH * open * sideScale);
            float w = eyeW * sideScale * (open > 1.f ? 1.f - (open - 1.f) * 0.3f : 1.f);
            if (open < 0.3f) w = eyeW * 1.15f;
            float r = std::min(w, h) / 2.f;
            float top = ey - h / 2, bottom = ey + h / 2;
            brush->SetColor(A(kEye, bodyAlpha));
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(ex - w / 2, top, ex + w / 2, bottom), r, r), brush);

            // Parıltı: gözün içinde küçük siyah nokta + minik ikinci nokta (anime gözü gibi)
            if (sparkle > 0.01f && h > 6 * u) {
                brush->SetColor(A(kBlack, sparkle * bodyAlpha));
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ex - w * 0.16f, top + h * 0.3f),
                                              w * 0.2f, w * 0.2f), brush);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ex + w * 0.14f, top + h * 0.52f),
                                              w * 0.1f, w * 0.1f), brush);
            }

            // Üst göz kapağı (kaş): kızgınken iç taraf, üzgünken dış taraf aşağı iner.
            if (lidTop > 0.01f) {
                float drop = h * lidTop;
                float yInner = top + drop * (1.f + brow * 0.9f);
                float yOuter = top + drop * (1.f - brow * 0.9f);
                float xi = ex + inner * (w / 2 + 1), xo = ex - inner * (w / 2 + 1);
                D2D1_POINT_2F pts[] = {{xo, top - 3}, {xi, top - 3}, {xi, yInner}, {xo, yOuter}};
                brush->SetColor(kBlack);
                FillPath(rt, f, brush, pts, 4);
            }
            // Alt kapak: yanaklar gözü yukarı iter (şirin gülümseme).
            if (lidBottom > 0.01f) {
                float rise = h * lidBottom;
                ComPtr<ID2D1PathGeometry> path;
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(f->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                    float l = ex - w / 2 - 1, rr = ex + w / 2 + 1;
                    sink->BeginFigure(D2D1::Point2F(l, bottom + 3), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(l, bottom - rise * 0.3f));
                    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                        D2D1::Point2F(ex, bottom - rise * 1.7f), D2D1::Point2F(rr, bottom - rise * 0.3f)));
                    sink->AddLine(D2D1::Point2F(rr, bottom + 3));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    brush->SetColor(kBlack);
                    rt->FillGeometry(path.Get(), brush);
                }
            }
        }

        if (heartAlpha > 0.01f) {
            brush->SetColor(A(kPink, heartAlpha));
            Heart(rt, f, brush, ex, ey, eyeW * 0.62f * std::max(0.3f, heart) * beat);
        }

        if (arcAlpha > 0.01f) {
            brush->SetColor(A(kEye, arcAlpha));
            Arc(rt, f, brush, round, ex, ey, eyeW * 0.75f, 6.f * u, 2.6f * u);
        }

        // Ter damlası: sağ gözün üst dış köşesinde, yavaşça kayar.
        if (sweat > 0.01f && side == 1) {
            float t = static_cast<float>(std::fmod(animTime_ * 0.5, 1.0));
            float sx = ex + eyeW * 0.9f, sy = ey - eyeH * 0.55f + t * 3.f * u;
            brush->SetColor(A(kTear, sweat * 0.9f));
            D2D1_POINT_2F drop[] = {{sx, sy - 2.4f * u}, {sx + 1.5f * u, sy + 0.5f * u},
                                    {sx - 1.5f * u, sy + 0.5f * u}};
            FillPath(rt, f, brush, drop, 3);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(sx, sy + 0.8f * u), 1.5f * u, 1.5f * u), brush);
        }

        // Gözyaşı: dış köşeden süzülen damla (sadece bir gözde).
        if (tear > 0.01f && side == 1) {
            float t = static_cast<float>(std::fmod(animTime_ * 0.8, 1.0));
            float tx = ex - inner * eyeW * 0.55f, ty = ey + eyeH * 0.45f + t * 6.f * u;
            brush->SetColor(A(kTear, tear * (1.f - t * 0.7f)));
            D2D1_POINT_2F drop[] = {{tx, ty - 2.6f * u}, {tx + 1.6f * u, ty + 0.6f * u},
                                    {tx - 1.6f * u, ty + 0.6f * u}};
            FillPath(rt, f, brush, drop, 3);
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(tx, ty + 0.8f * u), 1.6f * u, 1.6f * u), brush);
        }
    }
}
