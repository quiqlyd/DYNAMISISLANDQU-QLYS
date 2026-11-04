#pragma once
#include <windows.h>
#include <string>

#ifndef APP_VERSION
#define APP_VERSION 0
#endif

// GitHub Releases üzerinden kendini günceller:
// son sürümü sorar, yeni .exe'yi indirir, eskisinin yerine koyar ve yeniden başlatır.
namespace Updater {

constexpr int kCurrentVersion = APP_VERSION;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 3;

enum class Result : WPARAM {
    UpToDate,      // lParam: son sürüm
    Available,     // lParam: yeni sürüm numarası
    Installed,     // yeni sürüm yerine kondu, yeniden başlatılıyor
    Failed,
};

// Arka planda son sürümü sorar; sonucu WM_UPDATE_RESULT ile bildirir.
// install = true ise yeni sürüm varsa hemen indirip kurar.
void CheckAsync(HWND notify, bool install);

// Açılışta: önceki güncellemeden kalan eski dosyayı siler.
void CleanupOldFiles();

// Yeni sürümü yeniden başlatır (çağırandan sonra süreç kapanmalı).
bool RelaunchUpdated();

}  // namespace Updater
