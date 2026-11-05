#pragma once
#include <d2d1.h>
#include <random>

#include "Spring.h"

// Adanın "yüzü": siyah hapın içinde iki beyaz robot göz.
//
// Duygu sistemi: her duygu (Mood) bir "stil" tablosundan gelir: göz açıklığı, üst/alt göz
// kapağı, kaş açısı, yanak kızarması, kalp göz, gözyaşı, göz kırpma... Bu değerler yaylarla
// yumuşakça geçiş yapar. Yeni bir duygu eklemek = enum'a bir değer + tabloya bir satır.
//
// Sadece bir şey değişirken çizim ister; boşta işlemci harcamaz.
class Eyes {
public:
    enum class Mood {
        Normal,
        Happy,      // ^ ^
        Laugh,      // ^ ^ + kıkırdayarak zıplama
        Love,       // kalp gözler + yanak
        Cute,       // kocaman parlak gözler + yanak (şirinlik)
        Angry,      // çatık kaşlar
        Sad,        // düşük kaşlar + gözyaşı
        Surprised,  // büyük gözler
        Wink,       // göz kırpma
        Curious,    // bir göz büyük, bir göz kısık
        Glance,     // etrafa bakınma
        Dizzy,      // > <
        Sleepy,     // kapalı gözler
        Yawn,       // esneme (uzun çalışınca)
        Sulk,       // küsme: başını çevirir
        Focused,    // odak modu: ciddi bakış
        Sweaty,     // ter damlası (bilgisayar zorlanınca)
        Count
    };

    Eyes();

    // Fare konumu: -1..1 aralığında, adanın merkezine göre.
    void LookAt(float nx, float ny);
    void SetMood(Mood mood, double now, double duration);  // duration <= 0: süresiz
    Mood CurrentMood() const { return mood_; }
    bool IsAwake() const { return mood_ != Mood::Sleepy; }
    // Dostluk puanı (0-100): kendi kendine şirinliklerin sıklığını ve türünü belirler.
    void SetAffection(int a) { affection_ = a; }
    // Gözleri bir süreliğine renklendir (ör. renk seçicide seçilen renk). nullptr: beyaza dön.
    void SetTint(const D2D1_COLOR_F* c) {
        tinted_ = c != nullptr;
        if (c) tint_ = *c;
    }

    // Yavaş zamanlayıcıdan çağrılır (100 ms). Bir animasyon başlatmak gerekiyorsa true.
    bool Tick(double now);
    // Her karede çağrılır. Hâlâ hareket varsa true.
    bool Step(double now, float dt);

    // cx, cy: iki gözün ortası (DIP). unit: boyut çarpanı (1 = kompakt ada).
    void Draw(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
              ID2D1StrokeStyle* roundStroke, float cx, float cy, float unit, float bob) const;

private:
    struct Style {
        float open;       // göz yüksekliği çarpanı (1 normal)
        float lidTop;     // üst kapak ne kadar iniyor (0..1)
        float brow;       // kaş açısı: + kızgın (iç taraf aşağı), - üzgün (dış taraf aşağı)
        float lidBottom;  // alt kapak (gülümseyen yanak) (0..1)
        float happy;      // ^ ^ kemer gözler
        float heart;      // kalp gözler
        float blush;      // yanak kızarması
        float tear;       // gözyaşı
        float wink;       // sağ göz kırpık
        float sparkle;    // göz parıltısı
        float asym;       // + sol göz büyük, sağ göz küçük
        float sweat;      // ter damlası
        bool follow;      // fareyi takip etsin mi
        bool blinks;      // kendi kendine kırpsın mı
        bool animated;    // sürekli kare ister (zıplama, bakınma)
    };
    static const Style& StyleOf(Mood m);
    void UpdateTargets(double now);

    Spring lookX_, lookY_, open_, lidTop_, brow_, lidBottom_, happy_, heart_, blush_, tear_,
        wink_, sparkle_, asym_, dizzy_, sweat_;
    int affection_ = 50;
    Mood mood_ = Mood::Normal;
    bool tinted_ = false;
    D2D1_COLOR_F tint_{1, 1, 1, 1};
    double moodStart_ = 0;
    double moodUntil_ = 0;
    double nextBlink_ = 0;
    double blinkEnd_ = 0;
    double nextIdleAct_ = 0;
    double animTime_ = 0;
    float wantX_ = 0, wantY_ = 0;
    std::mt19937 rng_;
};
