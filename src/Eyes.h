#pragma once
#include <d2d1.h>
#include <random>

#include "Spring.h"

// Adanın "yüzü": siyah hapın içinde iki beyaz robot göz.
// Göz kırpar, fareyi takip eder, sevilince mutlu olur, dürtülünce başı döner, uzun süre
// kimse yokken uyur. Sadece bir şey değişirken çizim ister; boşta işlemci harcamaz.
class Eyes {
public:
    enum class Mood { Normal, Happy, Dizzy, Sleepy, Surprised };

    Eyes();

    // Fare konumu: -1..1 aralığında, adanın merkezine göre.
    void LookAt(float nx, float ny);
    void SetMood(Mood mood, double now, double duration);  // duration <= 0: süresiz
    Mood CurrentMood() const { return mood_; }

    // Yavaş zamanlayıcıdan çağrılır (100 ms). Bir animasyon başlatmak gerekiyorsa true.
    bool Tick(double now);
    // Her karede çağrılır. Hâlâ hareket varsa true.
    bool Step(double now, float dt);

    // cx, cy: iki gözün ortası (DIP). unit: boyut çarpanı (1 = kompakt ada).
    void Draw(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
              ID2D1StrokeStyle* roundStroke, float cx, float cy, float unit, float bob) const;

private:
    void UpdateTargets(double now);

    Spring lookX_, lookY_, open_, happy_, dizzy_;
    Mood mood_ = Mood::Normal;
    double moodUntil_ = 0;
    double nextBlink_ = 0;
    double blinkEnd_ = 0;
    float wantX_ = 0, wantY_ = 0;
    std::mt19937 rng_;
};
