#include "Lyrics.h"
#include "Net.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <thread>

namespace Lyrics {
namespace {

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    auto junk = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"' || c == 0x00A0; };
    while (a < b && junk(s[a])) ++a;
    while (b > a && (junk(s[b - 1]) || s[b - 1] == L'-' || s[b - 1] == L'|')) --b;
    return s.substr(a, b - a);
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && Lower(s.substr(0, prefix.size())) == Lower(prefix);
}

// ---- minik JSON okuyucu: sadece lrclib cevabındaki birkaç alan için ----

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

unsigned Hex4(const std::string& j, size_t p) {
    unsigned v = 0;
    for (size_t i = p; i < p + 4 && i < j.size(); ++i) {
        char c = j[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
    }
    return v;
}

// "key": sonrasındaki değerin başlangıcı; yoksa npos.
size_t ValueAt(const std::string& j, const char* key, size_t from, size_t until = std::string::npos) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = j.find(needle, from);
    if (k == std::string::npos || (until != std::string::npos && k >= until)) return std::string::npos;
    size_t p = j.find(':', k + needle.size());
    if (p == std::string::npos) return p;
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) ++p;
    return p;
}

// p bir tırnakta başlıyorsa metni çözer; null ise false.
bool StringAt(const std::string& j, size_t p, std::string& out) {
    out.clear();
    if (p >= j.size() || j[p] != '"') return false;
    for (size_t i = p + 1; i < j.size(); ++i) {
        char c = j[i];
        if (c == '"') return true;
        if (c != '\\' || i + 1 >= j.size()) {
            out += c;
            continue;
        }
        char e = j[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = Hex4(j, i + 1);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < j.size() && j[i + 1] == '\\' && j[i + 2] == 'u') {
                unsigned lo = Hex4(j, i + 3);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            AppendUtf8(out, cp);
            break;
        }
        default: out += e; break;
        }
    }
    return false;
}

double NumberAt(const std::string& j, size_t p) {
    if (p == std::string::npos || p >= j.size()) return -1;
    return atof(j.c_str() + p);
}

bool TrueAt(const std::string& j, size_t p) { return p != std::string::npos && j.compare(p, 4, "true") == 0; }

std::wstring Sanitize(std::wstring name) {
    for (auto& c : name)
        if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
    if (name.size() > 120) name.resize(120);
    return name;
}

std::wstring HashName(const std::wstring& key) {
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t c : key) {
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ull;
    }
    wchar_t buf[24];
    swprintf_s(buf, L"%016llx", h);
    return buf;
}

struct Found {
    std::string synced;
    bool instrumental = false;
    bool reached = false;  // sunucu cevap verdi (yoksa önbelleğe "yok" yazılmaz)
};

std::wstring Query(const std::wstring& artist, const std::wstring& title) {
    std::wstring q = L"track_name=" + Net::Encode(title);
    if (!artist.empty()) q += L"&artist_name=" + Net::Encode(artist);
    return q;
}

void TryGet(Found& f, const std::wstring& artist, const std::wstring& title, double dur) {
    if (!f.synced.empty() || artist.empty() || title.empty()) return;
    std::wstring url = L"https://lrclib.net/api/get?" + Query(artist, title);
    if (dur > 1) url += L"&duration=" + std::to_wstring(static_cast<int>(std::lround(dur)));
    std::string body;
    DWORD status = 0;
    if (!Net::Get(url, body, 512 * 1024, &status) || status == 0) return;
    f.reached = true;
    if (status != 200) return;
    StringAt(body, ValueAt(body, "syncedLyrics", 0), f.synced);
    if (TrueAt(body, ValueAt(body, "instrumental", 0))) f.instrumental = true;
}

