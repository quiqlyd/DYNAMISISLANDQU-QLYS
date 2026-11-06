#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <map>

#include "DockGlass.h"

// Pencere yerleştirme: öndeki pencereyi kısayolla yarıma, üçte bire, köşeye ya da tam ekrana
// yumuşak bir animasyonla taşır. Gideceği alan önce buzlu camla kısa bir an parlar.
// "Geri" pencereyi yerleştirmeden önceki boyutuna döndürür.
class Snapper {
public:
    enum class Action { Left, Right, ThirdL, ThirdC, ThirdR, TopLeft, TopRight, BottomLeft, BottomRight, Full, Back, Count };

    bool Init(HINSTANCE instance, ID2D1Factory* d2d, IDWriteFactory* dwrite);
    void Shutdown();
    // Öndeki pencereye uygular. Uygun pencere yoksa (masaüstü, bizim pencerelerimiz, küçültülmüş) false.
    bool Run(Action a);

    static const wchar_t* Label(Action a);  // "Sol yarı"
    static const wchar_t* Key(Action a);    // "←", "F", "↵"
    // Guti'nin bakacağı yön (-1..1).
    static void LookDir(Action a, float& x, float& y);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    bool CreateWindows();
    void Tick();
    void ShowPreview(const RECT& r, const wchar_t* label, UINT dpi);
    void SetPreviewAlpha(float a);
    void HidePreview();
    bool DrawLabel(int w, int h, const wchar_t* label, float scale);

    HINSTANCE instance_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    HWND glass_ = nullptr, label_ = nullptr;
    DockGlass glassFx_;
    bool previewShown_ = false;

    // Animasyon
    HWND win_ = nullptr;
    RECT from_{}, to_{}, margin_{};
    double startAt_ = 0;
    bool moving_ = false;

    std::map<HWND, RECT> original_;  // "Geri" için yerleştirmeden önceki pencere dikdörtgeni
    std::map<HWND, RECT> placed_;    // son yerleştirdiğimiz dikdörtgen (kullanıcı taşıdı mı?)
};
