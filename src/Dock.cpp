// Guti Dock: yeni taskbar. Bkz. Dock.h
#include "Dock.h"

#include <appmodel.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <ole2.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <winsock2.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <thread>
#include <unordered_map>

#include "IslandUi.h"
#include "Sounds.h"

using Microsoft::WRL::ComPtr;
using namespace isl;

namespace {

// Ölçüler (DIP)
constexpr float kIcon = 36.f;      // ikon boyu (gövdenin ~%72'si, macOS dock oranı)
constexpr float kSlot = 43.f;      // ikon + aralık (ikonlar sık dizilir)
constexpr float kSepW = 8.f;       // ayırıcı genişliği
constexpr float kBodyH = 50.f;     // cam gövde yüksekliği
constexpr float kBottom = 6.f;     // ekranın altından boşluk
constexpr float kPadX = 6.f;       // gövde iç yan boşluğu
constexpr float kRadius = 16.f;    // gövde köşe yarıçapı
constexpr float kMaxGrow = 0.f;    // büyütme yok: sade, Windows tarzı
constexpr float kRange = 150.f;    // büyütmenin etki yarıçapı
constexpr float kTopRoom = 96.f;   // gövdenin üstünde büyüyen ikon + isim balonu için yer
constexpr float kSideRoom = 110.f; // yanlarda büyüme ve gölge payı
constexpr float kIconBottom = 5.f + kIcon;  // üstte 5, altta açık noktası için 9 DIP
constexpr float kBounce = 6.f, kBouncePeriod = 0.62f;
constexpr float kPanelRoom = 330.f; // önizleme / yığın paneli için gövdenin üstünde ek yer
constexpr float kControlW = 150.f;   // kontrol hapı genişliği
constexpr float kCardW = 196.f, kThumbH = 116.f, kCardGap = 10.f, kPanelPad = 10.f;
constexpr float kStackW = 300.f, kStackRow = 30.f;
constexpr int kStackMax = 8;

constexpr UINT kFrameMsg = WM_APP + 60;
constexpr UINT kAppBarMsg = WM_APP + 61;
constexpr UINT kRehideMsg = WM_APP + 62;
constexpr UINT_PTR kTimerRescan = 1;
constexpr UINT_PTR kTimerHoverLabel = 2;
constexpr UINT_PTR kTimerPreview = 3;    // fare ikonda durunca önizleme açılır
constexpr UINT_PTR kTimerPanelOut = 4;   // panel dışına çıkınca kapanır
constexpr UINT_PTR kTimerAutoHide = 5;   // otomatik gizlemede fare ekranın altına geldi mi?
constexpr UINT_PTR kTimerBlink = 6;      // mini Guti ara sıra göz kırpar
constexpr UINT_PTR kTimerTrayPeek = 8;   // gizli simgeler kapanınca taskbar yine gizlenir
constexpr UINT_PTR kTimerClock = 9;      // saat dakika değişince yeniden çizilir
constexpr UINT_PTR kTimerFly = 10;       // açılan Windows panelini dock'un üstüne taşır
constexpr UINT_PTR kTimerWatch = 7;      // kare akışı takılırsa animasyonu yine de yürütür

constexpr wchar_t kPinnedDir[] = L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

// PKEY_AppUserModel_ID (propkey.h'yi INITGUID ile çekmemek için)
const PROPERTYKEY kAumidKey = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

Dock* g_dock = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    Dock::RestoreTaskbarNow();  // çökse bile kullanıcı taskbar'sız kalmasın
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* tail) {
    size_t n = wcslen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::wstring ProcessPath(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return out;
    wchar_t buf[MAX_PATH * 2];
    DWORD len = ARRAYSIZE(buf);
    if (QueryFullProcessImageNameW(p, 0, buf, &len)) out.assign(buf, len);
    CloseHandle(p);
    return out;
}

std::wstring ProcessAumid(DWORD pid) {
    std::wstring out;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return out;
    wchar_t buf[APPLICATION_USER_MODEL_ID_MAX_LENGTH];
    UINT32 len = ARRAYSIZE(buf);
    if (GetApplicationUserModelId(p, &len, buf) == ERROR_SUCCESS) out = buf;
    CloseHandle(p);
    return out;
}

std::wstring WindowAumid(HWND hwnd) {
    std::wstring out;
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return out;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(store->GetValue(kAumidKey, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
    PropVariantClear(&pv);
    return out;
}

std::wstring FileDescription(const std::wstring& exe) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &handle);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    struct Lang { WORD lang, cp; }* langs = nullptr;
    UINT n = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &n) || !n)
        return {};
    wchar_t q[80];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
    wchar_t* text = nullptr;
    UINT tl = 0;
    if (VerQueryValueW(data.data(), q, reinterpret_cast<void**>(&text), &tl) && text && *text) return text;
    return {};
}

std::wstring ShellName(const std::wstring& parsing) {
    std::wstring out;
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item)))) return out;
    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        out = name;
        CoTaskMemFree(name);
    }
    return out;
}

// .lnk dosyasının hedef exe'si ve AUMID'si
void ResolveLink(const std::wstring& lnk, std::wstring& exe, std::wstring& aumid) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)) || FAILED(file->Load(lnk.c_str(), STGM_READ))) return;
    wchar_t path[MAX_PATH * 2] = {};
    if (SUCCEEDED(link->GetPath(path, ARRAYSIZE(path), nullptr, 0)) && path[0]) {
        wchar_t expanded[MAX_PATH * 2];
        if (ExpandEnvironmentStringsW(path, expanded, ARRAYSIZE(expanded))) exe = expanded;
        else exe = path;
    }
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link.As(&store))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kAumidKey, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) aumid = pv.pwszVal;
        PropVariantClear(&pv);
    }
}

// Uygulamanın Windows görev çubuğundaki "Görevler" listesi (ör. Edge: Yeni pencere, Yeni InPrivate penceresi).
// Uygulamalar bu listeyi CustomDestinations klasörüne kaydeder; dosyadaki her kısayol bir ShellLink akışıdır.
struct JumpTask {
    std::wstring title, path, args, dir;
};
std::vector<JumpTask> JumpTasks(const std::wstring& exeLower) {
    std::vector<JumpTask> out;
    if (exeLower.empty()) return out;
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) || !roaming) return out;
    std::wstring dir = std::wstring(roaming) + L"\\Microsoft\\Windows\\Recent\\CustomDestinations\\";
    CoTaskMemFree(roaming);
    static const BYTE kLinkClsid[16] = {0x01, 0x14, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
    const PROPERTYKEY kTitle = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"*.customDestinations-ms").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.nFileSizeHigh || fd.nFileSizeLow > 4 * 1024 * 1024) continue;
        HANDLE f = CreateFileW((dir + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::vector<BYTE> buf(fd.nFileSizeLow);
        DWORD got = 0;
        BOOL ok = ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
        CloseHandle(f);
        if (!ok || got != buf.size()) continue;
        std::vector<JumpTask> found;
        bool mine = false;
        for (size_t off = 0; off + 16 < buf.size(); ++off) {
            if (memcmp(buf.data() + off, kLinkClsid, 16) != 0) continue;
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(buf.data() + off + 16, static_cast<UINT>(buf.size() - off - 16)));
            ComPtr<IShellLinkW> link;
            ComPtr<IPersistStream> ps;
            if (!stream || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
                FAILED(link.As(&ps)) || FAILED(ps->Load(stream.Get())))
                continue;
            JumpTask t;
            wchar_t tmp[2048] = {};
            if (SUCCEEDED(link->GetPath(tmp, ARRAYSIZE(tmp), nullptr, SLGP_RAWPATH)) && tmp[0]) {
                wchar_t ex[2048];
                t.path = ExpandEnvironmentStringsW(tmp, ex, ARRAYSIZE(ex)) ? ex : tmp;
            }
            tmp[0] = 0;
            if (SUCCEEDED(link->GetArguments(tmp, ARRAYSIZE(tmp)))) t.args = tmp;
            tmp[0] = 0;
            if (SUCCEEDED(link->GetWorkingDirectory(tmp, ARRAYSIZE(tmp)))) t.dir = tmp;
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(link.As(&store))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(kTitle, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) t.title = pv.pwszVal;
                PropVariantClear(&pv);
            }
            if (!t.title.empty() && t.title[0] == L'@') {
                wchar_t res[512];
                if (SUCCEEDED(SHLoadIndirectString(t.title.c_str(), res, ARRAYSIZE(res), nullptr))) t.title = res;
            }
            if (t.path.empty() || t.title.empty()) continue;
            std::wstring lp = t.path;
            for (auto& ch : lp) ch = static_cast<wchar_t>(std::towlower(ch));
            if (lp == exeLower) mine = true;
            found.push_back(std::move(t));
        }
        if (mine) {
            for (auto& t : found) out.push_back(std::move(t));
            break;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (out.size() > 10) out.resize(10);
    return out;
}

// Windows kısayolu gönderir (ör. Win+X, Win+A, Win+N): menüleri/panelleri Windows'un kendisi açar.
void SendWinKey(WORD vk) {
    INPUT in[4] = {};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = vk;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

bool WifiConnected() {
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                             aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, aa, &size) != NO_ERROR)
            return false;
    }
    bool wifi = false, wired = false;
    for (auto* p = aa; p; p = p->Next) {
        if (p->OperStatus != IfOperStatusUp || !p->FirstGatewayAddress) continue;
        if (p->IfType == IF_TYPE_IEEE80211) wifi = true;
        else if (p->IfType == IF_TYPE_ETHERNET_CSMACD) wired = true;
    }
    return wifi && !wired;
}

void DockLog(const char* fmt, ...);

// Windows taskbar'ındaki "Gizli simgeleri göster" okunu UI Automation ile tıklar (tuş göndermeden).
// Taskbar gizliyken XAML ağacı boş gelir; pencere görünür olduktan sonra ağaç birkaç yüz ms içinde dolar.
// Bu yüzden ok bulunana kadar ~2.5 sn boyunca tekrar dener. Ayrı iş parçacığında çalışır.
void InvokeTrayChevron(HWND tray) {
    std::thread([tray] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        {
            ComPtr<IUIAutomation> uia;
            ComPtr<IUIAutomationCondition> cond;
            if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) &&
                SUCCEEDED(uia->CreateTrueCondition(&cond))) {
                bool done = false;
                int lastN = -1;
                std::string sample;
                for (int tryNo = 0; tryNo < 25 && !done; ++tryNo) {
                    if (tryNo) Sleep(100);
                    ComPtr<IUIAutomationElement> root;
                    ComPtr<IUIAutomationElementArray> arr;
                    if (FAILED(uia->ElementFromHandle(tray, &root)) || !root ||
                        FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr)
                        continue;
                    int n = 0;
                    arr->get_Length(&n);
                    lastN = n;
                    sample.clear();
                    for (int k = 0; k < n; ++k) {
                        ComPtr<IUIAutomationElement> el;
                        if (FAILED(arr->GetElement(k, &el)) || !el) continue;
                        BSTR id = nullptr, name = nullptr, cls = nullptr;
                        el->get_CurrentAutomationId(&id);
                        el->get_CurrentName(&name);
                        el->get_CurrentClassName(&cls);
                        std::wstring sid = id ? id : L"", sname = name ? name : L"", scls = cls ? cls : L"";
                        if (id) SysFreeString(id);
                        if (name) SysFreeString(name);
                        if (cls) SysFreeString(cls);
                        for (auto& ch : sid) ch = static_cast<wchar_t>(std::towlower(ch));
                        for (auto& ch : sname) ch = static_cast<wchar_t>(std::towlower(ch));
                        for (auto& ch : scls) ch = static_cast<wchar_t>(std::towlower(ch));
                        if (sample.size() < 300 && (scls.find(L"systemtray") != std::wstring::npos || !sid.empty())) {
                            for (wchar_t ch : sid) sample += ch < 128 ? static_cast<char>(ch) : '?';
                            sample += '/';
                            for (wchar_t ch : scls) sample += ch < 128 ? static_cast<char>(ch) : '?';
                            sample += ' ';
                        }
                        bool chevron = sid.find(L"chevron") != std::wstring::npos ||
                                       sid == L"systemtrayicon" ||
                                       sname.find(L"gizli simge") != std::wstring::npos ||
                                       sname.find(L"hidden icons") != std::wstring::npos;
                        if (!chevron) continue;
                        ComPtr<IUIAutomationInvokePattern> inv;
                        ComPtr<IUIAutomationTogglePattern> tog;
                        HRESULT hr = E_FAIL;
                        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv)
                            hr = inv->Invoke();
                        else if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&tog))) && tog)
                            hr = tog->Toggle();
                        DockLog("chevron found try=%d k=%d/%d invoke=0x%08lx", tryNo, k, n, static_cast<unsigned long>(hr));
                        done = true;
                        break;
                    }
                }
                if (!done)
                    DockLog("chevron NOT found: elements=%d visible=%d sample=%s", lastN, IsWindowVisible(tray) ? 1 : 0,
                            sample.c_str());
            }
        }
        CoUninitialize();
    }).detach();
}

