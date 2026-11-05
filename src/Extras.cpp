// İkinci videodan gelen fikirler: albüm rengine bürünen kart, senkron şarkı sözleri,
// favori siteler, Guti'nin göz rengi ve adı, müzik uyku zamanlayıcısı.
#include "Island.h"
#include "IslandUi.h"
#include "Net.h"
#include "Sounds.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace isl;

namespace {

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return Rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

// Siyah zeminde göz rengi: koyu renkler biraz açılır ki Guti kaybolmasın.
D2D1_COLOR_F Brighten(COLORREF c) {
    D2D1_COLOR_F k = Rgba(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
    float lum = 0.2126f * k.r + 0.7152f * k.g + 0.0722f * k.b;
    if (lum < 0.45f) {
        float t = (0.45f - lum) / 0.45f * 0.55f;
        k.r += (1 - k.r) * t;
        k.g += (1 - k.g) * t;
        k.b += (1 - k.b) * t;
    }
    return k;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    auto junk = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"'; };
    while (a < b && junk(s[a])) ++a;
    while (b > a && junk(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool IsWeb(const std::wstring& t) {
    std::wstring l = Lower(t.substr(0, 8));
    return l.rfind(L"http://", 0) == 0 || l.rfind(L"https://", 0) == 0;
}

// "https://www.youtube.com/watch?v=1" -> "www.youtube.com"
std::wstring HostOf(const std::wstring& url) {
    if (!IsWeb(url)) return {};
    size_t a = url.find(L"://") + 3;
    size_t b = url.find_first_of(L"/?#:", a);
    return Lower(url.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
}

std::wstring LabelFor(const std::wstring& target) {
    std::wstring host = HostOf(target);
    if (host.empty()) {
        // Dosya / uygulama: uzantısız ad
        size_t slash = target.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? target : target.substr(slash + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name.resize(dot);
        return name.empty() ? target : name;
    }
    static const struct { const wchar_t* key; const wchar_t* label; } kKnown[] = {
        {L"mail.google", L"Gmail"},       {L"translate.google", L"Çeviri"}, {L"drive.google", L"Drive"},
        {L"youtube", L"YouTube"},         {L"youtu.be", L"YouTube"},        {L"chatgpt", L"ChatGPT"},
        {L"claude.ai", L"Claude"},        {L"github", L"GitHub"},           {L"whatsapp", L"WhatsApp"},
        {L"instagram", L"Instagram"},     {L"netflix", L"Netflix"},         {L"spotify", L"Spotify"},
        {L"twitch", L"Twitch"},           {L"twitter", L"X"},               {L"x.com", L"X"},
        {L"reddit", L"Reddit"},           {L"linkedin", L"LinkedIn"},       {L"discord", L"Discord"},
        {L"outlook", L"Outlook"},         {L"google", L"Google"},
    };
    for (const auto& k : kKnown)
        if (host.find(k.key) != std::wstring::npos) return k.label;
    if (host.rfind(L"www.", 0) == 0) host = host.substr(4);
    std::wstring name = host.substr(0, host.find(L'.'));
    if (!name.empty()) name[0] = static_cast<wchar_t>(towupper(name[0]));
    return name;
}

// Kullanıcının yazdığı / yapıştırdığı şeyi açılabilir bir hedefe çevirir. Boş: anlaşılmadı.
std::wstring NormalizeTarget(std::wstring t) {
    t = Trim(t);
    if (t.empty() || t.size() > 2000) return {};
    if (IsWeb(t)) return t.find(L' ') == std::wstring::npos ? t : std::wstring();
    if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) return t;
    bool spaces = t.find_first_of(L" \t\r\n") != std::wstring::npos;
    size_t dot = t.find(L'.');
    if (!spaces && dot != std::wstring::npos && dot > 0 && dot + 2 <= t.size() &&
        t.find(L'\\') == std::wstring::npos)
        return L"https://" + t;
    return {};
}

bool LooksLikeImage(const std::string& b) {
    if (b.size() < 8) return false;
    auto u = [&](size_t i) { return static_cast<unsigned char>(b[i]); };
    return (u(0) == 0x89 && b.compare(1, 3, "PNG") == 0) ||          // PNG
           (u(0) == 0 && u(1) == 0 && u(2) == 1 && u(3) == 0) ||      // ICO
           (u(0) == 0xFF && u(1) == 0xD8) ||                          // JPEG
           b.compare(0, 4, "GIF8") == 0;
}

std::wstring IconPath(const std::wstring& host) { return Net::DataDir(L"icons") + host + L".img"; }

// Bir sitenin simgesini arka planda indirir: önce büyük "apple-touch-icon", sonra favicon,
// en son Google'ın simge servisi. Önbelleğe yazar, pencereye haber verir.
void DownloadIcon(HWND notify, std::wstring host) {
    std::thread([notify, host] {
        std::string body;
        DWORD status = 0;
        std::wstring bare = host.rfind(L"www.", 0) == 0 ? host.substr(4) : host;
        const std::wstring urls[] = {
            L"https://" + host + L"/apple-touch-icon.png",
            L"https://" + host + L"/favicon.ico",
            L"https://www.google.com/s2/favicons?sz=64&domain=" + Net::Encode(bare),
        };
        for (const auto& u : urls) {
            if (Net::Get(u, body, 1024 * 1024, &status) && status == 200 && LooksLikeImage(body)) {
                if (Net::WriteFile(IconPath(host), body)) {
                    auto* msg = new std::wstring(host);
                    if (!PostMessageW(notify, Island::WM_FAVICON, 0, reinterpret_cast<LPARAM>(msg))) delete msg;
                }
                return;
            }
        }
    }).detach();
}


}  // namespace

// ---------------------------------------------------------------------------
// Albüm rengi
// ---------------------------------------------------------------------------

D2D1_COLOR_F Island::CardAccent() const {
    return Mix(accentFrom_, accent_, EaseOut(static_cast<float>((Now() - accentAt_) / 0.6)));
}

void Island::SetAccent(D2D1_COLOR_F c) {
    accentFrom_ = CardAccent();
    accent_ = c;
    accentAt_ = Now();
    rollUntil_ = std::max(rollUntil_, accentAt_ + 0.65);
    StartAnimation();
}

void Island::DrawAlbumGlow(float w, float h) {
    if (!albumTint_ || !snapshot_.art) return;
    // Kapak renginden aşağı doğru sönen bir degrade. Biraz koyulaştırılır: beyaz yazı okunur kalsın.
    D2D1_COLOR_F c = CardAccent();
    D2D1_COLOR_F top = Rgba(c.r * 0.78f, c.g * 0.78f, c.b * 0.78f, 0.58f);
    D2D1_GRADIENT_STOP stops[3] = {
        {0.f, top}, {0.55f, WithAlpha(top, 0.5f)}, {1.f, WithAlpha(top, 0.14f)}};
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(rt_->CreateGradientStopCollection(stops, 3, &coll))) return;
    ComPtr<ID2D1LinearGradientBrush> g;
    if (FAILED(rt_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, std::max(h, 1.f))), coll.Get(),
            &g)))
        return;
    rt_->FillRectangle(D2D1::RectF(-4, -4, w + 4, h + 4), g.Get());
}

// ---------------------------------------------------------------------------
// Şarkı sözleri
// ---------------------------------------------------------------------------

float Island::LyricsH() const { return HasLyrics() ? kLyricsH : 0.f; }

float Island::MediaCardH() const { return kExpandedMedia.h + LyricsH(); }

bool Island::LyricPill() const { return lyricsPill_ && HasLyrics() && !slim_ && snapshot_.playing; }

void Island::OnMediaForLyrics() {
    std::wstring key = snapshot_.hasSession ? snapshot_.title + L"\x1f" + snapshot_.artist : std::wstring();
    if (key == lyricsKey_) return;
    lyricsKey_ = key;
    lyrics_.clear();
    lyricLine_ = -1;
    ++lyricsToken_;
    KillTimer(hwnd_, kTimerLyrics);
    // Başlık ve sanatçı çoğu zaman ayrı ayrı gelir: kısa bir süre bekleyip tek istek at.
    if (lyricsOn_ && !snapshot_.title.empty()) SetTimer(hwnd_, kTimerLyrics, 700, nullptr);
}

void Island::StartLyricsFetch() {
    KillTimer(hwnd_, kTimerLyrics);
    if (!lyricsOn_ || snapshot_.title.empty()) return;
    Lyrics::Fetch(hwnd_, lyricsToken_, snapshot_.artist, snapshot_.title, snapshot_.durationSec);
}

void Island::OnLyrics(Lyrics::Result* raw) {
    std::unique_ptr<Lyrics::Result> r(raw);
    if (!r || r->token != lyricsToken_) return;  // bu arada şarkı değişti
    lyrics_ = std::move(r->lines);
    lyricLine_ = -1;
    lyricAt_ = -10;
    double now = Now();
    if (r->instrumental && snapshot_.playing && eyes_.CurrentMood() == Eyes::Mood::Normal)
        eyes_.SetMood(Eyes::Mood::Happy, now, 2.5);  // sözsüz parça: gözlerini kapatıp dinler
    UpdateLyricLine(now);
    Update();
}

void Island::UpdateLyricLine(double now) {
    if (lyrics_.empty()) {
        lyricLine_ = -1;
        return;
    }
    double pos = snapshot_.CurrentPosition() + 0.2;  // satır biraz erken gelsin: okumaya vakit
    auto it = std::upper_bound(lyrics_.begin(), lyrics_.end(), pos,
                               [](double p, const Lyrics::Line& l) { return p < l.t; });
    int idx = static_cast<int>(it - lyrics_.begin()) - 1;
    if (idx == lyricLine_) return;
    bool forward = idx == lyricLine_ + 1;
    lyricLine_ = idx;
    lyricAt_ = now;
    rollUntil_ = std::max(rollUntil_, now + 0.4);
    StartAnimation();
    if (!snapshot_.playing || idx < 0 || !forward) return;

    nodPulse_ = 1.f;  // her yeni satırda minik bir baş sallama
    using M = Eyes::Mood;
    if (eyes_.CurrentMood() != M::Normal) return;
    const Lyrics::Line& line = lyrics_[static_cast<size_t>(idx)];
    double next = static_cast<size_t>(idx) + 1 < lyrics_.size() ? lyrics_[static_cast<size_t>(idx) + 1].t
                                                                : snapshot_.durationSec;
    if (line.text.empty() && next - line.t >= 6) {
        // Enstrümantal ara: gözlerini kapatıp keyif yapar.
        eyes_.SetMood(M::Happy, now, std::min(4.0, next - line.t - 1.0));
    } else if (!line.text.empty() && now - lastChorus_ > 25) {
        int seen = 0;
        for (int i = 0; i < idx; ++i)
            if (lyrics_[static_cast<size_t>(i)].text == line.text) ++seen;
        if (seen >= 2) {  // nakarat: üçüncü kez geliyor
            lastChorus_ = now;
            eyes_.SetMood(M::Love, now, 1.3);
        }
    }
}

void Island::DrawLyricRows(D2D1_RECT_F area, float rowH, bool compact) {
    if (lyrics_.empty() || !fLyric_) return;
    float e = drawingOld_ ? 1.f : EaseOut(static_cast<float>((Now() - lyricAt_) / 0.38));
    float centerY = compact ? (area.top + area.bottom) / 2 : area.top + rowH * 1.5f;
    rt_->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_ALIASED);
    for (int r = -2; r <= 2; ++r) {
        int i = lyricLine_ + r;
        if (i < 0 || i >= static_cast<int>(lyrics_.size())) continue;
        // Satırlar yukarı kayar: yeni satır alttan gelip ortaya oturur.
        float cy = centerY + r * rowH + rowH * (1.f - e);
        float d = std::fabs(cy - centerY) / rowH;
        float a;
        if (compact) a = std::max(0.f, 1.f - d * 1.3f);
        else a = d < 1.f ? 1.f - 0.62f * d : std::max(0.f, 0.38f * (1.6f - d) / 0.6f);
        if (a <= 0.01f) continue;
        const std::wstring& t = lyrics_[static_cast<size_t>(i)].text;
        Text(t.empty() ? L"♪" : t.c_str(), fLyric_.Get(),
             D2D1::RectF(area.left, cy - rowH / 2 - 2, area.right, cy + rowH / 2 + 2), WithAlpha(kWhite, a));
    }
    rt_->PopAxisAlignedClip();
}

