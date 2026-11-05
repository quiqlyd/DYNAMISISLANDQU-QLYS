#include "Net.h"

#include <shlobj.h>
#include <winhttp.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace Net {

bool Get(const std::wstring& url, std::string& body, size_t maxBytes, DWORD* statusOut) {
    if (statusOut) *statusOut = 0;
    body.clear();
    URL_COMPONENTSW uc{sizeof(uc)};
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring fullPath = std::wstring(path) + extra;

    bool ok = false;
    HINTERNET session = WinHttpOpen(L"DynamisIsland/1.0 (+https://github.com/quiqlyd/DYNAMISISLANDQU-QLYS)",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (session) WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET request =
        connect ? WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (statusOut) *statusOut = status;
        ok = true;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(request, &avail)) { ok = false; break; }
            if (avail == 0) break;
            size_t old = body.size();
            if (old + avail > maxBytes) { ok = false; break; }
            body.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + old, avail, &read)) { ok = false; break; }
            body.resize(old + read);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Encode(const std::wstring& text) {
    static const wchar_t* hex = L"0123456789ABCDEF";
    std::wstring out;
    for (unsigned char c : Narrow(text)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<wchar_t>(c);
        } else {
            out += L'%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::wstring DataDir(const wchar_t* sub) {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) dir = base;
    CoTaskMemFree(base);
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tmp)) dir = tmp;
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"DynamisIsland\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (sub && *sub) {
        dir += sub;
        dir += L'\\';
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

bool ReadFile(const std::wstring& path, std::string& out, size_t maxBytes) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && static_cast<ULONGLONG>(size.QuadPart) <= maxBytes;
    if (ok && size.QuadPart > 0) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ::ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != FALSE;
        out.resize(read);
    }
    CloseHandle(f);
    return ok;
}

bool WriteFile(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() ||
              ::WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != FALSE;
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

}  // namespace Net