void TrySearch(Found& f, const std::wstring& query, double dur) {
    if (!f.synced.empty()) return;
    std::string body;
    DWORD status = 0;
    if (!Net::Get(L"https://lrclib.net/api/search?" + query, body, 2 * 1024 * 1024, &status) || status == 0) return;
    f.reached = true;
    if (status != 200) return;
    // Süresi en yakın, senkron sözü olan kayıt.
    double best = 1e9;
    std::string pick;
    size_t pos = 0;
    for (;;) {
        size_t k = ValueAt(body, "syncedLyrics", pos);
        if (k == std::string::npos) break;
        pos = k;
        std::string s;
        if (!StringAt(body, k, s) || s.empty()) continue;
        size_t start = body.rfind('{', k);
        if (start == std::string::npos) start = 0;
        double d = NumberAt(body, ValueAt(body, "duration", start, k));
        double diff = (dur > 1 && d > 0) ? std::fabs(d - dur) : 5.0;
        if (diff < best) {
            best = diff;
            pick = std::move(s);
        }
    }
    if (!pick.empty() && best <= 10.0) f.synced = std::move(pick);
}

void Post(HWND notify, Result* r) {
    if (!PostMessageW(notify, WM_LYRICS, 0, reinterpret_cast<LPARAM>(r))) delete r;
}

}  // namespace

std::vector<Line> ParseLrc(const std::wstring& lrc) {
    std::vector<Line> out;
    double offset = 0;
    size_t i = 0;
    while (i < lrc.size()) {
        size_t e = lrc.find(L'\n', i);
        if (e == std::wstring::npos) e = lrc.size();
        std::wstring line = lrc.substr(i, e - i);
        i = e + 1;
        std::vector<double> times;
        size_t p = 0;
        while (p < line.size() && line[p] == L'[') {
            size_t close = line.find(L']', p);
            if (close == std::wstring::npos) break;
            std::wstring tag = line.substr(p + 1, close - p - 1);
            p = close + 1;
            if (!tag.empty() && iswdigit(tag[0])) {
                int m = 0;
                double s = 0;
                if (swscanf_s(tag.c_str(), L"%d:%lf", &m, &s) == 2) times.push_back(m * 60.0 + s);
            } else if (StartsWithI(tag, L"offset:")) {
                offset = _wtof(tag.c_str() + 7) / 1000.0;
            }
        }
        if (times.empty()) continue;
        std::wstring text = line.substr(p);
        // Kelime kelime zamanlı biçim: <00:12.34> etiketlerini at.
        for (size_t a; (a = text.find(L'<')) != std::wstring::npos;) {
            size_t b = text.find(L'>', a);
            if (b == std::wstring::npos || b - a > 12) break;
            text.erase(a, b - a + 1);
        }
        text = Trim(text);
        for (double t : times) out.push_back({t, text});
    }
    for (auto& l : out) l.t = std::max(0.0, l.t - offset);
    std::stable_sort(out.begin(), out.end(), [](const Line& a, const Line& b) { return a.t < b.t; });
    return out;
}

void CleanTitle(std::wstring& artist, std::wstring& title) {
    artist = Trim(artist);
    for (const wchar_t* tail : {L" - Topic", L"VEVO", L" Official"}) {
        size_t n = wcslen(tail);
        if (artist.size() > n && Lower(artist.substr(artist.size() - n)) == Lower(tail)) artist.resize(artist.size() - n);
    }
    artist = Trim(artist);

    std::wstring t = title;
    for (const wchar_t* cut : {L" | ", L" // "}) {
        size_t c = t.find(cut);
        if (c != std::wstring::npos && c > 0) t.resize(c);
    }
    // Parantez içindeki "resmi video", "sözleri", "4K" gibi ekler.
    static const wchar_t* kNoise[] = {L"official", L"video", L"lyric", L"audio", L"klip", L"hd", L"4k",
                                      L"visual", L"mv", L"m/v", L"remaster", L"söz", L"feat", L"ft.",
                                      L"prod", L"live", L"canlı", L"resmi", L"music"};
    for (wchar_t open : {L'(', L'[', static_cast<wchar_t>(0x3010)}) {
        wchar_t close = open == L'(' ? L')' : (open == L'[' ? L']' : static_cast<wchar_t>(0x3011));
        size_t a = 0;
        while ((a = t.find(open, a)) != std::wstring::npos) {
            size_t b = t.find(close, a);
            if (b == std::wstring::npos) break;
            std::wstring inner = Lower(t.substr(a, b - a + 1));
            bool noise = false;
            for (const wchar_t* n : kNoise) noise |= inner.find(n) != std::wstring::npos;
            if (noise) t.erase(a, b - a + 1);
            else a = b + 1;
        }
    }
    {
        std::wstring low = Lower(t);
        for (const wchar_t* ft : {L" ft. ", L" feat. ", L" ft ", L" feat "}) {
            size_t c = low.find(ft);
            if (c != std::wstring::npos && c > 0) {
                t.resize(c);
                low.resize(c);
            }
        }
    }
    t = Trim(t);
    if (!artist.empty() && StartsWithI(t, artist + L" - ")) {
        t = Trim(t.substr(artist.size() + 3));
    } else if (artist.empty()) {
        size_t d = t.find(L" - ");
        if (d != std::wstring::npos && d > 0) {
            artist = Trim(t.substr(0, d));
            t = Trim(t.substr(d + 3));
        }
    }
    // Sanatçıda da "ft." kalmasın.
    {
        std::wstring low = Lower(artist);
        for (const wchar_t* ft : {L" ft. ", L" feat. ", L" ft ", L" feat "}) {
            size_t c = low.find(ft);
            if (c != std::wstring::npos && c > 0) {
                artist.resize(c);
                low.resize(c);
            }
        }
    }
    title = t.empty() ? Trim(title) : t;
}

