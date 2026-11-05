#pragma once
// Island.cpp ve Tools.cpp'nin ortak çizim sabitleri ve küçük yardımcıları.
#include <windows.h>
#include <d2d1.h>
#include <algorithm>

namespace isl {

inline constexpr wchar_t kSettingsKey[] = L"Software\\DynamisIsland";

constexpr float kTopMargin = 4.f;
// Gölge için hapın çevresinde bırakılan boşluk (yanlar ve alt).
constexpr float kPad = 16.f, kPadBottom = 20.f;
constexpr float kTransition = 0.38f;  // görünüm geçişi süresi (sn)

inline float EaseOut(float t) {
    t = std::clamp(t, 0.f, 1.f);
    float u = 1 - t;
    return 1 - u * u * u;
}
constexpr float kMaxW = 420.f, kMaxH = 300.f;

// Görünüm boyutları (DIP)
struct Size3 { float w, h, r; };
constexpr Size3 kIdle{78, 22, 11};
constexpr Size3 kMedia{236, 34, 17};
constexpr Size3 kExpandedMedia{400, 164, 40};
constexpr Size3 kExpandedIdle{380, 96, 36};
constexpr Size3 kAlert{300, 40, 20};
constexpr Size3 kTimer{128, 24, 12};
constexpr Size3 kDownload{340, 46, 23};
constexpr Size3 kShot{360, 92, 28};
constexpr Size3 kDrop{260, 46, 23};
constexpr Size3 kPicker{220, 40, 20};
constexpr float kShelfH = 58.f, kShelfCell = 58.f;
constexpr int kShelfMax = 8, kShelfShown = 5;
constexpr float kCommandW = 420.f, kCommandRow = 26.f, kCommandBar = 48.f;
// Araç çentiği: açık adanın altında sayfa sayfa araç kutucukları
constexpr float kToolsH = 74.f, kTile = 50.f, kTileGap = 8.f;
constexpr int kToolCols = 6;
constexpr Size3 kNote{380, 160, 28};
constexpr Size3 kInk{300, 44, 22};
constexpr Size3 kQuick{380, 196, 36};

// Segoe Fluent Icons
constexpr wchar_t kGlyphPrev[] = L"\xE892";
constexpr wchar_t kGlyphNext[] = L"\xE893";
constexpr wchar_t kGlyphPlay[] = L"\xE768";
constexpr wchar_t kGlyphPause[] = L"\xE769";
constexpr wchar_t kGlyphMusic[] = L"\xE8D6";
constexpr wchar_t kGlyphBolt[] = L"\xE945";
constexpr wchar_t kGlyphSync[] = L"\xE895";
constexpr wchar_t kGlyphCheck[] = L"\xE73E";
constexpr wchar_t kGlyphError[] = L"\xE783";
constexpr wchar_t kGlyphTimer[] = L"\xE916";
constexpr wchar_t kGlyphDownload[] = L"\xE896";
constexpr wchar_t kGlyphWarning[] = L"\xE7BA";
constexpr wchar_t kGlyphGlobe[] = L"\xE774";
constexpr wchar_t kGlyphClose[] = L"\xE711";
constexpr wchar_t kGlyphVolume[] = L"\xE767";
constexpr wchar_t kGlyphMute[] = L"\xE74F";
constexpr wchar_t kGlyphHeadphone[] = L"\xE7F6";
constexpr wchar_t kGlyphCamera[] = L"\xE722";
constexpr wchar_t kGlyphColor[] = L"\xE790";
constexpr wchar_t kGlyphCafe[] = L"\xEC32";
constexpr wchar_t kGlyphNote[] = L"\xE70B";
constexpr wchar_t kGlyphPen[] = L"\xE70F";
constexpr wchar_t kGlyphSettings[] = L"\xE713";
constexpr wchar_t kGlyphCalc[] = L"\xE8EF";
constexpr wchar_t kGlyphWifi[] = L"\xE701";
constexpr wchar_t kGlyphBluetooth[] = L"\xE702";
constexpr wchar_t kGlyphBrightness[] = L"\xE706";
constexpr wchar_t kGlyphMoon[] = L"\xE708";
constexpr wchar_t kGlyphBack[] = L"\xE72B";
constexpr wchar_t kGlyphDelete[] = L"\xE74D";
constexpr wchar_t kGlyphUndo[] = L"\xE7A7";
constexpr wchar_t kGlyphOpen[] = L"\xE8A7";

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"DynamisIsland";

inline D2D1_COLOR_F Rgba(float r, float g, float b, float a = 1.f) { return D2D1::ColorF(r, g, b, a); }
inline D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

inline const D2D1_COLOR_F kWhite = Rgba(1, 1, 1);
inline const D2D1_COLOR_F kGray = Rgba(1, 1, 1, 0.55f);
inline const D2D1_COLOR_F kGreen = Rgba(0.30f, 0.85f, 0.42f);
inline const D2D1_COLOR_F kRed = Rgba(1.f, 0.30f, 0.27f);
inline const D2D1_COLOR_F kOrange = Rgba(1.f, 0.62f, 0.25f);

inline DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

inline void WriteSetting(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

}  // namespace isl
