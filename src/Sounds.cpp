#include "Sounds.h"

#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace Sounds {
namespace {

constexpr int kRate = 22050;
constexpr double kPi = 3.14159265358979323846;
bool g_enabled = true;

// Bir notanın tarifi: başlangıç ve bitiş frekansı, süre, ses yüksekliği, dalga yumuşaklığı.
struct Tone {
    double f0, f1, ms, gain;
    double square = 0.0;  // 0 = saf sinüs (yumuşak), 1 = kare dalgaya yakın (homurtu)
    double gapMs = 0.0;   // notadan sonra sessizlik
};

void Append(std::vector<int16_t>& pcm, const Tone& t) {
    int n = static_cast<int>(kRate * t.ms / 1000.0);
    double phase = 0;
    for (int i = 0; i < n; ++i) {
        double x = static_cast<double>(i) / n;
        double f = t.f0 + (t.f1 - t.f0) * x;
        phase += 2 * kPi * f / kRate;
        double s = std::sin(phase);
        if (t.square > 0) s = (1 - t.square) * s + t.square * (s >= 0 ? 0.6 : -0.6);
        // Yumuşak giriş/çıkış: "tık" sesi olmasın.
        double env = std::min(1.0, x * 12.0) * std::pow(1.0 - x, 1.6);
        pcm.push_back(static_cast<int16_t>(s * env * t.gain * 32767));
    }
    int gap = static_cast<int>(kRate * t.gapMs / 1000.0);
    pcm.insert(pcm.end(), gap, 0);
}

void AppendNoise(std::vector<int16_t>& pcm, double ms, double gain) {
    int n = static_cast<int>(kRate * ms / 1000.0);
    uint32_t seed = 12345;
    for (int i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        double s = ((seed >> 8) & 0xFFFF) / 32768.0 - 1.0;
        double x = static_cast<double>(i) / n;
        double env = std::pow(1.0 - x, 3.0);
        pcm.push_back(static_cast<int16_t>(s * env * gain * 32767));
    }
}

std::vector<uint8_t> MakeWav(const std::vector<int16_t>& pcm) {
    uint32_t dataBytes = static_cast<uint32_t>(pcm.size() * 2);
    std::vector<uint8_t> wav(44 + dataBytes);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);   // PCM
    put16(22, 1);   // mono
    put32(24, kRate);
    put32(28, kRate * 2);
    put16(32, 2);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, dataBytes);
    std::memcpy(&wav[44], pcm.data(), dataBytes);
    return wav;
}

std::vector<uint8_t> Build(Sound s) {
    std::vector<int16_t> p;
    switch (s) {
    case Sound::Open:  Append(p, {620, 980, 70, 0.10}); break;
    case Sound::Close: Append(p, {900, 560, 60, 0.08}); break;
    case Sound::Pet:
        Append(p, {1040, 1250, 45, 0.16, 0, 25});
        Append(p, {1300, 1560, 55, 0.16});
        break;
    case Sound::Happy:
        Append(p, {880, 1180, 70, 0.14});
        break;
    case Sound::Laugh:
        for (int i = 0; i < 4; ++i) Append(p, {1150 + i * 60.0, 1400 + i * 60.0, 40, 0.15, 0, 30});
        break;
    case Sound::Love:  // Do - Mi - Sol, yumuşak
        Append(p, {1047, 1047, 90, 0.13, 0, 10});
        Append(p, {1319, 1319, 90, 0.13, 0, 10});
        Append(p, {1568, 1568, 160, 0.13});
        break;
    case Sound::Angry:  // kısa homurtu
        Append(p, {190, 160, 110, 0.12, 0.6, 30});
        Append(p, {175, 140, 150, 0.12, 0.6});
        break;
    case Sound::Sad:
        Append(p, {700, 520, 220, 0.11});
        break;
    case Sound::Yawn:
        Append(p, {520, 640, 180, 0.10});
        Append(p, {640, 260, 520, 0.10});
        break;
    case Sound::TimerDone:
        for (int r = 0; r < 2; ++r) {
            Append(p, {1319, 1319, 90, 0.16, 0, 20});
            Append(p, {1568, 1568, 90, 0.16, 0, 20});
            Append(p, {2093, 2093, 160, 0.16, 0, 140});
        }
        break;
    case Sound::Shutter:
        AppendNoise(p, 35, 0.12);
        p.insert(p.end(), kRate * 40 / 1000, 0);
        AppendNoise(p, 45, 0.10);
        break;
    default: break;
    }
    return MakeWav(p);
}

}  // namespace

void SetEnabled(bool enabled) { g_enabled = enabled; }
bool Enabled() { return g_enabled; }

void Play(Sound s) {
    if (!g_enabled) return;
    static std::vector<uint8_t> cache[static_cast<int>(Sound::Count)];
    auto& wav = cache[static_cast<int>(s)];
    if (wav.empty()) wav = Build(s);
    // Bellekten, beklemeden çal. Yeni ses öncekini keser: küçük sesler için ideal.
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}  // namespace Sounds