// ---------------------------------------------------------------------------
// Favori siteler
// ---------------------------------------------------------------------------

void Island::LoadFavorites() {
    favs_.clear();
    std::string data;
    std::wstring path = Net::DataDir() + L"favoriler.txt";
    if (!Net::ReadFile(path, data, 64 * 1024)) {
        // İlk açılış: birkaç sık kullanılan site
        for (const wchar_t* t : {L"https://www.youtube.com", L"https://chatgpt.com", L"https://mail.google.com",
                                 L"https://www.google.com"})
            favs_.push_back({t, LabelFor(t), HostOf(t)});
        SaveFavorites();
        return;
    }
    std::wstring text = Net::Widen(data);
    size_t i = 0;
    while (i < text.size() && favs_.size() < static_cast<size_t>(kFavMax)) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(i, e - i));
        i = e + 1;
        size_t tab = line.find(L'\t');
        std::wstring label = tab == std::wstring::npos ? std::wstring() : Trim(line.substr(0, tab));
        std::wstring target = Trim(tab == std::wstring::npos ? line : line.substr(tab + 1));
        if (target.empty()) continue;
        favs_.push_back({target, label.empty() ? LabelFor(target) : label, HostOf(target)});
    }
}

void Island::SaveFavorites() {
    std::wstring text;
    for (const auto& f : favs_) text += f.label + L"\t" + f.target + L"\r\n";
    Net::WriteFile(Net::DataDir() + L"favoriler.txt", Net::Narrow(text));
}

