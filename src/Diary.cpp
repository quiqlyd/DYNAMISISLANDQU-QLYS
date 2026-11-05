// 3. parti: Guti'nin günlüğü ("bugün beraber neler yaptık") ve şarkıyı kalple.
// Her şey yerelde, %LOCALAPPDATA%\DynamisIsland\ altında küçük metin dosyalarında durur.
#include "Island.h"
#include "IslandUi.h"
#include "Net.h"
#include "Sounds.h"

#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

using namespace isl;

namespace {

std::wstring TodayKey() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::vector<std::wstring> Lines(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(i, e - i);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (!line.empty()) out.push_back(line);
        i = e + 1;
    }
    return out;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return out;
}

// Bir günün "ruh hali" puanı 0..1: odak, şarkı ve sevgiden.
float Score(const Island* /*unused*/, int focusSec, int songs, int pets) {
    float v = focusSec / 3600.f + songs / 20.f + pets / 10.f;
    return std::clamp(v / 1.5f, 0.f, 1.f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Günlük
// ---------------------------------------------------------------------------

void Island::LoadDiary() {
    history_.clear();
    std::string data;
    Net::ReadFile(Net::DataDir() + L"gunluk.txt", data, 256 * 1024);
    std::wstring today = TodayKey();
    for (const auto& line : Lines(Net::Widen(data))) {
        auto f = Split(line, L'\t');
        if (f.size() < 4) continue;
        DayStats d;
        d.date = f[0];
        d.focusSec = _wtoi(f[1].c_str());
        d.songs = _wtoi(f[2].c_str());
        d.pets = _wtoi(f[3].c_str());
        if (f.size() > 4) d.topSong = f[4];
        if (d.date == today) today_ = d;
        else history_.push_back(d);
    }
    if (history_.size() > 7) history_.erase(history_.begin(), history_.end() - 7);
    if (today_.date.empty()) today_.date = today;
    diaryShownDay_ = static_cast<int>(ReadSetting(L"DiaryDay", 0));
    LoadLikes();
}

void Island::SaveDiary() {
    std::wstring text;
    auto add = [&](const DayStats& d) {
        wchar_t buf[96];
        swprintf_s(buf, L"%s\t%d\t%d\t%d\t", d.date.c_str(), d.focusSec, d.songs, d.pets);
        std::wstring top = d.topSong;
        for (auto& c : top)
            if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        text += buf + top + L"\r\n";
    };
    for (const auto& d : history_) add(d);
    add(today_);
    Net::WriteFile(Net::DataDir() + L"gunluk.txt", Net::Narrow(text));
}

void Island::RollDay() {
    std::wstring today = TodayKey();
    if (today_.date == today) return;
    if (!today_.date.empty()) {
        history_.push_back(today_);
        if (history_.size() > 7) history_.erase(history_.begin(), history_.end() - 7);
    }
    today_ = DayStats{};
    today_.date = today;
    listen_.clear();
    diarySong_.clear();
    SaveDiary();
}

void Island::OnDiaryTick() {
    RollDay();
    if (companion_.Kind() == Companion::TimerKind::Focus && companion_.TimerActive()) ++today_.focusSec;
    if (snapshot_.playing && !snapshot_.title.empty()) {
        std::wstring key = snapshot_.title;
        if (!snapshot_.artist.empty()) key += L" — " + snapshot_.artist;
        if (key != diarySong_) {
            diarySong_ = key;
            ++today_.songs;
        }
        auto it = std::find_if(listen_.begin(), listen_.end(), [&](const auto& p) { return p.first == key; });
        if (it == listen_.end()) {
            if (listen_.size() < 300) listen_.push_back({key, 1});
        } else {
            ++it->second;
            int best = 0;
            for (const auto& p : listen_) best = std::max(best, p.second);
            if (it->second >= best) today_.topSong = key;
        }
        if (today_.topSong.empty()) today_.topSong = key;
    }
    if (++diarySaveTick_ % 60 == 0) SaveDiary();

    // Akşam 21'den sonra, gün boyunca bir şeyler yaptıysak: günlük kendiliğinden bir kez açılır.
    SYSTEMTIME st;
    GetLocalTime(&st);
    int dayNo = st.wYear * 400 + st.wMonth * 32 + st.wDay;
    bool busy = today_.focusSec >= 300 || today_.songs >= 3 || today_.pets >= 3;
    if (st.wHour >= 21 && diaryShownDay_ != dayNo && busy && !fullscreen_ && !userHidden_ && !hover_ &&
        !alertActive_ && !Typing() && (view_ == View::Idle || view_ == View::Media)) {
        diaryShownDay_ = dayNo;
        WriteSetting(L"DiaryDay", static_cast<DWORD>(dayNo));
        OpenDiary(true);
    }
    if (diaryOpen_ && diaryUntil_ > 0 && Now() > diaryUntil_ && !hover_) {
        diaryOpen_ = false;
        Update();
    }
}

void Island::OpenDiary(bool automatic) {
    RollDay();
    diaryOpen_ = true;
    diaryUntil_ = automatic ? Now() + 9.0 : 0;
    if (automatic && userHidden_) return;
    eyes_.SetMood(Eyes::Mood::Happy, Now(), 1.6);
    Sounds::Play(Sounds::Sound::Open);
    lastActivity_ = Now();
    Update();
}

void Island::DrawDiary(float w, float h) {
    (void)h;
    float s0 = Stagger(0), s1 = Stagger(1), s2 = Stagger(2), s3 = Stagger(3);
    SYSTEMTIME st;
    GetLocalTime(&st);
    PushFade(s0, 0, 6.f * (1 - s0));
    Text(L"Bugün", fAlert_.Get(), D2D1::RectF(22, 12, w - 90, 32), kWhite);
    wchar_t date[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"d MMMM dddd", date, 64, nullptr);
    Text(date, fSmall_.Get(), D2D1::RectF(22, 32, w - 90, 46), kGray);
    PopFade();
    PushFade(s1, 0, 0, 0.85f + 0.15f * s1, w - 46, 28);
    DrawGuti(w - 46, 28, 0.95f, 0.f);
    PopFade();

    // Üç sayı: odak dakikası, şarkı, sevgi
    PushFade(s2, 0, 6.f * (1 - s2));
    struct Stat {
        int value;
        const wchar_t* label;
    } stats[3] = {{today_.focusSec / 60, L"dk odak"}, {today_.songs, L"şarkı"}, {today_.pets, L"sevgi"}};
    float bw = (w - 44 - 16) / 3.f;
    for (int i = 0; i < 3; ++i) {
        float x = 22 + i * (bw + 8);
        brush_->SetColor(Rgba(1, 1, 1, 0.07f));
        rt_->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, 54, x + bw, 104), 14, 14}, brush_.Get());
        wchar_t num[16];
        swprintf_s(num, L"%d", stats[i].value);
        if (fHomeTimer_) Text(num, fHomeTimer_.Get(), D2D1::RectF(x, 56, x + bw, 86), i == 2 ? Rgba(1.f, 0.55f, 0.7f) : kWhite);
        Text(stats[i].label, fSmallCenter_.Get(), D2D1::RectF(x, 84, x + bw, 100), kGray);
    }
    PopFade();

    PushFade(s3, 0, 6.f * (1 - s3));
    std::wstring top = today_.topSong.empty() ? std::wstring(L"Bugün sessiz bir gündü") : L"♪ " + today_.topSong;
    Text(top.c_str(), fDate_.Get(), D2D1::RectF(22, 110, w - 22, 128), kGray);
    // Son 7 günün ruh hali noktaları (sağdaki bugün)
    std::vector<const DayStats*> days;
    for (const auto& d : history_) days.push_back(&d);
    days.push_back(&today_);
    if (days.size() > 7) days.erase(days.begin(), days.end() - 7);
    float cy = 148, gap = 22;
    float x0 = w / 2 - (days.size() - 1) * gap / 2;
    for (size_t i = 0; i < days.size(); ++i) {
        float sc = Score(this, days[i]->focusSec, days[i]->songs, days[i]->pets);
        float r = 2.5f + 4.5f * sc;
        bool isToday = i + 1 == days.size();
        D2D1_COLOR_F c = isToday ? accent_ : Rgba(1, 1, 1, 0.35f + 0.5f * sc);
        brush_->SetColor(c);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + i * gap, cy), r, r), brush_.Get());
    }
    PopFade();
}

