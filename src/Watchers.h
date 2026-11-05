#pragma once
#include <windows.h>
#include <string>

// Guti'nin "duyuları": bilgisayarın sağlığı, kamera/mikrofon ve indirmeler.
// Hepsi çok ucuzdur: birkaç saniyede bir küçük bir okuma ya da olay bekleyen uyuyan bir iş
// parçacığı. Island, sonuçlara göre Guti'yi oynatır.

// İşlemci, bellek ve internet.
class SystemHealth {
public:
    enum class Net { Unknown, Offline, Online };
    void Sample();  // ~2 sn'de bir çağrılır
    float Cpu() const { return cpu_; }  // 0..1
    float Ram() const { return ram_; }  // 0..1
    Net Network() const { return net_; }

private:
    ULONGLONG lastIdle_ = 0, lastTotal_ = 0;
    float cpu_ = 0, ram_ = 0;
    Net net_ = Net::Unknown;
    using HintFn = DWORD(WINAPI*)(void*);
    HintFn hint_ = nullptr;
    bool hintLoaded_ = false;
};

// Kamera ve mikrofonu kullanan bir uygulama var mı? Windows'un gizlilik kaydından okunur;
// kayıt değişmedikçe hiçbir şey yapılmaz.
class PrivacyWatch {
public:
    ~PrivacyWatch();
    void Start();
    bool Poll();  // değişiklik olduysa true
    bool Camera() const { return camera_; }
    bool Mic() const { return mic_; }

private:
    struct Watch {
        HKEY key = nullptr;
        HANDLE event = nullptr;
    };
    static bool InUse(HKEY key);
    void Arm(Watch& w);
    Watch cam_, micW_;
    bool camera_ = false, mic_ = false;
};

// İndirilenler klasörü: biten bir dosya gelince pencereye mesaj gönderir.
// lParam = new std::wstring(tam yol); alıcı silmelidir.
namespace DownloadWatch {
constexpr UINT WM_DOWNLOAD_DONE = WM_APP + 4;
void Start(HWND notify);
}  // namespace DownloadWatch
