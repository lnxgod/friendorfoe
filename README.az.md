🌍 [English](README.md) | [עברית](README.he.md) | [Українська](README.uk.md) | [العربية](README.ar.md) | [Türkçe](README.tr.md) | **[Azərbaycan](README.az.md)** | [Türkmen](README.tk.md) | [پښتو](README.ps.md) | [اردو](README.ur.md) | [Kurdî](README.ku.md) | [Հայերեն](README.hy.md) | [ქართული](README.ka.md) | [فارسی](README.fa.md)

# Friend or Foe — Real Vaxtda Təyyarə və Dron Müəyyənləşdirilməsi

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)

**Telefonunuzu göyə doğru yönəldin. Orada nə olduğunu bilin.**

Friend or Foe artırılmış reallıqdan istifadə edərək təyyarə və dronları real vaxtda müəyyən edən açıq mənbəli, **quraşdır-və-başla** Android tətbiqidir. ADS-B transponder məlumatlarını, FAA Remote ID dron yayımlarını, WiFi siqnal analizini və cihaz üzərində vizual aşkarlamanı birləşdirərək kamera görüntüsünün üzərinə üzən etiketlər yerləşdirir — başınızın üzərində nəyin uçduğunu, kimin idarə etdiyini, hara getdiyini və dost yoxsa düşmən olduğunu sizə deyir. Hesab, qeydiyyat və ya API açarı tələb olunmur — sadəcə quraşdırın və başlayın.

