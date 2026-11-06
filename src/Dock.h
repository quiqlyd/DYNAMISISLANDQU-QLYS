#pragma once
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "DockGlass.h"
#include "Spring.h"

// Guti Dock: ekranın alt ortasında yüzen, buzlu cam görünümlü yeni taskbar.
// İki pencere: altta Windows'un Acrylic bulanıklığını taşıyan "cam" penceresi (yuvarlatılmış bölge),
// onun üstünde ikonları, gölgeyi ve kenar ışığını çizen katmanlı (layered) pencere.
// Windows taskbar'ı gizlenir (otomatik gizle + görünmez); dock kapanınca/çökünce geri gelir.
class Dock {
public:
    ~Dock() {
        if (frameThread_.joinable()) frameThread_.detach();  // süreç kapanıyor
    }
    bool Enable(HINSTANCE instance, ID2D1Factory* d2d, IDWriteFactory* dwrite);
    void Disable();
    bool Enabled() const { return overlay_ != nullptr; }
    void SetAccent(D2D1_COLOR_F c);
    std::function<void()> onMenu;   // Guti'ye sağ tık: adanın menüsü
    std::function<void()> onQuick;  // kontrol hapı: adanın hızlı ayarlar kartı
    void SetVolume(int percent, bool muted);
    void SetOnline(bool online);
    void SetAutoHide(bool on);
    bool AutoHide() const { return autoHide_; }
    // 4. parti: Guti ile bağ
    std::function<void()> onLaunch;  // bir uygulama açılıyor: adadaki Guti aşağı bakar
    void SetMusic(const std::wstring& sourceApp, float level);
    void SetFocus(bool on);

    // Uygulama kapanırken/çökerken Windows taskbar'ını geri getirir.
    static void RestoreTaskbarNow();
    // Açılışta: dock kapalıysa ama taskbar bizden gizli kaldıysa geri getir.
    static void RepairOnStartup(bool dockEnabled);
    // Öndeki pencere gerçekten tüm ekranı kaplıyor mu? (tıklama geçiren saydam katmanlar sayılmaz)
    static bool ForegroundFullscreen();

private:
    struct App {
        std::wstring key;      // eşleştirme: AUMID ya da exe yolu (küçük harf)
        std::wstring aumid;    // küçük harf
        std::wstring aumidRaw; // asıl yazımıyla (shell:AppsFolder için)
        std::wstring exe;      // küçük harf tam yol
        std::wstring launch;   // açmak için: .lnk, exe ya da shell:AppsFolder\AUMID
        std::wstring name;
        bool pinned = false;
        bool isStart = false;
        bool isSeparator = false;
        bool isStack = false;    // İndirilenler yığını
        bool isTrash = false;    // Geri Dönüşüm Kutusu
        bool isControl = false;  // Wi-Fi / ses / pil hapı
        bool isEnd = false;      // sağ uçtaki görünmez işaret (genişliği yok)
        bool IsApp() const { return !isStart && !isSeparator && !isStack && !isTrash && !isControl; }
        std::vector<HWND> windows;  // üstteki önce
        bool active = false;
        bool seen = false;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> icon;
        bool iconTried = false;
        Spring grow;    // büyütme: ikon boyuna eklenen DIP
        Spring press;   // basınca gömülme (DIP)
        Spring dot;     // açık göstergesi (0-100)
        Spring appear;  // yeni gelen ikon 0'dan büyür (0-100)
        Spring hov;     // üzerine gelme vurgusu (0-100)
        Spring slide;   // sürükleyerek sıralarken yer değiştiren komşunun kayması (DIP)
        bool attention = false;  // "bana bak" diyen uygulama: rozet
        double launchAt = -1;  // açılış zıplaması başladığı an
        float lift = 0;        // zıplama yüksekliği
        float x = 0, w = 0, size = 0;  // son yerleşim (DIP, dock içi)
    };

