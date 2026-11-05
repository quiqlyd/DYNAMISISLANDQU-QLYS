#pragma once

// Guti'nin minik sesleri. Hepsi açılışta küçük dalga formları olarak bellekte üretilir:
// dosya yok, kütüphane yok. Çalma eşzamansızdır, işlemciye yük bindirmez.
namespace Sounds {

enum class Sound { Open, Close, Pet, Laugh, Love, Angry, Yawn, TimerDone, Shutter, Happy, Sad, Count };

void SetEnabled(bool enabled);
bool Enabled();
void Play(Sound s);

}  // namespace Sounds
