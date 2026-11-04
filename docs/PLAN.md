# DYNAMIS ISLAND — Proje Planı

Windows 11 için sıfırdan yazılmış bir **Dinamik Ada** (ekranın üstünde canlı, şekil değiştiren hap) ve **modern taskbar**. Amaç: Windows'u baştan aşağı daha modern, akıcı ve havalı göstermek.

> Önemli gerçek: Windows'un kendisini (çekirdeği, Explorer'ı) yeniden yazamayız. Yapacağımız şey, Windows'un üstünde çalışan bir **"kabuk katmanı" (shell overlay)**: kendi pencerelerimizle orijinal taskbar'ı gizleyip yerine kendi arayüzümüzü koymak. Rainmeter, StartAllBack, TranslucentTB gibi projeler de bu mantıkla çalışır. Sistem dosyalarına dokunmadığımız için güvenli ve geri alınabilir.

---

## 1. Teknoloji önerisi

| Katman | Seçim | Neden |
|---|---|---|
| Dil / platform | **C# + .NET 8** | Windows API'lerine en kolay erişim, hızlı geliştirme |
| Arayüz | **WinUI 3 (Windows App SDK)** | Windows 11'in kendi Fluent tasarımı, Mica/Acrylic efektleri, akıcı animasyonlar |
| Düşük seviye | **Win32 interop (CsWin32)** | Taskbar gizleme, AppBar alanı ayırma, pencere takibi, saydam/çerçevesiz pencere |
| Sistem verileri | **WinRT API'leri** | Medya, bildirim, pil, ağ bilgileri |
| Ses / parlaklık | **Core Audio (NAudio)** + WMI | Ses seviyesi ve parlaklık okuma/değiştirme |
| Ayarlar | JSON dosyası (`%AppData%\DynamisIsland`) | Basit, taşınabilir |
| Derleme | **GitHub Actions (windows-latest)** | Her push'ta otomatik derleme ve indirilebilir .exe/.msix |

**Alternatif:** WPF. Saydam, her zaman üstte duran pencerelerde daha olgun ve sorunsuz, ama Windows 11 görünümünü elle yapmak gerekir. WinUI 3'te bir engel çıkarsa sadece arayüz katmanını WPF'e geçirebiliriz; mimari buna göre ayrıştırılmış olacak.

## 2. Mimari

```
DynamisIsland.sln
├─ src/
│  ├─ DynamisIsland.Core        → Servisler (UI'dan bağımsız, test edilebilir)
│  │   ├─ MediaService           (çalan şarkı, kapak, oynat/durdur/ileri)
│  │   ├─ NotificationService    (Windows bildirimlerini dinleme)
│  │   ├─ PowerService           (pil %, şarj durumu)
│  │   ├─ AudioService           (ses seviyesi, sessiz)
│  │   ├─ WindowTracker          (açık pencereler, aktif uygulama, ikonlar)
│  │   └─ TimerService           (zamanlayıcı, kronometre)
│  ├─ DynamisIsland.Native      → Win32 çağrıları (taskbar gizleme, AppBar, hook'lar)
│  ├─ DynamisIsland.Island      → Dinamik Ada penceresi ve "aktiviteler"
│  ├─ DynamisIsland.Taskbar     → Yeni taskbar penceresi
│  └─ DynamisIsland.App         → Giriş noktası, tepsi ikonu, ayarlar ekranı
└─ docs/
```

**Ada "aktivite" sistemi:** Her özellik (müzik, zamanlayıcı, şarj, bildirim…) bir `IIslandActivity` olur. Her birinin bir önceliği ve üç boyutu vardır: *kompakt* (küçük hap), *genişletilmiş* (hover/tık ile açılır), *uyarı* (kısa süreli patlama, ör. "şarj takıldı"). Ada o an en önemli aktiviteyi gösterir, aralarında yaylanma (spring) animasyonuyla geçer. Yeni özellik eklemek = yeni bir aktivite sınıfı yazmak.

## 3. Özellik listesi

### Dinamik Ada
- Boştayken: saat / tarih, ince siyah hap
- **Müzik:** Spotify, YouTube (tarayıcı), Apple Music vb. — kapak resmi, şarkı adı, oynat/durdur/ileri/geri, ilerleme çubuğu, ses dalgası animasyonu
- **Ses & parlaklık:** Windows'un çirkin ses penceresinin yerine adada şık bir çubuk
- **Şarj / pil:** şarj takılınca animasyon, düşük pil uyarısı
- **Zamanlayıcı & kronometre:** adada geri sayım
- **Bildirimler:** gelen bildirim adadan "akar", tıklayınca uygulamayı açar
- Sonra: arama/mikrofon/kamera kullanım göstergesi, dosya kopyalama ilerlemesi, odak modu, hava durumu

