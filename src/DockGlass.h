#pragma once
#include <windows.h>
#include <memory>

// Gerçek yuvarlak köşeli buzlu cam: Windows.UI.Composition'ın arka plan bulanıklığı
// (HostBackdropBrush) yumuşak kenarlı, yuvarlatılmış bir kırpma ile pencereye konur.
// Eski "accent" bulanıklığı pencere bölgesini yok saydığı için köşeler kare çıkıyordu.
class DockGlass {
public:
    DockGlass();
    ~DockGlass();
    // Pencere WS_EX_NOREDIRECTIONBITMAP ile açılmış olmalı. Desteklenmiyorsa false.
    bool Attach(HWND hwnd);
    void Detach();
    bool Attached() const;
    // Boyut (px), köşe yarıçapı (px) ve tema.
    void Update(int width, int height, float radius, bool light);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