bool Island::AddFavorite(std::wstring target) {
    target = NormalizeTarget(target);
    double now = Now();
    if (target.empty()) return false;
    for (const auto& f : favs_) {
        if (Lower(f.target) == Lower(target)) {
            eyes_.SetMood(Eyes::Mood::Wink, now, 0.8);  // "zaten var"
            StartAnimation();
            return true;
        }
    }
    if (favs_.size() >= static_cast<size_t>(kFavMax)) {
        // Yer yok: başını sallar. (Sağ tıkla biri çıkarılabilir.)
        eyes_.SetMood(Eyes::Mood::Curious, now, 1.0);
        shake_.velocity += 200.f;
        StartAnimation();
        return true;
    }
    favs_.push_back({target, LabelFor(target), HostOf(target)});
    SaveFavorites();
    MoodWithSound(Eyes::Mood::Happy, 1.4);  // "getirdim!"
    height_.velocity += 90.f;
    lastActivity_ = now;
    Update();
    return true;
}

void Island::RemoveFavorite(int i) {
    if (i < 0 || i >= static_cast<int>(favs_.size())) return;
    favs_.erase(favs_.begin() + i);
    SaveFavorites();
    eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.7);
    Sounds::Play(Sounds::Sound::Close);
    StartAnimation();
}

