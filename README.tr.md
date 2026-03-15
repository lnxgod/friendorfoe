🌍 [English](README.md) | [עברית](README.he.md) | [Українська](README.uk.md) | [العربية](README.ar.md) | **[Türkçe](README.tr.md)** | [Azərbaycan](README.az.md) | [Türkmen](README.tk.md) | [پښتو](README.ps.md) | [اردو](README.ur.md) | [Kurdî](README.ku.md) | [Հայերեն](README.hy.md) | [ქართული](README.ka.md) | [فارسی](README.fa.md)

# Friend or Foe — Gerçek Zamanlı Uçak ve Drone Tanımlama

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)

**Telefonunuzu gökyüzüne doğrultun. Orada ne olduğunu bilin.**

Friend or Foe, artırılmış gerçeklik kullanarak uçakları ve droneları gerçek zamanlı olarak tanımlayan açık kaynaklı, **kur-ve-başla** bir Android uygulamasıdır. ADS-B transponder verilerini, FAA Remote ID drone yayınlarını, WiFi sinyal analizini ve cihaz üzerinde görsel algılamayı birleştirerek kamera görüntüsü üzerine yüzen etiketler yerleştirir — başınızın üzerinde ne uçtuğunu, kimin işlettiğini, nereye gittiğini ve dost mu düşman mı olduğunu size söyler. Hesap, kayıt veya API anahtarı gerekmez — sadece kurun ve başlayın.