// Windows taskbar'ını görünmez yapar ya da geri getirir (başka sürecin penceresi; katmanlı saydamlık).
void SetTrayInvisible(HWND tray, bool invisible) {
    if (!tray) return;
    LONG_PTR ex = GetWindowLongPtrW(tray, GWL_EXSTYLE);
    if (invisible) {
        SetWindowLongPtrW(tray, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(tray, 0, 0, LWA_ALPHA);
    } else if (ex & WS_EX_LAYERED) {
        SetLayeredWindowAttributes(tray, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(tray, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    }
}

bool LightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

// Teşhis: dock.log (exe'nin yanında). Yalnız durum değişince bir satır yazar.
void DockLog(const char* fmt, ...) {
    static std::wstring path;
    if (path.empty()) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        path = exe;
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"dock.log";
        DeleteFileW(path.c_str());  // her açılışta temiz başla
    }
    char buf[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > static_cast<int>(sizeof(buf)) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, buf, n, &w, nullptr);
    CloseHandle(f);
}

struct AccentPolicy {
    int state;
    int flags;
    DWORD gradient;
    int animation;
};
struct CompositionData {
    int attribute;
    PVOID data;
    SIZE_T size;
};
using SetWca = BOOL(WINAPI*)(HWND, CompositionData*);

void SetAccentBlur(HWND hwnd, bool light) {
    static auto fn = reinterpret_cast<SetWca>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return;
    // Renk AABBGGRR: koyu temada dumanlı koyu cam, açıkta süt beyazı cam.
    AccentPolicy ap{4 /* ACRYLIC */, 2, light ? 0x70F4F2F2u : 0x5A1E1A1Au, 0};
    CompositionData d{19 /* WCA_ACCENT_POLICY */, &ap, sizeof(ap)};
    if (!fn(hwnd, &d)) {
        ap.state = 3;  // eski sistemlerde düz bulanıklık
        fn(hwnd, &d);
    }
}

std::unordered_map<HWND, std::pair<std::wstring, std::wstring>> g_describeCache;  // hwnd -> (aumid, exe)

}  // namespace

// Dosyayı dock'taki bir uygulamanın üstüne bırakınca onunla açılır.
class DockDrop : public IDropTarget {
public:
    explicit DockDrop(Dock* d) : dock_(d) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files_ = data && SUCCEEDED(data->QueryGetData(&fe));
        return DragOver(0, pt, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        int hit = dock_->DropOver(POINT{pt.x, pt.y});
        *effect = (files_ && hit >= 0) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        dock_->DropOver(POINT{LONG_MIN, 0});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        int hit = dock_->DropOver(POINT{pt.x, pt.y});
        *effect = DROPEFFECT_NONE;
        std::vector<std::wstring> paths;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        if (hit >= 0 && data && SUCCEEDED(data->GetData(&fe, &sm))) {
            HDROP drop = static_cast<HDROP>(GlobalLock(sm.hGlobal));
            if (drop) {
                UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT k = 0; k < n; ++k) {
                    wchar_t path[MAX_PATH * 2];
                    if (DragQueryFileW(drop, k, path, ARRAYSIZE(path))) paths.push_back(path);
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
        }
        dock_->DropOver(POINT{LONG_MIN, 0});
        if (!paths.empty()) {
            dock_->DropFiles(hit, paths);
            *effect = DROPEFFECT_COPY;
        }
        return S_OK;
    }

private:
    Dock* dock_;
    ULONG refs_ = 1;
    bool files_ = false;
};

// ---------------------------------------------------------------------------
// Windows taskbar'ını gizleme / geri getirme
// ---------------------------------------------------------------------------

void Dock::RestoreTaskbarNow() {
    SetTrayInvisible(FindWindowW(L"Shell_TrayWnd", nullptr), false);  // gizli simgeler sırasında saydam kaldıysa
    if (ReadSetting(L"DockTbHidden", 0) == 0) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        APPBARDATA abd{sizeof(abd)};
        abd.hWnd = tray;
        abd.lParam = static_cast<LPARAM>(ReadSetting(L"DockTbState", ABS_ALWAYSONTOP));
        SHAppBarMessage(ABM_SETSTATE, &abd);
        ShowWindow(tray, SW_SHOWNA);
    }
    WriteSetting(L"DockTbHidden", 0);
}

bool Dock::ForegroundFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64];
    GetClassNameW(fg, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    LONG_PTR ex = GetWindowLongPtrW(fg, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return false;  // ekran kaydı / kenar ışığı gibi katmanlar
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT r;
    if (!GetWindowRect(fg, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom;
}

void Dock::SetFullscreen(bool on) {
    if (on == fullscreen_) return;
    // Oyun / tam ekran video: dock tamamen çekilir.
    fullscreen_ = on;
    ShowWindow(glass_, on ? SW_HIDE : SW_SHOWNOACTIVATE);
    ShowWindow(overlay_, on ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (!on) {
        SetWindowPos(glass_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        StartAnimation();
    }
}

void Dock::RepairOnStartup(bool dockEnabled) {
    if (!dockEnabled) RestoreTaskbarNow();
}

void Dock::HideTaskbar() {
    tray_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = tray_;
    if (ReadSetting(L"DockTbHidden", 0) == 0) {
        // Kullanıcının asıl ayarını sakla: dock kapanınca aynen geri koyarız.
        WriteSetting(L"DockTbState", static_cast<DWORD>(SHAppBarMessage(ABM_GETSTATE, &abd)));
        WriteSetting(L"DockTbHidden", 1);
    }
    // Otomatik gizle: Windows taskbar'ı ekranda yer kaplamasın; görünmez yap: hiç çıkmasın.
    abd.lParam = ABS_AUTOHIDE;
    SHAppBarMessage(ABM_SETSTATE, &abd);
    ShowWindow(tray_, SW_HIDE);
}

void Dock::RestoreTaskbar() {
    RestoreTaskbarNow();
    tray_ = nullptr;
}

// ---------------------------------------------------------------------------
// Açma / kapama
// ---------------------------------------------------------------------------

bool Dock::Enable(HINSTANCE instance, ID2D1Factory* d2d, IDWriteFactory* dwrite) {
    if (overlay_) return true;
    instance_ = instance;
    d2d_ = d2d;
    dwrite_ = dwrite;
    g_dock = this;
    QueryPerformanceFrequency(&qpcFreq_);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"tr-tr", &labelFormat_);
    if (labelFormat_) {
        labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    auto mk = [&](const wchar_t* family, DWRITE_FONT_WEIGHT wt, float size, ComPtr<IDWriteTextFormat>& out) {
        dwrite_->CreateTextFormat(family, nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                                  L"tr-tr", &out);
        if (!out) return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ComPtr<IDWriteInlineObject> dots;
        dwrite_->CreateEllipsisTrimmingSign(out.Get(), &dots);
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        out->SetTrimming(&trim, dots.Get());
    };
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_NORMAL, 12.f, titleFormat_);
    mk(L"Segoe UI Variable Text", DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.f, headFormat_);
    mk(L"Segoe Fluent Icons", DWRITE_FONT_WEIGHT_NORMAL, 15.f, iconFormat_);
    if (titleFormat_) titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    if (iconFormat_) iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    autoHide_ = ReadSetting(L"DockAutoHide2", 1) != 0  /* yeni anahtar: eski "kapalı" kaydı varsayılanı ezmesin */;
    panelA_.stiffness = 420.f;
    panelA_.damping = 36.f;
    hide_.stiffness = 260.f;
    hide_.damping = 30.f;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = GlassProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"GutiDockGlass";
        RegisterClassExW(&wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"GutiDock";
        RegisterClassExW(&wc);
        wc.lpfnWndProc = PreviewProc;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"GutiDockPreview";
        RegisterClassExW(&wc);
        registered = true;
    }
    // Önce gerçek cam denenir (Windows.UI.Composition, yuvarlak ve pürüzsüz köşeler);
    // olmazsa eski accent bulanıklığı + Windows'un kendi yuvarlak köşeleri.
    glass_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                             L"GutiDockGlass", L"Guti Dock Glass", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance,
                             this);
    if (glass_ && !glassFx_.Attach(glass_)) {
        DestroyWindow(glass_);
        glass_ = nullptr;
    }
    if (!glass_)
        glass_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GutiDockGlass",
                                 L"Guti Dock Glass", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!glass_) return false;
    bodyRadius_ = glassFx_.Attached() ? kRadius : 8.f;  // Windows 11'in yuvarlak köşesi 8 DIP'tir
    // Üst pencerenin sahibi cam pencere: Windows onu her zaman camın üstünde tutar.
    overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GutiDock",
                               L"Guti Dock", WS_POPUP, 0, 0, 1, 1, glass_, nullptr, instance, this);
    if (!overlay_) {
        DestroyWindow(glass_);
        glass_ = nullptr;
        return false;
    }
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    DWORD corner = glassFx_.Attached() ? 1 : 2;  // DONOTROUND: köşeleri cam kırpar; yoksa ROUND
    DwmSetWindowAttribute(glass_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(glass_, 34 /* DWMWA_BORDER_COLOR */, &none, sizeof(none));

    labelA_.stiffness = 420.f;
    labelA_.damping = 36.f;
    UpdateMonitor();
    ApplyGlass();
    LoadPins();
    Rescan();
    for (auto& a : apps_) a.appear.Snap(100.f);

    quit_ = false;
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    frameThread_ = std::thread([this] { FrameLoop(); });

    // Pencere olayları: açılan/kapanan/öne gelen pencereleri sorgulamadan öğreniriz.
    auto hook = [&](DWORD a, DWORD b) {
        HWINEVENTHOOK h = SetWinEventHook(a, b, nullptr, WinEventProc, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) hooks_.push_back(h);
    };
    hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    hook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE);
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);

    shellHookMsg_ = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(overlay_);  // "bana bak" (yanıp sönme) bildirimleri
    drop_ = new DockDrop(this);
    RegisterDragDrop(overlay_, drop_);
    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);
    OnFrame();  // ilk kare: pencereler yerine otursun
    ShowWindow(glass_, SW_SHOWNOACTIVATE);
    ShowWindow(overlay_, SW_SHOWNOACTIVATE);
    UINT_PTR ht = autoHide_ ? SetTimer(overlay_, kTimerAutoHide, 100, nullptr) : 0;
    DockLog("enable autoHide=%d timer=%d fx=%d scale=%.2f mon=%ld,%ld,%ld,%ld", autoHide_ ? 1 : 0,
            static_cast<int>(ht), glassFx_.Attached() ? 1 : 0, scale_, monitor_.left, monitor_.top, monitor_.right,
            monitor_.bottom);
    SetTimer(overlay_, kTimerWatch, 50, nullptr);
    focusA_.stiffness = 200.f;
    focusA_.damping = 28.f;
    HideTaskbar();  // önce Windows taskbar'ı çekilsin ki dock'un yeri en alttan ayrılsın
    RegisterAppBar();
    StartAnimation();
    return true;
}

void Dock::Disable() {
    if (!overlay_) return;
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    quit_ = true;
    wantFrames_ = false;
    if (frameEvent_) SetEvent(frameEvent_);
    if (frameThread_.joinable()) frameThread_.join();
    if (frameEvent_) CloseHandle(frameEvent_);
    frameEvent_ = nullptr;
    RemoveAppBar();
    RestoreTaskbar();
    SetUnhandledExceptionFilter(g_prevFilter);
    DeregisterShellHookWindow(overlay_);
    ClosePanel();
    if (preview_) DestroyWindow(preview_);
    preview_ = nullptr;
    stack_.clear();
    RevokeDragDrop(overlay_);
    if (drop_) drop_->Release();
    drop_ = nullptr;
    dragging_ = false;
    pressed_ = hover_ = -1;
    DiscardDevice();
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (dib_) DeleteObject(dib_);
    dib_ = nullptr;
    glassFx_.Detach();
    glassRect_ = RECT{};
    HWND o = overlay_, g = glass_;
    overlay_ = glass_ = nullptr;
    DestroyWindow(o);
    DestroyWindow(g);
    apps_.clear();
    g_describeCache.clear();
    animating_ = false;
    framePending_ = false;
}

void Dock::SetAccent(D2D1_COLOR_F c) {
    accent_ = c;
    if (overlay_) StartAnimation();
}

void Dock::UpdateMonitor() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float scale = dx / 96.f;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = static_cast<int>(std::ceil((kPanelRoom + kTopRoom + kBodyH + kBottom) * scale));
    bool changed = scale != scale_ || w != texW_ || h != texH_;
    monitor_ = mi.rcMonitor;
    scale_ = scale;
    if (changed) {
        texW_ = w;
        texH_ = h;
        DiscardDevice();
        for (auto& a : apps_) {
            a.icon.Reset();
            a.iconTried = false;
        }
        if (memDC_) {
            SelectObject(memDC_, oldBmp_);
            DeleteDC(memDC_);
            memDC_ = nullptr;
        }
        if (dib_) DeleteObject(dib_);
        dib_ = nullptr;
    }
}

void Dock::ApplyGlass() {
    light_ = LightTheme();
    if (glassFx_.Attached())
        glassFx_.Update(glassRect_.right - glassRect_.left, glassRect_.bottom - glassRect_.top,
                        bodyRadius_ * scale_, light_);
    else
        SetAccentBlur(glass_, light_);
}

void Dock::RegisterAppBar() {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = glass_;
    abd.uCallbackMessage = kAppBarMsg;
    if (!appBar_) appBar_ = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
    if (!appBar_) return;
    // Dock'un yeri ayrılır: büyütülen pencereler dock'un üstünde biter.
    int reserve = autoHide_ ? 0 : static_cast<int>(std::ceil((kBodyH + kBottom + 4.f) * scale_));
    abd.uEdge = ABE_BOTTOM;
    abd.rc = monitor_;
    abd.rc.top = monitor_.bottom - reserve;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.top = abd.rc.bottom - reserve;
    SHAppBarMessage(ABM_SETPOS, &abd);
}

void Dock::RemoveAppBar() {
    if (!appBar_) return;
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = glass_;
    SHAppBarMessage(ABM_REMOVE, &abd);
    appBar_ = false;
}

// ---------------------------------------------------------------------------
// Uygulamalar
// ---------------------------------------------------------------------------

void Dock::LoadPins() {
    pinOrder_.clear();
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DockPins", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) ==
            ERROR_SUCCESS &&
        bytes > 2) {
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DockPins", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(),
                         &bytes) == ERROR_SUCCESS) {
            for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) pinOrder_.push_back(p);
        }
    } else {
        // İlk açılış: Windows taskbar'ına sabitlenmiş uygulamaları al.
        wchar_t appData[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
            std::wstring dir = std::wstring(appData) + kPinnedDir;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    pinOrder_.push_back(dir + L"\\" + fd.cFileName);
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
        }
        // Dosya Gezgini en başta dursun.
        std::stable_partition(pinOrder_.begin(), pinOrder_.end(), [](const std::wstring& p) {
            std::wstring l = Lower(p);
            return l.find(L"file explorer") != std::wstring::npos || l.find(L"dosya gezgini") != std::wstring::npos;
        });
        SavePins(false);
    }

    apps_.clear();
    App start;
    start.isStart = true;
    start.name = L"Başlat";
    apps_.push_back(std::move(start));
    App sep;
    sep.isSeparator = true;
    apps_.push_back(std::move(sep));
    for (const auto& path : pinOrder_) {
        App a;
        a.pinned = true;
        a.appear.Snap(100.f);
        a.launch = path;
        std::wstring exe, aumid;
        if (EndsWith(Lower(path), L".lnk")) {
            ResolveLink(path, exe, aumid);
            size_t slash = path.find_last_of(L'\\');
            a.name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
            a.name.resize(a.name.size() - 4);
        } else if (path.rfind(L"shell:AppsFolder\\", 0) == 0) {
            aumid = path.substr(17);
            a.name = ShellName(path);
        } else {
            exe = path;
            a.name = FileDescription(path);
        }
        a.exe = Lower(exe);
        a.aumid = Lower(aumid);
        a.key = !a.aumid.empty() ? a.aumid : a.exe;
        if (a.name.empty()) a.name = ShellName(path);
        apps_.push_back(std::move(a));
    }
    // Sağ uç: görünmez, genişliği olmayan işaret; açık uygulamalar bunun önüne eklenir.
    // (Saat / tepsi hapı kaldırıldı: Windows panelleri dock'un zarif görünümüne uymadı.)
    App sep2;
    sep2.isSeparator = true;
    sep2.isEnd = true;
    apps_.push_back(std::move(sep2));
    for (auto& a : apps_) a.appear.Snap(100.f);
}