void Island::OpenFavorite(int i) {
    if (i < 0 || i >= static_cast<int>(favs_.size())) return;
    AllowSetForegroundWindow(ASFW_ANY);
    ShellExecuteW(nullptr, L"open", favs_[static_cast<size_t>(i)].target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.8);
    Sounds::Play(Sounds::Sound::Pet);
    lastActivity_ = Now();
    SetHover(false);
    Update();
}

void Island::AddFavoriteFromClipboard() {
    // Panoda bir bağlantı ya da dosya yolu varsa doğrudan ekle; yoksa yazmak için komut çubuğunu aç.
    std::wstring text;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(hwnd_)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                text.assign(p, wcsnlen(p, 2048));
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    if (!NormalizeTarget(text).empty()) {
        AddFavorite(text);
        return;
    }
    OpenCommand();
    input_ = L"fav ";
    RefreshCommandResult();
}

ID2D1Bitmap* Island::FavoriteIcon(size_t i) {
    Favorite& f = favs_[i];
    if (f.icon || f.iconTried) return f.icon.Get();
    f.iconTried = true;
    IWICImagingFactory* wic = Wic();
    if (!wic || !rt_) return nullptr;
    ComPtr<IWICBitmapSource> src;
    if (f.domain.empty()) {
        // Dosya / uygulama: Windows'un kendi simgesi
        SHFILEINFOW sfi{};
        if (!SHGetFileInfoW(f.target.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) || !sfi.hIcon)
            return nullptr;
        ComPtr<IWICBitmap> wb;
        wic->CreateBitmapFromHICON(sfi.hIcon, &wb);
        DestroyIcon(sfi.hIcon);
        src = wb;
    } else {
        std::wstring path = IconPath(f.domain);
        ComPtr<IWICBitmapDecoder> dec;
        if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &dec))) {
            DownloadIcon(hwnd_, f.domain);  // gelince WM_FAVICON
            return nullptr;
        }
        // .ico içinde birden çok boy olabilir: en büyüğünü seç.
        UINT frames = 0, bestW = 0;
        dec->GetFrameCount(&frames);
        for (UINT k = 0; k < frames && k < 16; ++k) {
            ComPtr<IWICBitmapFrameDecode> fr;
            UINT w = 0, h = 0;
            if (SUCCEEDED(dec->GetFrame(k, &fr)) && SUCCEEDED(fr->GetSize(&w, &h)) && w > bestW) {
                bestW = w;
                src = fr;
            }
        }
        if (src && bestW > 64) {
            ComPtr<IWICBitmapScaler> sc;
            if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(src.Get(), 64, 64, WICBitmapInterpolationModeFant)))
                src = sc;
        }
    }
    if (!src) return nullptr;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom)))
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &f.icon);
    return f.icon.Get();
}

