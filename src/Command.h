#pragma once
#include <string>

// "Yazınca anlayan ada": kullanıcının yazdığı satırı yorumlar.
//   12*4+7        -> hesap makinesi
//   10 inç cm     -> birim çevirici
//   30 dk çamaşır -> hatırlatıcı (ya da "çamaşır 30 dk", "15:30 toplantı")
// Arayüzden bağımsızdır; Windows'a da bağlı değildir.
namespace Command {

struct Result {
    enum class Kind { None, Value, Reminder, Error } kind = Kind::None;
    std::wstring display;      // adada sağda gösterilen kısa sonuç
    std::wstring copy;         // Enter'a basınca panoya giden metin
    double reminderSeconds = 0;  // şu andan kaç saniye sonra
    std::wstring reminderText;
    bool divideByZero = false;
};

// hour/minute/second: yerel saat (mutlak saatli hatırlatmalar için).
Result Evaluate(const std::wstring& input, int hour, int minute, int second);

// Sayıyı Türkçe biçimde yazar (ondalık virgül, gereksiz sıfırlar yok).
std::wstring FormatNumber(double v);

}  // namespace Command