void Dock::SavePins(bool fromApps) {
    if (fromApps) {
        pinOrder_.clear();
        for (const auto& a : apps_)
            if (a.pinned && !a.launch.empty()) pinOrder_.push_back(a.launch);
    }
    std::wstring multi;
    for (const auto& p : pinOrder_) {
        multi += p;
        multi.push_back(L'\0');
    }
    multi.push_back(L'\0');
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, L"DockPins", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi.data()),
                       static_cast<DWORD>(multi.size() * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

bool Dock::Eligible(HWND hwnd) const {
    // Windows taskbar'ının gösterdiği pencerelerle aynı kurallar.
    if (!IsWindowVisible(hwnd) || hwnd == glass_ || hwnd == overlay_) return false;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if (!appWindow) {
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
        if (GetWindow(hwnd, GW_OWNER)) return false;
    }
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    if (GetWindowTextLengthW(hwnd) == 0) return false;
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd"))
        return false;
    return true;
}

void Dock::Describe(HWND hwnd, std::wstring& aumid, std::wstring& exe) const {
    auto it = g_describeCache.find(hwnd);
    if (it != g_describeCache.end()) {
        aumid = it->second.first;
        exe = it->second.second;
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    exe = ProcessPath(pid);
    aumid = WindowAumid(hwnd);
    if (EndsWith(Lower(exe), L"\\applicationframehost.exe")) {
        // Mağaza uygulaması: asıl süreç içteki CoreWindow'un sahibidir.
        HWND core = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
        if (core) {
            DWORD cpid = 0;
            GetWindowThreadProcessId(core, &cpid);
            if (cpid && cpid != pid) {
                exe = ProcessPath(cpid);
                if (aumid.empty()) aumid = ProcessAumid(cpid);
            }
        }
    }
    if (aumid.empty()) aumid = ProcessAumid(pid);
    g_describeCache[hwnd] = {aumid, exe};
}

std::wstring Dock::NameFor(const App& a, HWND hwnd) const {
    std::wstring n;
    if (!a.aumidRaw.empty()) n = ShellName(L"shell:AppsFolder\\" + a.aumidRaw);
    if (n.empty() && !a.exe.empty()) n = FileDescription(a.exe);
    if (n.empty() && hwnd) {
        wchar_t t[128];
        GetWindowTextW(hwnd, t, ARRAYSIZE(t));
        n = t;
    }
    return n;
}

Dock::App* Dock::FindApp(const std::wstring& aumid, const std::wstring& exe) {
    if (!aumid.empty())
        for (auto& a : apps_)
            if (a.IsApp() && a.aumid == aumid) return &a;
    if (!exe.empty() && !EndsWith(exe, L"\\applicationframehost.exe"))
        for (auto& a : apps_)
            if (a.IsApp() && a.exe == exe) return &a;
    return nullptr;
}

void Dock::Rescan() {
    for (auto& a : apps_) {
        a.windows.clear();
        a.active = false;
    }
    std::vector<HWND> list;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&list));
    std::unordered_map<HWND, std::pair<std::wstring, std::wstring>> keep;
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    for (HWND h : list) {
        if (!Eligible(h)) continue;
        std::wstring aumidRaw, exeRaw;
        Describe(h, aumidRaw, exeRaw);
        keep[h] = {aumidRaw, exeRaw};
        std::wstring aumid = Lower(aumidRaw), exe = Lower(exeRaw);
        App* app = FindApp(aumid, exe);
        if (!app) {
            App a;
            a.aumid = aumid;
            a.aumidRaw = aumidRaw;
            a.exe = exe;
            a.key = !aumid.empty() ? aumid : exe;
            bool packaged = exe.find(L"\\windowsapps\\") != std::wstring::npos ||
                            EndsWith(exe, L"\\applicationframehost.exe") || exe.empty();
            a.launch = (!aumidRaw.empty() && packaged) ? L"shell:AppsFolder\\" + aumidRaw : exeRaw;
            a.name = NameFor(a, h);
            a.appear.Snap(0.f);
            // Yeni uygulama, sağ uçtaki ayırıcıdan önce yer alır.
            size_t at = apps_.size();
            for (size_t k = 2; k < apps_.size(); ++k)
                if (apps_[k].isSeparator) {
                    at = k;
                    break;
                }
            apps_.insert(apps_.begin() + static_cast<long long>(at), std::move(a));
            app = &apps_[at];
        }
        app->windows.push_back(h);
        if (h == fg || h == fgRoot) {
            app->active = true;
            app->attention = false;
        }
    }
    g_describeCache.swap(keep);
    if (fullscreen_ && !ForegroundFullscreen()) SetFullscreen(false);
    for (auto& a : apps_) {
        if (a.isTrash) {
            // Çöp dolu/boş ikonu değişmiş olabilir
            SHQUERYRBINFO rb{sizeof(rb)};
            bool full = SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb)) && rb.i64NumItems > 0;
            if (full != (a.seen)) {
                a.seen = full;
                a.icon.Reset();
                a.iconTried = false;
            }
        }
        if (!a.IsApp()) continue;
        bool open = !a.windows.empty();
        a.dot.target = open ? 100.f : 0.f;
        a.appear.target = (open || a.pinned) ? 100.f : 0.f;
    }
    StartAnimation();
}

void Dock::OnClick(int i) {
    if (i < 0 || i >= static_cast<int>(apps_.size())) return;
    App& a = apps_[i];
    if (a.isSeparator) return;
    if (a.isStart) {
        OpenStart();
        return;
    }
    if (a.isControl) {
        int part = TrayPart(mouseX_);
        if (part >= 0) BeginFlyout((trayX_[part] + trayX_[part + 1]) / 2.f);
        if (part == 0) OpenHiddenIcons();
        else if (part == 1) SendWinKey('A');   // Hızlı ayarlar: ağ, ses, Bluetooth
        else if (part == 2) SendWinKey('N');   // Bildirimler ve takvim
        return;
    }
    if (a.isStack) {
        if (panel_ == Panel::Stack && panelFor_ == i) ClosePanel();
        else OpenStack(i);
        return;
    }
    if (a.isTrash) {
        ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    ClosePanel();
    if (a.windows.empty()) {
        if (a.launch.empty()) return;
        // Açılıyor: ikon pencere gelene kadar zıplar; adadaki Guti de dönüp bakar.
        a.launchAt = Now();
        Sounds::Play(Sounds::Sound::Open);
        if (onLaunch) onLaunch();
        StartAnimation();
        HINSTANCE r = ShellExecuteW(nullptr, L"open", a.launch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) a.launchAt = -1;
        return;
    }
    Activate(a);
}

void Dock::Activate(App& a) {
    HWND target = a.windows.front();
    if (a.active) {
        if (a.windows.size() == 1) {
            // Zaten öndeyse küçült (Windows taskbar'ı gibi).
            ShowWindowAsync(target, SW_MINIMIZE);
            return;
        }
        target = a.windows.back();  // birden çok pencere: sırayla dolaş
    }
    if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
    // Tıklama bize geldiği için öne getirme izni bizde; yine de olmazsa Alt hilesi.
    if (!SetForegroundWindow(target)) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(target);
    }
}

void Dock::OpenStart() {
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_LWIN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Yerleşim: macOS benzeri büyütme. Fare altındaki nokta sabit kalır, ikonlar iki yana açılır.
// Koordinatlar: x = 0 ekranın ortası, y = 0 gövdenin üstü (DIP).
// ---------------------------------------------------------------------------

void Dock::Layout(float* bodyL, float* bodyR) {
    size_t n = apps_.size();
    std::vector<float> baseW(n), w(n);
    float baseTotal = 0, total = 0;
    for (size_t i = 0; i < n; ++i) {
        const App& a = apps_[i];
        float appear = std::clamp(a.appear.value / 100.f, 0.f, 1.f);
        float slot = a.isControl ? kControlW : kSlot;
        baseW[i] = a.isSeparator ? (a.isEnd ? 0.f : kSepW) : slot * appear;
        w[i] = a.isSeparator ? (a.isEnd ? 0.f : kSepW) : (slot + a.grow.value * (kSlot / kIcon)) * appear;
        baseTotal += baseW[i];
        total += w[i];
    }
    baseW_ = baseTotal;
    float baseL = -baseTotal / 2.f;
    // Sabit noktanın taban düzendeki yeri: hangi öğenin hangi kesrinde?
    float anchor = std::clamp(anchorX_, baseL, baseL + baseTotal);
    float acc = baseL, before = 0;  // before: yeni düzende o öğeye kadar olan genişlik
    float left = baseL;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        if (!found && (anchor <= acc + baseW[i] || i + 1 == n)) {
            float t = baseW[i] > 0 ? std::clamp((anchor - acc) / baseW[i], 0.f, 1.f) : 0.f;
            left = anchor - (before + t * w[i]);
            found = true;
        }
        acc += baseW[i];
        before += w[i];
    }
    float x = left;
    for (size_t i = 0; i < n; ++i) {
        App& a = apps_[i];
        a.w = w[i];
        a.x = x + w[i] / 2.f + a.slide.value;
        if (dragging_ && static_cast<int>(i) == pressed_) a.x = dragX_;
        float appear = std::clamp(a.appear.value / 100.f, 0.f, 1.f);
        a.size = a.isSeparator ? 0.f : (kIcon + a.grow.value) * appear;
        x += w[i];
    }
    *bodyL = left - kPadX;
    *bodyR = left + total + kPadX;
}

int Dock::HitTest(float x, float y) const {
    // y: gövde üstüne göre (DIP)
    for (size_t i = 0; i < apps_.size(); ++i) {
        const App& a = apps_[i];
        if (a.isSeparator || a.appear.target <= 0.f) continue;
        if (x < a.x - a.w / 2.f || x >= a.x + a.w / 2.f) continue;
        float top = kIconBottom - a.size - 6.f;
        if (y >= top && y <= kBodyH) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Kare döngüsü
// ---------------------------------------------------------------------------

double Dock::Now() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / qpcFreq_.QuadPart;
}

void Dock::StartAnimation() {
    if (!overlay_) return;
    if (!animating_) QueryPerformanceCounter(&lastTick_);
    animating_ = true;
    if (!wantFrames_.exchange(true) && frameEvent_) SetEvent(frameEvent_);
}

void Dock::FrameLoop() {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)))
        adapter->EnumOutputs(0, &output);
    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    while (!quit_) {
        WaitForSingleObject(frameEvent_, INFINITE);
        while (wantFrames_.load() && !quit_) {
            bool waited = output && SUCCEEDED(output->WaitForVBlank());
            QueryPerformanceCounter(&now);
            double ms = (now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
            if (!waited || ms < 4) {
                if (ms < 16) Sleep(static_cast<DWORD>(16 - ms));
                QueryPerformanceCounter(&now);
            }
            last = now;
            HWND o = overlay_;
            if (o && !framePending_.exchange(true)) PostMessageW(o, kFrameMsg, 0, 0);
        }
    }
}

void Dock::OnFrame() {
    if (!overlay_) return;
    lastFrameAt_ = Now();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = static_cast<float>(now.QuadPart - lastTick_.QuadPart) / qpcFreq_.QuadPart;
    lastTick_ = now;
    dt = std::clamp(dt, 0.001f, 0.05f);
    double t = Now();

    // Büyütme hedefleri: fareye yakın ikon büyür, komşuları kosinüs eğrisiyle daha az.
    float baseL = -baseW_ / 2.f, acc = baseL;
    for (auto& a : apps_) {
        float bw = a.isSeparator ? (a.isEnd ? 0.f : kSepW) : (a.isControl ? kControlW : kSlot) * std::clamp(a.appear.value / 100.f, 0.f, 1.f);
        float c = acc + bw / 2.f;
        acc += bw;
        float g = 0;
        if (mouseIn_ && !a.isSeparator && !a.isControl && !fullscreen_ && !(dragging_ && dragY_ < -40.f)) {
            float d = std::fabs(c - mouseX_);
            if (d < kRange) g = kMaxGrow * (std::cos(3.14159265f * d / kRange) + 1.f) / 2.f;
        }
        a.grow.target = g;
        a.hov.target = (!dragging_ && hover_ >= 0 && &apps_[hover_] == &a && !a.isSeparator && !a.isControl) ? 100.f : 0.f;
        a.press.target = (!dragging_ && pressed_ >= 0 && &apps_[pressed_] == &a && hover_ == pressed_) ? 5.f : 0.f;
    }
    labelA_.target = (hover_ >= 0 && pressed_ < 0 && panel_ == Panel::None) ? 100.f : 0.f;
    panelA_.target = panel_ != Panel::None ? 100.f : 0.f;

    bool moving = false;
    for (float left = dt; left > 0; left -= 1.f / 240.f) {
        float step = std::min(left, 1.f / 240.f);
        for (auto& a : apps_) {
            a.grow.stiffness = 380.f;
            a.grow.damping = 32.f;
            a.press.stiffness = 700.f;
            a.press.damping = 34.f;
            a.dot.stiffness = 260.f;
            a.dot.damping = 30.f;
            a.appear.stiffness = 260.f;
            a.appear.damping = 30.f;
            moving |= a.grow.Step(step);
            moving |= a.press.Step(step);
            moving |= a.dot.Step(step);
            moving |= a.appear.Step(step);
            a.hov.stiffness = 520.f;
            a.hov.damping = 46.f;
            moving |= a.hov.Step(step);
            a.slide.stiffness = 420.f;
            a.slide.damping = 36.f;
            moving |= a.slide.Step(step);
        }
        moving |= labelA_.Step(step);
        moving |= panelA_.Step(step);
        moving |= hide_.Step(step);
        moving |= focusA_.Step(step);
    }
    // Açılış zıplaması: pencere gelene kadar (en çok 7 sn) yumuşak yaylar; bir yayı yarıda kesmez.
    for (auto& a : apps_) {
        a.lift = 0;
        if (a.launchAt < 0) continue;
        float e = static_cast<float>(t - a.launchAt);
        int n = static_cast<int>(e / kBouncePeriod);
        float phase = e - n * kBouncePeriod;
        bool done = (!a.windows.empty() || e > 7.f) && n >= 1;
        if (done && phase < kBouncePeriod * 0.12f) {
            a.launchAt = -1;
            continue;
        }
        a.lift = kBounce * std::sin(3.14159265f * phase / kBouncePeriod);
        moving = true;
    }
    if (puffAt_ >= 0) {
        if (t - puffAt_ > 0.45) puffAt_ = -1;
        else moving = true;
    }
    if (dragging_) moving = true;
    if (blinkAt_ >= 0) {
        if (t - blinkAt_ > 0.2) blinkAt_ = -1;
        else moving = true;
    }
    focusA_.target = focus_ ? 100.f : 0.f;
    // Kapanmış (sabitli olmayan) uygulamalar küçülerek kaybolur.
    for (size_t i = apps_.size(); i-- > 0;) {
        App& a = apps_[i];
        if (!a.pinned && a.IsApp() && a.appear.target <= 0.f && a.appear.value <= 0.5f) {
            apps_.erase(apps_.begin() + static_cast<long long>(i));
            if (hover_ >= static_cast<int>(i)) hover_ = -1;
            if (pressed_ >= static_cast<int>(i)) pressed_ = -1;
            if (labelFor_ >= static_cast<int>(i)) labelFor_ = -1;
        }
    }

    float bodyL, bodyR;
    Layout(&bodyL, &bodyR);

    // Cam pencereyi gövdeye oturt.
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    RECT g;
    g.left = static_cast<LONG>(std::lround(cxPx + bodyL * scale_));
    g.right = static_cast<LONG>(std::lround(cxPx + bodyR * scale_));
    g.bottom = monitor_.bottom - static_cast<LONG>(std::lround((kBottom - hide_.value) * scale_));
    g.top = g.bottom - static_cast<LONG>(std::lround(kBodyH * scale_));
    if (!EqualRect(&g, &glassRect_)) {
        bool sizeChanged = (g.right - g.left) != (glassRect_.right - glassRect_.left) ||
                           (g.bottom - g.top) != (glassRect_.bottom - glassRect_.top);
        glassRect_ = g;
        SetWindowPos(glass_, nullptr, g.left, g.top, g.right - g.left, g.bottom - g.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        if (sizeChanged && glassFx_.Attached())
            glassFx_.Update(g.right - g.left, g.bottom - g.top, bodyRadius_ * scale_, light_);
    }

    Render();

    if (!moving) {
        animating_ = false;
        wantFrames_ = false;
    }
}

// ---------------------------------------------------------------------------
// Çizim
// ---------------------------------------------------------------------------

bool Dock::CreateGpu() {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr)))
        return false;
    ComPtr<ID2D1Factory1> f1;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> dev;
    if (FAILED(d2d_.As(&f1)) || FAILED(d3d_.As(&dxgi)) || FAILED(f1->CreateDevice(dxgi.Get(), &dev)) ||
        FAILED(dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx_))) {
        d3d_.Reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(texW_);
    td.Height = static_cast<UINT>(texH_);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(d3d_->CreateTexture2D(&td, nullptr, &tex)) || FAILED(tex.As(&surface_))) {
        ctx_.Reset();
        d3d_.Reset();
        return false;
    }
    auto bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_GDI_COMPATIBLE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * scale_, 96.f * scale_);
    if (FAILED(ctx_->CreateBitmapFromDxgiSurface(surface_.Get(), &bp, &target_))) {
        surface_.Reset();
        ctx_.Reset();
        d3d_.Reset();
        return false;
    }
    ctx_->SetTarget(target_.Get());
    ctx_->SetDpi(96.f * scale_, 96.f * scale_);
    return true;
}

