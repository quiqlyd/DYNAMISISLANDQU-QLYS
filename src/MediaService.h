#pragma once
#include <windows.h>
#include <unknwn.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

// Windows'un "Sistem Medya Kontrolleri" üzerinden çalan şarkıyı okur
// (Spotify, tarayıcılar, Medya Oynatıcı ...). Değişince pencereye mesaj atar.
struct MediaSnapshot {
    bool hasSession = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    double positionSec = 0;   // stamp anındaki konum
    double durationSec = 0;
    std::chrono::steady_clock::time_point stamp{};
    Microsoft::WRL::ComPtr<IWICBitmapSource> art;  // 32bppPBGRA, null olabilir
    uint64_t artVersion = 0;

    double CurrentPosition() const;
};

enum class MediaCommand { Previous, PlayPause, Next };

class MediaService {
public:
    static constexpr UINT WM_MEDIA_CHANGED = WM_APP + 1;

    void Start(HWND notifyWindow);
    MediaSnapshot Snapshot();
    void Send(MediaCommand cmd);

private:
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;

    void OnSessionChanged();
    void Refresh();
    Microsoft::WRL::ComPtr<IWICBitmapSource> LoadThumbnail(
        winrt::Windows::Storage::Streams::IRandomAccessStreamReference const& ref);
    Session CurrentSession();

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;

    Manager manager_{nullptr};
    Manager::CurrentSessionChanged_revoker sessionChangedRevoker_;

    std::mutex sessionMx_;  // session_ ve revoker'lar
    Session session_{nullptr};
    Session::MediaPropertiesChanged_revoker propsRevoker_;
    Session::PlaybackInfoChanged_revoker playbackRevoker_;
    Session::TimelinePropertiesChanged_revoker timelineRevoker_;

    std::mutex refreshMx_;  // aynı anda tek Refresh
    std::wstring artKey_;
    bool artLoaded_ = false;

    std::mutex stateMx_;
    MediaSnapshot state_;
};