Bu layihə **süni intellekt ilə qurulub** — təkcə biri ilə deyil, hamısı ilə. Claude kodu yazdı, Grok dizaynı formalaşdırdı, Codex təhlükəsizliyi nəzərdən keçirdi və Gemini texnoloji yığını seçməyə kömək etdi. Süni intellekt insan yaradıcılığı ilə birləşdikdə nələrin mümkün olduğunu göstərmək üçün [GAMECHANGERSai](https://gamechangersai.org) tərəfindən buraxılıb. Versiya tarixçəsi üçün [CHANGELOG](CHANGELOG.md) faylına baxın.

### 72 Saatlıq Sürət Yarışı

> **Sıfırdan real cihazda təsdiqlənmiş təyyarə və dron aşkarlamalarına — 72 saatdan az müddətdə.**
>
> 12 Mart 2025-ci ildə ilk commit o axşam edildi, 13 commit və **8.500+ sətir kod** Claude ilə ~2 saatlıq süni intellekt cüt proqramlaşdırma ilə yazıldı — AR nişangah, çox mənbəli aşkarlama, Bayesian sensor sintezi və xəritə görünüşü olan qurulmağa hazır APK istehsal etdi. 14 Marta qədər tətbiq, təyyarə siluetləri, stilizə edilmiş xəritə işarəçiləri və cilalanma ilə açıq mənbə olaraq buraxıldı — Kotlin, Python və XML üzrə cəmi **22.000+ sətir**.
>
> Tətbiq **quraşdır-və-başla** — APK-nı quraşdırın, icazələri verin və təyyarələri müəyyənləşdirməyə başlayın. Server, açar və ya hesab olmadan birbaşa pulsuz ictimai ADS-B API-lərinə qoşulur. İstəyə bağlı Python arxa planı yalnız əlavə zənginləşdirmə əlavə edir (aviaşirkət adları, marşrut məlumatı).

---

## Nə Edir

- **AR Nişangahı** — Aşkar edilmiş təyyarə və dronlarda rəng kodlu üzən etiketlərlə canlı kamera görüntüsü. Etiketlər çağırış işarəsi, təyyarə tipi, hündürlük və məsafəni göstərir.
- **Çox Mənbəli Aşkarlama** — Hərtərəfli səma fərqindəliyi üçün dörd müstəqil aşkarlama metodunu birləşdirir:
  - ADS-B transponder məlumatları (kommersiya uçuşları, ümumi aviasiya)
  - Bluetooth LE vasitəsilə FAA Remote ID (uyğun dronlar)
  - WiFi SSID nümunə uyğunlaşdırması (DJI, Skydio, Parrot, 100-dən çox istehsalçı)
  - ML Kit ilə vizual aşkarlama (kamera əsaslı obyekt tanıma)
- **Ağıllı Təsnifat** — Hər şeyi 10 növə ayırır: Kommersiya, Ümumi Aviasiya, Hərbi, Helikopter, Dövlət, Təcili, Yük, Dron, Yerüstü Nəqliyyat və Naməlum. Hərbi aşkarlama çağırış işarəsi nümunələri, squawk kodları və operator verilənlər bazalarından istifadə edir.
- **Bayesian Sensor Sintezi** — Çox sayda sensor eyni obyekti aşkar etdikdə, güvən balları Bayesian log-odds istifadə edərək birləşdirilir — sadəcə "ən yüksəyi seç" deyil. İki zəif siqnalın razılığı bir güclü siqnaldan üstün ola bilər.
- **Təyyarə Siluetləri və Fotoşəkilləri** — 120-dən çox ICAO tip kodu 10 vektor siluet kateqoriyasına uyğunlaşdırılıb (dar gövdəli, geniş gövdəli, regional, turboprop, iş təyyarəsi, helikopter, qırıcı, yük, yüngül təyyarə, dron) üstəgəl ani vizual tanıma üçün 134 paketlənmiş təyyarə fotoşəkili — şəbəkə lazım deyil.
- **2D Xəritə Görünüşü** — Kateqoriya üzrə fərqli işarəçi formaları, məsafə halqaları, kompas izləmə rejimi və FOV konus örtüyü ilə OpenStreetMap.
- **Ətraflı Kartlar** — Tam detallar üçün istənilən obyektə toxunun: qeydiyyat, operator, marşrut (mənşə/təyinat), hündürlük, sürət, istiqamət, squawk kodu, aşkarlama mənbəyi və güvən səviyyəsi.
- **Tarixçə Jurnalı** — Müəyyənləşdirdiyiniz hər şeyin daimi verilənlər bazası, axtarıla və sıralana bilən.
- **Dron Arayış Bələdçisi** — Fotoşəkillər, texniki göstəricilər və təsvirlərlə 30-dan çox dron növünün daxili verilənlər bazası — DJI istehlakçı dronlarından Bayraktar TB2, MQ-9 Reaper və Shahed-136 kimi hərbi PUA-lara qədər. Kateqoriyaya görə baxın, adla axtarın və ya naməlum dron aşkarlamasından birbaşa gördüyünüzü müəyyənləşdirməyə keçin.
- **Arxa Plan Zənginləşdirməsi (İstəyə Bağlı)** — İstəyə bağlı Python API serveri aviaşirkət adları, qeydiyyat nömrələri və artıq paketlənmiş olanlardan əlavə marşrut məlumatı əlavə edə bilər. Tətbiq onsuz da tam işləyir — təyyarə fotoşəkilləri və dron arayışları APK-ya daxildir.

---

## Necə İşləyir

### Aşkarlama Mənbələri

| Mənbə | Nəyi Aşkar Edir | Mənzil | Güvən |
|-------|-----------------|--------|-------|
| **ADS-B** | Transponderi olan kommersiya və ümumi aviasiya təyyarələri | ~250 NM | Çox Yüksək (0.95) |
| **Remote ID (BLE)** | FAA uyğun dronlar (250q+) | ~300m | Yüksək (0.85) |
| **WiFi SSID** | DJI, Skydio, Parrot, büdcəli dronlar | ~100m | Orta (0.3-0.85) |
| **Vizual (ML Kit)** | Kamerada görünən hər şey | Görmə xətti | Dəyişkən |

### Sensor Sintezi və AR

Tətbiq, telefonun dəqiq olaraq hara baxdığını müəyyən etmək üçün akselerometr, maqnitometr və giroskop məlumatlarını birləşdirir. Təyyarə mövqeləri (en/uzunluq/hündürlük) haversine məsafə hesablamaları və kamera FOV həndəsəsindən istifadə edərək kamera görüntüsünə proyeksiya olunur.

**ARCore + Kompas Hibrid**: ARCore kamera yer xüsusiyyətlərini gördükdə əla izləmə təmin edir, lakin xüsusiyyətsiz səmaya yönəldildikdə çətinlik çəkir — tam da ən çox ehtiyacınız olduğunda. Tətbiq, ARCore izləməni itirdikdə avtomatik olaraq kompas-riyaziyyat istiqamətinə keçir və bütün şəraitdə kəsilməz etiketlər təmin edir.

### Memarlıq

```
┌──────────────────────────────────────────────────┐
│  Android Tətbiqi (Kotlin + Jetpack Compose)      │
│                                                  │
│  ┌─────────┐ ┌─────────┐ ┌──────────┐          │
│  │AR Görün.│ │  Xəritə │ │  Siyahı  │ ...İnterfeys│
│  └────┬────┘ └────┬────┘ └────┬─────┘          │
│       │           │           │                  │
│  ┌────┴───────────┴───────────┴─────┐           │
│  │         ViewModels (MVVM)        │           │
│  └────┬─────────────────────┬───────┘           │
│       │                     │                    │
│  ┌────┴────┐          ┌────┴──────┐             │
│  │ Sensor  │          │ Aşkarlama │             │
│  │ Sintez  │          │ Mənbələri │             │
│  │ Mühərriki│         │           │             │
│  │         │          │ • ADS-B   │             │
│  │ • ARCore│          │ • BLE RID │             │
│  │ • Aksel │          │ • WiFi    │             │
│  │ • Giro  │          │ • Vizual  │             │
│  │ • Maq   │          │           │             │
│  └─────────┘          └─────┬─────┘             │
│                             │                    │
│                    ┌────────┴────────┐           │
│                    │ Bayesian Sintez │           │
│                    │   Mühərriki     │           │
│                    └────────┬────────┘           │
│                             │                    │
└─────────────────────────────┼────────────────────┘
                              │ HTTP
                    ┌─────────┴─────────┐
                    │ Arxa Plan (FastAPI)│
                    │ • Təyyarə çəkmə   │
                    │ • Zənginləşdirmə   │
                    │ • Keş (Redis)      │
                    └─────────┬─────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
         adsb.fi     airplanes.live     OpenSky
        (əsas)        (ehtiyat)       (ehtiyat)
```

---

## Başlamaq

### Tələblər

- **Android Studio** Hedgehog (2023.1.1) və ya daha yeni
- **JDK 17**
- **Android SDK 35** (compileSdk), minSdk 26 (Android 8.0+)
- GPS, kompas və kamerası olan Android cihazı (emulyator siyahı/xəritə görünüşləri üçün işləyir, lakin AR üçün yox)

> **Hesab və ya qeydiyyat lazım deyil.** Tətbiq birbaşa pulsuz ictimai ADS-B API-lərinə qoşulur — arxa plan qurulması, API açarları, hesablar tələb olunmur.

### Sürətli Başlanğıc — Yalnız Android

Tətbiq birbaşa pulsuz ictimai ADS-B API-lərinə qoşulur — arxa plan qurulması və ya hesab lazım deyil. Təyyarə fotoşəkilləri və dron arayış bələdçisi APK-ya paketlənmişdir. Sadəcə qurmaq və işə salmaq üçün:

```bash
cd android
./gradlew assembleDebug
# Qoşulmuş cihaza quraşdır:
adb install app/build/outputs/apk/debug/app-debug.apk
```

Və ya ən son əvvəlcədən qurulmuş APK-nı [**GitHub Releases**](https://github.com/lnxgod/friendorfoe/releases) səhifəsindən yükləyin.

### Arxa Plan Quraşdırması (İstəyə Bağlı — zənginləşdirməni aktivləşdirir)

Arxa plan təyyarə fotoşəkilləri, aviaşirkət adları, qeydiyyat axtarışları və marşrut məlumatı əlavə edir. **Python 3.11+ tələb edir və isteğe bağlı olaraq keşləmə üçün Redis.**

```bash
cd backend

# Virtual mühit yarat
python -m venv .venv
source .venv/bin/activate  # və ya Windows-da .venv\Scripts\activate

# Asılılıqları quraşdır
pip install -r requirements.txt

# Mühit konfiqurasiyasını kopyala
cp .env.example .env
# OpenSky etimadnamələrini əlavə etmək istəyirsinizsə .env faylını redaktə edin (istəyə bağlı, sürət limitlərini artırır)

# Serveri işə sal
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

**Docker ilə** (Redis + PostgreSQL daxil):

```bash
cd backend
docker compose up
```

API `http://localhost:8000` ünvanında əlçatan olacaq. Sağlamlıq yoxlaması: `http://localhost:8000/health`

### Tətbiqi Arxa Planınıza Qoşma

Android tətbiqinin şəbəkə konfiqurasiyasındakı arxa plan URL-ni serverinizin IP ünvanına yönləndirin.

---

## Texnoloji Yığın

### Android Tətbiqi

| Komponent | Kitabxana | Versiya |
|-----------|-----------|---------|
| Dil | Kotlin | 1.9.22 |
| İnterfeys | Jetpack Compose + Material 3 | 2024.02.00 |
| AR | ARCore | 1.41.0 |
| Kamera | CameraX | 1.3.1 |
| ML | ML Kit Object Detection | 17.0.1 |
| Asılılıq İnyeksiyası | Hilt | 2.50 |
| HTTP | Retrofit + OkHttp | 2.9.0 / 4.12.0 |
| Verilənlər Bazası | Room | 2.6.1 |
| Şəkillər | Coil | 2.5.0 |
| Xəritələr | OSMDroid | 6.1.18 |
| Asinxron | Coroutines + Flow | 1.7.3 |

### Arxa Plan

| Komponent | Kitabxana | Versiya |
|-----------|-----------|---------|
| Çərçivə | FastAPI | 0.115.6 |
| Server | Uvicorn | 0.34.0 |
| HTTP Müştərisi | httpx | 0.28.1 |
| Keş | Redis | 5.2.1 |
| Verilənlər Bazası | SQLAlchemy + asyncpg | 2.0.36 |
| Doğrulama | Pydantic | 2.10.4 |

---

## Layihə Strukturu

```
friendorfoe/
├── android/                           # Android tətbiqi
│   └── app/src/main/java/com/friendorfoe/
│       ├── data/
│       │   ├── local/                 # Room verilənlər bazası (tarixçə, izləmə)
│       │   ├── remote/                # Retrofit API xidmətləri və DTO-lar
│       │   └── repository/            # Verilənlər depoları
│       ├── detection/                 # Aşkarlama mühərrikləri
│       │   ├── AdsbPoller.kt          # ADS-B məlumat sorğusu
│       │   ├── RemoteIdScanner.kt     # BLE Remote ID skan
│       │   ├── WifiDroneScanner.kt    # WiFi SSID aşkarlaması
│       │   ├── BayesianFusionEngine.kt # Çox sensorlu güvən sintezi
│       │   ├── MilitaryClassifier.kt  # Hərbi təyyarə müəyyənləşdirilməsi
│       │   └── VisualDetection*.kt    # ML Kit vizual aşkarlama
│       ├── domain/model/              # Domen modelləri (Aircraft, Drone, SkyObject)
│       ├── di/                        # Hilt asılılıq inyeksiyası
│       ├── presentation/
│       │   ├── ar/                    # AR nişangah ekranı
│       │   ├── map/                   # OpenStreetMap görünüşü
│       │   ├── list/                  # Sıralana bilən obyekt siyahısı
│       │   ├── detail/                # Tam obyekt ətraflı kartları
│       │   ├── drones/                # Dron arayış bələdçisi brauzeri
│       │   ├── history/               # Aşkarlama tarixçəsi
│       │   ├── welcome/               # Xoş gəldiniz/başlanğıc ekranı
│       │   ├── about/                 # Tətbiq məlumatı
│       │   └── util/                  # AircraftPhotos, CategoryColors, DroneDatabase
│       └── sensor/                    # Sensor sintezi və mövqeləşdirmə
├── backend/                           # Python FastAPI serveri
│   └── app/
│       ├── main.py                    # FastAPI giriş nöqtəsi
│       ├── config.py                  # Mühit konfiqurasiyası
│       ├── routers/aircraft.py        # API son nöqtələri
│       └── services/
│           ├── adsb.py                # Çox mənbəli ADS-B çəkmə
│           └── enrichment.py          # Təyyarə məlumat zənginləşdirməsi
├── images/                            # Təyyarə istinad fotoşəkilləri (Wikimedia CC)
├── scripts/                           # Yardımçı skriptlər (fotoşəkil yükləyici)
├── macos/                             # macOS yardımçı tətbiq (erkən mərhələ)
└── docs/                              # Dizayn sənədləri
```

---

## API Son Nöqtələri

| Metod | Son Nöqtə | Təsvir |
|-------|-----------|--------|
| `GET` | `/health` | Sağlamlıq yoxlaması (Redis, DB statusu) |
| `GET` | `/aircraft/nearby?lat=&lon=&radius_nm=` | Bir mövqeyə yaxın təyyarələr |
| `GET` | `/aircraft/{icao_hex}/detail` | Zənginləşdirilmiş təyyarə detalları |

---

## Töhfə Vermə

Töhfələr alqışlanır! İstər xəta düzəlişləri, istər yeni aşkarlama metodları, interfeys təkmilləşdirmələri və ya sənədləşdirmə olsun — köməyinizi istəyirik.

1. Deponu çəngəlləyin (fork)
2. Xüsusiyyət budağı yaradın (`git checkout -b feature/mohtesem-xususiyyet`)
3. Dəyişikliklərinizi qeyd edin (`git commit -m 'Möhtəşəm xüsusiyyət əlavə et'`)
4. Budağı göndərin (`git push origin feature/mohtesem-xususiyyet`)
5. Pull Request açın

### Töhfə Verənlər üçün Fikirlər

- Əlavə təyyarə tip kodu uyğunlaşdırmaları
- Təkmilləşdirilmiş WiFi dron istehsalçı barmaq izləri
- AR örtüyü üçün gecə rejimi / tünd mövzu
- Uçuş yolu proqnozu və trayektoriya vizuallaşdırması
- Sosial xüsusiyyətlər (müşahidələri paylaşma, icma hesabatları)
- macOS Swift təməlindən istifadə edərək iOS portu
- Əlavə ADS-B verilənlər mənbələri ilə inteqrasiya

---

## Süni İntellekt ilə Qurulub

Bu layihə yalnız bir süni intellektlə deyil — **hamısı** ilə qurulub. Hər böyük süni intellekt platformasını sınadıq və hər birini ən güclü olduğu yerdə istifadə etdik. Git tarixçəsi hekayəni danışır:

| Tarix | Nə Baş Verdi |
|-------|---------------|
| **12 Mart** | 13 commit edildi — ~2 saatlıq süni intellekt cüt proqramlaşdırma ilə **8.500+ sətir kod** yazıldı. Nəticə: AR nişangah, dörd aşkarlama mənbəyi, Bayesian sensor sintezi, xəritə görünüşü və tarixçə izləmə ilə qurulmağa hazır APK. |
| **14 Mart** | Açıq mənbə buraxılışı. 120-dən çox təyyarə silueti, kateqoriya formaları ilə stilizə edilmiş xəritə işarəçiləri, icazə idarəetmə cilalanması və təhlükəsizlik nəzərdən keçirilməsi əlavə edildi. Sonra: 134 təyyarə fotoşəkili oflayn aktivlər olaraq paketləndi, yeniləmə yoxlayıcısı ilə xoş gəldiniz ekranı və Coil şəkil yükləmə əlavə edildi. |
| **Cəmi** | **23.000+ sətir** Kotlin, Python və XML. Cihazda kommersiya təyyarələri və dronların real dünya aşkarlamaları təsdiqləndi. |

### Süni İntellekt Rolları

| Süni İntellekt | Rol |
|----------------|-----|
| **[Claude](https://claude.ai)** (Anthropic) | Əsas kodlaşdırma agenti — memarlıq, tətbiq, cüt proqramlaşdırma |
| **[Grok](https://grok.com)** (xAI) | Dizayn istiqaməti və araşdırma |
| **[Codex](https://chatgpt.com)** (OpenAI) | Təhlükəsizlik nəzərdən keçirilməsi və məsləhət |
| **[Gemini](https://gemini.google.com)** (Google) | Texnoloji yığın araşdırması — kitabxanaları, çərçivələri və yanaşmaları qiymətləndirmə |
| **[ML Kit](https://developers.google.com/ml-kit)** (Google) | Cihaz üzərində vizual obyekt aşkarlaması (birbaşa telefonda işləyir) |

**Vibe coding nədir?** İnsan və süni intellektin real vaxtda birlikdə qurduğu əməkdaşlıq, söhbət əsaslı proqram təminatı inkişafı yanaşmasıdır. Hər sətri əl ilə yazmaq əvəzinə, nə istədiyinizi təsvir edir, fikirlər üzərində təkrarlayırsınız, birlikdə xətaları düzəldir və süni intellektin şablon kodları həll etməsinə icazə verirsiniz, siz isə vizyona və memarlığa diqqət yetirirsiniz. Hissiyyatla proqramlaşdırma — və işləyir.

Friend or Foe, hər biri ən yaxşı olduğu sahədə töhfə verən çoxlu süni intellektlə qurulub. Claude, ilk memarlıqdan sensor sintez alqoritmlərinə, Bayesian riyaziyyatına və vektor çəkilə bilən sənət əsərlərinə qədər əsas kodlaşdırma tərəfdaşı olaraq xidmət etdi. Grok dizayn istiqamətini formalaşdırmağa kömək etdi. Codex açıq mənbə buraxılışından əvvəl təhlükəsizlik auditini həyata keçirdi. Gemini hansı texnologiyaların və çərçivələrin istifadə olunacağını araşdırdı. Və ML Kit, heç bir bulud asılılığı olmadan vizual aşkarlamanı gücləndirərək cihaz üzərində işləyir. Bu depodakı hər fayl insan-süni intellekt əməkdaşlığı ilə formalaşdırılıb.

**Proqram təminatı inkişafının gələcəyinin doğru iş üçün doğru süni intellekt istifadə etmək olduğuna inanırıq** və bunun praktikada necə göründüyünü görə bilməyiniz üçün bu layihəni açıq mənbə olaraq buraxırıq.

---

## GAMECHANGERSai Haqqında

**[GAMECHANGERSai](https://gamechangersai.org)** uşaqlar və ailələr üçün praktik, süni intellekt dəstəkli öyrənmə təcrübələri yaratmağa həsr olunmuş 501(c)(3) qeyri-kommersiya təşkilatıdır.

> *"Hər öyrənəni qurmağı və yenidən düzəltməyi öyrədən əyləncəli süni intellekt oyunları."*

Platformamız vasitəsilə öyrənənlər real, köçürülə bilən bacarıqlar inkişaf etdirirlər:

- **Yaradıcı Kodlaşdırma** — Süni intellekti yardımçı pilot olaraq istifadə edərək funksional proqramlar qurmaq
- **Sistem Düşüncəsi** — Mürəkkəb hissələrin necə qarşılıqlı təsir etdiyini və onları necə düzəltməyi anlamaq
- **Resurs İdarəetməsi** — Strateji planlaşdırma və optimallaşdırma
- **Etik Mühakimə** — Süni intellekt istifadə edərkən "edə bilərikmi?"nin yanında "etməliyikmi?" sualını verməyi öyrənmək

**Friend or Foe bu bacarıqların real olduğunun sübutudur.** Uşaqların platformamızda öyrəndiyi eyni yaradıcı kodlaşdırma, sistem düşüncəsi və süni intellekt əməkdaşlığı bu tam funksional təyyarə müəyyənləşdirmə sistemini qurmaq üçün istifadə edildi. Oyun olaraq başlayan alət olur. Əyləncə olaraq başlayan təcrübə olur.

Açıq mənbə icmasına töhfə verməyə sadiqik. Friend or Foe-nu ictimaiyyətə buraxaraq ümid edirik ki:

- Proqramçıları və öyrənənləri süni intellekt dəstəkli inkişafla nələrin mümkün olduğunu araşdırmağa **ilham vermək**
- Süni intellektin əvəzedici deyil, yaradıcı tərəfdaş olduğunu **nümayiş etdirmək** — bu tətbiqdəki hər qərar insan mühakiməsi ilə istiqamətləndirildi
- Uşaqlarımızın öyrəndiyi bacarıqların yalnız oyunlar üçün olmadığını — real, faydalı texnologiya qurmağın təməli olduğunu **göstərmək**

Missiyamız haqqında daha çox öyrənmək üçün **[gamechangersai.org](https://gamechangersai.org)** ünvanını ziyarət edin.

---

## Verilənlər Mənbələri və İstinad

Friend or Foe istifadəsi 100% pulsuzdur — aşağıdakı hər verilənlər mənbəyi açıqdır və **API açarı və ya ödənişli hesab tələb etmir**.

### ADS-B Təyyarə Mövqe Məlumatları

Tətbiq, bir mənbə çökdükdə belə həmişə təyyarə məlumatı almağınızı təmin edən üç pilləli ehtiyat zənciri istifadə edir:

| Prioritet | Mənbə | API Son Nöqtəsi | Nə Təmin Edir | Kimlik Doğrulama |
|-----------|-------|-----------------|---------------|------------------|
| 1-ci | **[adsb.fi](https://adsb.fi)** | `opendata.adsb.fi/api` | Real vaxt təyyarə mövqeləri, çağırış işarələri, hündürlük, sürət, istiqamət, squawk kodları | Pulsuz, açar yox |
| 2-ci | **[airplanes.live](https://airplanes.live)** | `api.airplanes.live` | adsb.fi ilə eyni məlumat (uyğun ADSBx v2 formatı) | Pulsuz, açar yox |
| 3-cü | **[OpenSky Network](https://opensky-network.org)** | `opensky-network.org/api` | ICAO vəziyyət vektorları — mövqe, sürət, istiqamət, şaquli sürət | Pulsuz, açar yox (daha yüksək sürət limitləri üçün istəyə bağlı hesab) |

Hər üç mənbə dünya üzrə qəbuledici şəbəkələrdən canlı ADS-B transponder məlumatları təmin edir. Tətbiq GPS koordinatları və radiusa görə sorğu edir və uğursuzluq və ya vaxt aşımında avtomatik olaraq növbəti mənbəyə keçir.

### Zənginləşdirmə və Dəstəkləyici Verilənlər

| Mənbə | Nə Təmin Edir | Kimlik Doğrulama |
|-------|---------------|------------------|
| **[Planespotters.net](https://planespotters.net)** | ICAO hex kodu ilə təyyarə fotoşəkilləri — ətraflı kartlarda göstərilir | Pulsuz, açar yox |
| **[Open-Meteo](https://open-meteo.com)** | Cari hava (bulud örtüyü, külək sürəti, şərait) — aşkarlama parametrlərini tənzimləmək üçün istifadə olunur | Pulsuz, açar yox |
| **[OpenStreetMap](https://openstreetmap.org)** | 2D xəritə görünüşü üçün OSMDroid vasitəsilə xəritə kafelləri | Pulsuz, açar yox |

### Cihaz Üzərində Aşkarlama (Xarici API Yoxdur)

Bu aşkarlama metodları tamamilə telefonda işləyir, şəbəkə zəngləri yoxdur:

- **FAA Remote ID (Bluetooth LE)** — ~300m daxilində uyğun dron yayımlarını skan edir
- **WiFi SSID Uyğunlaşdırma** — WiFi nümunələrinə görə DJI, Skydio, Parrot və 100-dən çox digər dron istehsalçısını müəyyən edir
- **Vizual Aşkarlama (ML Kit)** — Cihaz üzərində işləyən kamera əsaslı obyekt tanıma
- **Hərbi Təsnifat** — Çağırış işarəsi nümunələri, squawk kodları və operator verilənlər bazası (hamısı yerli olaraq paketlənmiş)

---

## Lisenziya

Bu layihə **MIT Lisenziyası** altında lisenziyalanıb — ətraflı məlumat üçün [LICENSE](LICENSE) faylına baxın.

Bu proqram təminatını istənilən məqsədlə istifadə etmək, dəyişdirmək və yaymaq azadsınız. Yalnız müəllif hüququ bildirişini saxlamağınızı və lazım olduqda mənbə göstərməyinizi xahiş edirik.

---

*Maraq, kod və əlimizə keçirə bildiyimiz hər süni intellekt ilə hazırlandı.*
*[GAMECHANGERSai](https://gamechangersai.org) tərəfindən sevgi ilə buraxıldı.*