void Dock::DiscardDevice() {
    ctx_.Reset();
    target_.Reset();
    surface_.Reset();
    d3d_.Reset();
    dcRt_.Reset();
    brush_.Reset();
    for (auto& a : apps_) {
        a.icon.Reset();
        a.iconTried = false;
    }
    for (auto& s : stack_) {
        s.icon.Reset();
        s.iconTried = false;
    }
    rt_.Reset();
}

bool Dock::EnsureResources() {
    if (rt_) return true;
    if (!gpuFailed_ && CreateGpu()) {
        gpu_ = true;
        rt_ = ctx_;
    } else {
        gpuFailed_ = true;
        gpu_ = false;
        if (!memDC_) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = texW_;
            bi.bmiHeader.biHeight = -texH_;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void* bits = nullptr;
            HDC screen = GetDC(nullptr);
            memDC_ = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (!dib_) return false;
            oldBmp_ = SelectObject(memDC_, dib_);
        }
        auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f * scale_,
            96.f * scale_);
        if (FAILED(d2d_->CreateDCRenderTarget(&props, &dcRt_))) return false;
        rt_ = dcRt_;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(kWhite, &brush_);
    return brush_ != nullptr;
}

ID2D1Bitmap* Dock::Icon(App& a) {
    if (a.icon || a.iconTried || !wic_) return a.icon.Get();
    a.iconTried = true;
    std::wstring parsing = a.launch;
    if (parsing.empty()) parsing = a.exe;
    if (parsing.empty()) return nullptr;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    int px = static_cast<int>(std::ceil((kIcon + kMaxGrow) * scale_));
    px = px <= 96 ? 96 : (px <= 128 ? 128 : 256);  // hazır ikon boylarından büyüğü: büyütmede bulanıklaşmasın
    HBITMAP hbmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) || !hbmp)
        return nullptr;
    ComPtr<IWICBitmap> wbmp;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(wic_->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &wbmp)) &&
        SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wbmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &a.icon);
    }
    DeleteObject(hbmp);
    return a.icon.Get();
}

void Dock::DrawStart(float cx, float bottom, float size) {
    // Mini Guti: siyah yuvarlak kare, iki beyaz göz; gözler fareye bakar.
    float r = size * 0.30f;
    D2D1_ROUNDED_RECT face{D2D1::RectF(cx - size / 2, bottom - size, cx + size / 2, bottom), r, r};
    brush_->SetColor(Rgba(0.02f, 0.02f, 0.03f, 1.f));
    rt_->FillRoundedRectangle(face, brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, 0.14f));
    D2D1_ROUNDED_RECT edge{D2D1::RectF(cx - size / 2 + 0.5f, bottom - size + 0.5f, cx + size / 2 - 0.5f, bottom - 0.5f),
                           r - 0.5f, r - 0.5f};
    rt_->DrawRoundedRectangle(edge, brush_.Get(), 1.f);
    float look = mouseIn_ ? std::clamp((mouseX_ - cx) / 260.f, -1.f, 1.f) : 0.f;
    float ex = size * 0.17f, ew = size * 0.15f, eh = size * 0.30f;
    float ey = bottom - size * 0.52f;
    float dx = look * size * 0.07f;
    bool happy = hover_ == 0;  // üstüne gelince sevinir: gözler ^ ^ olur
    float blink = 1.f;
    if (blinkAt_ >= 0) blink = 1.f - 0.9f * std::sin(3.14159265f * std::clamp(static_cast<float>(Now() - blinkAt_) / 0.2f, 0.f, 1.f));
    brush_->SetColor(accent_);
    for (int s = -1; s <= 1; s += 2) {
        float x = cx + s * ex + dx;
        if (happy) {
            ComPtr<ID2D1PathGeometry> arc;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d_->CreatePathGeometry(&arc)) && SUCCEEDED(arc->Open(&sink))) {
                float hw = ew * 0.85f;
                sink->BeginFigure(D2D1::Point2F(x - hw, ey + eh * 0.12f), D2D1_FIGURE_BEGIN_HOLLOW);
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(x, ey - eh * 0.45f),
                                                                      D2D1::Point2F(x + hw, ey + eh * 0.12f)));
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                ComPtr<ID2D1StrokeStyle> round;
                d2d_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                    D2D1_CAP_STYLE_ROUND),
                                        nullptr, 0, &round);
                rt_->DrawGeometry(arc.Get(), brush_.Get(), ew * 0.62f, round.Get());
            }
            continue;
        }
        float h = std::max(ew * 0.5f, eh * blink);
        D2D1_ROUNDED_RECT eye{D2D1::RectF(x - ew / 2, ey - h / 2, x + ew / 2, ey + h / 2), ew / 2, ew / 2};
        rt_->FillRoundedRectangle(eye, brush_.Get());
    }
}

void Dock::DrawWindowsLogo(float cx, float bottom, float size) {
    // Başlat: dört karelik Windows logosu, mavi geçişli.
    float s = size * 0.70f, gap = std::max(1.5f, s * 0.06f), q = (s - gap) / 2.f;
    float l = cx - s / 2.f, t = bottom - size / 2.f - s / 2.f;
    ComPtr<ID2D1GradientStopCollection> stops;
    D2D1_GRADIENT_STOP gs[2] = {{0.f, Rgba(0.33f, 0.75f, 1.f)}, {1.f, Rgba(0.02f, 0.43f, 0.87f)}};
    rt_->CreateGradientStopCollection(gs, 2, &stops);
    ComPtr<ID2D1LinearGradientBrush> fill;
    if (!stops || FAILED(rt_->CreateLinearGradientBrush(
                      D2D1::LinearGradientBrushProperties(D2D1::Point2F(l, t), D2D1::Point2F(l + s, t + s)),
                      stops.Get(), &fill)))
        return;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            float x0 = l + x * (q + gap), y0 = t + y * (q + gap);
            rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x0, y0, x0 + q, y0 + q), 1.2f, 1.2f}, fill.Get());
        }
}

void Dock::DrawIcon(App& a, float cx, float bottom, float size, float opacity) {
    ID2D1Bitmap* bmp = Icon(a);
    D2D1_RECT_F rc = D2D1::RectF(cx - size / 2, bottom - size, cx + size / 2, bottom);
    if (bmp) {
        // Ekran kartında yüksek kaliteli küçültme: ikonlar keskin ve pürüzsüz.
        if (gpu_ && ctx_) ctx_->DrawBitmap(bmp, &rc, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        else rt_->DrawBitmap(bmp, rc, opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    // İkon yoksa: baş harfli yuvarlak kare
    float r = size * 0.24f;
    brush_->SetColor(light_ ? Rgba(0.35f, 0.38f, 0.45f, 1.f) : Rgba(0.30f, 0.32f, 0.38f, 1.f));
    rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{rc, r, r}, brush_.Get());
    if (!a.name.empty() && labelFormat_) {
        wchar_t c[2] = {static_cast<wchar_t>(std::towupper(a.name[0])), 0};
        brush_->SetColor(kWhite);
        D2D1::Matrix3x2F old;
        rt_->GetTransform(&old);
        float k = size / 22.f;
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(cx, bottom - size / 2)) * old);
        rt_->DrawText(c, 1, labelFormat_.Get(), D2D1::RectF(cx - 10, bottom - size / 2 - 10, cx + 10, bottom - size / 2 + 10),
                      brush_.Get());
        rt_->SetTransform(old);
    }
}

void Dock::DrawFolderIcon(App& a, float cx, float bottom, float size, float opacity) {
    // İndirilenler ve Çöp: Guti'nin kendi çizdiği, dock'a uyumlu yuvarlak kare ikonlar.
    float r = size * 0.24f;
    D2D1_RECT_F rc = D2D1::RectF(cx - size / 2, bottom - size, cx + size / 2, bottom);
    D2D1_COLOR_F top = a.isStack ? Rgba(0.38f, 0.74f, 1.f, opacity) : Rgba(0.72f, 0.74f, 0.79f, opacity);
    D2D1_COLOR_F bot = a.isStack ? Rgba(0.09f, 0.43f, 0.96f, opacity) : Rgba(0.40f, 0.42f, 0.48f, opacity);
    ComPtr<ID2D1GradientStopCollection> stops;
    D2D1_GRADIENT_STOP gs[2] = {{0.f, top}, {1.f, bot}};
    rt_->CreateGradientStopCollection(gs, 2, &stops);
    ComPtr<ID2D1LinearGradientBrush> fill;
    if (stops && SUCCEEDED(rt_->CreateLinearGradientBrush(
                     D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx, rc.top), D2D1::Point2F(cx, rc.bottom)),
                     stops.Get(), &fill))) {
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{rc, r, r}, fill.Get());
    }
    brush_->SetColor(Rgba(1, 1, 1, 0.28f * opacity));
    rt_->DrawRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(rc.left + 0.5f, rc.top + 0.5f, rc.right - 0.5f, rc.bottom - 0.5f), r - 0.5f, r - 0.5f},
        brush_.Get(), 1.f);
    if (!iconFormat_) return;
    const wchar_t* glyph = a.isStack ? L"\uE896" : L"\uE74D";  // İndir / Çöp kutusu
    D2D1::Matrix3x2F old;
    rt_->GetTransform(&old);
    float k = size * 0.50f / 15.f;
    D2D1_POINT_2F c = D2D1::Point2F(cx, bottom - size / 2);
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, c) * old);
    D2D1_RECT_F g = D2D1::RectF(c.x - 12, c.y - 12, c.x + 12, c.y + 12);
    brush_->SetColor(Rgba(0, 0, 0, 0.18f * opacity));  // hafif kabartma gölgesi
    rt_->DrawText(glyph, 1, iconFormat_.Get(), D2D1::RectF(g.left, g.top + 0.8f, g.right, g.bottom + 0.8f), brush_.Get());
    brush_->SetColor(Rgba(1, 1, 1, opacity));
    rt_->DrawText(glyph, 1, iconFormat_.Get(), g, brush_.Get());
    rt_->SetTransform(old);
    if (a.isTrash && a.seen) {
        // Dolu çöp: ikonun altında küçük beyaz bir çizgi
        brush_->SetColor(Rgba(1, 1, 1, 0.85f * opacity));
        float w = size * 0.18f;
        rt_->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1::RectF(cx - w, bottom - size * 0.15f, cx + w, bottom - size * 0.11f), 1.f, 1.f},
            brush_.Get());
    }
}

