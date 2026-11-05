# DYNAMIS ISLAND

Windows 11 için Dinamik Ada (ve ileride modern taskbar). C++ ve saf Win32 + Direct2D ile yazıldı: kurulum gerektirmez, tek bir `.exe`, zayıf bilgisayarlarda bile hafif çalışır.

## Neler yapıyor (1. aşama)

- Ekranın üst ortasında iPhone tarzı siyah ada
- **İki robot göz ve duyguları:** boştayken sadece küçük gözler görünür. Göz kırpar, fareyi takip eder, ara sıra kendi kendine etrafa bakınır, göz kırpar ya da şirinlik yapar
  - Ada açıkken gözlere tıkla: 1-2 tık **mutlu**, 3 tık **kıkırdar**, 4 tık **aşık olur** (kalp gözler), 5+ tık **kızar** ve sallanır, 8+ tık **başı döner**
  - Şarj takılınca kalp gözler, şarj çıkınca ya da pil azalınca **üzülür** (gözyaşı)
  - Müzik başlayınca sevinir, durunca üzülür, yeni şarkıda şaşırır
  - Kimse yokken uyur (gece daha çabuk), fare yaklaşınca uyanır
- **Müzik:** Spotify, tarayıcı, Medya Oynatıcı... Kapak resmi, şarkı/sanatçı, ilerleme çubuğu, geri / oynat-durdur / ileri. Ekolayzer kapak rengini alır ve hoparlörden çıkan gerçek sesle oynar; her vuruşta Guti zıplayarak dans eder
- Üzerine gelince açılır: müzik yoksa büyük saat, tarih, pil ve gözler
- Şarj takıldı / çıkarıldı / pil azaldı uyarıları
- Oyun veya tam ekran video açıkken kendini gizler
- **Guti'nin duyuları:** işlemci zorlanınca terler, bellek dolunca başı döner, internet gidince üzülür, gelince sevinir
- Kamera veya mikrofon açıkken adada küçük nokta (yeşil kamera, turuncu mikrofon); Guti merakla bakar
- Ekran görüntüsü alınca göz kırpar ve minik deklanşör sesi çıkarır
- İndirme bitince kart çıkar: tıkla klasörde göster, ya da dosyayı adadan sürükleyip istediğin yere bırak
- **Yazınca anlayan ada** (Ctrl+Alt+Boşluk ya da açık adada saate tıkla):
  - `12*4+7`, `200*18%`, `sqrt(16)` → hesaplar, Enter sonucu kopyalar
  - `10 inç cm`, `100 f c`, `5 km mil`, `2 gb mb` → birim çevirir
  - `30 dk çamaşır`, `çamaşır 1 saat sonra`, `15:30 toplantı` → hatırlatıcı kurar (kapatıp açsan da kalır)
  - Boşken son 5 kopyaladığın şey listelenir (↑ ↓ / teker, Enter kopyalar). Pano geçmişi sadece bellekte tutulur, şifre yöneticilerinin "geçmişe ekleme" işaretine uyulur
- **Ekran görüntüsü** (Ctrl+Alt+S): önizleme kartı çıkar, tıkla aç ya da sürükleyip bırak
- **Dosya rafı:** dosyayı adaya sürükle bırak, Guti tutar. Adayı açınca altta görünür; oradan istediğin yere sürükle
- **Renk seçici** (Ctrl+Alt+C): ekranda bir yere tıkla, renk kodu (#RRGGBB) kopyalanır; Guti'nin gözleri o renge bürünür
- Ses düzeyi göstergesi (Ayarlar'dan kapatılabilir) ve Bluetooth kulaklık bağlanınca pil yüzdesi
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