// ---------------------------------------------------------------------------
// Beğenilen şarkılar
// ---------------------------------------------------------------------------

void Island::LoadLikes() {
    likes_.clear();
    std::string data;
    if (Net::ReadFile(Net::DataDir() + L"begeniler.txt", data, 512 * 1024)) likes_ = Lines(Net::Widen(data));
}

bool Island::IsLiked() const {
    if (snapshot_.title.empty()) return false;
    std::wstring key = snapshot_.artist.empty() ? snapshot_.title : snapshot_.artist + L" — " + snapshot_.title;
    return std::find(likes_.begin(), likes_.end(), key) != likes_.end();
}

void Island::ToggleLike() {
    if (snapshot_.title.empty()) return;
    std::wstring key = snapshot_.artist.empty() ? snapshot_.title : snapshot_.artist + L" — " + snapshot_.title;
    auto it = std::find(likes_.begin(), likes_.end(), key);
    if (it != likes_.end()) {
        likes_.erase(it);
        eyes_.SetMood(Eyes::Mood::Wink, Now(), 0.6);
        Sounds::Play(Sounds::Sound::Close);
    } else {
        likes_.push_back(key);
        MoodWithSound(Eyes::Mood::Love, 1.6);
        heartPop_.velocity += 14.f;  // kalp "pıt" diye büyüyüp oturur
    }
    std::wstring text;
    for (const auto& l : likes_) text += l + L"\r\n";
    Net::WriteFile(Net::DataDir() + L"begeniler.txt", Net::Narrow(text));
    lastActivity_ = Now();
    StartAnimation();
}