void Dock::DrawLabel(const App& a, float cx, float top) {
    if (a.name.empty() || !labelFormat_) return;
    float alpha = std::clamp(labelA_.value / 100.f, 0.f, 1.f);
    if (alpha <= 0.01f) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(a.name.c_str(), static_cast<UINT32>(a.name.size()), labelFormat_.Get(), 400.f,
                                         24.f, &layout)))
        return;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    float w = std::min(400.f, m.width) + 22.f, h = 26.f;
    float y1 = top - 9.f + (1.f - alpha) * 4.f, y0 = y1 - h;
    D2D1_ROUNDED_RECT rr{D2D1::RectF(cx - w / 2, y0, cx + w / 2, y1), h / 2, h / 2};
    brush_->SetColor(light_ ? Rgba(0.98f, 0.98f, 0.99f, 0.94f * alpha) : Rgba(0.08f, 0.08f, 0.10f, 0.86f * alpha));
    rt_->FillRoundedRectangle(rr, brush_.Get());
    brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.10f * alpha) : Rgba(1, 1, 1, 0.14f * alpha));
    rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.f);
    brush_->SetColor(light_ ? Rgba(0.08f, 0.08f, 0.10f, alpha) : Rgba(1, 1, 1, 0.95f * alpha));
    layout->SetMaxWidth(w);
    layout->SetMaxHeight(h);
    rt_->DrawTextLayout(D2D1::Point2F(cx - w / 2, y0), layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Dock::Render() {
    if (fullscreen_ || !EnsureResources()) return;
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    float bodyL = (glassRect_.left - cxPx) / scale_;
    float bodyR = (glassRect_.right - cxPx) / scale_;
    // Pencere: taban genişlik + büyüme/gölge payı; piksele hizalı.
    float half = std::max(baseW_ / 2.f + kPadX + kSideRoom, std::max(-bodyL, bodyR) + 40.f);
    int leftPx = static_cast<int>(std::floor(cxPx - half * scale_));
    int widthPx = std::min(texW_, static_cast<int>(std::ceil(2 * half * scale_)) + 2);
    leftPx = std::clamp(leftPx, static_cast<int>(monitor_.left), static_cast<int>(monitor_.right) - widthPx);
    float originX = (leftPx - cxPx) / scale_;  // pencerenin sol kenarının dock koordinatı

    if (!gpu_) {
        RECT bind{0, 0, widthPx, texH_};
        if (FAILED(dcRt_->BindDC(memDC_, &bind))) return;
    }
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(-originX, kPanelRoom + kTopRoom));

    float bw = bodyR - bodyL;
    float r = bodyRadius_;
    // Hafif, kısa gölge
    for (int i = 8; i >= 1; --i) {
        float d = i * 1.5f;
        float k = 1.f - i / 9.f;
        float a = (light_ ? 0.022f : 0.045f) * k * k;
        brush_->SetColor(Rgba(0, 0, 0, a));
        D2D1_ROUNDED_RECT rr{D2D1::RectF(bodyL - d + 0.75f, -d + 2.f, bodyR + d - 0.75f, kBodyH + d + 2.f), r + d, r + d};
        rt_->DrawRoundedRectangle(rr, brush_.Get(), 1.6f);
    }
    // Gövde: düz, sade cam; yalnız ince bir kenar çizgisi (parlama yok).
    // macOS dock'u gibi: dumanlı cam, dışta koyu ince çizgi, içte ince açık kenar.
    D2D1_ROUNDED_RECT body{D2D1::RectF(bodyL, 0, bodyR, kBodyH), r, r};
    brush_->SetColor(light_ ? Rgba(1, 1, 1, 0.06f) : Rgba(0, 0, 0, 0.10f));
    rt_->FillRoundedRectangle(body, brush_.Get());
    brush_->SetColor(Rgba(0, 0, 0, light_ ? 0.14f : 0.35f));
    rt_->DrawRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(bodyL + 0.5f, 0.5f, bodyR - 0.5f, kBodyH - 0.5f), r - 0.5f, r - 0.5f},
        brush_.Get(), 1.f);
    brush_->SetColor(Rgba(1, 1, 1, light_ ? 0.45f : 0.17f));
    rt_->DrawRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(bodyL + 1.5f, 1.5f, bodyR - 1.5f, kBodyH - 1.5f), r - 1.5f, r - 1.5f},
        brush_.Get(), 1.f);
    (void)bw;

    // Öğeler
    for (size_t i = 0; i < apps_.size(); ++i) {
        App& a = apps_[i];
        if (a.isSeparator && a.isEnd) continue;
        if (a.isSeparator) {
            // macOS'taki gibi ince dikey çizgi
            brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.16f) : Rgba(1, 1, 1, 0.20f));
            rt_->FillRectangle(D2D1::RectF(a.x - 0.5f, 11.f, a.x + 0.5f, kBodyH - 11.f), brush_.Get());
            continue;
        }
        if (a.isControl) {
            DrawControl(a);
            continue;
        }
        if (a.size < 1.f) continue;
        bool dragged = dragging_ && static_cast<int>(i) == pressed_;
        if (dragged) continue;  // sürüklenen ikon en son, her şeyin üstünde çizilir
        // Üzerine gelince arka kare yok: ikon yalnız hafifçe yükselir (büyüme/dalga yok).
        float hv = std::clamp(a.hov.value / 100.f, 0.f, 1.f);
        float press = a.press.value;
        float size = a.size * (1.f - press / 60.f);
        float bottom = kIconBottom - a.lift - press * 0.25f - 2.f * hv;
        // Zıplamada yere değerken hafif ezilme
        if (a.launchAt >= 0 && a.lift < 3.f) {
            float squash = (3.f - a.lift) / 3.f * 0.06f;
            D2D1::Matrix3x2F old;
            rt_->GetTransform(&old);
            rt_->SetTransform(D2D1::Matrix3x2F::Scale(1.f + squash, 1.f - squash, D2D1::Point2F(a.x, bottom)) * old);
            if (a.isStart) DrawWindowsLogo(a.x, bottom, size);
            else if (a.isStack || a.isTrash) DrawFolderIcon(a, a.x, bottom, size, 1.f);
            else DrawIcon(a, a.x, bottom, size);
            rt_->SetTransform(old);
        } else {
            // Odak modu: öndeki ve müzik çalan dışındaki uygulamalar sönükleşir.
            float op = 1.f;
            if (a.IsApp() && !a.active && !IsMusicApp(a)) op = 1.f - 0.6f * std::clamp(focusA_.value / 100.f, 0.f, 1.f);
            if (a.isStart) DrawWindowsLogo(a.x, bottom, size);
            else if (a.isStack || a.isTrash) DrawFolderIcon(a, a.x, bottom, size, 1.f);
            else DrawIcon(a, a.x, bottom, size, op);
        }
        if (a.attention) {
            // Dikkat rozeti: ikonun sağ üst köşesinde turuncu nokta
            float bx = a.x + size * 0.38f, by = bottom - size * 0.88f, br = std::max(4.5f, size * 0.12f);
            brush_->SetColor(light_ ? Rgba(1, 1, 1, 0.95f) : Rgba(0.08f, 0.08f, 0.10f, 0.95f));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), br + 1.5f, br + 1.5f), brush_.Get());
            brush_->SetColor(kOrange);
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bx, by), br, br), brush_.Get());
        }
        // Açık göstergesi: küçük nokta; öndeki uygulamada vurgu renginde ve biraz daha uzun.
        float dot = std::clamp(a.dot.value / 100.f, 0.f, 1.f);
        if (dot > 0.01f && IsMusicApp(a)) {
            // Müzik çalan uygulama: nokta yerine sesle oynayan üç minik çubuk
            D2D1_COLOR_F c = a.active ? accent_ : (light_ ? Rgba(0, 0, 0, 0.6f) : Rgba(1, 1, 1, 0.75f));
            brush_->SetColor(WithAlpha(c, dot));
            float tt = static_cast<float>(Now());
            for (int b = -1; b <= 1; ++b) {
                float wob = 0.55f + 0.45f * std::sin(tt * (7.f + b * 2.3f) + b * 1.7f);
                float hgt = 2.5f + 6.f * std::clamp(musicLevel_ * 1.6f, 0.f, 1.f) * wob;
                float bx = a.x + b * 4.f;
                float y1 = kBodyH - 2.5f;
                rt_->FillRoundedRectangle(
                    D2D1_ROUNDED_RECT{D2D1::RectF(bx - 1.1f, y1 - hgt, bx + 1.1f, y1), 1.1f, 1.1f}, brush_.Get());
            }
        } else if (dot > 0.01f) {
            // macOS'taki gibi ikonun altında küçük yuvarlak nokta; öndeki uygulamada daha belirgin.
            float rad = 2.f * dot;
            float al = a.active ? 0.95f : 0.55f;
            brush_->SetColor(light_ ? Rgba(0, 0, 0, al * 0.75f * dot) : Rgba(1, 1, 1, al * dot));
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(a.x, kBodyH - 4.5f), rad, rad), brush_.Get());
        }
    }
    // Sürüklenen ikon: fareyi izler; dock'tan yukarı çıkınca solar ("bırakırsan kalkar").
    if (dragging_ && pressed_ >= 0 && pressed_ < static_cast<int>(apps_.size())) {
        App& a = apps_[pressed_];
        float size = kIcon + kMaxGrow * 0.6f;
        float cy = std::min(kIconBottom - size / 2.f, dragY_);
        bool removing = dragY_ < -60.f && a.pinned && a.windows.empty();
        DrawIcon(a, dragX_, cy + size / 2.f, size, removing ? 0.45f : 0.96f);
    }
    // Kaldırma "puf"u: dışa açılan, sönen küçük bulutçuklar
    if (puffAt_ >= 0) {
        float e = static_cast<float>(Now() - puffAt_) / 0.45f;
        float k = EaseOut(e);
        for (int j = 0; j < 7; ++j) {
            float ang = j * 6.2831853f / 7.f + 0.4f;
            float dist = 8.f + 22.f * k;
            float rr = (7.f - 2.f * static_cast<float>(j % 3)) * (1.f - e * 0.6f);
            brush_->SetColor(light_ ? Rgba(0.4f, 0.4f, 0.45f, 0.55f * (1 - e)) : Rgba(1, 1, 1, 0.55f * (1 - e)));
            rt_->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(puffX_ + std::cos(ang) * dist, puffY_ + std::sin(ang) * dist), rr, rr),
                brush_.Get());
        }
    }
    DrawPanel();
    // İsim balonu
    int li = dragging_ ? -1 : (hover_ >= 0 ? hover_ : labelFor_);
    if (li >= 0 && li < static_cast<int>(apps_.size())) {
        labelFor_ = li;
        const App& a = apps_[li];
        DrawLabel(a, a.x, kIconBottom - a.size - a.lift);
    }

    HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDevice();
        return;
    }
    // Panel kapalıyken dokunun üst kısmı (panel yeri) pencereye aktarılmaz: daha az piksel, daha az iş.
    int topPx = (panel_ != Panel::None || panelA_.value > 0.5f) ? 0
                                                                 : static_cast<int>(std::floor(kPanelRoom * scale_));
    int hidePx = static_cast<int>(std::lround(hide_.value * scale_));
    POINT dst{leftPx, monitor_.bottom - texH_ + topPx + hidePx};
    SIZE size{widthPx, texH_ - topPx};
    POINT src{0, topPx};
    presentedTop_ = topPx;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (gpu_) {
        HDC dc = nullptr;
        if (SUCCEEDED(surface_->GetDC(FALSE, &dc))) {
            UpdateLayeredWindow(overlay_, nullptr, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);
            surface_->ReleaseDC(nullptr);
        }
    } else {
        UpdateLayeredWindow(overlay_, nullptr, &dst, &size, memDC_, &src, 0, &blend, ULW_ALPHA);
    }
    overlayLeft_ = leftPx;
}

// ---------------------------------------------------------------------------
// Pencere işlemleri
// ---------------------------------------------------------------------------

void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    Dock* d = g_dock;
    if (!d || !d->overlay_ || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (event == EVENT_OBJECT_SHOW && d->tray_ && hwnd == d->tray_) {
        PostMessageW(d->overlay_, kRehideMsg, 0, 0);  // Windows taskbar'ı kendini gösterdi: yine gizle
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND) {
        // Yalnız taskbar'da görünen pencereler ilgilendirir; araç ipuçları, menüler, alt pencereler değil.
        bool known = g_describeCache.count(hwnd) != 0;
        if (event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE) {
            if (!known) return;
        } else if (!known) {
            if (GetAncestor(hwnd, GA_ROOT) != hwnd || !d->Eligible(hwnd)) return;
        }
    }
    // Olaylar art arda gelir: kısa bir süre bekleyip bir kez tara.
    SetTimer(d->overlay_, kTimerRescan, event == EVENT_SYSTEM_FOREGROUND ? 30 : 120, nullptr);
}

LRESULT CALLBACK Dock::GlassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* d = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case kAppBarMsg:
        if (!d || !d->overlay_) return 0;
        if (wp == ABN_POSCHANGED) {
            d->RegisterAppBar();
        } else if (wp == ABN_FULLSCREENAPP) {
            d->SetFullscreen(lp != 0 && ForegroundFullscreen());
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Dock::OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* d = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!d || !d->overlay_) return DefWindowProcW(hwnd, msg, wp, lp);
    return d->OnOverlay(msg, wp, lp);
}

