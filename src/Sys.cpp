#include "Sys.h"

#include <objbase.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Radios.h>

#pragma comment(lib, "wbemuuid.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Yazı tanıma
// ---------------------------------------------------------------------------

void Ocr::Recognize(HWND notify, std::vector<BYTE> bgra, UINT w, UINT h) {
    std::thread([notify, px = std::move(bgra), w, h]() {
        int code = -2;
        auto* text = new std::wstring();
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        try {
            using namespace winrt::Windows::Media::Ocr;
            using namespace winrt::Windows::Graphics::Imaging;
            // Önce kullanıcının dilleri (Türkçe), yoksa İngilizce.
            OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine) {
                winrt::Windows::Globalization::Language en(L"en-US");
                if (OcrEngine::IsLanguageSupported(en)) engine = OcrEngine::TryCreateFromLanguage(en);
            }
            if (!engine) {
                code = -1;
            } else {
                winrt::Windows::Storage::Streams::Buffer buf(static_cast<uint32_t>(px.size()));
                std::memcpy(buf.data(), px.data(), px.size());
                buf.Length(static_cast<uint32_t>(px.size()));
                SoftwareBitmap bmp = SoftwareBitmap::CreateCopyFromBuffer(
                    buf, BitmapPixelFormat::Bgra8, static_cast<int32_t>(w), static_cast<int32_t>(h),
                    BitmapAlphaMode::Premultiplied);
                OcrResult result = engine.RecognizeAsync(bmp).get();
                int lines = 0;
                for (const auto& line : result.Lines()) {
                    if (!text->empty()) *text += L"\r\n";
                    *text += line.Text().c_str();
                    ++lines;
                }
                code = lines;
            }
        } catch (...) {
            code = -2;
        }
        if (!PostMessageW(notify, WM_OCR_DONE, static_cast<WPARAM>(code), reinterpret_cast<LPARAM>(text)))
            delete text;
        if (com) CoUninitialize();
    }).detach();
}

// ---------------------------------------------------------------------------
// Wi-Fi / Bluetooth
// ---------------------------------------------------------------------------

namespace {
void RadioJob(HWND notify, int toggle) {  // toggle: 0 yok, 1 Wi-Fi, 2 Bluetooth
    std::thread([notify, toggle] {
        int bits = 0;
        bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        try {
            using namespace winrt::Windows::Devices::Radios;
            Radio::RequestAccessAsync().get();
            auto radios = Radio::GetRadiosAsync().get();
            for (const auto& r : radios) {
                bool wifi = r.Kind() == RadioKind::WiFi, bt = r.Kind() == RadioKind::Bluetooth;
                if (!wifi && !bt) continue;
                if ((toggle == 1 && wifi) || (toggle == 2 && bt)) {
                    RadioState next = r.State() == RadioState::On ? RadioState::Off : RadioState::On;
                    r.SetStateAsync(next).get();
                }
                bool on = r.State() == RadioState::On;
                if (wifi) bits |= Radios::kWifi | (on ? Radios::kWifiOn : 0);
                if (bt) bits |= Radios::kBt | (on ? Radios::kBtOn : 0);
            }
        } catch (...) {
        }
        PostMessageW(notify, Radios::WM_RADIOS, static_cast<WPARAM>(bits), 0);
        if (com) CoUninitialize();
    }).detach();
}
}  // namespace

void Radios::Refresh(HWND notify) { RadioJob(notify, 0); }
void Radios::Toggle(HWND notify, bool wifi) { RadioJob(notify, wifi ? 1 : 2); }

// ---------------------------------------------------------------------------
// Parlaklık (WMI: dizüstü panelleri)
// ---------------------------------------------------------------------------

