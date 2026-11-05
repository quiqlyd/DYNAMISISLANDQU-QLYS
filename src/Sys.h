#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Araç çentiğinin sistem tarafı: yazı tanıma, kablosuz radyolar, ekran parlaklığı.
// Hepsi kısa ömürlü arka plan iş parçacıklarında çalışır; boşta hiçbir şey yapmaz.

// Windows'un kendi yazı tanıması (internet yok). Sonuç pencereye mesajla gelir:
// wParam = satır sayısı (0: yazı yok, -1: tanıma dili yüklü değil, -2: hata),
// lParam = new std::wstring(metin); alıcı siler.
namespace Ocr {
constexpr UINT WM_OCR_DONE = WM_APP + 20;
// bgra: üstten alta, satır başına w*4 bayt, 32 bit BGRA.
void Recognize(HWND notify, std::vector<BYTE> bgra, UINT w, UINT h);
}  // namespace Ocr

// Wi-Fi ve Bluetooth açık/kapalı (Windows.Devices.Radios).
// wParam: bit0 Wi-Fi var, bit1 Wi-Fi açık, bit2 Bluetooth var, bit3 Bluetooth açık.
namespace Radios {
constexpr UINT WM_RADIOS = WM_APP + 21;
constexpr int kWifi = 1, kWifiOn = 2, kBt = 4, kBtOn = 8;
void Refresh(HWND notify);
void Toggle(HWND notify, bool wifi);
}  // namespace Radios

// Dizüstü ekranının parlaklığı (WMI). wParam = 0..100, ya da 255: desteklenmiyor.
namespace Brightness {
constexpr UINT WM_BRIGHTNESS = WM_APP + 22;
void Refresh(HWND notify);
// Sürüklerken art arda çağrılabilir: arka planda sadece en son değer uygulanır.
void Set(HWND notify, int percent);
}  // namespace Brightness