LRESULT Dock::OnOverlay(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        // Explorer yeniden başladı: taskbar yine geldi ve dock'un yer kaydı silindi.
        appBar_ = false;
        RegisterAppBar();
        HideTaskbar();
        Rescan();
        return 0;
    }
    auto toDock = [&](LPARAM l, float* x, float* y) {
        float cxPx = (monitor_.left + monitor_.right) / 2.f;
        *x = (overlayLeft_ + GET_X_LPARAM(l) - cxPx) / scale_;
        *y = (GET_Y_LPARAM(l) + presentedTop_) / scale_ - (kPanelRoom + kTopRoom);
    };
    if (msg == shellHookMsg_ && shellHookMsg_) {
        OnShellHook(wp, reinterpret_cast<HWND>(lp));
        return 0;
    }
    switch (msg) {
    case kFrameMsg:
        framePending_ = false;
        OnFrame();
        return 0;
    case kRehideMsg:
        if (trayPeekAt_ >= 0) return 0;  // gizli simgeler açıkken taskbar'a dokunma
        if (tray_ && IsWindowVisible(tray_)) ShowWindow(tray_, SW_HIDE);
        return 0;
    case WM_TIMER:
        if (wp == kTimerRescan) {
            KillTimer(overlay_, kTimerRescan);
            Rescan();
        } else if (wp == kTimerPreview) {
            KillTimer(overlay_, kTimerPreview);
            if (hover_ >= 0 && pressed_ < 0 && !dragging_ && apps_[hover_].IsApp() && !apps_[hover_].windows.empty())
                OpenPreview(hover_);
        } else if (wp == kTimerPanelOut) {
            CheckPanelHover();
        } else if (wp == kTimerAutoHide) {
            UpdateAutoHide();
        } else if (wp == kTimerClock) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wMinute != lastMinute_) {
                lastMinute_ = st.wMinute;
                StartAnimation();
            }
        } else if (wp == kTimerTrayPeek) {
            // Gizli simgeler paneli kapanınca (ya da en geç 30 sn sonra) Windows taskbar'ı yine gizlenir.
            HWND flyout = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
            if (!flyout) flyout = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
            bool open = flyout && IsWindowVisible(flyout);
            double e = Now() - trayPeekAt_;
            if ((e > 3.0 && !open) || e > 30.0) {
                KillTimer(overlay_, kTimerTrayPeek);
                trayPeekAt_ = -1;
                if (tray_ && IsWindowVisible(tray_)) ShowWindow(tray_, SW_HIDE);
                SetTrayInvisible(tray_, false);
            }
        } else if (wp == kTimerFly) {
            MoveFlyouts();
        } else if (wp == kTimerWatch) {
            // Ekran kartının dikey senkron beklemesi bazı sistemlerde kare göndermeyi kesebiliyor:
            // animasyon sürerken 80 ms kare gelmezse kareyi buradan çizeriz.
            if (animating_ && Now() - lastFrameAt_ > 0.08) {
                if (stallLogs_ < 5) {
                    ++stallLogs_;
                    DockLog("frames stalled want=%d pending=%d", wantFrames_.load() ? 1 : 0,
                            framePending_.load() ? 1 : 0);
                }
                framePending_ = false;
                OnFrame();
                if (animating_ && frameEvent_) SetEvent(frameEvent_);
            }
        } else if (wp == kTimerBlink) {
            blinkAt_ = Now();
            StartAnimation();
            SetTimer(overlay_, kTimerBlink, 2600 + (GetTickCount() % 4000), nullptr);  // düzensiz aralık
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        float x, y;
        toDock(lp, &x, &y);
        if (!mouseIn_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, overlay_, 0};
            TrackMouseEvent(&tme);
            mouseIn_ = true;
        }
        if (pressed_ >= 2 && pressed_ < static_cast<int>(apps_.size()) && apps_[pressed_].IsApp() && !dragging_ &&
            (wp & MK_LBUTTON)) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (std::abs(pt.x - downPt_.x) + std::abs(pt.y - downPt_.y) > static_cast<int>(7 * scale_)) dragging_ = true;
        }
        if (dragging_) {
            DragTo(x, y);
            StartAnimation();
            return 0;
        }
        if (panel_ != Panel::None) CheckPanelHover();
        // Büyütme yalnız gövdenin üstündeyken başlar; büyümüş ikonların üstünde de sürer.
        mouseX_ = x;
        anchorX_ = x;
        int was = hover_;
        hover_ = HitTest(x, y);
        if (hover_ != was) {
            bool hasWins = hover_ >= 0 && apps_[hover_].IsApp() && !apps_[hover_].windows.empty();
            if (panel_ == Panel::Preview && hasWins) OpenPreview(hover_);  // önizleme açıkken anında geçer
            else if (hasWins && panel_ == Panel::None) SetTimer(overlay_, kTimerPreview, 550, nullptr);
            else KillTimer(overlay_, kTimerPreview);
        }
        StartAnimation();
        return 0;
    }
    case WM_MOUSELEAVE:
        mouseIn_ = false;
        hover_ = -1;
        if (pressed_ < 0) StartAnimation();
        return 0;
    case WM_LBUTTONDOWN: {
        float x, y;
        toDock(lp, &x, &y);
        pressed_ = HitTest(x, y);
        downPt_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        dragging_ = false;
        if (pressed_ >= 0) SetCapture(overlay_);
        StartAnimation();
        return 0;
    }
    case WM_LBUTTONUP: {
        float x, y;
        toDock(lp, &x, &y);
        if (dragging_) {
            DragTo(x, y);
            EndDrag(true);
            if (GetCapture() == overlay_) ReleaseCapture();
            StartAnimation();
            return 0;
        }
        if (panel_ != Panel::None && pressed_ < 0) {
            int ph = PanelHit(x, y);
            if (ph != -1) PanelClick(ph, false);
            return 0;
        }
        int hit = HitTest(x, y);
        int p = pressed_;
        pressed_ = -1;
        if (GetCapture() == overlay_) ReleaseCapture();
        if (p >= 0 && hit == p) OnClick(p);
        StartAnimation();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_) EndDrag(false);
        return 0;
    case WM_RBUTTONUP: {
        float x, y;
        toDock(lp, &x, &y);
        int hit = HitTest(x, y);
        if (hit == 0) {
            BeginFlyout(apps_[0].x);
            SendWinKey('X');  // Windows logosu: Windows'un kendi sağ tık menüsü
        } else if (hit >= 2 && !apps_[hit].isControl) {
            ShowAppMenu(hit);
        } else {
            ShowEmptyMenu();
        }
        return 0;
    }
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        UpdateMonitor();
        glassRect_ = RECT{};
        RegisterAppBar();
        StartAnimation();
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) {
            ApplyGlass();
            StartAnimation();
        }
        return 0;
    }
    return DefWindowProcW(overlay_, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// 2. parti: menü, sabitleme, sürükle sırala, dosya bırakma, dikkat rozeti
// ---------------------------------------------------------------------------

void Dock::SetPinned(int i, bool pinned) {
    if (i < 2 || i >= static_cast<int>(apps_.size())) return;
    App& a = apps_[i];
    if (a.pinned == pinned || (pinned && a.launch.empty())) return;
    a.pinned = pinned;
    if (!pinned && a.windows.empty()) {
        a.appear.target = 0.f;  // küçülerek kaybolur
        Puff(a.x, kIconBottom - a.size / 2.f);
    }
    Sounds::Play(pinned ? Sounds::Sound::Happy : Sounds::Sound::Close);
    SavePins();
    StartAnimation();
}

void Dock::Puff(float x, float y) {
    puffAt_ = Now();
    puffX_ = x;
    puffY_ = y;
    StartAnimation();
}

void Dock::ShowAppMenu(int i) {
    App& a = apps_[i];
    ClosePanel();
    if (a.isStack || a.isTrash) {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, a.isStack ? L"Klasörü aç" : L"Aç");
        if (a.isTrash) {
            SHQUERYRBINFO rb{sizeof(rb)};
            bool full = SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb)) && rb.i64NumItems > 0;
            AppendMenuW(m, MF_STRING | (full ? 0 : MF_GRAYED), 2, L"Çöpü boşalt…");
        }
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(overlay_);
        UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_CENTERALIGN,
                                  pt.x, pt.y - static_cast<int>(8 * scale_), 0, overlay_, nullptr);
        DestroyMenu(m);
        if (cmd == 1)
            ShellExecuteW(nullptr, L"open", a.isStack ? downloads_.c_str() : L"shell:RecycleBinFolder", nullptr,
                          nullptr, SW_SHOWNORMAL);
        if (cmd == 2) {
            SHEmptyRecycleBinW(nullptr, nullptr, 0);  // Windows kendi onayını sorar
            SetTimer(overlay_, kTimerRescan, 300, nullptr);
        }
        return;
    }
    // Windows görev çubuğundaki gibi: Görevler, uygulama adı, sabitle/kaldır, pencereyi kapat.
    enum { kNew = 1, kPin, kUnpin, kClose, kCloseAll, kTask = 100 };
    std::vector<JumpTask> tasks = JumpTasks(a.exe);
    HMENU m = CreatePopupMenu();
    if (!tasks.empty()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Görevler");
        for (size_t k = 0; k < tasks.size(); ++k)
            AppendMenuW(m, MF_STRING, kTask + static_cast<UINT>(k), tasks[k].title.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    if (!a.launch.empty()) AppendMenuW(m, MF_STRING, kNew, a.name.empty() ? L"Aç" : a.name.c_str());
    if (a.pinned) AppendMenuW(m, MF_STRING, kUnpin, L"Görev çubuğundan kaldır");
    else if (!a.launch.empty()) AppendMenuW(m, MF_STRING, kPin, L"Görev çubuğuna sabitle");
    if (!a.windows.empty()) {
        if (a.windows.size() == 1) {
            AppendMenuW(m, MF_STRING, kClose, L"Pencereyi kapat");
        } else {
            wchar_t label[64];
            swprintf_s(label, L"Tüm pencereleri kapat (%zu)", a.windows.size());
            AppendMenuW(m, MF_STRING, kCloseAll, label);
        }
    }
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(overlay_);
    hover_ = -1;
    StartAnimation();
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_CENTERALIGN,
                              pt.x, pt.y - static_cast<int>(8 * scale_), 0, overlay_, nullptr);
    DestroyMenu(m);
    if (i >= static_cast<int>(apps_.size())) return;
    App& b = apps_[i];
    if (cmd >= static_cast<UINT>(kTask) && cmd - static_cast<UINT>(kTask) < tasks.size()) {
        const JumpTask& t = tasks[cmd - kTask];
        ShellExecuteW(nullptr, L"open", t.path.c_str(), t.args.empty() ? nullptr : t.args.c_str(),
                      t.dir.empty() ? nullptr : t.dir.c_str(), SW_SHOWNORMAL);
        StartAnimation();
        return;
    }
    switch (cmd) {
    case kNew:
        if (b.windows.empty()) b.launchAt = Now();
        ShellExecuteW(nullptr, L"open", b.launch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kPin: SetPinned(i, true); break;
    case kUnpin: SetPinned(i, false); break;
    case kClose:
    case kCloseAll:
        for (HWND h : b.windows) PostMessageW(h, WM_CLOSE, 0, 0);
        break;
    }
    StartAnimation();
}

void Dock::DragTo(float x, float y) {
    dragX_ = x;
    dragY_ = y;
    mouseX_ = x;
    anchorX_ = x;
    if (pressed_ < 2 || pressed_ >= static_cast<int>(apps_.size())) return;
    // Komşunun ortasını geçince yer değiştir; komşu yaylı kayarak yeni yerine gider.
    for (;;) {
        int i = pressed_;
        App& me = apps_[i];
        if (i + 1 < static_cast<int>(apps_.size()) && apps_[i + 1].IsApp() &&
            dragX_ > apps_[i + 1].x - apps_[i + 1].slide.value) {
            apps_[i + 1].slide.value += me.w;
            std::swap(apps_[i], apps_[i + 1]);
            pressed_ = i + 1;
        } else if (i - 1 >= 2 && apps_[i - 1].IsApp() && dragX_ < apps_[i - 1].x - apps_[i - 1].slide.value) {
            apps_[i - 1].slide.value -= me.w;
            std::swap(apps_[i], apps_[i - 1]);
            pressed_ = i - 1;
        } else {
            break;
        }
        float l, r;
        Layout(&l, &r);
    }
    hover_ = pressed_;
}

void Dock::EndDrag(bool commit) {
    if (!dragging_) return;
    dragging_ = false;
    int i = pressed_;
    pressed_ = -1;
    if (i < 2 || i >= static_cast<int>(apps_.size())) return;
    App& a = apps_[i];
    // Bıraktığı yerden yerine kayarak otursun.
    float l, r;
    Layout(&l, &r);
    a.slide.value += dragX_ - a.x;
    if (commit && dragY_ < -60.f && a.pinned) {
        if (a.windows.empty()) {
            a.slide.Snap(0.f);
            a.pinned = false;
            a.appear.Snap(0.f);  // sürüklenen ikon dock dışında "puf" diye kaybolur
            Puff(dragX_, dragY_);
            Sounds::Play(Sounds::Sound::Close);
        } else {
            a.pinned = false;  // açık uygulama yerinde kalır, yalnız sabitlemesi kalkar
        }
    }
    SavePins();
    StartAnimation();
}

void Dock::OnShellHook(WPARAM code, HWND hwnd) {
    switch (code) {
    case HSHELL_FLASH:
        for (auto& a : apps_) {
            if (std::find(a.windows.begin(), a.windows.end(), hwnd) == a.windows.end()) continue;
            if (!a.active && !a.attention) {
                a.attention = true;
                a.launchAt = Now();  // bir iki kez zıplar
                StartAnimation();
            }
        }
        break;
    case HSHELL_WINDOWACTIVATED:
    case HSHELL_RUDEAPPACTIVATED:
        for (auto& a : apps_)
            if (std::find(a.windows.begin(), a.windows.end(), hwnd) != a.windows.end()) a.attention = false;
        SetTimer(overlay_, kTimerRescan, 30, nullptr);
        break;
    case HSHELL_WINDOWCREATED:
    case HSHELL_WINDOWDESTROYED:
        SetTimer(overlay_, kTimerRescan, 120, nullptr);
        break;
    }
}

int Dock::DropOver(POINT screen) {
    if (screen.x == LONG_MIN) {
        mouseIn_ = false;
        hover_ = -1;
        StartAnimation();
        return -1;
    }
    float x, y;
    ScreenToDock(screen, &x, &y);
    mouseIn_ = true;
    mouseX_ = anchorX_ = x;
    int hit = HitTest(x, y);
    hover_ = hit;
    StartAnimation();
    if (hit < 2) return -1;
    const App& a = apps_[hit];
    return ((a.IsApp() && !a.launch.empty()) || a.isTrash) ? hit : -1;
}

void Dock::DropFiles(int i, const std::vector<std::wstring>& paths) {
    if (i < 2 || i >= static_cast<int>(apps_.size()) || paths.empty()) return;
    App& a = apps_[i];
    if (a.isTrash) {
        // Çöpe bırakılan dosyalar Geri Dönüşüm Kutusu'na gider (geri alınabilir).
        std::wstring list;
        for (const auto& p : paths) {
            list += p;
            list.push_back(L'\0');
        }
        list.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = list.c_str();
        op.fFlags = FOF_ALLOWUNDO;
        SHFileOperationW(&op);
        a.icon.Reset();
        a.iconTried = false;
        Puff(a.x, kIconBottom - a.size / 2.f);
        return;
    }
    std::wstring params;
    for (const auto& p : paths) {
        if (!params.empty()) params += L' ';
        params += L'"' + p + L'"';
    }
    // Exe'yi biliyorsak doğrudan ona ver; yoksa kısayolu dosyalarla aç.
    const std::wstring& target = (!a.exe.empty() && a.launch.rfind(L"shell:", 0) != 0) ? a.exe : a.launch;
    if (a.windows.empty()) a.launchAt = Now();
    ShellExecuteW(nullptr, L"open", target.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
    StartAnimation();
}


// ---------------------------------------------------------------------------
// 3. parti: önizlemeler, yığınlar, kontrol hapı, otomatik gizleme
// ---------------------------------------------------------------------------

bool Dock::ScreenToDock(POINT pt, float* x, float* y) const {
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    int hidePx = static_cast<int>(std::lround(hide_.value * scale_));
    *x = (pt.x - cxPx) / scale_;
    *y = (pt.y - (monitor_.bottom - texH_ + hidePx)) / scale_ - (kPanelRoom + kTopRoom);
    return true;
}

void Dock::SetVolume(int percent, bool muted) {
    volume_ = percent;
    muted_ = muted;
    StartAnimation();
}

void Dock::SetOnline(bool online) {
    online_ = online;
    StartAnimation();
}

void Dock::SetAutoHide(bool on) {
    autoHide_ = on;
    WriteSetting(L"DockAutoHide2", on ? 1 : 0);
    if (!overlay_) return;
    RegisterAppBar();
    if (on) {
        keepShownAt_ = Now();
        SetTimer(overlay_, kTimerAutoHide, 100, nullptr);
    } else {
        KillTimer(overlay_, kTimerAutoHide);
        hide_.target = 0.f;
    }
    StartAnimation();
}

void Dock::UpdateAutoHide() {
    if (!autoHide_) return;
    POINT pt;
    GetCursorPos(&pt);
    bool inMonitor = pt.x >= monitor_.left && pt.x < monitor_.right;
    bool atBottom = inMonitor && pt.y >= monitor_.bottom - 2;
    bool shown = hide_.target <= 0.f;
    RECT nearRc = glassRect_;
    InflateRect(&nearRc, static_cast<int>(30 * scale_), 0);
    nearRc.top -= static_cast<LONG>((kMaxGrow + 30.f) * scale_);
    nearRc.bottom = monitor_.bottom;
    bool overDock = shown && PtInRect(&nearRc, pt);
    double t = Now();
    // Basılı/sürükleme durumu yalnız tuş gerçekten basılıyken sayılır: takılı kalırsa dock hiç inmezdi.
    bool holding = (dragging_ || pressed_ >= 0) && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    if (atBottom || overDock || panel_ != Panel::None || holding) keepShownAt_ = t;
    // Fare ayrıldıktan biraz sonra (0.7 sn) aşağı kayar; alta değince yukarı çıkar.
    float target = (t - keepShownAt_ < 0.7) ? 0.f : (kBodyH + kBottom + 32.f);  // gölge de tamamen insin
    {
        static int lastSig = -1;
        int sig = (atBottom ? 1 : 0) | (overDock ? 2 : 0) | (panel_ != Panel::None ? 4 : 0) | (holding ? 8 : 0) |
                  (target > 0 ? 16 : 0) | (static_cast<int>(hide_.value) > 1 ? 32 : 0);
        if (sig != lastSig) {
            lastSig = sig;
            DockLog("autohide cur=%ld,%ld glass=%ld,%ld,%ld,%ld bottom=%d near=%d panel=%d hold=%d(p%d d%d) target=%.0f "
                    "value=%.1f anim=%d",
                    pt.x, pt.y, glassRect_.left, glassRect_.top, glassRect_.right, glassRect_.bottom, atBottom ? 1 : 0,
                    overDock ? 1 : 0, static_cast<int>(panel_), holding ? 1 : 0, pressed_, dragging_ ? 1 : 0, target,
                    hide_.value, animating_ ? 1 : 0);
        }
    }
    if (target != hide_.target) {
        hide_.target = target;
        StartAnimation();
    }
}

namespace {
struct FlyEnum {
    std::vector<HWND>* list;
    DWORD selfPid;
};
BOOL CALLBACK CollectVisible(HWND h, LPARAM lp) {
    auto* fe = reinterpret_cast<FlyEnum*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == fe->selfPid) return TRUE;
    fe->list->push_back(h);
    return TRUE;
}
bool IsShellProcess(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(p, 0, path, &n) != FALSE;
    CloseHandle(p);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return !_wcsicmp(name, L"explorer.exe") || !_wcsicmp(name, L"ShellExperienceHost.exe") ||
           !_wcsicmp(name, L"ShellHost.exe") || !_wcsicmp(name, L"StartMenuExperienceHost.exe");
}
std::vector<HWND> VisibleTopWindows() {
    std::vector<HWND> v;
    FlyEnum fe{&v, GetCurrentProcessId()};
    EnumWindows(CollectVisible, reinterpret_cast<LPARAM>(&fe));
    return v;
}
}  // namespace

void Dock::BeginFlyout(float dockX) {
    flyBefore_ = VisibleTopWindows();
    std::sort(flyBefore_.begin(), flyBefore_.end());
    flyAt_ = Now();
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    flyX_ = static_cast<int>(std::lround(cxPx + dockX * scale_));
    SetTimer(overlay_, kTimerFly, 10, nullptr);
}

void Dock::MoveFlyouts() {
    double e = Now() - flyAt_;
    if (flyAt_ < 0 || e > 2.5) {
        KillTimer(overlay_, kTimerFly);
        flyAt_ = -1;
        return;
    }
    // Panelin alt kenarı dock'un üst kenarının biraz üstünde; yatayda tıklanan yerin ortasında.
    int bottom = monitor_.bottom - static_cast<int>(std::lround((kBottom + kBodyH + 10.f) * scale_));
    for (HWND h : VisibleTopWindows()) {
        if (std::binary_search(flyBefore_.begin(), flyBefore_.end(), h) || h == tray_) continue;
        wchar_t cls[96] = {};
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        if (!wcscmp(cls, L"tooltips_class32") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"IME")) continue;
        if (!IsShellProcess(h)) continue;  // yalnız Windows'un kendi panelleri; açılan uygulama pencerelerine dokunma
        RECT r;
        if (!GetWindowRect(h, &r)) continue;
        int w = r.right - r.left, ht = r.bottom - r.top;
        if (w <= 0 || ht <= 0 || w > (monitor_.right - monitor_.left) * 9 / 10) continue;
        // Yalnız ekranın altına yapışık açılan Windows panelleri (ör. sağ alt köşe) taşınır.
        if (r.bottom < monitor_.bottom - static_cast<int>(160 * scale_)) continue;
        int left = std::clamp(flyX_ - w / 2, static_cast<int>(monitor_.left) + 8, static_cast<int>(monitor_.right) - 8 - w);
        int top = std::max(static_cast<int>(monitor_.top) + 8, bottom - ht);
        if (std::abs(r.left - left) > 1 || std::abs(r.top - top) > 1)
            SetWindowPos(h, nullptr, left, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

int Dock::TrayPart(float x) const {
    for (int k = 0; k < 3; ++k)
        if (x >= trayX_[k] && x < trayX_[k + 1]) return k;
    return -1;
}

void Dock::OpenHiddenIcons() {
    // Gizli simgeler Windows'un kendi panelinde açılır. Panel taskbar'a bağlı olduğu için taskbar kısa süre
    // açılır ama tamamen saydamdır; panel kapanınca yine gizlenir ve saydamlığı geri alınır.
    if (!tray_) tray_ = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray_) return;
    trayPeekAt_ = Now();
    SetTrayInvisible(tray_, true);  // taskbar'ın kendisi hiç görünmesin; yalnız paneli açılır
    ShowWindow(tray_, SW_SHOWNA);
    InvokeTrayChevron(tray_);  // tuş göndermez: odaktaki simgeye yanlışlıkla basılmaz
    SetTimer(overlay_, kTimerTrayPeek, 120, nullptr);
}

void Dock::ShowEmptyMenu() {
    enum { kTaskMgr = 1, kSettings, kGuti };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kTaskMgr, L"Görev Yöneticisi");
    AppendMenuW(m, MF_STRING, kSettings, L"Görev çubuğu ayarları");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kGuti, L"Guti menüsü");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(overlay_);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_CENTERALIGN,
                              pt.x, pt.y - static_cast<int>(8 * scale_), 0, overlay_, nullptr);
    DestroyMenu(m);
    if (cmd == kTaskMgr) ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == kSettings) ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == kGuti && onMenu) onMenu();
}

