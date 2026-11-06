#pragma once
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "Command.h"
#include "Companion.h"
#include "Eyes.h"
#include "Lyrics.h"
#include "Weather.h"
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
    enum class View { Idle, Media, ExpandedMedia, ExpandedIdle, Alert, Timer, Command, Drop, Picker, Note, Ink, Quick, Home, Diary };
    enum class AlertKind { Charging, Unplugged, LowBattery, UpdateAvailable, Updating, UpToDate, UpdateFailed, TimerDone, FocusDone, BreakDone,
                           CpuBusy, MemoryFull, Offline, Online, Download,
                           Reminder, ReminderSet, Screenshot, Volume, Headphones, HeadphonesLow, Copied,
                           Reading, OcrFailed, Notification };

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
        // Komut çubuğu: hesap, birim, hatırlatıcı, pano geçmişi
    void OpenCommand();
    void CloseCommand(bool restoreFocus);
    void OnCommandChar(wchar_t c);
    void OnCommandKey(UINT vk);
    void AcceptCommand();
    void RefreshCommandResult();
    float CommandHeight() const;
    int VisibleClips() const;
    void CaptureClipboardText();
    void CopyToClipboard(const std::wstring& text);
    void CheckReminders();
    void LoadReminders();
    void SaveReminders();
    bool IsFileCard() const {
        return alertActive_ && (alertKind_ == AlertKind::Download || alertKind_ == AlertKind::Screenshot);
    }
    // Ekran görüntüsü ve dosya rafı
    friend class DropTarget;
    bool DragFiles(const std::vector<std::wstring>& paths);
    void OpenCard();
    void StartCardDrag();
    void TakeScreenshot();
    bool SaveClipboardImage();
    void OnDropEnter();
    void OnDropLeave();
    void OnDropFiles(const std::vector<std::wstring>& paths);
    float ShelfTop() const;
    int ShelfHit(float x, float y) const;
    bool ShelfVisible() const;
    ID2D1Bitmap* ShelfIcon(size_t i);
    IWICImagingFactory* Wic();
    // Ses göstergesi, kulaklık pili, renk seçici
    void OnVolume(int percent, bool muted);
    void OnBtBattery(int level, const std::wstring& name);
    void StartPicker();
    void EndPicker(bool pick);
    void OnPickerMove();
    static LRESULT CALLBACK PickerProc(HWND, UINT, WPARAM, LPARAM);

public:
    // Araç çentiği (Tools.cpp): araç ızgarası, yazı tanıma, kahve, not, ekrana çizim, hızlı ayarlar
    enum class Tool { Ocr, Shot, Picker, Coffee, Note, Ink, Quick, Focus, Calc, Sounds, Update, Sleep, Diary, Count };
    static constexpr int kToolCount = static_cast<int>(Tool::Count);
    // Favori siteler: araç ızgarasının son sayfası. ToolHit bunlar için kFavHit + sıra döndürür,
    // "+" kutucuğu için kFavHit + kFavMax.
    static constexpr int kFavMax = 6, kFavHit = 100;
    static constexpr UINT WM_FAVICON = WM_APP + 31;  // lParam = new std::wstring(alan adı)