Bu proje **yapay zeka ile inşa edildi** — sadece biriyle değil, hepsiyle. Claude kodu yazdı, Grok tasarımı şekillendirdi, Codex güvenliği inceledi ve Gemini teknoloji yığınını seçmeye yardımcı oldu. Yapay zeka ile insan yaratıcılığı bir araya geldiğinde neler mümkün olduğunu göstermek için [GAMECHANGERSai](https://gamechangersai.org) tarafından yayınlandı. Sürüm geçmişi için [CHANGELOG](CHANGELOG.md) dosyasına bakın.

### 72 Saatlik Hız Koşusu

> **Sıfırdan gerçek bir cihazda onaylanmış uçak ve drone algılamalarına — 72 saatten kısa sürede.**
>
> 12 Mart 2025'te ilk commit o akşam yapıldı, 13 commit ve **8.500+ satır kod** Claude ile ~2 saatlik yapay zeka eşli programlama ile yazıldı — AR vizör, çoklu kaynak algılama, Bayesian sensör füzyonu ve harita görünümüne sahip derlemeye hazır bir APK üretti. 14 Mart'a kadar uygulama, uçak siluetleri, stilize harita işaretçileri ve cilalamayla açık kaynak olarak yayınlandı — Kotlin, Python ve XML genelinde toplam **22.000+ satır**.
>
> Uygulama **kur-ve-başla** — APK'yı yükleyin, izinleri verin ve uçakları tanımlamaya başlayın. Sunucu, anahtar veya hesap olmadan doğrudan ücretsiz halka açık ADS-B API'lerine bağlanır. İsteğe bağlı Python arka ucu yalnızca ek zenginleştirme ekler (havayolu adları, rota bilgisi).

---

## Ne Yapar

- **AR Vizör** — Algılanan uçak ve dronelarda renk kodlu yüzen etiketlerle canlı kamera görüntüsü. Etiketler çağrı işareti, uçak tipi, irtifa ve mesafeyi gösterir.
- **Çoklu Kaynak Algılama** — Kapsamlı gökyüzü farkındalığı için dört bağımsız algılama yöntemini birleştirir:
  - ADS-B transponder verileri (ticari uçuşlar, genel havacılık)
  - Bluetooth LE üzerinden FAA Remote ID (uyumlu droneler)
  - WiFi SSID desen eşleştirme (DJI, Skydio, Parrot, 100'den fazla üretici)
  - ML Kit ile görsel algılama (kamera tabanlı nesne tanıma)
- **Akıllı Sınıflandırma** — Her şeyi 10 türe ayırır: Ticari, Genel Havacılık, Askeri, Helikopter, Devlet, Acil Durum, Kargo, Drone, Kara Aracı ve Bilinmeyen. Askeri algılama çağrı işareti kalıplarını, squawk kodlarını ve operatör veritabanlarını kullanır.
- **Bayesian Sensör Füzyonu** — Birden fazla sensör aynı nesneyi algıladığında, güven puanları Bayesian log-odds kullanılarak birleştirilir — sadece "en yükseği seç" değil. İki zayıf sinyalin uyuşması, bir güçlü sinyalden ağır basabilir.
- **Uçak Siluetleri ve Fotoğrafları** — 120'den fazla ICAO tip kodu 10 vektör siluet kategorisine eşlenmiştir (dar gövde, geniş gövde, bölgesel, turboprop, iş jeti, helikopter, savaş uçağı, kargo, hafif uçak, drone) artı anlık görsel tanıma için 134 paketlenmiş uçak fotoğrafı — ağ gerekmez.
- **2D Harita Görünümü** — Kategori başına farklı işaretçi şekilleri, mesafe halkaları, pusula takip modu ve görüş alanı (FOV) koni kaplamasıyla OpenStreetMap.
- **Detay Kartları** — Tam detaylar için herhangi bir nesneye dokunun: tescil, operatör, rota (kalkış/varış), irtifa, hız, başlık, squawk kodu, algılama kaynağı ve güven seviyesi.
- **Geçmiş Günlüğü** — Tanımladığınız her şeyin kalıcı veritabanı, aranabilir ve sıralanabilir.
- **Drone Referans Rehberi** — Fotoğraflar, teknik özellikler ve açıklamalarla 30'dan fazla drone türünün dahili veritabanı — DJI tüketici drone'larından Bayraktar TB2, MQ-9 Reaper ve Shahed-136 gibi askeri İHA'lara kadar. Kategoriye göre göz atın, ada göre arayın veya bilinmeyen bir drone algılamasından doğrudan ne gördüğünüzü tanımlamaya geçin.
- **Arka Uç Zenginleştirme (İsteğe Bağlı)** — İsteğe bağlı bir Python API sunucusu havayolu adları, tescil numaraları ve zaten paketlenmiş olanların ötesinde rota bilgisi ekleyebilir. Uygulama onsuz da tamamen çalışır — uçak fotoğrafları ve drone referansları APK'ya dahildir.

---

## Nasıl Çalışır

### Algılama Kaynakları

| Kaynak | Ne Algılar | Menzil | Güven |
|--------|-----------|--------|-------|
| **ADS-B** | Transponderli ticari ve genel havacılık uçakları | ~250 NM | Çok Yüksek (0.95) |
| **Remote ID (BLE)** | FAA uyumlu droneler (250g+) | ~300m | Yüksek (0.85) |
| **WiFi SSID** | DJI, Skydio, Parrot, bütçe droneler | ~100m | Orta (0.3-0.85) |
| **Görsel (ML Kit)** | Kamerada görünen her şey | Görüş hattı | Değişken |

### Sensör Füzyonu ve AR

Uygulama, telefonun tam olarak nereye baktığını belirlemek için ivmeölçer, manyetometre ve jiroskop verilerini birleştirir. Uçak konumları (enlem/boylam/irtifa), haversine mesafe hesaplamaları ve kamera görüş alanı geometrisi kullanılarak kamera görüntüsüne yansıtılır.

**ARCore + Pusula Hibrit**: ARCore, kamera yer özelliklerini gördüğünde mükemmel izleme sağlar, ancak özelliksiz gökyüzüne yöneltildiğinde zorlanır — tam da en çok ihtiyacınız olduğunda. Uygulama, ARCore izlemeyi kaybettiğinde otomatik olarak pusula-matematik yönlendirmesine geri döner ve tüm koşullarda kesintisiz etiketler sağlar.

### Mimari

```
┌──────────────────────────────────────────────────┐
│  Android Uygulama (Kotlin + Jetpack Compose)     │
│                                                  │
│  ┌─────────┐ ┌─────────┐ ┌──────────┐          │
│  │ AR Görün│ │  Harita  │ │  Liste   │  ...Arayüz│
│  └────┬────┘ └────┬────┘ └────┬─────┘          │
│       │           │           │                  │
│  ┌────┴───────────┴───────────┴─────┐           │
│  │         ViewModels (MVVM)        │           │
│  └────┬─────────────────────┬───────┘           │
│       │                     │                    │
│  ┌────┴────┐          ┌────┴──────┐             │
│  │ Sensör  │          │ Algılama  │             │
│  │ Füzyon  │          │ Kaynakları│             │
│  │ Motoru  │          │           │             │
│  │         │          │ • ADS-B   │             │
│  │ • ARCore│          │ • BLE RID │             │
│  │ • İvme  │          │ • WiFi    │             │
│  │ • Jiro  │          │ • Görsel  │             │
│  │ • Mag   │          │           │             │
│  └─────────┘          └─────┬─────┘             │
│                             │                    │
│                    ┌────────┴────────┐           │
│                    │ Bayesian Füzyon │           │
│                    │   Motoru        │           │
│                    └────────┬────────┘           │
│                             │                    │
└─────────────────────────────┼────────────────────┘
                              │ HTTP
                    ┌─────────┴─────────┐
                    │  Arka Uç (FastAPI)│
                    │  • Uçak çekme     │
                    │  • Zenginleştirme  │
                    │  • Önbellek (Redis)│
                    └─────────┬─────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
         adsb.fi     airplanes.live     OpenSky
        (birincil)    (yedek)         (yedek)
```

---

## Başlarken

### Ön Koşullar

- **Android Studio** Hedgehog (2023.1.1) veya üstü
- **JDK 17**
- **Android SDK 35** (compileSdk), minSdk 26 (Android 8.0+)
- GPS, pusula ve kamerası olan bir Android cihaz (emülatör liste/harita görünümleri için çalışır ancak AR için çalışmaz)

> **Hesap veya kayıt gerekmez.** Uygulama doğrudan ücretsiz halka açık ADS-B API'lerine bağlanır — arka uç kurulumu, API anahtarları, hesaplar gerekmez.

### Hızlı Başlangıç — Sadece Android

Uygulama doğrudan ücretsiz halka açık ADS-B API'lerine bağlanır — arka uç kurulumu veya hesap gerekmez. Uçak fotoğrafları ve drone referans rehberi APK'ya paketlenmiştir. Sadece derleyip çalıştırmak için:

```bash
cd android
./gradlew assembleDebug
# Bağlı cihaza yükle:
adb install app/build/outputs/apk/debug/app-debug.apk
```

Veya en son önceden derlenmiş APK'yı [**GitHub Releases**](https://github.com/lnxgod/friendorfoe/releases) sayfasından indirin.

### Arka Uç Kurulumu (İsteğe Bağlı — zenginleştirmeyi etkinleştirir)

Arka uç, uçak fotoğrafları, havayolu adları, tescil aramaları ve rota bilgisi ekler. **Python 3.11+ gerektirir ve isteğe bağlı olarak önbellek için Redis.**

```bash
cd backend

# Sanal ortam oluştur
python -m venv .venv
source .venv/bin/activate  # veya Windows'ta .venv\Scripts\activate

# Bağımlılıkları yükle
pip install -r requirements.txt

# Ortam yapılandırmasını kopyala
cp .env.example .env
# OpenSky kimlik bilgilerini eklemek istiyorsanız .env dosyasını düzenleyin (isteğe bağlı, hız sınırlarını artırır)

# Sunucuyu çalıştır
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

**Docker ile** (Redis + PostgreSQL dahil):

```bash
cd backend
docker compose up
```

API `http://localhost:8000` adresinde kullanılabilir olacaktır. Sağlık kontrolü: `http://localhost:8000/health`

### Uygulamayı Arka Ucunuza Bağlama

Android uygulamasının ağ yapılandırmasındaki arka uç URL'sini sunucunuzun IP adresine işaret edecek şekilde güncelleyin.

---

## Teknoloji Yığını

### Android Uygulaması

| Bileşen | Kütüphane | Sürüm |
|---------|-----------|-------|
| Dil | Kotlin | 1.9.22 |
| Arayüz | Jetpack Compose + Material 3 | 2024.02.00 |
| AR | ARCore | 1.41.0 |
| Kamera | CameraX | 1.3.1 |
| ML | ML Kit Object Detection | 17.0.1 |
| Bağımlılık Enjeksiyonu | Hilt | 2.50 |
| HTTP | Retrofit + OkHttp | 2.9.0 / 4.12.0 |
| Veritabanı | Room | 2.6.1 |
| Görseller | Coil | 2.5.0 |
| Haritalar | OSMDroid | 6.1.18 |
| Asenkron | Coroutines + Flow | 1.7.3 |

### Arka Uç

| Bileşen | Kütüphane | Sürüm |
|---------|-----------|-------|
| Çerçeve | FastAPI | 0.115.6 |
| Sunucu | Uvicorn | 0.34.0 |
| HTTP İstemcisi | httpx | 0.28.1 |
| Önbellek | Redis | 5.2.1 |
| Veritabanı | SQLAlchemy + asyncpg | 2.0.36 |
| Doğrulama | Pydantic | 2.10.4 |

---

## Proje Yapısı

```
friendorfoe/
├── android/                           # Android uygulaması
│   └── app/src/main/java/com/friendorfoe/
│       ├── data/
│       │   ├── local/                 # Room veritabanı (geçmiş, izleme)
│       │   ├── remote/                # Retrofit API servisleri ve DTO'lar
│       │   └── repository/            # Veri depoları
│       ├── detection/                 # Algılama motorları
│       │   ├── AdsbPoller.kt          # ADS-B veri sorgulama
│       │   ├── RemoteIdScanner.kt     # BLE Remote ID tarama
│       │   ├── WifiDroneScanner.kt    # WiFi SSID algılama
│       │   ├── BayesianFusionEngine.kt # Çoklu sensör güven füzyonu
│       │   ├── MilitaryClassifier.kt  # Askeri uçak tanımlama
│       │   └── VisualDetection*.kt    # ML Kit görsel algılama
│       ├── domain/model/              # Alan modelleri (Aircraft, Drone, SkyObject)
│       ├── di/                        # Hilt bağımlılık enjeksiyonu
│       ├── presentation/
│       │   ├── ar/                    # AR vizör ekranı
│       │   ├── map/                   # OpenStreetMap görünümü
│       │   ├── list/                  # Sıralanabilir nesne listesi
│       │   ├── detail/                # Tam nesne detay kartları
│       │   ├── drones/                # Drone referans rehberi tarayıcısı
│       │   ├── history/               # Algılama geçmişi
│       │   ├── welcome/               # Karşılama/başlatma ekranı
│       │   ├── about/                 # Uygulama bilgisi
│       │   └── util/                  # AircraftPhotos, CategoryColors, DroneDatabase
│       └── sensor/                    # Sensör füzyonu ve konumlandırma
├── backend/                           # Python FastAPI sunucusu
│   └── app/
│       ├── main.py                    # FastAPI giriş noktası
│       ├── config.py                  # Ortam yapılandırması
│       ├── routers/aircraft.py        # API uç noktaları
│       └── services/
│           ├── adsb.py                # Çoklu kaynak ADS-B çekme
│           └── enrichment.py          # Uçak veri zenginleştirme
├── images/                            # Uçak referans fotoğrafları (Wikimedia CC)
├── scripts/                           # Yardımcı betikler (fotoğraf indirici)
├── macos/                             # macOS yardımcı uygulama (erken aşama)
└── docs/                              # Tasarım belgeleri
```

---

## API Uç Noktaları

| Yöntem | Uç Nokta | Açıklama |
|--------|----------|----------|
| `GET` | `/health` | Sağlık kontrolü (Redis, DB durumu) |
| `GET` | `/aircraft/nearby?lat=&lon=&radius_nm=` | Bir konuma yakın uçaklar |
| `GET` | `/aircraft/{icao_hex}/detail` | Zenginleştirilmiş uçak detayları |

---

## Katkıda Bulunma

Katkılar memnuniyetle karşılanır! İster hata düzeltmeleri, ister yeni algılama yöntemleri, arayüz iyileştirmeleri veya belgeleme olsun — yardımınızı isteriz.

1. Depoyu çatallayın (fork)
2. Bir özellik dalı oluşturun (`git checkout -b feature/harika-ozellik`)
3. Değişikliklerinizi kaydedin (`git commit -m 'Harika özellik ekle'`)
4. Dalı gönderin (`git push origin feature/harika-ozellik`)
5. Bir Pull Request açın

### Katkıda Bulunanlar İçin Fikirler

- Ek uçak tip kodu eşlemeleri
- İyileştirilmiş WiFi drone üretici parmak izleri
- AR kaplaması için gece modu / koyu tema
- Uçuş yolu tahmini ve yörünge görselleştirmesi
- Sosyal özellikler (gözlemleri paylaşma, topluluk raporları)
- macOS Swift temeli kullanılarak iOS portu
- Ek ADS-B veri kaynaklarıyla entegrasyon

---

## Yapay Zeka ile İnşa Edildi

Bu proje sadece bir yapay zeka ile değil — **hepsiyle** inşa edildi. Her büyük yapay zeka platformunu test ettik ve her birini en güçlü olduğu yerde kullandık. Git geçmişi hikayeyi anlatır:

| Tarih | Ne Oldu |
|-------|---------|
| **12 Mart** | 13 commit yapıldı — ~2 saatlik yapay zeka eşli programlama ile **8.500+ satır kod** yazıldı. Sonuç: AR vizör, dört algılama kaynağı, Bayesian sensör füzyonu, harita görünümü ve geçmiş takibi ile derlemeye hazır bir APK. |
| **14 Mart** | Açık kaynak yayını. 120'den fazla uçak silueti, kategori şekillerine sahip stilize harita işaretçileri, izin yönetimi cilalaması ve güvenlik incelemesi eklendi. Sonra: 134 uçak fotoğrafı çevrimdışı varlıklar olarak paketlendi, güncelleme denetleyicili karşılama ekranı ve Coil görsel yükleme eklendi. |
| **Toplam** | **23.000+ satır** Kotlin, Python ve XML. Cihazda ticari uçak ve droneların gerçek dünya algılamaları onaylandı. |

### Yapay Zeka Rolleri

| Yapay Zeka | Rol |
|------------|-----|
| **[Claude](https://claude.ai)** (Anthropic) | Birincil kodlama ajanı — mimari, uygulama, eşli programlama |
| **[Grok](https://grok.com)** (xAI) | Tasarım yönlendirmesi ve araştırma |
| **[Codex](https://chatgpt.com)** (OpenAI) | Güvenlik incelemesi ve danışmanlık |
| **[Gemini](https://gemini.google.com)** (Google) | Teknoloji yığını araştırması — kütüphaneleri, çerçeveleri ve yaklaşımları değerlendirme |
| **[ML Kit](https://developers.google.com/ml-kit)** (Google) | Cihaz üzerinde görsel nesne algılama (doğrudan telefonda çalışır) |

**Vibe coding nedir?** İnsan ve yapay zekanın gerçek zamanlı olarak birlikte inşa ettiği işbirlikçi, sohbet tabanlı bir yazılım geliştirme yaklaşımıdır. Her satırı elle yazmak yerine, ne istediğinizi tarif eder, fikirler üzerinde yinelersiniz, birlikte hata ayıklarsınız ve yapay zekanın kalıp kodu halletmesine izin verirken siz vizyona ve mimariye odaklanırsınız. Hissiyatla programlama — ve işe yarıyor.

Friend or Foe, her biri en iyi olduğu alanda katkıda bulunan birden fazla yapay zeka ile inşa edildi. Claude, ilk mimariden sensör füzyon algoritmalarına, Bayesian matematiğine ve vektör çizilebilir sanat eserine kadar birincil kodlama ortağı olarak hizmet etti. Grok tasarım yönünü şekillendirmeye yardımcı oldu. Codex açık kaynak yayını öncesinde güvenlik denetimini gerçekleştirdi. Gemini hangi teknolojilerin ve çerçevelerin kullanılacağını araştırdı. Ve ML Kit, herhangi bir bulut bağımlılığı olmadan görsel algılamayı güçlendirerek cihaz üzerinde çalışır. Bu depodaki her dosya insan-yapay zeka işbirliğiyle şekillendirildi.

**Yazılım geliştirmenin geleceğinin doğru iş için doğru yapay zekayı kullanmak olduğuna inanıyoruz** ve bunun pratikte nasıl göründüğünü görebilmeniz için bu projeyi açık kaynak olarak yayınlıyoruz.

---

## GAMECHANGERSai Hakkında

**[GAMECHANGERSai](https://gamechangersai.org)** çocuklar ve aileler için uygulamalı, yapay zeka destekli öğrenme deneyimleri oluşturmaya adanmış bir 501(c)(3) kar amacı gütmeyen kuruluştur.

> *"Her öğrenciye inşa etmeyi ve yeniden düzenlemeyi öğreten eğlenceli yapay zeka oyunları."*

Platformumuz aracılığıyla öğrenciler gerçek, aktarılabilir beceriler geliştirirler:

- **Yaratıcı Kodlama** — Yapay zekayı yardımcı pilot olarak kullanarak işlevsel programlar oluşturma
- **Sistem Düşüncesi** — Karmaşık parçaların nasıl etkileştiğini ve nasıl hata ayıklanacağını anlama
- **Kaynak Yönetimi** — Stratejik planlama ve optimizasyon
- **Etik Muhakeme** — Yapay zeka kullanırken "yapabilir miyiz?"nin yanında "yapmalı mıyız?" diye sormayı öğrenme

**Friend or Foe, bu becerilerin gerçek olduğunun kanıtıdır.** Çocukların platformumuzda öğrendiği aynı yaratıcı kodlama, sistem düşüncesi ve yapay zeka işbirliği, bu tam işlevsel uçak tanımlama sistemini oluşturmak için kullanıldı. Bir oyun olarak başlayan bir araç haline gelir. Oyun olarak başlayan uzmanlık haline gelir.

Açık kaynak topluluğuna katkıda bulunmaya kararlıyız. Friend or Foe'yu kamuya açık olarak yayınlayarak şunları umuyoruz:

- Geliştiricileri ve öğrencileri yapay zeka destekli geliştirme ile neler mümkün olduğunu keşfetmeye **ilham vermek**
- Yapay zekanın bir yedek değil, yaratıcı bir ortak olduğunu **göstermek** — bu uygulamadaki her karar insan yargısıyla yönlendirildi
- Çocuklarımızın öğrendiği becerilerin sadece oyunlar için olmadığını — gerçek, kullanışlı teknoloji oluşturmanın temeli olduğunu **kanıtlamak**

Misyonumuz hakkında daha fazla bilgi için **[gamechangersai.org](https://gamechangersai.org)** adresini ziyaret edin.

---

## Veri Kaynakları ve Atıf

Friend or Foe kullanımı %100 ücretsizdir — aşağıdaki her veri kaynağı açıktır ve **API anahtarı veya ücretli hesap gerektirmez**.

### ADS-B Uçak Konum Verileri

Uygulama, bir kaynak çökse bile her zaman uçak verisi almanızı sağlayan üç katmanlı bir yedek zinciri kullanır:

| Öncelik | Kaynak | API Uç Noktası | Ne Sağlar | Kimlik Doğrulama |
|---------|--------|----------------|-----------|------------------|
| 1. | **[adsb.fi](https://adsb.fi)** | `opendata.adsb.fi/api` | Gerçek zamanlı uçak konumları, çağrı işaretleri, irtifa, hız, başlık, squawk kodları | Ücretsiz, anahtar yok |
| 2. | **[airplanes.live](https://airplanes.live)** | `api.airplanes.live` | adsb.fi ile aynı veri (uyumlu ADSBx v2 formatı) | Ücretsiz, anahtar yok |
| 3. | **[OpenSky Network](https://opensky-network.org)** | `opensky-network.org/api` | ICAO durum vektörleri — konum, hız, başlık, dikey hız | Ücretsiz, anahtar yok (daha yüksek hız sınırları için isteğe bağlı hesap) |

Her üç kaynak da dünya çapındaki alıcı ağlarından canlı ADS-B transponder verileri sağlar. Uygulama GPS koordinatları ve yarıçapa göre sorgular ve başarısızlık veya zaman aşımında otomatik olarak bir sonraki kaynağa geçer.

### Zenginleştirme ve Destekleyici Veriler

| Kaynak | Ne Sağlar | Kimlik Doğrulama |
|--------|-----------|------------------|
| **[Planespotters.net](https://planespotters.net)** | ICAO hex koduna göre uçak fotoğrafları — detay kartlarında gösterilir | Ücretsiz, anahtar yok |
| **[Open-Meteo](https://open-meteo.com)** | Mevcut hava durumu (bulut örtüsü, rüzgar hızı, koşullar) — algılama parametrelerini ayarlamak için kullanılır | Ücretsiz, anahtar yok |
| **[OpenStreetMap](https://openstreetmap.org)** | 2D harita görünümü için OSMDroid aracılığıyla harita karoları | Ücretsiz, anahtar yok |

### Cihaz Üzerinde Algılama (Harici API Yok)

Bu algılama yöntemleri tamamen telefonda çalışır, ağ çağrısı yapılmaz:

- **FAA Remote ID (Bluetooth LE)** — ~300m içindeki uyumlu drone yayınlarını tarar
- **WiFi SSID Eşleştirme** — WiFi kalıplarına göre DJI, Skydio, Parrot ve 100'den fazla drone üreticisini tanımlar
- **Görsel Algılama (ML Kit)** — Cihaz üzerinde çalışan kamera tabanlı nesne tanıma
- **Askeri Sınıflandırma** — Çağrı işareti kalıpları, squawk kodları ve operatör veritabanı (tümü yerel olarak paketlenmiş)

---

## Lisans

Bu proje **MIT Lisansı** altında lisanslanmıştır — detaylar için [LICENSE](LICENSE) dosyasına bakın.

Bu yazılımı herhangi bir amaçla kullanmakta, değiştirmekte ve dağıtmakta özgürsünüz. Sadece telif hakkı bildirimini korumanızı ve gerektiğinde kaynak göstermenizi rica ediyoruz.

---

*Merak, kod ve elime geçirebildiğimiz her yapay zeka ile yapıldı.*
*[GAMECHANGERSai](https://gamechangersai.org) tarafından sevgiyle yayınlandı.*
