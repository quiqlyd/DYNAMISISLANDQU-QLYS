#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Senkron şarkı sözleri. Kaynak sırası:
//   1) %LOCALAPPDATA%\DynamisIsland\sozler\  "Sanatçı - Şarkı.lrc"  (kullanıcının kendi dosyaları)
//   2) önbellek (lyrics\), 3) lrclib.net (ücretsiz, anahtarsız; şarkı başına bir istek).
// Sonuç pencereye WM_LYRICS ile gelir: lParam = new Lyrics::Result; alıcı siler.
namespace Lyrics {

constexpr UINT WM_LYRICS = WM_APP + 30;

struct Line {
    double t;  // saniye
    std::wstring text;
};

struct Result {
    uint64_t token = 0;
    bool instrumental = false;
    std::vector<Line> lines;  // boş: söz bulunamadı
};

void Fetch(HWND notify, uint64_t token, std::wstring artist, std::wstring title, double durationSec);

// LRC metnini satırlara ayırır (test edilebilir, Windows'a bağlı değil).
std::vector<Line> ParseLrc(const std::wstring& lrc);
// YouTube/tarayıcı başlıklarını temizler: "(Official Video)", "[4K]", "ft. X", "Sanatçı - Şarkı" ...
void CleanTitle(std::wstring& artist, std::wstring& title);

}  // namespace Lyrics
