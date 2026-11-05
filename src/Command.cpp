#include "Command.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace Command {
namespace {

std::wstring Lower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = c == L'İ' ? L'i' : static_cast<wchar_t>(std::towlower(c));
    return r;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------
// Sayılar
// ---------------------------------------------------------------------------

// "12", "3,5", "3.5", "1.000.000" -> değer. Tamamı sayı değilse false.
bool ParseNumber(const std::wstring& s, double& out) {
    if (s.empty()) return false;
    std::string t;
    int dots = 0, commas = 0, digits = 0;
    for (wchar_t c : s) {
        if (c >= L'0' && c <= L'9') { ++digits; continue; }
        if (c == L'.') ++dots;
        else if (c == L',') ++commas;
        else return false;
    }
    if (!digits || commas > 1 || (commas && dots > 1)) return false;
    for (wchar_t c : s) {
        if (c >= L'0' && c <= L'9') t += static_cast<char>(c);
        else if (c == L',') t += '.';
        else if (c == L'.' && dots == 1 && !commas) t += '.';  // tek nokta: ondalık
        // birden çok nokta ya da virgülle birlikte nokta: binlik ayırıcı, atla
    }
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == 0;
}

// ---------------------------------------------------------------------------
// Hesap makinesi: + - * / ^ % ( ) ve birkaç fonksiyon
// ---------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::wstring& s) : s_(Lower(s)) {}

    bool Run(double& out) {
        out = Expr();
        Skip();
        return ok_ && pos_ == s_.size();
    }
    bool divZero = false;
    bool sawOperator = false;

private:
    void Skip() { while (pos_ < s_.size() && std::iswspace(s_[pos_])) ++pos_; }
    wchar_t Peek() { Skip(); return pos_ < s_.size() ? s_[pos_] : 0; }
    bool Eat(wchar_t c) { if (Peek() == c) { ++pos_; return true; } return false; }

    double Expr() {
        double v = Term();
        for (;;) {
            if (Eat(L'+')) { sawOperator = true; v += Term(); }
            else if (Eat(L'-') || Eat(L'−')) { sawOperator = true; v -= Term(); }
            else return v;
        }
    }
    double Term() {
        double v = Unary();
        for (;;) {
            wchar_t c = Peek();
            if (c == L'*' || c == L'x' || c == L'×' || c == L'·') {
                ++pos_; sawOperator = true; v *= Unary();
            } else if (c == L'/' || c == L'÷' || c == L':') {
                ++pos_; sawOperator = true;
                double d = Unary();
                if (d == 0) { divZero = true; ok_ = false; return 0; }
                v /= d;
            } else if (c == L'(') {
                sawOperator = true; v *= Unary();  // 2(3+4)
            } else {
                return v;
            }
        }
    }
    double Unary() {
        if (Eat(L'-') || Eat(L'−')) { sawOperator = true; return -Unary(); }
        if (Eat(L'+')) return Unary();
        return Power();
    }
    double Power() {
        double base = Postfix();
        if (Eat(L'^')) { sawOperator = true; return std::pow(base, Unary()); }
        return base;
    }
    double Postfix() {
        double v = Primary();
        for (;;) {
            if (Eat(L'%')) { sawOperator = true; v /= 100.0; }
            else if (Eat(L'²')) { sawOperator = true; v *= v; }
            else return v;
        }
    }
    double Primary() {
        wchar_t c = Peek();
        if (c == L'(') {
            ++pos_;
            double v = Expr();
            if (!Eat(L')')) ok_ = ok_ && pos_ >= s_.size();  // kapanmamış parantez: yazarken sorun etme
            return v;
        }
        if (c == L'π') { ++pos_; sawOperator = true; return 3.14159265358979323846; }
        if (c == L'√') { ++pos_; sawOperator = true; return Sqrt(Unary()); }
        if ((c >= L'0' && c <= L'9') || c == L'.' || c == L',') {
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   ((s_[pos_] >= L'0' && s_[pos_] <= L'9') || s_[pos_] == L'.' || s_[pos_] == L','))
                ++pos_;
            double v = 0;
            if (!ParseNumber(s_.substr(start, pos_ - start), v)) ok_ = false;
            return v;
        }
        if (std::iswalpha(c)) {
            size_t start = pos_;
            while (pos_ < s_.size() && std::iswalpha(s_[pos_]) && s_[pos_] != L'x') ++pos_;
            std::wstring name = s_.substr(start, pos_ - start);
            sawOperator = true;
            if (name == L"pi") return 3.14159265358979323846;
            if (name == L"e") return 2.71828182845904523536;
            double a = Unary();
            if (name == L"sqrt" || name == L"kök" || name == L"karekök") return Sqrt(a);
            if (name == L"sin") return std::sin(a * 3.14159265358979323846 / 180);  // derece
            if (name == L"cos") return std::cos(a * 3.14159265358979323846 / 180);
            if (name == L"tan") return std::tan(a * 3.14159265358979323846 / 180);
            if (name == L"log") return std::log10(a);
            if (name == L"ln") return std::log(a);
            if (name == L"abs") return std::fabs(a);
            ok_ = false;
            return 0;
        }
        ok_ = false;
        return 0;
    }
    double Sqrt(double a) {
        if (a < 0) ok_ = false;
        return std::sqrt(std::fabs(a));
    }

    std::wstring s_;
    size_t pos_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------------------