void Island::OnFavicon(const std::wstring& domain) {
    for (auto& f : favs_) {
        if (f.domain == domain) {
            f.icon.Reset();
            f.iconTried = false;
        }
    }
    StartAnimation();
}

void Island::DrawFavorites(float x0, float y0, float off) {
    int n = static_cast<int>(favs_.size());
    int tiles = std::min(n + 1, kFavMax);
    for (int i = 0; i < tiles; ++i) {
        float x = x0 + i * (kTile + kTileGap) + off;
        float st = Stagger(3 + i);
        PushFade(st, 0, 8.f * (1 - st), 0.85f + 0.15f * st, x + kTile / 2, y0 + kTile / 2);
        float glow = std::clamp(favGlow_[i].value, 0.f, 1.f);
        PushFade(1.f, 0, -1.5f * glow, 1.f + 0.04f * glow, x + kTile / 2, y0 + kTile / 2);
        D2D1_ROUNDED_RECT rr{D2D1::RectF(x, y0, x + kTile, y0 + kTile), 14, 14};
        brush_->SetColor(Rgba(1, 1, 1, 0.07f + 0.08f * glow));
        rt_->FillRoundedRectangle(rr, brush_.Get());
        float cx = x + kTile / 2, cy = y0 + 18.f;
        if (i < n) {
            if (ID2D1Bitmap* icon = FavoriteIcon(static_cast<size_t>(i))) {
                rt_->DrawBitmap(icon, D2D1::RectF(cx - 12, cy - 12, cx + 12, cy + 12), 1.f,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                // Simge yoksa: renkli daire üstünde baş harf
                const std::wstring& label = favs_[static_cast<size_t>(i)].label;
                unsigned hsh = 0;
                for (wchar_t c : label) hsh = hsh * 31 + c;
                static const D2D1_COLOR_F kHues[] = {Rgba(0.95f, 0.36f, 0.36f), Rgba(0.36f, 0.62f, 1.f),
                                                     Rgba(0.35f, 0.8f, 0.5f),  Rgba(0.95f, 0.66f, 0.25f),
                                                     Rgba(0.7f, 0.5f, 1.f),    Rgba(0.2f, 0.75f, 0.8f)};
                brush_->SetColor(kHues[hsh % 6]);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 12, 12), brush_.Get());
                wchar_t letter[2] = {label.empty() ? L'?' : static_cast<wchar_t>(towupper(label[0])), 0};
                Text(letter, fAa_.Get(), D2D1::RectF(cx - 12, cy - 12, cx + 12, cy + 12), kWhite);
            }
            Text(favs_[static_cast<size_t>(i)].label.c_str(), fSmallCenter_.Get(),
                 D2D1::RectF(x + 2, y0 + 31, x + kTile - 2, y0 + 46), kGray);
        } else {
            // "+" kutucuğu: panodaki bağlantıyı ekler
            brush_->SetColor(Rgba(1, 1, 1, 0.75f));
            rt_->DrawLine(D2D1::Point2F(cx - 7, cy), D2D1::Point2F(cx + 7, cy), brush_.Get(), 2.f, roundStroke_.Get());
            rt_->DrawLine(D2D1::Point2F(cx, cy - 7), D2D1::Point2F(cx, cy + 7), brush_.Get(), 2.f, roundStroke_.Get());
            Text(L"Ekle", fSmallCenter_.Get(), D2D1::RectF(x + 2, y0 + 31, x + kTile - 2, y0 + 46), kGray);
        }
        PopFade();
        PopFade();
    }
}