### Taskbar
- Yüzen (floating), yuvarlak köşeli, Mica/Acrylic camsı görünüm — tam genişlik veya dock tarzı seçenekli
- Sabitlenmiş + açık uygulamalar, aktif uygulama göstergesi, hover'da büyüme animasyonu
- Pencere önizlemesi (DWM thumbnail)
- Kendi başlat butonu → kendi hızlı başlatıcımız (arama + uygulama listesi)
- Saat, Wi-Fi, ses, pil için kendi hızlı ayar paneli
- Orijinal Windows taskbar'ını gizleme / geri getirme (uygulama kapanınca otomatik geri gelir)

### Genel
- Ayarlar ekranı (renkler, boyut, konum, hangi aktiviteler açık)
- Açık/koyu tema, vurgu rengi
- Windows ile birlikte başlama
- Çoklu monitör desteği
- Düşük kaynak kullanımı (hedef: < 100 MB RAM, boşta ~%0 CPU)

## 4. Aşamalar (milestone)

| Aşama | İçerik | Sonuç |
|---|---|---|
| **0 — Temel** | Solution yapısı, GitHub Actions ile otomatik derleme, boş saydam her-zaman-üstte pencere | İndirip çalıştırabildiğin ilk .exe |
| **1 — Ada MVP** | Ekranın üst ortasında hap, saat, hover'da genişleme animasyonu, **müzik kontrolü** | İlk "vay be" anı |
| **2 — Ada aktiviteleri** | Ses/parlaklık, pil/şarj, zamanlayıcı, bildirimler, öncelik sistemi | Gerçek Dinamik Ada deneyimi |
| **3 — Taskbar** | Orijinal taskbar'ı gizleme, AppBar, uygulama ikonları, aktif pencere takibi, saat, hızlı ayarlar | Windows taskbar'ının yerini alan bar |
| **4 — Başlatıcı & cila** | Kendi başlat menümüz, pencere önizlemeleri, temalar, ayarlar ekranı, otomatik başlatma | Günlük kullanılabilir sürüm |
| **5 — Ekstralar** | Widget'lar, çoklu monitör, hava durumu, eklenti sistemi | v1.0 |

## 5. Bilinen zorluklar

- **Sistem tepsisi ikonları** (sağ alttaki küçük uygulama ikonları) Windows'ta resmi bir API ile alınamıyor. İlk sürümlerde kendi hızlı ayar panelimizle başlayıp, tepsiyi sonraya bırakıyoruz.
- **Bildirim okuma** Windows'un izin istemesini gerektirir ve uygulamanın paketli (MSIX) olmasını isteyebilir.
- **Test:** Kodu ben bulut ortamında (Linux) yazıyorum; Windows'ta çalıştıramıyorum. GitHub Actions Windows'ta derleyip .exe üretecek, sen indirip deneyeceksin ve gördüklerini (ekran görüntüsü / hata) bana ileteceksin. Bu döngüyü hızlı tutmak için küçük adımlarla ilerleyeceğiz.
- **Windows güncellemeleri** taskbar davranışını değiştirebilir; taskbar gizleme kodunu tek bir yerde izole tutuyoruz.

## 6. Senin kararın gereken sorular

1. **Teknoloji:** C# + WinUI 3 uygun mu? (C# bilgin var mı, yoksa kodu tamamen bana mı bırakıyorsun?)
2. **Taskbar stili:** Tam genişlikte modern bar mı, yoksa ortada yüzen macOS-dock tarzı mı?
3. **Ada konumu:** Ekranın üst ortası mı (iPhone gibi), yoksa başka bir yer mi?
4. **Orijinal taskbar:** Tamamen gizleyelim mi?
5. **Tasarım dili:** Simsiyah iPhone tarzı ada mı, yoksa Windows'a uygun camsı (Mica/Acrylic) görünüm mü? Favori renk/tema?
6. **İlk öncelik:** Önce adayı mı bitirelim (önerim), yoksa taskbar'ı mı?
7. **Bilgisayarın:** Windows 11 sürümün (23H2 / 24H2 / 25H2)? Laptop mı (pil özellikleri için), kaç monitör?
8. **Kullanım:** Sadece kendin için mi, yoksa ileride başkalarıyla paylaşmak/yayınlamak mı istiyorsun?