// Birimler
// ---------------------------------------------------------------------------

enum class Cat { Length, Mass, Volume, Temp, Speed, Data, Time, Area };
struct Unit {
    const wchar_t* names;  // boşlukla ayrılmış eş anlamlılar; ilki gösterilir
    Cat cat;
    double factor;  // temel birime çarpan (sıcaklık hariç)
};

const Unit kUnits[] = {
    {L"mm milimetre", Cat::Length, 0.001},
    {L"cm santim santimetre", Cat::Length, 0.01},
    {L"m metre", Cat::Length, 1},
    {L"km kilometre", Cat::Length, 1000},
    {L"inç inc inch in \" inches", Cat::Length, 0.0254},
    {L"ft feet fit foot", Cat::Length, 0.3048},
    {L"yd yard yarda", Cat::Length, 0.9144},
    {L"mil mile miles", Cat::Length, 1609.344},
    {L"mg miligram", Cat::Mass, 1e-6},
    {L"g gr gram", Cat::Mass, 0.001},
    {L"kg kilo kilogram", Cat::Mass, 1},
    {L"ton", Cat::Mass, 1000},
    {L"lb lbs pound libre", Cat::Mass, 0.45359237},
    {L"oz ons ounce", Cat::Mass, 0.028349523125},
    {L"ml mililitre", Cat::Volume, 0.001},
    {L"cl", Cat::Volume, 0.01},
    {L"l lt litre liter", Cat::Volume, 1},
    {L"gal galon gallon", Cat::Volume, 3.785411784},
    {L"°c c santigrat celsius derece", Cat::Temp, 0},
    {L"°f f fahrenheit", Cat::Temp, 1},
    {L"k kelvin", Cat::Temp, 2},
    {L"km/sa km/h kmh kmsa", Cat::Speed, 1 / 3.6},
    {L"m/s m/sn", Cat::Speed, 1},
    {L"mph", Cat::Speed, 0.44704},
    {L"knot kn deniz", Cat::Speed, 0.514444},
    {L"bayt b byte", Cat::Data, 1},
    {L"kb", Cat::Data, 1024.0},
    {L"mb", Cat::Data, 1024.0 * 1024},
    {L"gb", Cat::Data, 1024.0 * 1024 * 1024},
    {L"tb", Cat::Data, 1024.0 * 1024 * 1024 * 1024},
    {L"ms milisaniye", Cat::Time, 0.001},
    {L"sn saniye s sec", Cat::Time, 1},
    {L"dk dakika min d", Cat::Time, 60},
    {L"saat sa h", Cat::Time, 3600},
    {L"gün gun day", Cat::Time, 86400},
    {L"hafta week", Cat::Time, 604800},
    {L"yıl yil year", Cat::Time, 31557600},
    {L"m² m2 metrekare", Cat::Area, 1},
    {L"km² km2", Cat::Area, 1e6},
    {L"dönüm donum", Cat::Area, 1000},
    {L"hektar ha", Cat::Area, 10000},
    {L"ft² ft2", Cat::Area, 0.09290304},
};