void Island::OnDropUrl(const std::wstring& url) {
    dropHover_ = false;
    dropArmed_ = false;
    dropUrl_ = false;
    eyes_.SetMood(Eyes::Mood::Normal, Now(), 0);
    if (!AddFavorite(url)) {
        eyes_.SetMood(Eyes::Mood::Curious, Now(), 1.0);
        shake_.velocity += 160.f;
    }
    Update();
}

// ---------------------------------------------------------------------------
// Guti'nin göz rengi ve adı
// ---------------------------------------------------------------------------

void Island::LoadLook() {
    eyeMode_ = static_cast<int>(ReadSetting(L"EyeMode", 0));
    eyeRgb_ = static_cast<COLORREF>(ReadSetting(L"EyeRgb", RGB(255, 255, 255)));
    wchar_t buf[64]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Name", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS &&
        buf[0])
        name_ = buf;
    albumTint_ = ReadSetting(L"AlbumTint", 1) != 0;
    lyricsOn_ = ReadSetting(L"Lyrics", 1) != 0;
    lyricsPill_ = ReadSetting(L"LyricsPill", 1) != 0;
    ApplyEyeColor();
}

void Island::ApplyEyeColor() {
    D2D1_COLOR_F c = Rgba(0.93f, 0.97f, 1.f);
    if (eyeMode_ == 1) {
        c = Brighten(eyeRgb_);
    } else if (eyeMode_ == 2) {
        DWORD argb = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
            c = Brighten(RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF));
    }
    eyes_.SetBaseColor(c);
    StartAnimation();
}

