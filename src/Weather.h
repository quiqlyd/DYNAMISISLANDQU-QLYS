#pragma once
#include <windows.h>
#include <string>

// Hava durumu: Open-Meteo (ücretsiz, anahtarsız). Konum: ayarlardaki şehir, yoksa IP'den tahmin.
// Arka planda sorulur; sonuç WM_WEATHER ile gelir: lParam = new Weather::Info; alıcı siler.
namespace Weather {

constexpr UINT WM_WEATHER = WM_APP + 32;

struct Info {
    bool ok = false;
    double temp = 0;   // °C
    int code = -1;     // WMO hava kodu
    bool day = true;
    std::wstring city;
};

void Refresh(HWND notify, std::wstring city);

bool IsRain(int code);
bool IsSnow(int code);
bool IsClear(int code);
bool IsStorm(int code);
// "☀ 21°" gibi kısa metin
std::wstring Short(const Info& info);

}  // namespace Weather