const Unit* FindUnit(const std::wstring& lowered) {
    for (const Unit& u : kUnits) {
        const wchar_t* p = u.names;
        while (*p) {
            const wchar_t* e = p;
            while (*e && *e != L' ') ++e;
            if (lowered.size() == static_cast<size_t>(e - p) && lowered.compare(0, e - p, p, e - p) == 0)
                return &u;
            p = *e ? e + 1 : e;
        }
    }
    return nullptr;
}

std::wstring UnitLabel(const Unit& u) {
    const wchar_t* e = u.names;
    while (*e && *e != L' ') ++e;
    return std::wstring(u.names, e);
}

double ToKelvin(double v, int kind) {
    if (kind == 0) return v + 273.15;
    if (kind == 1) return (v - 32) * 5 / 9 + 273.15;
    return v;
}
double FromKelvin(double v, int kind) {
    if (kind == 0) return v - 273.15;
    if (kind == 1) return (v - 273.15) * 9 / 5 + 32;
    return v;
}

bool IsConnector(const std::wstring& t) {
    for (const wchar_t* c : {L"to", L"->", L"=", L"→", L"kaç", L"kac", L"ne", L"kadar", L"eder",
                             L"?", L"in"})
        if (t == c) return true;
    return false;
}

// Hatırlatma için zaman birimi (sadece sn/dk/saat).
double ReminderUnit(const std::wstring& lowered) {
    const Unit* u = FindUnit(lowered);
    if (!u || u->cat != Cat::Time || u->factor < 1 || u->factor > 3600) return 0;
    return u->factor;
}

struct Token {
    std::wstring text, low;
};

std::vector<Token> Tokenize(const std::wstring& s) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::iswspace(s[i])) ++i;
        size_t start = i;
        while (i < s.size() && !std::iswspace(s[i])) ++i;
        if (i > start) {
            std::wstring t = s.substr(start, i - start);
            // "10cm", "30dk" gibi yapışık sayı + birimi ayır.
            size_t k = 0;
            while (k < t.size() && ((t[k] >= L'0' && t[k] <= L'9') || t[k] == L'.' || t[k] == L','))
                ++k;
            if (k > 0 && k < t.size() && std::iswalpha(t[k]) && out.empty()) {
                out.push_back({t.substr(0, k), Lower(t.substr(0, k))});
                t = t.substr(k);
            }
            out.push_back({t, Lower(t)});
        }
    }
    return out;
}

std::wstring Join(const std::vector<Token>& t, size_t from, size_t to) {
    std::wstring r;
    for (size_t i = from; i < to && i < t.size(); ++i) {
        if (!r.empty()) r += L' ';
        r += t[i].text;
    }
    return r;
}

std::wstring Clock(double seconds) {
    int m = static_cast<int>(seconds / 60 + 0.5);
    wchar_t buf[48];
    if (m < 60) swprintf(buf, 48, L"%d dk sonra", m < 1 ? 1 : m);
    else if (m % 60 == 0) swprintf(buf, 48, L"%d saat sonra", m / 60);
    else swprintf(buf, 48, L"%d sa %d dk sonra", m / 60, m % 60);
    return buf;
}

bool ParseClock(const std::wstring& t, int& h, int& m) {
    size_t c = t.find(L':');
    if (c == std::wstring::npos || c == 0 || c > 2 || t.size() != c + 3) return false;
    for (size_t i = 0; i < t.size(); ++i)
        if (i != c && (t[i] < L'0' || t[i] > L'9')) return false;
    h = std::stoi(t.substr(0, c));
    m = std::stoi(t.substr(c + 1));
    return h < 24 && m < 60;
}

Result MakeReminder(double seconds, std::wstring text, const std::wstring& when) {
    Result r;
    r.kind = Result::Kind::Reminder;
    r.reminderSeconds = seconds;
    if (text.empty()) text = L"Hatırlatma";
    r.reminderText = text;
    r.display = L"⏰ " + when;
    return r;
}

}  // namespace

