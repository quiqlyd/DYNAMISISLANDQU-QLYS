#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>

#include "MediaService.h"
#include "Spring.h"

// Ekranın üst ortasında duran, şekil değiştiren siyah hap.
// Katmanlı (layered) pencere + Direct2D: saydam kısımlar tıklamayı alttaki pencereye geçirir.
class Island {
public:
    bool Create(HINSTANCE instance);
    int Run();

private:
    enum class View { Idle, Media, ExpandedMedia, ExpandedIdle, Alert };
    enum class AlertKind { Charging, Unplugged, LowBattery };

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
    void ShowAlert(AlertKind kind);
    void OnPowerChanged(bool initial);

    // Çizim
    bool EnsureResources();
    void DiscardDeviceResources();
    void Render();
    void DrawIdle(float w, float h);
    void DrawCompactMedia(float w, float h);
    void DrawExpandedMedia(float w, float h);
    void DrawExpandedIdle(float w, float h);
    void DrawAlert(float w, float h);
    void DrawArt(D2D1_RECT_F rect, float radius);
    void DrawBars(float right, float centerY, float maxHeight, float barWidth);
    void Text(const wchar_t* text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                  D2D1_COLOR_F color);
    void DrawBattery(float x, float y, float w, float h, int percent, bool charging);

    // Girdi
    void OnClick(float x, float y);

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
    bool hidden_ = false;

    // GDI yüzeyi
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    int dibW_ = 0, dibH_ = 0;

    // Direct2D / DirectWrite
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> artBitmap_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fClock_, fTitle_, fArtist_, fSmall_, fSmallRight_,
        fBigClock_, fDate_, fIcon_, fIconBig_, fIconSmall_, fAlert_, fAlertRight_;

    // Animasyon
    Spring width_, height_, radius_;
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
    bool alertActive_ = false;
    AlertKind alertKind_ = AlertKind::Charging;
    int lastAc_ = -1;
    bool lowBatteryShown_ = false;
    int lastMinute_ = -1;
};