void Dock::DrawControl(const App& a) {
    // Küçük sistem tepsisi: [gizli simgeler] [ağ · ses] [saat / tarih], Windows'taki gibi.
    float w = a.w - 8.f;
    if (w < 40.f) return;
    float L = a.x - w / 2.f;
    trayX_[0] = L;
    trayX_[1] = L + 28.f;
    trayX_[2] = L + 82.f;
    trayX_[3] = L + w;
    float cy = kBodyH / 2.f;
    bool hot = hover_ >= 0 && &apps_[hover_] == &a;
    int part = hot ? TrayPart(mouseX_) : -1;
    if (part >= 0) {
        brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.055f) : Rgba(1, 1, 1, 0.085f));
        rt_->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{D2D1::RectF(trayX_[part] + 1.f, 6.f, trayX_[part + 1] - 1.f, kBodyH - 6.f), 7.f, 7.f},
            brush_.Get());
    }
    D2D1_COLOR_F ink = light_ ? Rgba(0.08f, 0.08f, 0.10f, 0.92f) : Rgba(1, 1, 1, 0.92f);
    if (iconFormat_) {
        D2D1::Matrix3x2F old;
        rt_->GetTransform(&old);
        auto glyph = [&](const wchar_t* g, float x, float k, float alpha) {
            rt_->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(x, cy)) * old);
            brush_->SetColor(WithAlpha(ink, alpha));
            rt_->DrawText(g, 1, iconFormat_.Get(), D2D1::RectF(x - 12, cy - 12, x + 12, cy + 12), brush_.Get());
            rt_->SetTransform(old);
        };
        glyph(L"\xE70E", (trayX_[0] + trayX_[1]) / 2.f, 0.72f, 0.9f);  // yukarı ok
        double t = Now();
        if (t - wifiCheckedAt_ > 15.0) {
            wifiCheckedAt_ = t;
            wifi_ = WifiConnected();
        }
        float gx = (trayX_[1] + trayX_[2]) / 2.f;
        glyph(wifi_ ? L"\xE701" : L"\xE839", gx - 11.f, 0.95f, online_ ? 1.f : 0.35f);  // Wi-Fi ya da kablo
        const wchar_t* vol = muted_ || volume_ == 0 ? L"\xE74F"
                             : volume_ < 0       ? L"\xE767"
                             : volume_ < 34      ? L"\xE993"
                             : volume_ < 67      ? L"\xE994"
                                                 : L"\xE995";
        glyph(vol, gx + 11.f, 0.95f, 1.f);
    }
    if (titleFormat_) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        lastMinute_ = st.wMinute;
        wchar_t tm[16], dt[24];
        swprintf_s(tm, L"%02d:%02d", st.wHour, st.wMinute);
        swprintf_s(dt, L"%02d.%02d.%04d", st.wDay, st.wMonth, st.wYear);
        float cx = (trayX_[2] + trayX_[3]) / 2.f, hw = (trayX_[3] - trayX_[2]) / 2.f;
        brush_->SetColor(ink);
        rt_->DrawText(tm, static_cast<UINT32>(wcslen(tm)), titleFormat_.Get(),
                      D2D1::RectF(cx - hw, cy - 15.f, cx + hw, cy + 1.f), brush_.Get());
        rt_->DrawText(dt, static_cast<UINT32>(wcslen(dt)), titleFormat_.Get(),
                      D2D1::RectF(cx - hw, cy - 1.f, cx + hw, cy + 15.f), brush_.Get());
    }
}

// --- Panel ortak --------------------------------------------------------------

void Dock::ClosePanel() {
    if (panel_ == Panel::None) return;
    for (auto t : thumbs_) DwmUnregisterThumbnail(t);
    thumbs_.clear();
    if (preview_) ShowWindow(preview_, SW_HIDE);
    panel_ = Panel::None;
    panelFor_ = -1;
    panelHover_ = -1;
    KillTimer(overlay_, kTimerPanelOut);
    StartAnimation();
}

void Dock::LayoutPanel() {
    cards_.clear();
    if (panelFor_ < 0 || panelFor_ >= static_cast<int>(apps_.size())) return;
    float ax = apps_[panelFor_].x;
    if (panel_ == Panel::Preview) {
        size_t n = previewWins_.size();
        panelW_ = n * kCardW + (n - 1) * kCardGap + 2 * kPanelPad;
        panelH_ = kThumbH + 40.f + 2 * kPanelPad;
    } else {
        size_t rows = std::max<size_t>(1, stack_.size());
        panelW_ = kStackW;
        panelH_ = 38.f + rows * kStackRow + 8.f + 34.f + 8.f;
    }
    float halfMon = (monitor_.right - monitor_.left) / scale_ / 2.f - 10.f;
    panelX_ = std::clamp(ax, -halfMon + panelW_ / 2, halfMon - panelW_ / 2);
    float L = panelX_ - panelW_ / 2, T = PanelBottom() - panelH_;
    if (panel_ == Panel::Preview) {
        for (size_t k = 0; k < previewWins_.size(); ++k) {
            float l = L + kPanelPad + k * (kCardW + kCardGap);
            cards_.push_back(D2D1::RectF(l, T + kPanelPad, l + kCardW, T + kPanelPad + kThumbH + 40.f));
        }
    } else {
        float y = T + 38.f;
        for (size_t k = 0; k < stack_.size(); ++k, y += kStackRow)
            cards_.push_back(D2D1::RectF(L + 6, y, L + panelW_ - 6, y + kStackRow));
        if (stack_.empty()) y += kStackRow;
        cards_.push_back(D2D1::RectF(L + 6, y + 8, L + panelW_ - 6, y + 8 + 34));  // "Klasörde aç"
    }
}

int Dock::PanelHit(float x, float y) const {
    for (size_t k = 0; k < cards_.size(); ++k) {
        const D2D1_RECT_F& r = cards_[k];
        if (x < r.left || x > r.right || y < r.top || y > r.bottom) continue;
        if (panel_ == Panel::Preview) {
            // Sağ üstteki kapat düğmesi
            if (x > r.right - 26 && y < r.top + 26) return 1000 + static_cast<int>(k);
            return static_cast<int>(k);
        }
        return k + 1 == cards_.size() ? -2 : static_cast<int>(k);
    }
    return -1;
}

