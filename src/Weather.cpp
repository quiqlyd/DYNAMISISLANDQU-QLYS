#include "Weather.h"
#include "Net.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace Weather {
namespace {

// "key": sonrasındaki sayı (from'dan itibaren). Yoksa NAN.
double Number(const std::string& j, const char* key, size_t from) {
    std::string needle = std::string("\"") + key + "\":";
    size_t k = j.find(needle, from);
    if (k == std::string::npos) return NAN;
    const char* p = j.c_str() + k + needle.size();
    while (*p == ' ') ++p;
    if (!(*p == '-' || (*p >= '0' && *p <= '9'))) return NAN;
    return atof(p);
}

std::string String(const std::string& j, const char* key, size_t from) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t k = j.find(needle, from);
    if (k == std::string::npos) return {};
    size_t a = k + needle.size(), b = j.find('"', a);
    return b == std::string::npos ? std::string() : j.substr(a, b - a);
}

std::mutex g_mx;
std::wstring g_place;  // son çözülen konumun anahtarı
double g_lat = NAN, g_lon = NAN;
std::wstring g_city;

}  // namespace

bool IsRain(int c) { return (c >= 51 && c <= 67) || (c >= 80 && c <= 82); }
bool IsSnow(int c) { return (c >= 71 && c <= 77) || c == 85 || c == 86; }
bool IsClear(int c) { return c == 0 || c == 1; }
bool IsStorm(int c) { return c >= 95; }

std::wstring Short(const Info& i) {
    if (!i.ok) return {};
    const wchar_t* sym = L"☁";  // bulut
    if (IsClear(i.code)) sym = i.day ? L"☀" : L"☾";
    else if (IsStorm(i.code)) sym = L"⚡";
    else if (IsSnow(i.code)) sym = L"❄";
    else if (IsRain(i.code)) sym = L"☂";
    wchar_t buf[32];
    swprintf_s(buf, L"%s %d°", sym, static_cast<int>(std::lround(i.temp)));
    return buf;
}

void Refresh(HWND notify, std::wstring city) {
    std::thread([notify, city] {
        auto* info = new Info;
        double lat = NAN, lon = NAN;
        std::wstring name;
        {
            std::lock_guard<std::mutex> lock(g_mx);
            if (g_place == city && !std::isnan(g_lat)) {
                lat = g_lat;
                lon = g_lon;
                name = g_city;
            }
        }
        std::string body;
        DWORD status = 0;
        if (std::isnan(lat)) {
            if (!city.empty()) {
                if (Net::Get(L"https://geocoding-api.open-meteo.com/v1/search?count=1&language=tr&name=" + Net::Encode(city),
                             body, 64 * 1024, &status) && status == 200) {
                    size_t r = body.find("\"results\"");
                    if (r != std::string::npos) {
                        lat = Number(body, "latitude", r);
                        lon = Number(body, "longitude", r);
                        name = Net::Widen(String(body, "name", r));
                    }
                }
            } else if (Net::Get(L"https://ipwho.is/?fields=success,city,latitude,longitude", body, 64 * 1024, &status) &&
                       status == 200) {
                lat = Number(body, "latitude", 0);
                lon = Number(body, "longitude", 0);
                name = Net::Widen(String(body, "city", 0));
            }
            if (!std::isnan(lat) && !std::isnan(lon)) {
                std::lock_guard<std::mutex> lock(g_mx);
                g_place = city;
                g_lat = lat;
                g_lon = lon;
                g_city = name;
            }
        }
        if (!std::isnan(lat) && !std::isnan(lon)) {
            wchar_t url[256];
            swprintf_s(url,
                       L"https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
                       L"&current=temperature_2m,weather_code,is_day&timezone=auto",
                       lat, lon);
            if (Net::Get(url, body, 64 * 1024, &status) && status == 200) {
                size_t cur = body.find("\"current\":");
                if (cur != std::string::npos) {
                    double t = Number(body, "temperature_2m", cur);
                    double c = Number(body, "weather_code", cur);
                    double d = Number(body, "is_day", cur);
                    if (!std::isnan(t) && !std::isnan(c)) {
                        info->ok = true;
                        info->temp = t;
                        info->code = static_cast<int>(c);
                        info->day = std::isnan(d) || d >= 0.5;
                        info->city = name;
                    }
                }
            }
        }
        if (!PostMessageW(notify, WM_WEATHER, 0, reinterpret_cast<LPARAM>(info))) delete info;
    }).detach();
}

}  // namespace Weather