    static LRESULT CALLBACK OverlayProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK GlassProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnOverlay(UINT msg, WPARAM wp, LPARAM lp);
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                      DWORD);

    void UpdateMonitor();
    void ApplyGlass();
    void RegisterAppBar();
    void RemoveAppBar();
    void HideTaskbar();
    void RestoreTaskbar();
    void SetFullscreen(bool on);

    void LoadPins();
    void SavePins(bool fromApps = true);
    void Rescan();
    bool Eligible(HWND hwnd) const;
    void Describe(HWND hwnd, std::wstring& aumid, std::wstring& exe) const;
    std::wstring NameFor(const App& a, HWND hwnd) const;
    App* FindApp(const std::wstring& aumid, const std::wstring& exe);

    void Layout(float* bodyL, float* bodyR);
    int HitTest(float x, float y) const;
    void OnClick(int i);
    void Activate(App& a);
    void OpenStart();
    // 2. parti: sağ tık menüsü, sabitleme, sürükle sırala, dosya bırakma, dikkat rozeti
    friend class DockDrop;
    void ShowAppMenu(int i);
    void SetPinned(int i, bool pinned);
    void DragTo(float x, float y);
    void EndDrag(bool commit);
    void OnShellHook(WPARAM code, HWND hwnd);
    int DropOver(POINT screen);
    void DropFiles(int i, const std::vector<std::wstring>& paths);
    void Puff(float x, float y);
    // 3. parti: pencere önizlemeleri, İndirilenler/Çöp yığınları, kontrol hapı, otomatik gizleme
    enum class Panel { None, Preview, Stack };
    struct StackItem {
        std::wstring path, name;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> icon;
        bool iconTried = false;
    };
    static LRESULT CALLBACK PreviewProc(HWND, UINT, WPARAM, LPARAM);
    void OpenPreview(int i);
    void OpenStack(int i);
    void ClosePanel();
    void LayoutPanel();
    int PanelHit(float x, float y) const;  // önizleme kartı / yığın satırı; -2: klasörü aç; -1: yok
    void PanelClick(int hit, bool middle);
    void CheckPanelHover();
    void DrawPanel();
    void DrawControl(const App& a);
    void UpdateAutoHide();
    ID2D1Bitmap* FileIcon(StackItem& s);
    float PanelBottom() const { return -18.f; }

    void StartAnimation();
    void FrameLoop();
    void OnFrame();
    bool EnsureResources();
    bool CreateGpu();
    void DiscardDevice();
    void Render();
    void DrawIcon(App& a, float cx, float bottom, float size, float opacity = 1.f);
    void DrawStart(float cx, float bottom, float size);
    void DrawWindowsLogo(float cx, float bottom, float size);
    void DrawLabel(const App& a, float cx, float top);
    ID2D1Bitmap* Icon(App& a);
    double Now() const;

    HINSTANCE instance_ = nullptr;
    HWND glass_ = nullptr, overlay_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> labelFormat_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;

    // Çizim (adadaki ile aynı yol: ekran kartı, olmazsa yazılım)
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt_;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<IDXGISurface1> surface_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    bool gpu_ = false, gpuFailed_ = false;
    HDC memDC_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    int texW_ = 0, texH_ = 0;

    RECT monitor_{};
    float scale_ = 1.f;
    bool light_ = false;
    D2D1_COLOR_F accent_ = D2D1::ColorF(0.93f, 0.97f, 1.f);

    std::vector<App> apps_;
    std::vector<std::wstring> pinOrder_;  // launch yolları
    int hover_ = -1, pressed_ = -1;
    POINT downPt_{};
    bool dragging_ = false;
    float dragX_ = 0, dragY_ = 0;
    double puffAt_ = -1;
    float puffX_ = 0, puffY_ = 0;
    UINT shellHookMsg_ = 0;
    class DockDrop* drop_ = nullptr;
    bool mouseIn_ = false;
    float mouseX_ = 0;   // dock içi DIP
    float anchorX_ = 0;  // büyütmenin sabit tuttuğu nokta
    Spring labelA_;      // isim balonu görünürlüğü (0-100)
    int labelFor_ = -1;
    double hoverSince_ = 0;
    Panel panel_ = Panel::None;
    int panelFor_ = -1;
    float panelX_ = 0, panelW_ = 0, panelH_ = 0;  // panel orta x ve boyutları (DIP)
    Spring panelA_;                              // panel görünürlüğü (0-100)
    HWND preview_ = nullptr;
    std::vector<HWND> previewWins_;
    std::vector<HTHUMBNAIL> thumbs_;
    std::vector<std::wstring> previewTitles_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_, headFormat_, iconFormat_;
    double keepShownAt_ = 0;
    bool ScreenToDock(POINT pt, float* x, float* y) const;
    std::vector<D2D1_RECT_F> cards_;  // panel kartları/satırları (dock koordinatı)
    int panelHover_ = -1;
    int panelOut_ = 0;
    std::vector<StackItem> stack_;
    std::wstring downloads_;
    int volume_ = -1;
    bool muted_ = false, online_ = true;
    bool autoHide_ = false;
    Spring hide_;           // otomatik gizlemede aşağı kayma (DIP)
    bool hideWanted_ = false;
    int presentedTop_ = 0;  // dokudan pencereye aktarılan ilk satır (px)
    std::wstring musicApp_;  // küçük harf
    float musicLevel_ = 0;
    double musicFrameAt_ = 0;
    bool focus_ = false;
    Spring focusA_;          // odak modunda ilgisiz ikonların sönükleşmesi (0-100)
    double blinkAt_ = -1;    // mini Guti göz kırpıyor
    bool IsMusicApp(const App& a) const;
    float baseW_ = 0;    // büyütmesiz gövde genişliği

    std::vector<HWINEVENTHOOK> hooks_;
    HWND tray_ = nullptr;
    bool fullscreen_ = false;
    bool appBar_ = false;
    UINT taskbarCreatedMsg_ = 0;
    RECT glassRect_{};
    DockGlass glassFx_;      // yuvarlak köşeli gerçek cam (olmazsa eski accent bulanıklığı)
    float bodyRadius_ = 22.f;
    void DrawFolderIcon(App& a, float cx, float bottom, float size, float opacity);

    int overlayLeft_ = 0;  // katmanlı pencerenin ekran x'i (px)
    std::thread frameThread_;
    HANDLE frameEvent_ = nullptr;
    std::atomic<bool> wantFrames_{false}, framePending_{false}, quit_{false};
    bool animating_ = false;
    // Küçük sistem tepsisi: gizli simgeler oku, ağ+ses, saat/tarih (x aralıkları, dock içi DIP)
    float trayX_[4] = {};
    int TrayPart(float x) const;
    void OpenHiddenIcons();
    void ShowEmptyMenu();
    double trayPeekAt_ = -1;   // gizli simgeler için Windows taskbar'ı kısa süre görünür bırakıldı
    bool wifi_ = true;
    double wifiCheckedAt_ = -100;
    int lastMinute_ = -1;
    // Windows panelleri (gizli simgeler, hızlı ayarlar, bildirimler, Win+X) açılınca tıklanan yerin üstüne taşınır.
    void BeginFlyout(float dockX);
    void MoveFlyouts();
    std::vector<HWND> flyBefore_;
    double flyAt_ = -1;
    int flyX_ = 0;
    double lastFrameAt_ = 0;  // son karenin zamanı: kareler takılırsa bekçi zamanlayıcısı çizer
    int stallLogs_ = 0;
    LARGE_INTEGER qpcFreq_{}, lastTick_{};
};