void Fetch(HWND notify, uint64_t token, std::wstring artist, std::wstring title, double durationSec) {
    std::thread([=]() mutable {
        auto* r = new Result;
        r->token = token;
        CleanTitle(artist, title);
        if (title.empty()) {
            Post(notify, r);
            return;
        }
        auto finish = [&](const std::string& lrc) {
            if (lrc.rfind("#instrumental", 0) == 0) r->instrumental = true;
            else r->lines = ParseLrc(Net::Widen(lrc));
            Post(notify, r);
        };

        // 1) Kullanıcının kendi .lrc dosyaları
        std::string data;
        std::wstring own = Net::DataDir(L"sozler");
        if (Net::ReadFile(own + Sanitize(artist + L" - " + title) + L".lrc", data, 1 << 20) && !data.empty()) {
            finish(data);
            return;
        }
        if (Net::ReadFile(own + Sanitize(title) + L".lrc", data, 1 << 20) && !data.empty()) {
            finish(data);
            return;
        }

        // 2) Önbellek. Boş dosya "bulunamadı" demektir: 3 gün sonra yeniden sorulur.
        std::wstring cache = Net::DataDir(L"lyrics") + HashName(Lower(artist) + L"|" + Lower(title)) + L".lrc";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(cache.c_str(), GetFileExInfoStandard, &fa)) {
            bool empty = fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            auto q = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            bool fresh = q(now) - q(fa.ftLastWriteTime) < 3ull * 24 * 3600 * 10000000ull;
            if (!empty && Net::ReadFile(cache, data, 1 << 20)) {
                finish(data);
                return;
            }
            if (empty && fresh) {
                Post(notify, r);
                return;
            }
        }

        // 3) lrclib.net
        Found f;
        TryGet(f, artist, title, durationSec);
        TrySearch(f, Query(artist, title), durationSec);
        size_t dash = title.find(L" - ");
        if (f.synced.empty() && dash != std::wstring::npos && dash > 0) {
            // "Sanatçı - Şarkı" biçimli video başlığı (kanal adı sanatçı değil)
            std::wstring a2 = Trim(title.substr(0, dash)), t2 = Trim(title.substr(dash + 3));
            TryGet(f, a2, t2, durationSec);
            TrySearch(f, Query(a2, t2), durationSec);
        }
        if (f.synced.empty() && !f.instrumental)
            TrySearch(f, L"q=" + Net::Encode(artist.empty() ? title : artist + L" " + title), durationSec);

        if (!f.synced.empty()) Net::WriteFile(cache, f.synced);
        else if (f.instrumental) Net::WriteFile(cache, "#instrumental");
        else if (f.reached) Net::WriteFile(cache, "");
        if (!f.synced.empty()) {
            finish(f.synced);
            return;
        }
        r->instrumental = f.instrumental;
        Post(notify, r);
    }).detach();
}

}  // namespace Lyrics
