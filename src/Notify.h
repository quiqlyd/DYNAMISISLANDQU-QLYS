#pragma once
#include <windows.h>
#include <string>

// Bildirim aynası: Windows'ta bir bildirim (toast) çıkınca adada da kısaca görünür.
// İki yol: Windows'un bildirim dinleyicisi (izin verilirse) ya da bildirim penceresi açılınca
// Erişilebilirlik (UI Automation) ile içindeki yazıları okumak. Hepsi arka plan iş parçacığında.
// Sonuç WM_TOAST ile gelir: lParam = new NotifyMirror::Toast; alıcı siler.
namespace NotifyMirror {

constexpr UINT WM_TOAST = WM_APP + 33;

struct Toast {
    std::wstring app, title, body;
};

void Start(HWND notify);
void SetEnabled(bool on);
// 0 başlıyor, 1 dinleyici, 2 pencere izleme, 3 çalışmıyor
int Mode();

}  // namespace NotifyMirror
