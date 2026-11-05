#include "MediaService.h"

#include <shcore.h>
#include <thread>
#include <winrt/Windows.Storage.Streams.h>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Media::Control;
using winrt::Windows::Storage::Streams::IRandomAccessStreamReference;

double MediaSnapshot::CurrentPosition() const {
    double pos = positionSec;
    if (playing) {
        pos += std::chrono::duration<double>(std::chrono::steady_clock::now() - stamp).count();
    }
    if (durationSec > 0 && pos > durationSec) pos = durationSec;
    return pos < 0 ? 0 : pos;
}

void MediaService::Start(HWND notifyWindow) {
    hwnd_ = notifyWindow;
    // Yönetici arka planda alınır, açılış gecikmesin. Kapak resimleri bu MTA tarafında
    // çözüldüğü için WIC fabrikası da burada, kendi dairesinde oluşturulur.
    std::thread([this] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
        try {
            manager_ = Manager::RequestAsync().get();
            sessionChangedRevoker_ = manager_.CurrentSessionChanged(
                winrt::auto_revoke, [this](auto&&, auto&&) { OnSessionChanged(); });
            OnSessionChanged();
        } catch (...) {
            // Medya API'si yoksa ada yine de saatle çalışır.
        }
    }).detach();
}

MediaService::Session MediaService::CurrentSession() {
    std::lock_guard g(sessionMx_);
    return session_;
}

void MediaService::OnSessionChanged() {
    {
        std::lock_guard g(sessionMx_);
        propsRevoker_ = {};
        playbackRevoker_ = {};
        timelineRevoker_ = {};
        session_ = nullptr;
        try {
            session_ = manager_.GetCurrentSession();
        } catch (...) {
        }
        if (session_) {
            propsRevoker_ = session_.MediaPropertiesChanged(
                winrt::auto_revoke, [this](auto&&, auto&&) { Refresh(); });
            playbackRevoker_ = session_.PlaybackInfoChanged(
                winrt::auto_revoke, [this](auto&&, auto&&) { Refresh(); });
            timelineRevoker_ = session_.TimelinePropertiesChanged(
                winrt::auto_revoke, [this](auto&&, auto&&) { Refresh(); });
        }
    }
    Refresh();
}

void MediaService::Refresh() {
    std::lock_guard refreshLock(refreshMx_);
    Session s = CurrentSession();

    MediaSnapshot snap;
    {
        std::lock_guard g(stateMx_);
        snap.art = state_.art;
        snap.artVersion = state_.artVersion;
    }

    if (s) {
        try {
            auto props = s.TryGetMediaPropertiesAsync().get();
            snap.hasSession = true;
            snap.title = props.Title().c_str();
            snap.artist = props.Artist().c_str();

            auto playback = s.GetPlaybackInfo();
            snap.playing = playback.PlaybackStatus() ==
                           GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

            auto timeline = s.GetTimelineProperties();
            auto toSec = [](winrt::Windows::Foundation::TimeSpan t) {
                return std::chrono::duration<double>(t).count();
            };
            snap.durationSec = toSec(timeline.EndTime() - timeline.StartTime());
            snap.positionSec = toSec(timeline.Position() - timeline.StartTime());
            if (snap.playing) {
                auto since = winrt::clock::now() - timeline.LastUpdatedTime();
                double add = std::chrono::duration<double>(since).count();
                if (add > 0 && add < 24 * 3600) snap.positionSec += add;
            }
            snap.stamp = std::chrono::steady_clock::now();

            std::wstring key = snap.title + L"\x1f" + snap.artist;
            if (key != artKey_ || !artLoaded_) {
                if (key != artKey_) {
                    snap.art.Reset();
                    snap.artVersion++;
                }
                artKey_ = key;
                artLoaded_ = false;
                if (auto thumb = props.Thumbnail()) {
                    if (auto bmp = LoadThumbnail(thumb)) {
                        snap.art = bmp;
                        snap.artVersion++;
                        artLoaded_ = true;
                    }
                }
            }
        } catch (...) {
            snap.hasSession = false;
        }
    }

    if (!snap.hasSession) {
        snap.art.Reset();
        artKey_.clear();
        artLoaded_ = false;
    }

    {
        std::lock_guard g(stateMx_);
        state_ = snap;
    }
    PostMessageW(hwnd_, WM_MEDIA_CHANGED, 0, 0);
}

ComPtr<IWICBitmapSource> MediaService::LoadThumbnail(IRandomAccessStreamReference const& ref) {
    try {
        auto stream = ref.OpenReadAsync().get();
        ComPtr<IStream> istream;
        if (FAILED(CreateStreamOverRandomAccessStream(
                static_cast<::IUnknown*>(winrt::get_abi(stream)), IID_PPV_ARGS(&istream))))
            return nullptr;

        if (!wic_) return nullptr;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wic_->CreateDecoderFromStream(istream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnLoad, &decoder)))
            return nullptr;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;

        // Kapak küçük tutulur: bellek ve çizim maliyeti düşük kalsın.
        UINT w = 0, h = 0;
        frame->GetSize(&w, &h);
        ComPtr<IWICBitmapSource> source = frame;
        constexpr UINT kMax = 160;
        if (w > kMax || h > kMax) {
            ComPtr<IWICBitmapScaler> scaler;
            if (SUCCEEDED(wic_->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(frame.Get(), kMax, kMax * h / (w ? w : 1),
                                             WICBitmapInterpolationModeFant)))
                source = scaler;
        }

        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wic_->CreateFormatConverter(&converter))) return nullptr;
        if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                         WICBitmapDitherTypeNone, nullptr, 0,
                                         WICBitmapPaletteTypeMedianCut)))
            return nullptr;

        // Belleğe alınır; böylece akış kapatılabilir ve UI iş parçacığı güvenle okur.
        ComPtr<IWICBitmap> bitmap;
        if (FAILED(wic_->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &bitmap)))
            return nullptr;
        return bitmap;
    } catch (...) {
        return nullptr;
    }
}

MediaSnapshot MediaService::Snapshot() {
    std::lock_guard g(stateMx_);
    return state_;
}

void MediaService::Send(MediaCommand cmd) {
    Session s = CurrentSession();
    if (!s) return;
    // Beklemeden gönderilir; sonuç olay olarak geri gelir.
    std::thread([s, cmd] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            switch (cmd) {
            case MediaCommand::Previous: s.TrySkipPreviousAsync().get(); break;
            case MediaCommand::PlayPause: s.TryTogglePlayPauseAsync().get(); break;
            case MediaCommand::Next: s.TrySkipNextAsync().get(); break;
            }
        } catch (...) {
        }
    }).detach();
}

void MediaService::Seek(double seconds) {
    Session s = CurrentSession();
    if (!s) return;
    int64_t ticks = static_cast<int64_t>((seconds < 0 ? 0 : seconds) * 10000000.0);  // 100 ns birimi
    std::thread([s, ticks] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            s.TryChangePlaybackPositionAsync(ticks).get();
        } catch (...) {
        }
    }).detach();
}