namespace {
std::mutex g_brMx;
std::condition_variable g_brCv;
bool g_brStarted = false;
bool g_brRead = false;  // okuma istendi
int g_brWant = -1;      // uygulanacak değer
HWND g_brNotify = nullptr;

class Wmi {
public:
    bool Connect() {
        if (svc_) return true;
        ComPtr<IWbemLocator> loc;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&loc))))
            return false;
        BSTR ns = SysAllocString(L"ROOT\\WMI");
        HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc_);
        SysFreeString(ns);
        if (FAILED(hr)) return false;
        CoSetProxyBlanket(svc_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        return true;
    }

    ComPtr<IWbemClassObject> First(const wchar_t* query) {
        ComPtr<IEnumWbemClassObject> en;
        BSTR lang = SysAllocString(L"WQL"), q = SysAllocString(query);
        HRESULT hr = svc_->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
        SysFreeString(lang);
        SysFreeString(q);
        ComPtr<IWbemClassObject> obj;
        ULONG n = 0;
        if (SUCCEEDED(hr) && en) en->Next(3000, 1, &obj, &n);
        return n ? obj : nullptr;
    }

    int Read() {
        if (!Connect()) return 255;
        auto obj = First(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
        if (!obj) return 255;
        VARIANT v;
        VariantInit(&v);
        int result = 255;
        if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) {
            if (v.vt == VT_UI1) result = v.bVal;
            else if (v.vt == VT_I4) result = v.lVal;
        }
        VariantClear(&v);
        return result;
    }

    bool Write(int percent) {
        if (!Connect()) return false;
        auto inst = First(L"SELECT * FROM WmiMonitorBrightnessMethods");
        if (!inst) return false;
        VARIANT path;
        VariantInit(&path);
        if (FAILED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) {
            VariantClear(&path);
            return false;
        }
        ComPtr<IWbemClassObject> cls, sig, in;
        BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
        BSTR method = SysAllocString(L"WmiSetBrightness");
        bool ok = SUCCEEDED(svc_->GetObject(clsName, 0, nullptr, &cls, nullptr)) &&
                  SUCCEEDED(cls->GetMethod(method, 0, &sig, nullptr)) && sig &&
                  SUCCEEDED(sig->SpawnInstance(0, &in));
        if (ok) {
            VARIANT t, b;
            VariantInit(&t);
            VariantInit(&b);
            t.vt = VT_I4;
            t.lVal = 0;
            b.vt = VT_UI1;
            b.bVal = static_cast<BYTE>(percent);
            in->Put(L"Timeout", 0, &t, 0);
            in->Put(L"Brightness", 0, &b, 0);
            ok = SUCCEEDED(svc_->ExecMethod(path.bstrVal, method, 0, nullptr, in.Get(), nullptr, nullptr));
        }
        SysFreeString(clsName);
        SysFreeString(method);
        VariantClear(&path);
        return ok;
    }

private:
    ComPtr<IWbemServices> svc_;
};

void BrightnessWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);  // süreçte zaten ayarlıysa başarısız olur, sorun değil
    Wmi wmi;
    for (;;) {
        int want;
        HWND notify;
        {
            std::unique_lock lock(g_brMx);
            g_brCv.wait(lock, [] { return g_brRead || g_brWant >= 0; });
            want = g_brWant;
            notify = g_brNotify;
            g_brWant = -1;
            g_brRead = false;
        }
        int value;
        if (want >= 0) value = wmi.Write(want) ? want : 255;
        else value = wmi.Read();
        PostMessageW(notify, Brightness::WM_BRIGHTNESS, static_cast<WPARAM>(value), 0);
    }
}

void BrightnessRequest(HWND notify, int want) {
    std::lock_guard lock(g_brMx);
    g_brNotify = notify;
    if (want >= 0) g_brWant = want;
    else g_brRead = true;
    if (!g_brStarted) {
        g_brStarted = true;
        std::thread(BrightnessWorker).detach();
    }
    g_brCv.notify_one();
}
}  // namespace

void Brightness::Refresh(HWND notify) { BrightnessRequest(notify, -1); }
void Brightness::Set(HWND notify, int percent) {
    BrightnessRequest(notify, percent < 0 ? 0 : (percent > 100 ? 100 : percent));
}
