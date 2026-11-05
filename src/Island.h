#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <string>

#include "Companion.h"
#include "Eyes.h"
#include "MediaService.h"
#include "Spring.h"
#include "Watchers.h"

// Ekranın üst ortasında duran, şekil değiştiren siyah hap.
// Katmanlı (layered) pencere + Direct2D: saydam kısımlar tıklamayı alttaki pencereye geçirir.
class Island {
public:
    bool Create(HINSTANCE instance);
    int Run();

private:
    enum class View { Idle, Media, ExpandedMedia, ExpandedIdle, Alert, Timer };
    enum class AlertKind { Charging, Unplugged, LowBattery, UpdateAvailable, Updating, UpToDate, UpdateFailed, TimerDone, FocusDone, BreakDone,
                           CpuBusy, MemoryFull, Offline, Online, Download };

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp);

    // Yerleşim / durum
    View TargetView() const;
    void Update();
    void SetTargets(View v);
    void StartAnimation();
    void OnAnimationTick();
    bool NeedsContinuousFrames() const;
    void UpdateMonitor();
    void UpdateDpi(UINT dpi);
    void CheckFullscreen();
    void ShowAlert(AlertKind kind, int value = 0, UINT durationMs = 3000);
    void ApplyVisibility();
    void ToggleUserHidden();
    void OnPowerChanged(bool initial);
    void OnHealthTick();
    void OnPrivacyTick();
    void OnClipboardUpdate();
    void OnDownload(const std::wstring& path);
    void OpenDownloadFolder();
    void StartDownloadDrag();
    bool IsDownloadAlert() const { return alertActive_ && alertKind_ == AlertKind::Download; }

    // Çizim
    bool EnsureResources();
    void DiscardDeviceResources();
    void Render();
    void DrawIdle(float w, float h);
    void DrawCompactMedia(float w, float h);
    void DrawExpandedMedia(float w, float h);
    void DrawExpandedIdle(float w, float h);
    void DrawAlert(float w, float h);
    void DrawTimer(float w, float h);
    void DrawDownload(float w, float h);
    void DrawPrivacyDots(float right, float cy);
    std::wstring TimerText() const;
    D2D1_COLOR_F TimerColor() const;
    void DrawArt(D2D1_RECT_F rect, float radius);
    void DrawBars(float right, float centerY, float maxHeight, float barWidth);
    void Text(const wchar_t* text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                  D2D1_COLOR_F color);
    void DrawBattery(float x, float y, float w, float h, int percent, bool charging);

    // Girdi
    void OnClick(float x, float y);
    void OnLookTimer();
    void Pet();
    void MoodWithSound(Eyes::Mood mood, double duration);
    void OnCompanionTick();
    void SetHover(bool hover);
    double Now() const;
    bool ViewShowsEyes() const;

    // Tepsi
    void AddTrayIcon();
    void ShowTrayMenu();
    static bool IsAutostartEnabled();
    static void SetAutostart(bool enable);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;

    // Monitör / DPI
    float scale_ = 1.f;
    RECT monitor_{};
    bool hidden_ = false;      // pencere şu an gizli mi
    bool userHidden_ = false;  // kullanıcı gizledi
    bool fullscreen_ = false;  // tam ekran uygulama var

    // GDI yüzeyi
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    int dibW_ = 0, dibH_ = 0;

    // Direct2D / DirectWrite
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> artBitmap_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fClock_, fTitle_, fArtist_, fSmall_, fSmallRight_,
        fBigClock_, fDate_, fTiny_, fTinyRight_, fIcon_, fIconBig_, fIconSmall_, fAlert_, fAlertRight_;

    // Animasyon
    Spring width_, height_, radius_, shake_;
    float contentAlpha_ = 1.f;
    View view_ = View::Idle;
    bool animating_ = false;
    UINT animInterval_ = 0;
    LARGE_INTEGER lastTick_{}, qpcFreq_{};
    double time_ = 0;  // ekolayzer çubukları için

    // Durum
    MediaService media_;
    MediaSnapshot snapshot_;
    uint64_t artVersion_ = ~0ull;
    D2D1_COLOR_F accent_{0.62f, 0.80f, 1.f, 1.f};
    bool hover_ = false;
    bool trackingMouse_ = false;
    bool hoverPending_ = false;
    double hoverSince_ = 0;
    double outsideSince_ = 0;
    bool alertActive_ = false;
    AlertKind alertKind_ = AlertKind::Charging;
    int alertValue_ = 0;
    int availableVersion_ = 0;
    int lastAc_ = -1;
    bool lowBatteryShown_ = false;
    int lastMinute_ = -1;

    // Gözler
    Eyes eyes_;
    double lastActivity_ = 0;
    double lastClick_ = 0;
    int clickStreak_ = 0;
    bool lastPlaying_ = false;
    std::wstring lastTitle_;

    // Guti'nin beyni: zamanlayıcı, odak, mola, dostluk
    Companion companion_;
    double reunionAt_ = 0;

    // Guti'nin duyuları
    SystemHealth health_;
    PrivacyWatch privacy_;
    int healthTick_ = 0;
    int cpuHotSamples_ = 0, ramHotSamples_ = 0, offlineSamples_ = 0;
    bool cpuHot_ = false;
    bool offlineShown_ = false;
    double lastCpuAlert_ = -1e9, lastRamAlert_ = -1e9;
    double lastShot_ = -1e9;
    double lastSweat_ = -1e9;
    std::wstring downloadPath_;
    // Müziğe gerçek ritimle dans
    AudioMeter meter_;
    float level_ = -1.f;     // ekranda gösterilen seviye (hızlı yükselir, yavaş iner)
    float avgLevel_ = 0.f;   // uzun ortalama: vuruşu bunun üstündeki sıçrama belirler
    float beatPulse_ = 0.f;  // vuruştan sonra sönen zıplama
    double lastBeat_ = 0;
    double swallowUpUntil_ = 0;  // indirme kartına basınca gelen bırakma tıklamasını yut
};
