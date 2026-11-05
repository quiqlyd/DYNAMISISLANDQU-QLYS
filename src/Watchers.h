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

// Hoparlöre giden sesin anlık seviyesi (Windows'un ses ölçeri). Sadece müzik çalarken ve
// ada görünürken okunur; ses verisinin kendisine dokunmaz, sadece 0..1 tepe değeri.
class AudioMeter {
public:
    ~AudioMeter();
    float Peak(double now);  // < 0: ölçer yok

private:
    void Acquire();
    struct IAudioMeterInformation* meter_ = nullptr;
    double acquiredAt_ = -1e9;
};

// İndirilenler klasörü: biten bir dosya gelince pencereye mesaj gönderir.
// lParam = new std::wstring(tam yol); alıcı silmelidir.
namespace DownloadWatch {
constexpr UINT WM_DOWNLOAD_DONE = WM_APP + 4;
void Start(HWND notify);
}  // namespace DownloadWatch

// Ses düzeyi değişince haber verir (Windows'un ses olayları; yoklama yok).
// wParam = yüzde (0-100), lParam = sessiz mi.
namespace VolumeWatch {
constexpr UINT WM_VOLUME_CHANGED = WM_APP + 5;
constexpr UINT WM_VOLUME_DEVICE = WM_APP + 6;  // varsayılan hoparlör değişti: Rebind çağır
void Start(HWND notify);
void Rebind();
}  // namespace VolumeWatch

// Bluetooth kulaklık pili: arka planda 30 sn'de bir bakar, değişince haber verir.
// wParam = pil yüzdesi (bağlı cihaz yoksa 255), lParam = new std::wstring(ad); alıcı siler.
namespace BtBattery {
constexpr UINT WM_BT_BATTERY = WM_APP + 7;
void Start(HWND notify);
}  // namespace BtBattery