void Island::SetEyeMode(int mode, COLORREF rgb) {
    eyeMode_ = mode;
    eyeRgb_ = rgb;
    WriteSetting(L"EyeMode", static_cast<DWORD>(mode));
    WriteSetting(L"EyeRgb", static_cast<DWORD>(rgb));
    ApplyEyeColor();
    // Yeni gözlerini beğendi.
    MoodWithSound(Eyes::Mood::Love, 1.6);
    height_.velocity += 90.f;
    lastActivity_ = Now();
}

void Island::SetName(std::wstring name) {
    name = Trim(name);
    if (name.size() > 20) name.resize(20);
    if (name.empty()) name = L"Guti";
    name_ = name;
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, L"Name", 0, REG_SZ, reinterpret_cast<const BYTE*>(name_.c_str()),
                       static_cast<DWORD>((name_.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    swprintf_s(nid.szTip, L"%s · Dynamis Island", name_.c_str());
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    alertLabel_ = L"Yeni adı: " + name_;
    ShowAlert(AlertKind::Copied, 0, 2500);
    height_.velocity += 80.f;
}

// ---------------------------------------------------------------------------
// Müzik uyku zamanlayıcısı: süre dolunca müzik yavaşça kısılıp durur, Guti de uyur.
// ---------------------------------------------------------------------------

void Island::CycleSleep() {
    int next = sleepMinutes_ == 0 ? 15 : (sleepMinutes_ == 15 ? 30 : (sleepMinutes_ == 30 ? 60 : 0));
    if (sleepFading_ && sleepRestoreVol_ >= 0) {
        volQuietUntil_ = Now() + 1.5;
        VolumeWatch::Set(sleepRestoreVol_);
    }
    sleepFading_ = false;
    sleepRestoreVol_ = -1;
    sleepMinutes_ = next;
    if (next == 0) {
        sleepEnd_ = 0;
        eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.6);
        Sounds::Play(Sounds::Sound::Close);
    } else {
        sleepEnd_ = Now() + next * 60.0;
        MoodWithSound(Eyes::Mood::Yawn, 1.4);  // "uykum geldi bile"
    }
    StartAnimation();
}

void Island::OnSleepTick() {
    if (sleepEnd_ <= 0) return;
    double now = Now(), left = sleepEnd_ - now;
    if (sleepFading_ && !snapshot_.playing && left > 0) {
        // Kullanıcı kendisi durdurdu: sesi hemen geri ver.
        if (sleepRestoreVol_ >= 0) {
            volQuietUntil_ = now + 1.5;
            VolumeWatch::Set(sleepRestoreVol_);
        }
        sleepRestoreVol_ = -1;
        sleepFading_ = false;
    }
    if (left > 0 && left <= 30 && snapshot_.playing) {
        if (sleepRestoreVol_ < 0) {
            int v = 0;
            bool m = false;
            if (VolumeWatch::Get(v, m) && !m && v > 0) {
                sleepRestoreVol_ = v;
                MoodWithSound(Eyes::Mood::Yawn, 2.5);
            }
        }
        if (sleepRestoreVol_ > 0) {
            sleepFading_ = true;
            VolumeWatch::Set(std::max(1, static_cast<int>(sleepRestoreVol_ * left / 30.0)));
        }
    }
    if (left <= 0) {
        sleepEnd_ = 0;
        sleepMinutes_ = 0;
        if (snapshot_.playing) media_.Send(MediaCommand::PlayPause);
        // Durunca ses eski düzeyine döner (müzik durduktan sonra).
        if (sleepRestoreVol_ >= 0) SetTimer(hwnd_, kTimerSleepRestore, 1500, nullptr);
        else sleepFading_ = false;
        eyes_.SetMood(Eyes::Mood::Sleepy, now, 0);
        Sounds::Play(Sounds::Sound::Yawn);
        lastActivity_ = now - 1000;
        StartAnimation();
    }
}
