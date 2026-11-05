#pragma once
#include <windows.h>

// Guti'nin "beyni": zamanlayıcı, odak modu, mola arkadaşı ve dostluk puanı.
// Arayüzden bağımsızdır; Island her saniye Tick() çağırır ve dönen olaya göre Guti'yi oynatır.
class Companion {
public:
    enum class TimerKind { None, Quick, Focus, Break };
    enum class Event { None, TimerStarted, TimerDone, FocusDone, BreakDone, Distracted, NeedBreak };

    static constexpr int kFocusMinutes = 25;
    static constexpr int kBreakMinutes = 5;

    // Dostluk
    void Load();  // açılışta: puan ve son görülme
    void Save();
    int Affection() const { return affection_; }
    int DaysAway() const { return daysAway_; }  // Load() anında hesaplanır
    void OnPet(double now);

    // Fare tekeriyle süre ayarlama: +/- dakika. Kısa bir süre dokunulmazsa başlar.
    void AdjustPending(int deltaMinutes, double now);
    bool Pending() const { return pendingMinutes_ > 0 && pendingSince_ > 0; }
    int PendingMinutes() const { return pendingMinutes_; }

    void StartTimer(TimerKind kind, int minutes, double now);
    void CancelTimer();
    TimerKind Kind() const { return kind_; }
    bool TimerActive() const { return kind_ != TimerKind::None; }
    double Remaining(double now) const;
    double Total() const { return total_; }

    // Her saniye.
    Event Tick(double now);

private:
    bool IsDistracting() const;

    int affection_ = 50;
    int daysAway_ = 0;
    double lastPetGain_ = 0;
    double lastSave_ = 0;

    int pendingMinutes_ = 0;
    double pendingSince_ = 0;

    TimerKind kind_ = TimerKind::None;
    double endsAt_ = 0;
    double total_ = 0;

    double lastDistractCheck_ = 0;
    double lastDistracted_ = -1000;

    double workStartTick_ = 0;  // GetTickCount64 / 1000
    double lastBreakNudge_ = 0;
};
