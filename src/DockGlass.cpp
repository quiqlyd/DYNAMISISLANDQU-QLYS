#include "DockGlass.h"

#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;

namespace {
// Süreç boyunca tek bir derleyici (compositor). Kapanışta serbest bırakılmaz:
// COM kapandıktan sonra Release çağrılıp çökmesin.
struct CompHolder {
    wuc::Compositor comp;
};
CompHolder* g_comp = nullptr;

bool EnsureCompositor() {
    if (g_comp) return true;
    try {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions o{sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
            ABI::Windows::System::IDispatcherQueueController* c = nullptr;
            if (FAILED(CreateDispatcherQueueController(o, &c)) || !c) return false;
            // Bilerek sızdırılır: UI iş parçacığının kuyruğu uygulama boyunca yaşar.
        }
        g_comp = new CompHolder{wuc::Compositor()};
        return true;
    } catch (...) {
        return false;
    }
}
}  // namespace

struct DockGlass::Impl {
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr};
    wuc::SpriteVisual blur{nullptr}, tint{nullptr};
    wuc::CompositionColorBrush tintBrush{nullptr};
    wuc::CompositionRoundedRectangleGeometry geo{nullptr};
};

DockGlass::DockGlass() = default;
DockGlass::~DockGlass() = default;

bool DockGlass::Attached() const { return impl_ != nullptr; }

bool DockGlass::Attach(HWND hwnd) {
    Detach();
    if (!EnsureCompositor()) return false;
    try {
        auto& comp = g_comp->comp;
        auto impl = std::make_unique<Impl>();
        auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        if (FAILED(interop->CreateDesktopWindowTarget(
                hwnd, FALSE,
                reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
                    winrt::put_abi(impl->target)))))
            return false;
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));

        impl->root = comp.CreateContainerVisual();
        impl->blur = comp.CreateSpriteVisual();
        impl->blur.Brush(comp.CreateHostBackdropBrush());
        impl->blur.RelativeSizeAdjustment(float2{1.f, 1.f});
        impl->tintBrush = comp.CreateColorBrush();
        impl->tint = comp.CreateSpriteVisual();
        impl->tint.Brush(impl->tintBrush);
        impl->tint.RelativeSizeAdjustment(float2{1.f, 1.f});
        impl->root.Children().InsertAtTop(impl->blur);
        impl->root.Children().InsertAtTop(impl->tint);
        // Yumuşak kenarlı yuvarlak kırpma: köşeler pürüzsüz çıkar.
        impl->geo = comp.CreateRoundedRectangleGeometry();
        impl->root.Clip(comp.CreateGeometricClip(impl->geo));
        impl->target.Root(impl->root);
        impl_ = std::move(impl);
        return true;
    } catch (...) {
        return false;
    }
}

void DockGlass::Detach() {
    if (!impl_) return;
    try {
        impl_->target.Root(nullptr);
    } catch (...) {
    }
    impl_.reset();
}

void DockGlass::Update(int width, int height, float radius, bool light) {
    if (!impl_) return;
    try {
        float2 size{static_cast<float>(width), static_cast<float>(height)};
        impl_->root.Size(size);
        impl_->geo.Size(size);
        impl_->geo.CornerRadius(float2{radius, radius});
        // Renk (A, R, G, B): açık temada hafif süt beyazı, koyuda dumanlı koyu cam.
        impl_->tintBrush.Color(light ? winrt::Windows::UI::Color{0x55, 0xF7, 0xF7, 0xFA}
                                     : winrt::Windows::UI::Color{0x80, 0x1A, 0x1A, 0x20});
    } catch (...) {
    }
}
