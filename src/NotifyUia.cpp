// Bildirim aynası, 2. yol: bildirim penceresi açılınca UI Automation ile yazılarını oku.
// winrt başlıklarıyla çakıştığı için ayrı dosyada.
#include "Notify.h"

#include <ole2.h>  // WIN32_LEAN_AND_MEAN ile "interface" tanımı buradan gelir
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <wrl/client.h>
#include <atomic>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace NotifyMirror {
extern std::atomic<bool> g_enabled;
extern std::atomic<int> g_mode;

namespace {

HWND g_target = nullptr;

void Post(Toast* t) {
    if (t->title.empty() && t->body.empty()) {
        delete t;
        return;
    }
    if (!PostMessageW(g_target, WM_TOAST, 0, reinterpret_cast<LPARAM>(t))) delete t;
}

// --- 2) Bildirim penceresini izle ve yazılarını oku --------------------------
ComPtr<IUIAutomation> g_uia;
HWND g_pending = nullptr;
DWORD g_lastToast = 0;
constexpr UINT_PTR kReadTimer = 77;

bool IsToastWindow(HWND hwnd) {
    wchar_t cls[64]{}, title[128]{};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") != 0) return false;
    GetWindowTextW(hwnd, title, 128);
    _wcslwr_s(title);
    return wcsstr(title, L"bildirim") || wcsstr(title, L"notification");
}

void ReadToast(HWND hwnd) {
    if (!g_uia || !IsWindow(hwnd)) return;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(g_uia->ElementFromHandle(hwnd, &root)) || !root) return;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = UIA_TextControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(g_uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &cond))) return;
    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr) return;
    int n = 0;
    arr->get_Length(&n);
    std::vector<std::wstring> texts;
    for (int i = 0; i < n && texts.size() < 4; ++i) {
        ComPtr<IUIAutomationElement> el;
        BSTR name = nullptr;
        if (SUCCEEDED(arr->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentName(&name)) && name) {
            std::wstring s(name, SysStringLen(name));
            SysFreeString(name);
            if (!s.empty() && s.size() < 400) texts.push_back(s);
        }
    }
    if (texts.empty()) return;
    auto* t = new Toast;
    if (texts.size() >= 3) {
        t->app = texts[0];
        t->title = texts[1];
        t->body = texts[2];
    } else {
        t->title = texts[0];
        if (texts.size() > 1) t->body = texts[1];
    }
    Post(t);
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_enabled || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (!IsToastWindow(hwnd)) return;
    DWORD now = GetTickCount();
    if (hwnd == g_pending && now - g_lastToast < 800) return;
    g_lastToast = now;
    g_pending = hwnd;
    // İçerik pencere görünür olduktan biraz sonra dolar.
    SetTimer(nullptr, kReadTimer, 450, nullptr);
}

void RunWatcherImpl() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia));
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook || !g_uia) {
        g_mode = 3;
        return;
    }
    g_mode = 2;
    MSG msg;
    UINT_PTR timer = 0;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
            KillTimer(nullptr, msg.wParam);
            (void)timer;
            HWND h = g_pending;
            g_pending = nullptr;
            ReadToast(h);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWinEvent(hook);
}

}  // namespace

void RunWatcher(HWND notify) {
    g_target = notify;
    RunWatcherImpl();
}

}  // namespace NotifyMirror