std::wstring FormatNumber(double v) {
    if (v == 0) v = 0;  // -0
    wchar_t buf[64];
    if (std::fabs(v) < 1e15 && std::fabs(v - std::round(v)) < 1e-9 * std::max(1.0, std::fabs(v)))
        swprintf(buf, 64, L"%.0f", std::round(v));
    else
        swprintf(buf, 64, L"%.10g", v);
    std::wstring s = buf;
    if (s.find(L'e') == std::wstring::npos && s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    for (auto& c : s) if (c == L'.') c = L',';
    return s;
}

Result Evaluate(const std::wstring& rawInput, int hour, int minute, int second) {
    Result r;
    std::wstring input = Trim(rawInput);
    if (input.empty()) return r;
    std::vector<Token> t = Tokenize(input);

    // 1) Saatli hatırlatma: "15:30 toplantı" ya da "toplantı 15:30"
    int h = 0, m = 0;
    bool clockFirst = ParseClock(t.front().low, h, m), clockLast = !clockFirst && t.size() > 1 &&
                                                                    ParseClock(t.back().low, h, m);
    if (clockFirst || clockLast) {
        double now = hour * 3600.0 + minute * 60.0 + second;
        double target = h * 3600.0 + m * 60.0;
        double diff = target - now;
        if (diff <= 0) diff += 86400;
        std::wstring text = clockFirst ? Join(t, 1, t.size()) : Join(t, 0, t.size() - 1);
        wchar_t when[32];
        swprintf(when, 32, L"%02d:%02d", h, m);
        return MakeReminder(diff, text, when);
    }

    double num = 0;
    bool startsWithNumber = ParseNumber(t.front().low, num);

    // 2) Birim çevirme: "10 inç cm", "100 f to c"
    if (startsWithNumber && t.size() >= 3) {
        std::vector<const Unit*> units;
        bool clean = true;
        for (size_t i = 1; i < t.size(); ++i) {
            if (IsConnector(t[i].low) && units.size() == 1) continue;
            const Unit* u = FindUnit(t[i].low);
            if (!u) { clean = false; break; }
            units.push_back(u);
        }
        if (clean && units.size() == 2 && units[0]->cat == units[1]->cat) {
            double out;
            if (units[0]->cat == Cat::Temp) {
                out = FromKelvin(ToKelvin(num, static_cast<int>(units[0]->factor)),
                                 static_cast<int>(units[1]->factor));
            } else {
                out = num * units[0]->factor / units[1]->factor;
            }
            if (out != 0) {  // çevirilerde 6 anlamlı basamak yeter
                double mag = std::pow(10.0, 5 - std::floor(std::log10(std::fabs(out))));
                out = std::round(out * mag) / mag;
            }
            r.kind = Result::Kind::Value;
            r.copy = FormatNumber(out);
            r.display = L"= " + r.copy + L" " + UnitLabel(*units[1]);
            return r;
        }
    }

    // 3) Süreli hatırlatma: "30 dk çamaşır", "çamaşır 30 dk (sonra)"
    if (startsWithNumber && t.size() >= 2) {
        if (double f = ReminderUnit(t[1].low)) {
            size_t from = 2;
            if (from < t.size() && t[from].low == L"sonra") ++from;
            double sec = num * f;
            if (sec >= 1 && sec <= 7 * 86400) return MakeReminder(sec, Join(t, from, t.size()), Clock(sec));
        }
    }
    {
        size_t end = t.size();
        if (end >= 3 && t[end - 1].low == L"sonra") --end;
        if (end >= 3) {
            double n = 0, f = ReminderUnit(t[end - 1].low);
            if (f && ParseNumber(t[end - 2].low, n)) {
                double sec = n * f;
                if (sec >= 1 && sec <= 7 * 86400) return MakeReminder(sec, Join(t, 0, end - 2), Clock(sec));
            }
        }
    }

    // 4) Hesap
    Parser p(input);
    double v = 0;
    if (p.Run(v) && std::isfinite(v)) {
        if (!p.sawOperator) return r;  // tek sayı: gösterecek bir şey yok
        r.kind = Result::Kind::Value;
        r.copy = FormatNumber(v);
        r.display = L"= " + r.copy;
        return r;
    }
    if (p.divZero) {
        r.kind = Result::Kind::Error;
        r.divideByZero = true;
        r.display = L"tanımsız";
        return r;
    }
    wchar_t c0 = input[0];
    bool mathy = (c0 >= L'0' && c0 <= L'9') || c0 == L'(' || c0 == L'-' || c0 == L'√';
    if (mathy) {
        r.kind = Result::Kind::Error;
        r.display = L"?";
    }
    return r;
}

}  // namespace Command
