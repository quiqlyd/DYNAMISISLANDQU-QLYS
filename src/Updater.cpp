#include "Updater.h"

#include <winhttp.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace Updater {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kLatestPath[] = L"/repos/quiqlyd/DYNAMISISLANDQU-QLYS/releases/latest";
constexpr wchar_t kAssetName[] = L"DynamisIsland.exe";
constexpr wchar_t kUserAgent[] = L"DynamisIsland-Updater";

std::atomic<bool> g_busy{false};
int g_stage = 0;  // başarısızlığın olduğu adım: adada "hata N" olarak görünür

// Her adımı exe'nin yanındaki update.log dosyasına yazar: sorun olursa nedenini görürüz.
void Log(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring logPath(path, n);
    size_t slash = logPath.find_last_of(L"\\/");
    logPath = logPath.substr(0, slash + 1) + L"update.log";

    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    swprintf_s(line, L"%04d-%02d-%02d %02d:%02d:%02d v%d  %s\r\n", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, kCurrentVersion, msg);

    HANDLE f = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 0 ? bytes - 1 : 0, '\0');
    if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, n);
}

// Basit HTTPS GET; yönlendirmeleri WinHTTP kendisi izler.
bool HttpGet(const std::wstring& url, std::vector<char>& out, size_t maxBytes) {
    URL_COMPONENTSW uc{sizeof(uc)};
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048]{};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        Log(L"URL çözülemedi (%lu): %s", GetLastError(), url.c_str());
        return false;
    }
    std::wstring fullPath = std::wstring(path) + extra;

    bool ok = false;
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET request =
        connect ? WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                : nullptr;
    if (request) {
        const wchar_t* headers = L"Accept: application/vnd.github+json, application/octet-stream\r\n";
        if (WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                               0, 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != 200) Log(L"HTTP %lu: %s", status, url.c_str());
            if (status == 200) {
                ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
                    if (avail == 0) break;
                    size_t old = out.size();
                    if (old + avail > maxBytes) { ok = false; break; }
                    out.resize(old + avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, out.data() + old, avail, &read)) { ok = false; break; }
                    out.resize(old + read);
                }
            }
        }
    }
    else if (!session || !connect || !request) {
        Log(L"Bağlantı açılamadı (%lu): %s", GetLastError(), url.c_str());
    }
    if (!ok) Log(L"İstek başarısız (%lu, %zu bayt): %s", GetLastError(), out.size(), url.c_str());
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// JSON'da "key":"value" değerini bulur (start konumundan itibaren).
std::string JsonString(const std::string& json, const std::string& key, size_t start = 0,
                       size_t* foundAt = nullptr) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, start);
    if (k == std::string::npos) return {};
    size_t colon = json.find(':', k + needle.size());
    size_t q1 = colon == std::string::npos ? colon : json.find('"', colon);
    size_t q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    if (foundAt) *foundAt = q2;
    return json.substr(q1 + 1, q2 - q1 - 1);
}

int VersionFromTag(const std::string& tag) {
    // "build-42" veya "v42" -> 42
    int v = 0;
    bool any = false;
    for (char c : tag) {
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); any = true; }
        else if (any) break;
    }
    return any ? v : 0;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Antivirüs dosyayı kısa süre kilitleyebilir: birkaç kez dene.
bool MoveWithRetry(const std::wstring& from, const std::wstring& to) {
    for (int i = 0; i < 10; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(300);
    }
    return false;
}

bool Install(const std::wstring& url) {
    std::vector<char> exe;
    if (!HttpGet(url, exe, 64u * 1024 * 1024)) { g_stage = 3; return false; }
    if (exe.size() < 64 * 1024 || exe[0] != 'M' || exe[1] != 'Z') {  // gerçek bir .exe mi
        Log(L"İndirilen dosya geçerli bir exe değil (%zu bayt)", exe.size());
        g_stage = 4;
        return false;
    }

    std::wstring current = ExePath();
    std::wstring fresh = current + L".new";
    std::wstring old = current + L".old";

    HANDLE f = CreateFileW(fresh.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        Log(L"Yeni dosya yazılamadı (%lu): %s", GetLastError(), fresh.c_str());
        g_stage = 5;
        return false;
    }
    DWORD written = 0;
    BOOL wrote = WriteFile(f, exe.data(), static_cast<DWORD>(exe.size()), &written, nullptr);
    CloseHandle(f);
    if (!wrote || written != exe.size()) {
        Log(L"Yeni dosya eksik yazıldı (%lu)", GetLastError());
        g_stage = 5;
        DeleteFileW(fresh.c_str());
        return false;
    }

    // Çalışan .exe silinemez ama yeniden adlandırılabilir.
    DeleteFileW(old.c_str());
    if (!MoveWithRetry(current, old)) {
        Log(L"Eski exe kenara alınamadı (%lu): %s", GetLastError(), current.c_str());
        g_stage = 6;
        DeleteFileW(fresh.c_str());
        return false;
    }
    if (!MoveWithRetry(fresh, current)) {
        Log(L"Yeni exe yerine konamadı (%lu)", GetLastError());
        g_stage = 7;
        MoveFileExW(old.c_str(), current.c_str(), MOVEFILE_REPLACE_EXISTING);  // geri al
        DeleteFileW(fresh.c_str());
        return false;
    }
    return true;
}

}  // namespace

void CheckAsync(HWND notify, bool install) {
    if (g_busy.exchange(true)) return;
    std::thread([notify, install] {
        Result result = Result::Failed;
        LPARAM value = 0;
        g_stage = 1;
        std::vector<char> body;
        if (HttpGet(std::wstring(L"https://") + kApiHost + kLatestPath, body, 2u * 1024 * 1024)) {
            std::string json(body.begin(), body.end());
            int latest = VersionFromTag(JsonString(json, "tag_name"));

            // Doğru dosyanın indirme adresini bul.
            std::string url;
            size_t pos = 0;
            for (;;) {
                size_t at = 0;
                std::string u = JsonString(json, "browser_download_url", pos, &at);
                if (u.empty()) break;
                if (u.size() >= 17 && u.compare(u.size() - 17, 17, "DynamisIsland.exe") == 0) {
                    url = u;
                    break;
                }
                pos = at + 1;
            }

            Log(L"Son sürüm: %d, indirme adresi: %s", latest, url.empty() ? L"(yok)" : Widen(url).c_str());
            g_stage = 2;
            if (latest > 0 && latest <= kCurrentVersion) {
                result = Result::UpToDate;
                value = latest;
            } else if (latest > kCurrentVersion && !url.empty()) {
                value = latest;
                if (!install) {
                    result = Result::Available;
                } else {
                    result = Install(Widen(url)) ? Result::Installed : Result::Failed;
                }
            }
        }
        if (result == Result::Failed) value = g_stage;  // hata adımı
        Log(L"Sonuç: %d (adım %d)", static_cast<int>(result), g_stage);
        g_busy = false;
        PostMessageW(notify, WM_UPDATE_RESULT, static_cast<WPARAM>(result), value);
    }).detach();
}

void CleanupOldFiles() {
    std::wstring current = ExePath();
    DeleteFileW((current + L".old").c_str());
    DeleteFileW((current + L".new").c_str());
}

bool RelaunchUpdated() {
    std::wstring exe = ExePath();
    std::wstring cmd = L"\"" + exe + L"\" --updated";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        Log(L"Yeni sürüm başlatılamadı (%lu)", GetLastError());
        return false;
    }
    Log(L"Yeni sürüm başlatıldı");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

}  // namespace Updater