private:
    bool Typing() const { return commandMode_ || noteMode_; }
    bool ToolsShown(View v) const { return v == View::ExpandedIdle || v == View::ExpandedMedia || v == View::Home; }
    float CardH(View v) const;  // araçların başladığı yükseklik
    float ToolsTop() const;
    float ToolsX0(float w) const;
    int ToolHit(float x, float y) const;  // araç no; -10/-11: sayfa noktası; -1: yok
    bool ToolActive(Tool t) const;
    void DrawTools(View v, float w);
    void RunTool(Tool t);
    void SetToolPage(int page);
    void PageWheel(int delta);
    void ToggleFocus();
    void ToggleSounds();
    void StartUpdate();
    // Yazı tanıma
    void StartOcr();
    bool RunOcr(IWICBitmapSource* src);
    void OnOcrDone(int code, const std::wstring& text);
    Microsoft::WRL::ComPtr<IWICBitmap> ClipboardBitmap();
    // Kahve: bilgisayar uyumaz, Guti de uyumaz
    void SetCoffee(bool on);
    void DrawCup(float x, float cy, float alpha);
    // Not
    void OpenNote();
    void CloseNote(bool restoreFocus);
    void OnNoteChar(wchar_t c);
    void LoadNote();
    void SaveNote();
    void DrawNote(float w, float h);
    // Ekrana çizim
    void StartInk();
    void EndInk();
    void InkPoint(POINT pt, bool begin);
    void RedrawInk();
    void PresentInk(const RECT* dirty);
    void InkUndo();
    void InkClear();
    void SetInkColor(int i);
    int InkBarHit(float x, float y) const;
    void DrawInkBar(float w, float h);
    static LRESULT CALLBACK InkProc(HWND, UINT, WPARAM, LPARAM);
    // Hızlı ayarlar
    void OpenQuick();
    int QuickHit(float x, float y) const;
    void QuickSlider(int which, float x);
    void QuickClick(int hit);
    void DrawQuick(float w, float h);
    bool CheckSlim();
    int MediaHit(float x, float y) const;  // 20: ilerleme çubuğu, 21: ses
    void MediaSlider(int which, float x);

    // Albüm rengi, şarkı sözleri, favoriler, göz rengi, isim, uyku zamanlayıcısı (Extras.cpp)
    D2D1_COLOR_F CardAccent() const;
    void SetAccent(D2D1_COLOR_F c);
    void DrawAlbumGlow(float w, float h);
    bool HasLyrics() const { return lyricsOn_ && !lyrics_.empty(); }
    float LyricsH() const;     // açık müzik kartındaki söz alanı (0: yok)
    float MediaCardH() const;  // açık müzik kartının araçlardan önceki yüksekliği
    bool LyricPill() const;    // kapalı müzik hapında söz satırı
    void OnMediaForLyrics();
    void StartLyricsFetch();
    void OnLyrics(Lyrics::Result* r);
    void UpdateLyricLine(double now);
    void DrawLyricRows(D2D1_RECT_F area, float rowH, bool compact);
    void LoadFavorites();
    void SaveFavorites();
    bool AddFavorite(std::wstring target);
    void RemoveFavorite(int i);
    void OpenFavorite(int i);
    void AddFavoriteFromClipboard();
    ID2D1Bitmap* FavoriteIcon(size_t i);
    void OnFavicon(const std::wstring& domain);
    void DrawFavorites(float x0, float y0, float off);
    void OnDropUrl(const std::wstring& url);
    void LoadLook();
    void SetEyeMode(int mode, COLORREF rgb);
    void ApplyEyeColor();
    void SetName(std::wstring name);
    void CycleSleep();
    void OnSleepTick();
    // 2. parti (Home.cpp): ana sayfa, hava durumu, albüm ışığı, aksesuarlar
    void DrawHome(float w, float h);
    int HomeHit(float x, float y) const;
    void HomeClick(int hit);
    void SyncFavGlow();
    void DrawPillGlow(float w, float h, float r);
    void DrawGuti(float cx, float cy, float u, float bob);
    void DrawAccessories(float cx, float cy, float u, float bob);
    void CheckAccessoryUnlocks();
    void RefreshWeather();
    void OnWeather(Weather::Info* info);
    void OnWeatherMood(double now);
    void SetCity(std::wstring city);
    // 3. parti (Diary.cpp): Guti'nin günlüğü, şarkıyı kalple
    struct DayStats {
        std::wstring date;  // YYYY-MM-DD
        int focusSec = 0, songs = 0, pets = 0;
        std::wstring topSong;
    };
    void LoadDiary();
    void SaveDiary();
    void OnDiaryTick();
    void RollDay();
    void OpenDiary(bool automatic);
    void DrawDiary(float w, float h);
    bool IsLiked() const;
    void ToggleLike();
    void LoadLikes();

    // Çizim
    bool CreateGpu();
    void FrameLoop();
    void DrawView(View v, float w, float h);
    void DrawShadow(float w, float h, float r);
    float Stagger(int i) const;
    void PushFade(float alpha, float dx, float dy, float scale = 1.f, float cx = 0, float cy = 0);
    void PopFade();
    struct Roll {
        std::wstring cur, prev;
        double at = -10;
    };
    void DrawRolling(Roll& roll, const std::wstring& text, IDWriteTextFormat* format, D2D1_RECT_F rect,
                     D2D1_COLOR_F color);
    int ButtonAt(float x, float y) const;
    void OnMouseMove(float x, float y);
    void UpdatePerf();
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
    void DrawCommand(float w, float h);
    void DrawScreenshot(float w, float h);
    void DrawShelf(float w);
    void DrawDrop(float w, float h);
    void DrawPicker(float w, float h);
    void DrawVolume(float w, float h);
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
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt_;      // çizimin yapıldığı hedef (GPU ya da yazılım)
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRt_;  // yazılım yedeği
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> ctx_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<IDXGISurface1> surface_;
    Microsoft::WRL::ComPtr<IDWriteTypography> tabular_;
    bool gpu_ = false, gpuFailed_ = false;
    struct Fade {
        D2D1_MATRIX_3X2_F old;
        bool layer;
    };
    std::vector<Fade> fades_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> artBitmap_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fClock_, fTitle_, fArtist_, fSmall_, fSmallRight_,
        fBigClock_, fDate_, fTiny_, fTinyRight_, fIcon_, fIconBig_, fIconSmall_, fAlert_, fAlertRight_, fSmallCenter_;

    // Animasyon
    Spring width_, height_, radius_, shake_;
    View view_ = View::Idle;
    bool animating_ = false;
    // Kare saati: ekran yenilemesine kilitli ayrı bir iş parçacığı kare ister.
    HANDLE frameEvent_ = nullptr;
    std::atomic<bool> wantFrames_{false}, framePending_{false};
    std::atomic<int> frameDivider_{1};
    // Görünümler arası geçiş
    View prevView_ = View::Idle;
    float prevW_ = 78, prevH_ = 22, viewW_ = 78, viewH_ = 22;
    float trans_ = 1.f;
    bool drawingOld_ = false;
    // Butonlar
    int hoverBtn_ = -1, pressBtn_ = -1;
    Spring btnScale_[3], btnGlow_[3];
    bool trackingLeave_ = false;
    Spring selY_, volBar_;
    Roll rollClock_, rollTimer_, rollVolume_;
    double rollUntil_ = 0;
    // Performans
    ULONGLONG lastProcTime_ = 0, lastWallTime_ = 0;
    float cpuPct_ = 0;
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
    std::wstring cardPath_;
    // Müziğe gerçek ritimle dans
    AudioMeter meter_;
    float level_ = -1.f;     // ekranda gösterilen seviye (hızlı yükselir, yavaş iner)
    float avgLevel_ = 0.f;   // uzun ortalama: vuruşu bunun üstündeki sıçrama belirler
    float beatPulse_ = 0.f;  // vuruştan sonra sönen zıplama
    double lastBeat_ = 0;
    // Komut çubuğu
    bool commandMode_ = false;
    HWND prevForeground_ = nullptr;
    std::wstring input_;
    Command::Result cmdResult_;
    int clipSel_ = 0;
    std::vector<std::wstring> clips_;  // sadece bellekte, diske yazılmaz
    struct Reminder {
        ULONGLONG at;  // duvar saati, saniye
        std::wstring text;
    };
    std::vector<Reminder> reminders_;
    std::wstring alertLabel_;
    // Ekran görüntüsü + raf
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<IWICBitmap> shotThumb_;   // küçültülmüş önizleme
    Microsoft::WRL::ComPtr<ID2D1Bitmap> shotBitmap_;
    struct ShelfItem {
        std::wstring path;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> icon;
    };
    std::vector<ShelfItem> shelf_;
    class DropTarget* dropTarget_ = nullptr;
    bool dropArmed_ = false;   // dışarıdan sürükleme gelirken ada tıklanabilir olur
    bool dropHover_ = false;   // dosya şu an adanın üstünde
    bool lastLeftDown_ = false, downStartedOver_ = false;
    // Ses / kulaklık / renk
    double startedAt_ = 0;
    int volume_ = -1;
    bool muted_ = false;
    bool volumeOsd_ = true;
    int btLevel_ = 255;
    std::wstring btName_;
    bool btLowShown_ = false;
    HWND picker_ = nullptr;
    COLORREF pickColor_ = 0;
    double swallowUpUntil_ = 0;  // indirme kartına basınca gelen bırakma tıklamasını yut

    // Araç çentiği
    int toolPage_ = 0;
    float swipeX0_ = -1;  // araç sırasında basılan yer (sürükleyerek sayfa çevirme)
    bool swipeMoved_ = false;
    int wheelAcc_ = 0;
    double lastPageFlip_ = 0;
    int hoverTool_ = -1;
    Spring pageX_, toolGlow_[kToolCount];
    bool ocrPending_ = false;
    double ocrAskedAt_ = 0;
    bool coffee_ = false;
    bool noteMode_ = false;
    std::wstring note_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fNote_, fAa_;
    struct Stroke {
        D2D1_COLOR_F color;
        std::vector<D2D1_POINT_2F> pts;
    };
    HWND ink_ = nullptr;
    HDC inkDC_ = nullptr;
    HBITMAP inkDib_ = nullptr;
    HGDIOBJ inkOld_ = nullptr;
    void* inkBits_ = nullptr;
    int inkW_ = 0, inkH_ = 0;
    RECT inkRect_{};
    Microsoft::WRL::ComPtr<IWICBitmap> inkWic_;  // çizimin kalıcı tuvali
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> inkRt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> inkBrush_;
    std::vector<Stroke> strokes_;
    int inkColor_ = 0;
    bool inkDown_ = false;
    bool quickOpen_ = false;
    int radios_ = -1;       // Sys.h Radios bitleri; -1: bilinmiyor
    int brightness_ = -1;   // 0-100, 255: desteklenmiyor, -1: okunuyor
    int sliderDrag_ = -1;   // 0 parlaklık, 1 ses
    int nightLight_ = -1;   // 1 açık, 0 kapalı, -1 bilinmiyor
    // Sekmelerin üstünde ezmesin: ince hap, üstünden geçerken saydamlaşma, durunca açılma
    bool slim_ = false;
    Spring ghost_;
    POINT hoverAnchor_{};
    double lastMoveOver_ = -1e9;
    // Müzik kartı: ilerleme çubuğundan sarma ve ses sürgüsü (sliderDrag_ 2 / 3)
    double seekPreview_ = 0;
    int mediaHover_ = -1;
    double brightSetAt_ = -1e9;  // son elle ayar: eski yankılar sürgüyü geri itmesin

    // Albüm rengi kartı: renk değişince eskisinden yenisine yumuşakça geçer
    D2D1_COLOR_F accentFrom_{0.62f, 0.80f, 1.f, 1.f};
    double accentAt_ = -10;
    bool albumTint_ = true;
    // Şarkı sözleri
    std::vector<Lyrics::Line> lyrics_;
    bool lyricsOn_ = true, lyricsPill_ = true;
    uint64_t lyricsToken_ = 0;
    std::wstring lyricsKey_;
    int lyricLine_ = -1;
    double lyricAt_ = -10;
    float nodPulse_ = 0.f;  // satır değişince Guti'nin minik baş sallaması
    double lastChorus_ = -1e9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fLyric_;
    // Favoriler
    struct Favorite {
        std::wstring target, label, domain;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> icon;
        bool iconTried = false;
    };
    std::vector<Favorite> favs_;
    Spring favGlow_[kFavMax + 1];
    bool dropUrl_ = false;  // sürüklenen şey bir bağlantı
    // Görünüş: göz rengi (0 beyaz, 1 seçilen renk, 2 Windows vurgu rengi) ve isim
    bool pickForEyes_ = false;
    int eyeMode_ = 0;
    COLORREF eyeRgb_ = RGB(255, 255, 255);
    std::wstring name_ = L"Guti";
    int cmdSpecial_ = 0;  // komut çubuğu: 1 favori ekle, 2 isim
    // Müzik uyku zamanlayıcısı
    double sleepEnd_ = 0;
    int sleepMinutes_ = 0;
    int sleepRestoreVol_ = -1;
    bool sleepFading_ = false;
    double volQuietUntil_ = 0;  // kendi yaptığımız ses değişikliklerinde gösterge açılmasın

    // Ana sayfa
    bool homeOn_ = true;
    bool homeMedia_ = false;  // ana sayfadan büyük müzik kartına geçildi
    int homeHover_ = -1;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fHomeTimer_;
    // Hava durumu
    bool weatherOn_ = true;
    Weather::Info weather_;
    std::wstring city_;
    double lastShiver_ = -1e9, lastHeat_ = -1e9;
    // Albüm ışığı ve aksesuarlar
    bool albumGlow_ = true, accessories_ = true;
    // Günlük
    DayStats today_;
    std::vector<DayStats> history_;  // eski günler, en yenisi sonda (en çok 7)
    std::vector<std::pair<std::wstring, int>> listen_;  // bugün: şarkı -> dinlenen saniye
    bool diaryOpen_ = false;
    double diaryUntil_ = 0;
    int diaryShownDay_ = -1;
    int diarySaveTick_ = 0;
    std::wstring diarySong_;  // en son sayılan şarkı
    // Beğenilen şarkılar
    std::vector<std::wstring> likes_;
    Spring heartPop_;
    // Bildirim aynası
    bool notifyOn_ = true;
};