void Dock::PanelClick(int hit, bool middle) {
    if (panel_ == Panel::Preview) {
        int k = hit >= 1000 ? hit - 1000 : hit;
        if (k < 0 || k >= static_cast<int>(previewWins_.size())) return;
        HWND h = previewWins_[k];
        if (hit >= 1000 || middle) {
            PostMessageW(h, WM_CLOSE, 0, 0);
            ClosePanel();
            return;
        }
        ClosePanel();
        if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
        SetForegroundWindow(h);
        return;
    }
    if (panel_ == Panel::Stack) {
        if (hit == -2) {
            ShellExecuteW(nullptr, L"open", downloads_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (hit >= 0 && hit < static_cast<int>(stack_.size())) {
            ShellExecuteW(nullptr, L"open", stack_[hit].path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ClosePanel();
    }
}

void Dock::CheckPanelHover() {
    if (panel_ == Panel::None) {
        KillTimer(overlay_, kTimerPanelOut);
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    float x, y;
    ScreenToDock(pt, &x, &y);
    int h = PanelHit(x, y);
    if (h != panelHover_) {
        panelHover_ = h;
        StartAnimation();
    }
    float L = panelX_ - panelW_ / 2 - 10, R = panelX_ + panelW_ / 2 + 10, T = PanelBottom() - panelH_ - 10;
    bool inPanel = x >= L && x <= R && y >= T && y <= PanelBottom() + 24;
    RECT body = glassRect_;
    body.top -= static_cast<LONG>((kMaxGrow + 8.f) * scale_);
    bool inDock = PtInRect(&body, pt) != FALSE;
    if (inPanel || inDock) {
        panelOut_ = 0;
    } else if (++panelOut_ >= 3) {
        ClosePanel();
    }
}

// --- Pencere önizlemeleri -------------------------------------------------------

LRESULT CALLBACK Dock::PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* d = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (d) d->CheckPanelHover();
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
        if (d) {
            POINT pt;
            GetCursorPos(&pt);
            float x, y;
            d->ScreenToDock(pt, &x, &y);
            int h = d->PanelHit(x, y);
            if (h != -1) d->PanelClick(h, msg == WM_MBUTTONUP);
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Dock::OpenPreview(int i) {
    if (i < 0 || i >= static_cast<int>(apps_.size()) || apps_[i].windows.empty()) return;
    for (auto t : thumbs_) DwmUnregisterThumbnail(t);
    thumbs_.clear();
    panel_ = Panel::Preview;
    panelFor_ = i;
    panelHover_ = -1;
    panelOut_ = 0;
    previewWins_.assign(apps_[i].windows.begin(),
                        apps_[i].windows.begin() + std::min<size_t>(4, apps_[i].windows.size()));
    previewTitles_.clear();
    for (HWND h : previewWins_) {
        wchar_t t[256] = {};
        GetWindowTextW(h, t, ARRAYSIZE(t));
        previewTitles_.push_back(t);
    }
    LayoutPanel();
    if (!preview_) {
        preview_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GutiDockPreview",
                                   L"Guti Dock Preview", WS_POPUP, 0, 0, 1, 1, glass_, nullptr, instance_, this);
        if (!preview_) return;
        DWORD corner = 1;
        DwmSetWindowAttribute(preview_, 33, &corner, sizeof(corner));
        COLORREF none = 0xFFFFFFFE;
        DwmSetWindowAttribute(preview_, 34, &none, sizeof(none));
    }
    SetAccentBlur(preview_, light_);
    // Panelin ekran yeri (px)
    float cxPx = (monitor_.left + monitor_.right) / 2.f;
    int hidePx = static_cast<int>(std::lround(hide_.value * scale_));
    float bodyTopPx = monitor_.bottom - (kBottom + kBodyH) * scale_ + hidePx;
    int left = static_cast<int>(std::lround(cxPx + (panelX_ - panelW_ / 2) * scale_));
    int top = static_cast<int>(std::lround(bodyTopPx + (PanelBottom() - panelH_) * scale_));
    int w = static_cast<int>(std::lround(panelW_ * scale_)), h = static_cast<int>(std::lround(panelH_ * scale_));
    int d = static_cast<int>(std::lround(32 * scale_));
    SetWindowRgn(preview_, CreateRoundRectRgn(0, 0, w + 1, h + 1, d, d), FALSE);
    // Katmanlı dock penceresinin hemen altına: başlıklar ve kapat düğmeleri önizlemenin üstünde çizilir.
    SetWindowPos(preview_, overlay_, left, top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    for (size_t k = 0; k < previewWins_.size(); ++k) {
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(preview_, previewWins_[k], &th)) || !th) {
            thumbs_.push_back(nullptr);
            continue;
        }
        SIZE src{};
        DwmQueryThumbnailSourceSize(th, &src);
        const D2D1_RECT_F& c = cards_[k];
        float areaW = kCardW - 12.f, areaH = kThumbH - 6.f;
        float sw = src.cx > 0 ? static_cast<float>(src.cx) : areaW, sh = src.cy > 0 ? static_cast<float>(src.cy) : areaH;
        float k2 = std::min(areaW * scale_ / sw, areaH * scale_ / sh);
        float tw = sw * k2, tht = sh * k2;
        float ox = (c.left - (panelX_ - panelW_ / 2) + 6.f) * scale_ + (areaW * scale_ - tw) / 2;
        float oy = (c.top - (PanelBottom() - panelH_) + 6.f) * scale_ + (areaH * scale_ - tht) / 2;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = RECT{static_cast<LONG>(ox), static_cast<LONG>(oy), static_cast<LONG>(ox + tw),
                               static_cast<LONG>(oy + tht)};
        p.fVisible = TRUE;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(th, &p);
        thumbs_.push_back(th);
    }
    SetTimer(overlay_, kTimerPanelOut, 150, nullptr);
    StartAnimation();
}

// --- İndirilenler yığını -----------------------------------------------------------

void Dock::OpenStack(int i) {
    for (auto t : thumbs_) DwmUnregisterThumbnail(t);
    thumbs_.clear();
    if (preview_) ShowWindow(preview_, SW_HIDE);
    stack_.clear();
    struct F {
        std::wstring name;
        FILETIME t;
    };
    std::vector<F> files;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((downloads_ + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            std::wstring n = fd.cFileName, l = Lower(n);
            if (EndsWith(l, L".crdownload") || EndsWith(l, L".part") || EndsWith(l, L".tmp") || EndsWith(l, L".ini"))
                continue;
            files.push_back({n, fd.ftLastWriteTime});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return CompareFileTime(&a.t, &b.t) > 0; });
    for (size_t k = 0; k < files.size() && k < static_cast<size_t>(kStackMax); ++k) {
        StackItem s;
        s.path = downloads_ + L"\\" + files[k].name;
        s.name = files[k].name;
        stack_.push_back(std::move(s));
    }
    panel_ = Panel::Stack;
    panelFor_ = i;
    panelHover_ = -1;
    panelOut_ = 0;
    LayoutPanel();
    SetTimer(overlay_, kTimerPanelOut, 150, nullptr);
    StartAnimation();
}

ID2D1Bitmap* Dock::FileIcon(StackItem& s) {
    if (s.icon || s.iconTried || !wic_) return s.icon.Get();
    s.iconTried = true;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(s.path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    int px = static_cast<int>(std::ceil(22 * scale_));
    HBITMAP hbmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) || !hbmp)
        return nullptr;
    ComPtr<IWICBitmap> wbmp;
    ComPtr<IWICFormatConverter> conv;
    if (SUCCEEDED(wic_->CreateBitmapFromHBITMAP(hbmp, nullptr, WICBitmapUseAlpha, &wbmp)) &&
        SUCCEEDED(wic_->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wbmp.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom))) {
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &s.icon);
    }
    DeleteObject(hbmp);
    return s.icon.Get();
}

void Dock::DrawPanel() {
    float alpha = std::clamp(panelA_.value / 100.f, 0.f, 1.f);
    if (panel_ == Panel::None || alpha <= 0.01f) return;
    float L = panelX_ - panelW_ / 2, T = PanelBottom() - panelH_, R = L + panelW_, B = PanelBottom();
    D2D1_COLOR_F ink = light_ ? Rgba(0.08f, 0.08f, 0.10f, 0.95f * alpha) : Rgba(1, 1, 1, 0.95f * alpha);
    D2D1_COLOR_F soft = light_ ? Rgba(0.08f, 0.08f, 0.10f, 0.55f * alpha) : Rgba(1, 1, 1, 0.55f * alpha);
    if (panel_ == Panel::Preview) {
        // Cam zemin önizleme penceresinden gelir; burada kenar ışığı, başlıklar ve kapat düğmesi.
        brush_->SetColor(light_ ? Rgba(1, 1, 1, 0.7f * alpha) : Rgba(1, 1, 1, 0.18f * alpha));
        rt_->DrawRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(L + 0.5f, T + 0.5f, R - 0.5f, B - 0.5f), 15.5f, 15.5f},
                                  brush_.Get(), 1.f);
        for (size_t k = 0; k < cards_.size() && k < previewTitles_.size(); ++k) {
            const D2D1_RECT_F& c = cards_[k];
            bool hot = panelHover_ == static_cast<int>(k) || panelHover_ == 1000 + static_cast<int>(k);
            if (hot) {
                brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.07f * alpha) : Rgba(1, 1, 1, 0.10f * alpha));
                rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{c, 10.f, 10.f}, brush_.Get());
            }
            if (k < previewWins_.size() && IsIconic(previewWins_[k]) && panelFor_ >= 0) {
                // Küçültülmüş pencerenin görüntüsü olmaz: uygulamanın ikonunu göster.
                App& a = apps_[panelFor_];
                DrawIcon(a, (c.left + c.right) / 2, c.top + kThumbH / 2 + 24, 48.f, 0.85f * alpha);
            }
            if (titleFormat_) {
                const std::wstring& t = previewTitles_[k];
                brush_->SetColor(ink);
                rt_->DrawText(t.c_str(), static_cast<UINT32>(t.size()), titleFormat_.Get(),
                              D2D1::RectF(c.left + 8, c.top + kThumbH + 6, c.right - 8, c.bottom - 6), brush_.Get());
            }
            if (hot) {
                float cx = c.right - 13, cy = c.top + 13;
                brush_->SetColor(Rgba(0.10f, 0.10f, 0.12f, 0.85f * alpha));
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 9, 9), brush_.Get());
                brush_->SetColor(panelHover_ == 1000 + static_cast<int>(k) ? Rgba(1, 0.45f, 0.42f, alpha)
                                                                             : Rgba(1, 1, 1, 0.9f * alpha));
                rt_->DrawLine(D2D1::Point2F(cx - 3.5f, cy - 3.5f), D2D1::Point2F(cx + 3.5f, cy + 3.5f), brush_.Get(), 1.5f);
                rt_->DrawLine(D2D1::Point2F(cx + 3.5f, cy - 3.5f), D2D1::Point2F(cx - 3.5f, cy + 3.5f), brush_.Get(), 1.5f);
            }
        }
        return;
    }
    // İndirilenler: kendi çizdiğimiz kart, aşağıdan hafifçe kayarak gelir.
    float dy = (1.f - alpha) * 10.f;
    D2D1::Matrix3x2F old;
    rt_->GetTransform(&old);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * old);
    D2D1_ROUNDED_RECT card{D2D1::RectF(L, T, R, B), 16.f, 16.f};
    for (int i = 8; i >= 1; --i) {
        float d = i * 1.8f;
        brush_->SetColor(Rgba(0, 0, 0, 0.035f * (1.f - i / 9.f) * alpha));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(L - d, T - d + 4, R + d, B + d + 4), 16 + d, 16 + d},
                                  brush_.Get());
    }
    brush_->SetColor(light_ ? Rgba(0.97f, 0.97f, 0.98f, 0.97f * alpha) : Rgba(0.11f, 0.11f, 0.13f, 0.96f * alpha));
    rt_->FillRoundedRectangle(card, brush_.Get());
    brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.10f * alpha) : Rgba(1, 1, 1, 0.14f * alpha));
    rt_->DrawRoundedRectangle(card, brush_.Get(), 1.f);
    if (headFormat_) {
        brush_->SetColor(ink);
        rt_->DrawText(L"İndirilenler", 12, headFormat_.Get(), D2D1::RectF(L + 16, T + 8, R - 16, T + 34), brush_.Get());
    }
    for (size_t k = 0; k + 1 < cards_.size() || (stack_.empty() && k == 0); ++k) {
        if (stack_.empty()) {
            brush_->SetColor(soft);
            if (titleFormat_) {
                D2D1_RECT_F r = D2D1::RectF(L + 16, T + 38, R - 16, T + 38 + kStackRow);
                rt_->DrawText(L"Klasör boş", 10, headFormat_.Get(), r, brush_.Get());
            }
            break;
        }
        const D2D1_RECT_F& r = cards_[k];
        if (panelHover_ == static_cast<int>(k)) {
            brush_->SetColor(light_ ? Rgba(0, 0, 0, 0.06f * alpha) : Rgba(1, 1, 1, 0.09f * alpha));
            rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, 8.f, 8.f}, brush_.Get());
        }
        StackItem& s = stack_[k];
        if (ID2D1Bitmap* ic = FileIcon(s)) {
            float iy = (r.top + r.bottom) / 2;
            rt_->DrawBitmap(ic, D2D1::RectF(r.left + 8, iy - 11, r.left + 30, iy + 11), alpha,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (headFormat_) {
            ComPtr<IDWriteTextLayout> lay;
            if (SUCCEEDED(dwrite_->CreateTextLayout(s.name.c_str(), static_cast<UINT32>(s.name.size()),
                                                    titleFormat_.Get(), r.right - r.left - 46, kStackRow, &lay))) {
                lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                brush_->SetColor(ink);
                rt_->DrawTextLayout(D2D1::Point2F(r.left + 38, r.top), lay.Get(), brush_.Get());
            }
        }
    }
    if (!cards_.empty()) {
        const D2D1_RECT_F& r = cards_.back();
        bool hot = panelHover_ == -2;
        brush_->SetColor(light_ ? Rgba(0, 0, 0, (hot ? 0.09f : 0.05f) * alpha)
                                : Rgba(1, 1, 1, (hot ? 0.13f : 0.07f) * alpha));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{r, 10.f, 10.f}, brush_.Get());
        if (headFormat_) {
            ComPtr<IDWriteTextLayout> lay;
            if (SUCCEEDED(dwrite_->CreateTextLayout(L"Klasörde aç", 11, headFormat_.Get(), r.right - r.left,
                                                    r.bottom - r.top, &lay))) {
                lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                brush_->SetColor(ink);
                rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), lay.Get(), brush_.Get());
            }
        }
    }
    rt_->SetTransform(old);
}

// ---------------------------------------------------------------------------
// 4. parti: Guti ile bağ (müzik ritmi, odak modu)
// ---------------------------------------------------------------------------

bool Dock::IsMusicApp(const App& a) const {
    if (musicApp_.empty() || !a.IsApp() || a.windows.empty()) return false;
    if (a.aumid == musicApp_) return true;
    // Spotify "Spotify.exe", Chrome "Chrome" gibi kısa adlar: exe dosya adıyla karşılaştır.
    size_t slash = a.exe.find_last_of(L'\\');
    std::wstring file = slash == std::wstring::npos ? a.exe : a.exe.substr(slash + 1);
    if (file == musicApp_) return true;
    if (EndsWith(file, L".exe") && file.substr(0, file.size() - 4) == musicApp_) return true;
    return !a.aumid.empty() && musicApp_.size() > 3 && a.aumid.rfind(musicApp_, 0) == 0;
}

void Dock::SetMusic(const std::wstring& sourceApp, float level) {
    std::wstring app = Lower(sourceApp);
    bool changed = app != musicApp_;
    musicApp_ = app;
    musicLevel_ = std::max(0.f, level);
    if (!overlay_) return;
    // Çubuklar ~25 kare/sn yeter: dock'u sürekli 60 Hz çizmeyelim.
    double t = Now();
    if (changed || (!musicApp_.empty() && t - musicFrameAt_ > 0.04)) {
        musicFrameAt_ = t;
        StartAnimation();
    }
}

void Dock::SetFocus(bool on) {
    if (on == focus_) return;
    focus_ = on;
    StartAnimation();
}
