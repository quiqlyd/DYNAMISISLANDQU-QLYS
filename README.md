# DYNAMIS ISLAND

Windows 11 için Dinamik Ada (ve ileride modern taskbar). C++ ve saf Win32 + Direct2D ile yazıldı: kurulum gerektirmez, tek bir `.exe`, zayıf bilgisayarlarda bile hafif çalışır.

## Neler yapıyor (1. aşama)

- Ekranın üst ortasında iPhone tarzı siyah ada
- **İki robot göz:** göz kırpar, fareyi takip eder, tıklayınca (sevince) mutlu olur ve zıplar, art arda çok tıklarsan başı döner ve sallanır, 90 saniye kimse yokken uyur, fare yaklaşınca uyanır
- **Müzik:** Spotify, tarayıcı, Medya Oynatıcı... Kapak resmi, şarkı/sanatçı, ilerleme çubuğu, geri / oynat-durdur / ileri. Ekolayzer kapak rengini alır, gözler ritimle sallanır
- Üzerine gelince açılır: müzik yoksa büyük saat, tarih, pil ve gözler
- Şarj takıldı / çıkarıldı / pil azaldı uyarıları
- Oyun veya tam ekran video açıkken kendini gizler
- Sağ alttaki tepsi simgesine sağ tık: **Windows ile başlat** ve **Çıkış**

## İndirme

1. Depodaki **Actions** sekmesine gir, en üstteki yeşil tikli **build** çalışmasına tıkla.
2. Sayfanın altındaki **Artifacts** bölümünden `DynamisIsland` dosyasını indir (GitHub'a giriş yapmış olman gerekir).
3. Zip'i aç, `DynamisIsland.exe`'yi çalıştır. Windows SmartScreen uyarırsa **Ek bilgi → Yine de çalıştır**.

## Derleme (geliştiriciler için)

Visual Studio 2022+ (C++ masaüstü iş yükü) ve CMake:

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

Plan ve yol haritası: [docs/PLAN.md](docs/PLAN.md)
