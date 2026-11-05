#include "Notify.h"

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>

namespace NotifyMirror {
std::atomic<bool> g_enabled{true};
std::atomic<int> g_mode{0};
void RunWatcher(HWND notify);

namespace {

HWND g_notify = nullptr;

void Post(Toast* t) {
    if (t->title.empty() && t->body.empty()) {
        delete t;
        return;
    }
    if (!PostMessageW(g_notify, WM_TOAST, 0, reinterpret_cast<LPARAM>(t))) delete t;
}

// --- 1) Windows bildirim dinleyicisi --------------------------------------
bool RunListener() {
    using namespace winrt::Windows::UI::Notifications;
    using namespace winrt::Windows::UI::Notifications::Management;
    try {
        auto listener = UserNotificationListener::Current();
        auto status = listener.GetAccessStatus();
        if (status == UserNotificationListenerAccessStatus::Unspecified) status = listener.RequestAccessAsync().get();
        if (status != UserNotificationListenerAccessStatus::Allowed) return false;
        // Erişimi gerçekten var mı? (Paket kimliği olmayan uygulamalarda burada hata çıkar.)
        auto first = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
        std::set<uint32_t> seen;
        for (auto const& n : first) seen.insert(n.Id());
        g_mode = 1;
        for (;;) {
            Sleep(2500);
            if (!g_enabled) continue;
            auto list = listener.GetNotificationsAsync(NotificationKinds::Toast).get();
            std::set<uint32_t> now;
            for (auto const& n : list) {
                uint32_t id = n.Id();
                now.insert(id);
                if (seen.count(id)) continue;
                auto* t = new Toast;
                try {
                    t->app = n.AppInfo().DisplayInfo().DisplayName().c_str();
                } catch (...) {
                }
                try {
                    auto binding = n.Notification().Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                    if (binding) {
                        int i = 0;
                        for (auto const& e : binding.GetTextElements()) {
                            std::wstring text = e.Text().c_str();
                            if (i == 0) t->title = text;
                            else if (t->body.empty()) t->body = text;
                            ++i;
                        }
                    }
                } catch (...) {
                }
                Post(t);
            }
            seen.swap(now);
        }
    } catch (...) {
        return false;
    }
}

}  // namespace

void SetEnabled(bool on) { g_enabled = on; }
int Mode() { return g_mode; }

void Start(HWND notify) {
    g_notify = notify;
    std::thread([] {
        bool listened = false;
        {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            listened = RunListener();  // izin varsa burada kalır
            winrt::uninit_apartment();
        }
        if (!listened) RunWatcher(g_notify);
    }).detach();
}

}  // namespace NotifyMirror
