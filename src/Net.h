#pragma once
#include <windows.h>
#include <string>

// Küçük ağ ve dosya yardımcıları: şarkı sözleri ve site simgeleri bunlarla gelir.
// Hepsi arka plan iş parçacıklarından çağrılır; arayüzü hiç bekletmez.
namespace Net {

// HTTPS GET. status: sunucunun cevabı (0: bağlanılamadı). 200 dışında da gövde okunur.
bool Get(const std::wstring& url, std::string& body, size_t maxBytes, DWORD* status = nullptr);

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
// URL sorgu parametresi için yüzde kodlama (UTF-8).
std::wstring Encode(const std::wstring& text);

// %LOCALAPPDATA%\DynamisIsland\<sub>\ (yoksa oluşturulur). Sonunda ters bölü vardır.
std::wstring DataDir(const wchar_t* sub = nullptr);
bool ReadFile(const std::wstring& path, std::string& out, size_t maxBytes);
bool WriteFile(const std::wstring& path, const std::string& data);

}  // namespace Net
