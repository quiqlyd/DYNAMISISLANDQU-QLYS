#include "Eyes.h"

#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

Eyes::Eyes() : rng_(std::random_device{}()) {
    lookX_.stiffness = lookY_.stiffness = 140.f;
    lookX_.damping = lookY_.damping = 18.f;
    open_.stiffness = 900.f;   // kırpma hızlı olsun
    open_.damping = 42.f;
    happy_.stiffness = dizzy_.stiffness = 260.f;
    happy_.damping = dizzy_.damping = 26.f;
    open_.Snap(1.f);
}

void Eyes::LookAt(float nx, float ny) {
    wantX_ = std::clamp(nx, -1.f, 1.f);
    wantY_ = std::clamp(ny, -1.f, 1.f);
}

void Eyes::SetMood(Mood mood, double now, double duration) {
    mood_ = mood;
    moodUntil_ = duration > 0 ? now + duration : 0;
    blinkEnd_ = 0;
    UpdateTargets(now);
}

void Eyes::UpdateTargets(double now) {
    if (moodUntil_ > 0 && now >= moodUntil_) {
        mood_ = Mood::Normal;
        moodUntil_ = 0;
    }

    bool blinking = blinkEnd_ > now;
    float open = 1.f;
    switch (mood_) {
    case Mood::Normal: open = 1.f; break;
    case Mood::Happy: open = 1.f; break;
    case Mood::Dizzy: open = 1.f; break;
    case Mood::Sleepy: open = 0.12f; break;
    case Mood::Surprised: open = 1.28f; break;
    }
    if (blinking) open = 0.06f;
    open_.target = open;
    happy_.target = mood_ == Mood::Happy ? 1.f : 0.f;
    dizzy_.target = mood_ == Mood::Dizzy ? 1.f : 0.f;

    bool follows = mood_ == Mood::Normal || mood_ == Mood::Surprised || mood_ == Mood::Happy;
    lookX_.target = follows ? wantX_ : 0.f;
    lookY_.target = follows ? wantY_ : (mood_ == Mood::Sleepy ? 0.4f : 0.f);
}

bool Eyes::Tick(double now) {
    if (nextBlink_ == 0) nextBlink_ = now + 2.0;
    bool canBlink = mood_ == Mood::Normal || mood_ == Mood::Surprised;
    if (canBlink && now >= nextBlink_ && blinkEnd_ <= now) {
        blinkEnd_ = now + 0.11;
        std::uniform_real_distribution<double> gap(2.5, 6.0);
        std::uniform_int_distribution<int> coin(0, 4);
        // Ara sıra çift kırpma: daha canlı görünür.
        nextBlink_ = coin(rng_) == 0 ? now + 0.28 : now + gap(rng_);
    }

    float ox = open_.target, lx = lookX_.target, ly = lookY_.target, hx = happy_.target,
          dx = dizzy_.target;
    UpdateTargets(now);
    auto diff = [](float a, float b) { return std::fabs(a - b) > 0.01f; };
    return diff(ox, open_.target) || diff(lx, lookX_.target) || diff(ly, lookY_.target) ||
           diff(hx, happy_.target) || diff(dx, dizzy_.target) || blinkEnd_ > now;
}

bool Eyes::Step(double now, float dt) {
    UpdateTargets(now);
    bool moving = false;
    moving |= lookX_.Step(dt);
    moving |= lookY_.Step(dt);
    moving |= open_.Step(dt);
    moving |= happy_.Step(dt);
    moving |= dizzy_.Step(dt);
    return moving || blinkEnd_ > now;
}

void Eyes::Draw(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
                ID2D1StrokeStyle* roundStroke, float cx, float cy, float u, float bob) const {
    const float eyeW = 9.f * u, eyeH = 14.f * u, spacing = 23.f * u;
    const D2D1_COLOR_F color = D2D1::ColorF(0.93f, 0.97f, 1.f, 1.f);

    float happy = std::clamp(happy_.value, 0.f, 1.f);
    float dizzy = std::clamp(dizzy_.value, 0.f, 1.f);
    float open = std::max(0.f, open_.value);

    for (int side = -1; side <= 1; side += 2) {
        float ex = cx + side * spacing / 2 + lookX_.value * 3.5f * u;
        float ey = cy + lookY_.value * 2.5f * u + bob;

        if (dizzy > 0.5f) {
            // > <  : çok dürtülünce başı döner
            float s = 4.5f * u, a = (dizzy - 0.5f) * 2.f;
            brush->SetColor(D2D1::ColorF(color.r, color.g, color.b, a));
            float dir = static_cast<float>(-side);  // sol göz '>', sağ göz '<'
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(ex - dir * s * 0.6f, ey - s), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddLine(D2D1::Point2F(ex + dir * s * 0.6f, ey));
                sink->AddLine(D2D1::Point2F(ex - dir * s * 0.6f, ey + s));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                rt->DrawGeometry(path.Get(), brush, 2.4f * u, roundStroke);
            }
            continue;
        }

        float bodyAlpha = (1.f - happy) * (1.f - dizzy * 2.f);
        if (bodyAlpha > 0.01f) {
            // Şaşırınca uzar ve hafif incelir; kırparken çizgiye döner.
            float h = std::max(2.f * u, eyeH * open);
            float w = eyeW * (open > 1.f ? 1.f - (open - 1.f) * 0.35f : 1.f);
            if (open < 0.3f) w = eyeW * 1.15f;
            float r = std::min(w, h) / 2.f;
            brush->SetColor(D2D1::ColorF(color.r, color.g, color.b, bodyAlpha));
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(ex - w / 2, ey - h / 2, ex + w / 2, ey + h / 2), r, r),
                brush);
        }

        if (happy > 0.01f) {
            // ^ ^ : mutlu göz kemerleri
            brush->SetColor(D2D1::ColorF(color.r, color.g, color.b, happy));
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                float hw = eyeW * 0.75f;
                sink->BeginFigure(D2D1::Point2F(ex - hw, ey + 2.5f * u), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                    D2D1::Point2F(ex, ey - 6.f * u), D2D1::Point2F(ex + hw, ey + 2.5f * u)));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                rt->DrawGeometry(path.Get(), brush, 2.6f * u, roundStroke);
            }
        }
    }
}
