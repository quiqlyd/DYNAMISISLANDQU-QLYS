#include "Companion.h"

#include <algorithm>
#include <cwctype>
#include <string>

namespace {
constexpr wchar_t kKey[] = L"Software\\DynamisIsland";

DWORD ReadDword(const wchar_t* name, DWORD fallback) {
    DWORD v = fallback, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return fallback;
    return v;
}

void WriteDword(const wchar_t* name, DWORD v) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
        RegCloseKey(key);
    }
}

DWORD TodayNumber() {
    // 1601'den beri gün sayısı: tarih karşılaştırması için yeterli.
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<DWORD>(u.QuadPart / (10000000ull * 86400ull));
}

double Seconds() { return static_cast<double>(GetTickCount64()) / 1000.0; }

// Odak sırasında dikkat dağıtan şeyler: pencere başlığında geçerse Guti kaşlarını çatar.
const wchar_t* kDistractions[] = {L"youtube", L"netflix", L"twitch", L"tiktok", L"instagram",
                                  L"reddit", L"facebook", L"twitter", L"disney+", L"prime video",
                                  L"exxen", L"blutv", L"9gag", L"kick.com"};
const wchar_t* kGameProcesses[] = {L"steam.exe", L"epicgameslauncher.exe", L"riotclientservices.exe",
                                   L"leagueclient.exe", L"valorant.exe", L"cs2.exe",
                                   L"robloxplayerbeta.exe", L"minecraft.exe", L"javaw.exe",
                                   L"fortniteclient-win64-shipping.exe"};
}  // namespace

void Companion::Load() {
    affection_ = static_cast<int>(ReadDword(L"Affection", 50));
    DWORD lastSeen = ReadDword(L"LastSeen", 0);
    DWORD today = TodayNumber();
    daysAway_ = lastSeen ? static_cast<int>(today - lastSeen) : 0;
    if (daysAway_ >= 2) affection_ = std::max(0, affection_ - 5 * (daysAway_ - 1));  // özledi, biraz küstü
    affection_ = std::clamp(affection_, 0, 100);
    workStartTick_ = Seconds();
    Save();
}

void Companion::Save() {
    WriteDword(L"Affection", static_cast<DWORD>(affection_));
    WriteDword(L"LastSeen", TodayNumber());
}

void Companion::OnPet(double now) {
    if (now - lastPetGain_ < 3.0) return;  // tık yağmuru puanı şişirmesin
    lastPetGain_ = now;
    affection_ = std::min(100, affection_ + 1);
}

void Companion::AdjustPending(int delta, double now) {
    if (TimerActive() && !Pending()) pendingMinutes_ = static_cast<int>((Remaining(now) + 59) / 60);
    pendingMinutes_ = std::clamp(pendingMinutes_ + delta, 0, 180);
    pendingSince_ = now;
}

void Companion::StartTimer(TimerKind kind, int minutes, double now) {
    kind_ = kind;
    total_ = minutes * 60.0;
    endsAt_ = now + total_;
    pendingMinutes_ = 0;
    pendingSince_ = 0;
}

void Companion::CancelTimer() {
    kind_ = TimerKind::None;
    pendingMinutes_ = 0;
    pendingSince_ = 0;
}

double Companion::Remaining(double now) const {
    return kind_ == TimerKind::None ? 0 : std::max(0.0, endsAt_ - now);
}

bool Companion::IsDistracting() const {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t title[512] = {};
    GetWindowTextW(fg, title, 512);
    std::wstring t = title;
    for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c));
    for (const wchar_t* d : kDistractions)
        if (t.find(d) != std::wstring::npos) return true;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool game = false;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        std::wstring p(path, len);
        for (auto& c : p) c = static_cast<wchar_t>(std::towlower(c));
        size_t slash = p.find_last_of(L'\\');
        std::wstring exe = slash == std::wstring::npos ? p : p.substr(slash + 1);
        for (const wchar_t* g : kGameProcesses)
            if (exe == g) game = true;
    }
    CloseHandle(proc);
    return game;
}

Companion::Event Companion::Tick(double now) {
    // Teker bırakıldıktan 1.2 sn sonra zamanlayıcı başlar (0 dk = iptal).
    if (pendingSince_ > 0 && now - pendingSince_ > 1.2) {
        int m = pendingMinutes_;
        pendingSince_ = 0;
        if (m <= 0) {
            CancelTimer();
            return Event::None;
        }
        bool wasFocus = kind_ == TimerKind::Focus;
        StartTimer(wasFocus ? TimerKind::Focus : TimerKind::Quick, m, now);
        return Event::TimerStarted;
    }

    if (TimerActive() && !Pending() && now >= endsAt_) {
        TimerKind k = kind_;
        kind_ = TimerKind::None;
        if (k == TimerKind::Focus) {
            StartTimer(TimerKind::Break, kBreakMinutes, now);  // odaktan sonra otomatik mola
            workStartTick_ = Seconds();
            return Event::FocusDone;
        }
        if (k == TimerKind::Break) return Event::BreakDone;
        return Event::TimerDone;
    }

    // Odak modunda dikkat dağıtıcı kontrolü: 2 sn'de bir, en fazla 20 sn'de bir uyarı.
    if (kind_ == TimerKind::Focus && now - lastDistractCheck_ >= 2.0) {
        lastDistractCheck_ = now;
        if (now - lastDistracted_ > 20.0 && IsDistracting()) {
            lastDistracted_ = now;
            return Event::Distracted;
        }
    }

    // Mola arkadaşı: 5 dk boşta kalırsan sayaç sıfırlanır; 60 dk aralıksız çalışınca esner.
    LASTINPUTINFO li{sizeof(li)};
    if (GetLastInputInfo(&li)) {
        double idleSec = (GetTickCount() - li.dwTime) / 1000.0;
        double nowTick = Seconds();
        if (idleSec > 5 * 60) workStartTick_ = nowTick;
        if (kind_ != TimerKind::Focus && kind_ != TimerKind::Break && idleSec < 60 &&
            nowTick - workStartTick_ > 60 * 60 && nowTick - lastBreakNudge_ > 20 * 60) {
            lastBreakNudge_ = nowTick;
            workStartTick_ = nowTick;
            return Event::NeedBreak;
        }
    }

    if (now - lastSave_ > 300) {
        lastSave_ = now;
        Save();
    }
    return Event::None;
}
