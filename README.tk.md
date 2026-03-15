🌍 [English](README.md) | [עברית](README.he.md) | [Українська](README.uk.md) | [العربية](README.ar.md) | [Türkçe](README.tr.md) | [Azərbaycan](README.az.md) | **[Türkmen](README.tk.md)** | [پښتو](README.ps.md) | [اردو](README.ur.md) | [Kurdî](README.ku.md) | [Հայերեն](README.hy.md) | [ქართული](README.ka.md) | [فارسی](README.fa.md)

# Friend or Foe — Hakyky wagtda uçarlary we dronlary kesgitlemek

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)

**Telefonyňyzy asmana tarap tutup görüň. Ol ýerde näme baryny biliň.**

Friend or Foe — bu açyk çeşme, **gurnap-we-başla** Android programmasy bolup, giňeldilen hakykat ulanyp uçarlary we dronlary hakyky wagtda kesgitleýär. Ol ADS-B transponder maglumatlaryny, FAA Remote ID dron ýaýlymlaryny, WiFi signal seljerişini we enjamyň üstündäki wizual kesgitleýşi birleşdirip, kamera görnüşiniň üstünde ýüzýän belgileri görkezýär — başyňyzyň üstünde näme uçýandygyny, kime degişlidigini, nirä barýandygyny we dostuňyzmy ýa-da duşmanyňyzmy aýdýar. Hasap, hasaba alyş ýa-da API açary gerek däl — diňe gurnaň we başlaň.

Bu taslama **emeli aň bilen guruldy** — diňe biri bilen däl, hemmesi bilen. Claude kody ýazdy, Grok dizaýny kesgitledi, Codex howpsuzlygy bardy, Gemini tehnologiýa toplumyny saýlamaga kömek etdi. Emeli aňyň adam döredijiligine duşanda nämäniň mümkindigini görkezmek üçin [GAMECHANGERSai](https://gamechangersai.org) tarapyndan çykaryldy. Wersiýa taryhy üçin [CHANGELOG](CHANGELOG.md) serediň.

### 72 sagatlyk tizlik ýaryşy

> **Noldan hakyky enjamda tassyklanan uçar we dron kesgitlemelerine — 72 sagatdan az wagtda.**
>
> 2025-nji ýylyň 12-nji martynda ilkinji commit şol agşam edildi, 13 commit we **8,500+ setir kod** Claude bilen ~2 sagatlyk emeli aň jübüt programmirleme arkaly ýazyldy — AR wizör, köp çeşme kesgitlemek, Baýes sensor birleşmesi we karta görnüşi bolan gurnamaga taýýar APK öndürildi. 14-nji marta çenli programma uçar siluetleri, bezelen karta belgileri we jylalama bilen açyk çeşme hökmünde neşir edildi — Kotlin, Python we XML boýunça jemi **22,000+ setir**.
>
> Programma **gurnap-we-başla** — APK-ny gurnaň, rugsatlary beriň we uçarlary kesgitläp başlaň. Hasaba alynmak, açar ýa-da hasap zerurlygy bolmazdan mugt jemgyýetçilik ADS-B API-lerine gönüden-göni birikýär. Islege bagly Python arka tarapy diňe goşmaça baýlaşdyrma goşýar (awiakompaniýa atlary, ugur maglumaty).

---

## Näme edýär

- **AR wizör** — Kesgitlenen uçarlarda we dronlarda reňk kodly ýüzýän belgiler bilen göni kamera görnüşi. Belgiler çagyryş belgisini, uçar görnüşini, beýikligi we aralygy görkezýär.
- **Köp çeşme kesgitlemek** — Giň asman habardarlygy üçin dört garaşsyz kesgitleme usulyny birleşdirýär:
  - ADS-B transponder maglumatlary (täjirçilik uçuşlary, umumy awiasiýa)
  - Bluetooth LE arkaly FAA Remote ID (laýyk dronlar)
  - WiFi SSID nusga deňeşdirmesi (DJI, Skydio, Parrot, 100-den gowrak öndüriji)
  - ML Kit bilen wizual kesgitlemek (kamera esasly obýekt tanamak)
- **Akylly klassifikasiýa** — Hemme zady 10 görnüşe bölýär: Täjirçilik, Umumy Awiasiýa, Harby, Dikuçar, Hökümet, Gyssagly, Ýük, Dron, Ýer ulagysy we Näbelli. Harby kesgitlemek çagyryş belgisi nusgalaryny, squawk kodlaryny we operator maglumat bazalaryny ulanýar.
- **Baýes sensor birleşmesi** — Birnäçe sensor şol bir obýekti kesgitlände, ynam ballary Baýes log-odds ulanylyp birleşdirilýär — diňe "iň ýokarysyny saýla" däl. Ylalaşýan iki gowşak signal bir güýçli signaldan ağyr basýar.
- **Uçar siluetleri we suratlary** — 120-den gowrak ICAO görnüş kody 10 wektor siluet kategoriýasyna (inçe gövde, giň gövde, sebitlik, turboprop, işewürlik jeti, dikuçar, söweşiji, ýük, ýeňil uçar, dron) eşlenýär we gyssagly wizual tanamak üçin 134 goşulan uçar suraty — tor gerek däl.
- **2D karta görnüşi** — Her kategoriýa üçin tapawutly belgi şekilleri, aralyk halkalary, kompas yzarlaýyş tertibi we FOV konus örtügi bilen OpenStreetMap.
- **Jikme-jik kartlar** — Doly maglumatlar üçin islendik obýekte basyň: hasaba alyş, operator, ugur (çykyş/baryş), beýiklik, tizlik, ugur, squawk kody, kesgitleme çeşmesi we ynam derejesi.
- **Taryh ýazgysy** — Kesgitlän ähli zadyňyzyň hemişelik maglumat bazasy, gözlenip we tertiplenip bolýar.
- **Dron salgylanma gollanmasy** — Suratlar, tehniki häsiýetler we düşündirişler bilen 30-dan gowrak dron görnüşiniň goşulan maglumat bazasy — DJI sarp ediji dronlaryndan Bayraktar TB2, MQ-9 Reaper we Shahed-136 ýaly harby pilotsyz uçuş enjamlaryna çenli. Kategoriýa boýunça göz aýlaň, at boýunça gözläň ýa-da näbelli dron kesgitlemesinden göni näme görýändigiňizi kesgitlemäge geçiň.
- **Arka tarap baýlaşdyrma (islege bagly)** — Islege bagly Python API serweri awiakompaniýa atlaryny, hasaba alyş belgilerini we ugur maglumatyny goşup biler. Programma onsuz doly işleýär — uçar suratlary we dron salgylanmalary APK-a goşulan.

---

## Nähili işleýär

### Kesgitleme çeşmeleri

| Çeşme | Näme kesgitleýär | Aralyk | Ynam |
|-------|-----------------|--------|------|
| **ADS-B** | Transponderli täjirçilik we GA uçarlary | ~250 NM | Gaty ýokary (0.95) |
| **Remote ID (BLE)** | FAA laýyk dronlar (250g+) | ~300m | Ýokary (0.85) |
| **WiFi SSID** | DJI, Skydio, Parrot, býudjet dronlar | ~100m | Orta (0.3-0.85) |
| **Wizual (ML Kit)** | Kamerada görünýän islendik zat | Göni görüş | Üýtgeýän |

### Sensor birleşmesi we AR

Programma telefonyň anyk nirede durýandygyny kesgitlemek üçin akselerometr, magnetometr we giroskop maglumatlaryny birleşdirýär. Uçar ýerleri (giňişlik/uzynlyk/beýiklik) haversine aralyk hasaplamalary we kamera FOV geometriýasy ulanylyp kamera görnüşine proýektirlenýär.

**ARCore + Kompas gibrid**: ARCore kamera ýer aýratynlyklaryny görende ajaýyp yzarlaýyş üpjün edýär, ýöne aýratynlyksyz asmana tarap görkezilende kynçylyk çekýär — edil iň köp gerek wagtyňyz. Programma ARCore yzarlamagy ýitirende awtomatiki usulda kompas-matematika ugrukmasyna geçýär we ähli şertlerde üznüksiz belgileri üpjün edýär.

### Arhitektura

```
┌──────────────────────────────────────────────────┐
│  Android App (Kotlin + Jetpack Compose)          │
│                                                  │
│  ┌─────────┐ ┌─────────┐ ┌──────────┐          │
│  │ AR View │ │   Map   │ │   List   │  ...UI   │
│  └────┬────┘ └────┬────┘ └────┬─────┘          │
│       │           │           │                  │
│  ┌────┴───────────┴───────────┴─────┐           │
│  │         ViewModels (MVVM)        │           │
│  └────┬─────────────────────┬───────┘           │
│       │                     │                    │
│  ┌────┴────┐          ┌────┴──────┐             │
│  │ Sensor  │          │ Detection │             │
│  │ Fusion  │          │  Sources  │             │
│  │ Engine  │          │           │             │
│  │         │          │ • ADS-B   │             │
│  │ • ARCore│          │ • BLE RID │             │
│  │ • Accel │          │ • WiFi    │             │
│  │ • Gyro  │          │ • Visual  │             │
│  │ • Mag   │          │           │             │
│  └─────────┘          └─────┬─────┘             │
│                             │                    │
│                    ┌────────┴────────┐           │
│                    │ Bayesian Fusion │           │
│                    │   Engine        │           │
│                    └────────┬────────┘           │
│                             │                    │
└─────────────────────────────┼────────────────────┘
                              │ HTTP
                    ┌─────────┴─────────┐
                    │  Backend (FastAPI) │
                    │  • Aircraft fetch  │
                    │  • Enrichment      │
                    │  • Caching (Redis) │
                    └─────────┬─────────┘
                              │
              ┌───────────────┼───────────────┐
              │               │               │
         adsb.fi     airplanes.live     OpenSky
        (esasy)      (ätiýaçlyk)     (ätiýaçlyk)
```

---

## Başlamak

### Öňünden zerur zatlar

- **Android Studio** Hedgehog (2023.1.1) ýa-da soňky
- **JDK 17**
- **Android SDK 35** (compileSdk) minSdk 26 (Android 8.0+) bilen
- GPS, kompas we kamerasy bolan Android enjam (emuliator sanaw/karta görnüşleri üçin işleýär, ýöne AR üçin däl)

> **Hasap ýa-da hasaba alyş gerek däl.** Programma gönüden-göni mugt jemgyýetçilik ADS-B API-lerine birikýär — arka tarap gurmak, API açarlary, hasaplar gerek däl.

### Çalt başlamak — diňe Android

Programma gönüden-göni mugt jemgyýetçilik ADS-B API-lerine birikýär — arka tarap gurmak ýa-da hasap gerek däl. Uçar suratlary we dron salgylanma gollanmasy APK-a goşulan. Gurnamak we işletmek üçin:

```bash
cd android
./gradlew assembleDebug
# Birikdirilen enjama gurmak:
adb install app/build/outputs/apk/debug/app-debug.apk
```

Ýa-da iň soňky öňünden gurlan APK-ny [**GitHub Releases**](https://github.com/lnxgod/friendorfoe/releases) sahypasyndan ýükläp alyň.

### Arka tarapy gurmak (islege bagly — baýlaşdyrmagy açýar)

Arka tarap uçar suratlaryny, awiakompaniýa atlaryny, hasaba alyş gözleglerini we ugur maglumatyny goşýar. **Python 3.11+ we islege bagly keşlemek üçin Redis gerek.**

```bash
cd backend

# Wirtual gurşaw döretmek
python -m venv .venv
source .venv/bin/activate  # ýa-da Windows-da .venv\Scripts\activate

# Baglylyklary gurmak
pip install -r requirements.txt

# Gurşaw sazlamasyny göçürmek
cp .env.example .env
# OpenSky şahsyýet maglumatlaryny goşmak isleseňiz .env faýlyny redaktirläň (islege bagly)

# Serweri işletmek
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

**Docker bilen** (Redis + PostgreSQL goşulan):

```bash
cd backend
docker compose up
```

API `http://localhost:8000` salgysynda elýeterli bolar. Saglyk barlagy: `http://localhost:8000/health`

### Programmany arka tarapa birikdirmek

Android programmasynyň tor sazlamasynda arka tarap URL-ni serweriňiziň IP salgysyna görkezmek üçin täzeläň.

---

## Tehnologiýa toplumy

### Android programmasy

| Komponent | Kitaphana | Wersiýa |
|-----------|-----------|---------|
| Dil | Kotlin | 1.9.22 |
| UI | Jetpack Compose + Material 3 | 2024.02.00 |
| AR | ARCore | 1.41.0 |
| Kamera | CameraX | 1.3.1 |
| ML | ML Kit Object Detection | 17.0.1 |
| DI | Hilt | 2.50 |
| HTTP | Retrofit + OkHttp | 2.9.0 / 4.12.0 |
| Maglumat bazasy | Room | 2.6.1 |
| Suratlar | Coil | 2.5.0 |
| Kartalar | OSMDroid | 6.1.18 |
| Asinkhron | Coroutines + Flow | 1.7.3 |

### Arka tarap

| Komponent | Kitaphana | Wersiýa |
|-----------|-----------|---------|
| Çarçuwa | FastAPI | 0.115.6 |
| Serwer | Uvicorn | 0.34.0 |
| HTTP müşderi | httpx | 0.28.1 |
| Keş | Redis | 5.2.1 |
| Maglumat bazasy | SQLAlchemy + asyncpg | 2.0.36 |
| Barlag | Pydantic | 2.10.4 |

---

## Taslama gurluşy

```
friendorfoe/
├── android/                           # Android programmasy
│   └── app/src/main/java/com/friendorfoe/
│       ├── data/
│       │   ├── local/                 # Room maglumat bazasy (taryh, yzarlamak)
│       │   ├── remote/                # Retrofit API hyzmatlary we DTO-lar
│       │   └── repository/            # Maglumat ammarlar
│       ├── detection/                 # Kesgitleme motorlary
│       │   ├── AdsbPoller.kt          # ADS-B maglumat soragysy
│       │   ├── RemoteIdScanner.kt     # BLE Remote ID skanirlemek
│       │   ├── WifiDroneScanner.kt    # WiFi SSID kesgitlemek
│       │   ├── BayesianFusionEngine.kt # Köp sensor ynam birleşmesi
│       │   ├── MilitaryClassifier.kt  # Harby uçar kesgitlemek
│       │   └── VisualDetection*.kt    # ML Kit wizual kesgitlemek
│       ├── domain/model/              # Domen modelleri (Aircraft, Drone, SkyObject)
│       ├── di/                        # Hilt baglylyk sanjymy
│       ├── presentation/
│       │   ├── ar/                    # AR wizör ekrany
│       │   ├── map/                   # OpenStreetMap görnüşi
│       │   ├── list/                  # Tertiplenip bolýan obýekt sanawy
│       │   ├── detail/                # Doly obýekt jikme-jik kartlary
│       │   ├── drones/                # Dron salgylanma gollanma brauzeri
│       │   ├── history/               # Kesgitleme taryhy
│       │   ├── welcome/               # Garşylama/başlatma ekrany
│       │   ├── about/                 # Programma maglumaty
│       │   └── util/                  # AircraftPhotos, CategoryColors, DroneDatabase
│       └── sensor/                    # Sensor birleşmesi we ýerleşdirmek
├── backend/                           # Python FastAPI serwer
│   └── app/
│       ├── main.py                    # FastAPI giriş nokady
│       ├── config.py                  # Gurşaw sazlamasy
│       ├── routers/aircraft.py        # API gutarnykly nokatlary
│       └── services/
│           ├── adsb.py                # Köp çeşme ADS-B çekmek
│           └── enrichment.py          # Uçar maglumat baýlaşdyrma
├── images/                            # Uçar salgylanma suratlary (Wikimedia CC)
├── scripts/                           # Kömekçi skriptler (surat ýükleýji)
├── macos/                             # macOS ýoldaş programma (irki tapgyr)
└── docs/                              # Dizaýn resminamalary
```

---

## API gutarnykly nokatlary

| Usul | Gutarnykly nokat | Düşündiriş |
|------|-----------------|------------|
| `GET` | `/health` | Saglyk barlagy (Redis, DB ýagdaýy) |
| `GET` | `/aircraft/nearby?lat=&lon=&radius_nm=` | Ýere ýakyn uçarlar |
| `GET` | `/aircraft/{icao_hex}/detail` | Baýlaşdyrylan uçar jikme-jiklikleri |

---

## Goşant goşmak

Goşantlar hoş garşylanýar! Ýalňyşlyk düzetmeleri, täze kesgitleme usullary, UI gowulandyrmalary ýa-da resminama bolsun — siziň kömegňizi isleýäris.

1. Ammary çatallanyň (fork)
2. Aýratynlyk şahasy dörediň (`git checkout -b feature/amazing-feature`)
3. Üýtgeşmeleriňizi ýazga geçiriň (`git commit -m 'Add amazing feature'`)
4. Şahany iberiň (`git push origin feature/amazing-feature`)
5. Pull Request açyň

### Goşant goşujylar üçin pikirler

- Goşmaça uçar görnüş kody eşlemeleri
- Gowulandyrylan WiFi dron öndüriji barmak yzlary
- AR örtügi üçin gije tertibi / garaňky tema
- Uçuş ýoly çaklamasy we traýektoriýa görkezilişi
- Jemgyýetçilik aýratynlyklary (synlary paýlaşmak, jemgyýet hasabatlary)
- macOS Swift esasynda iOS porty
- Goşmaça ADS-B maglumat çeşmeleri bilen integrasiýa

---

## Emeli aň bilen guruldy

Bu taslama diňe bir emeli aň bilen gurulmady — **hemmesi bilen** guruldy. Her esasy emeli aň platformasyny synap gördük we her birini iň güýçli ýerinde ulandyk. Git taryhy hekaýany gürrüň berýär:

| Sene | Näme boldy |
|------|-----------|
| **12-nji mart** | 13 commit goýuldy — ~2 sagatlyk emeli aň jübüt programmirlemesinde **8,500+ setir kod** ýazyldy. Netije: AR wizör, dört kesgitleme çeşmesi, Baýes sensor birleşmesi, karta görnüşi we taryh yzarlamasy bilen gurnamaga taýýar APK. |
| **14-nji mart** | Açyk çeşme neşiri. 120-den gowrak uçar siluetleri, kategoriýa şekilleri bilen bezelen karta belgileri, rugsat dolandyryşy jylalamasy we howpsuzlyk barlagy goşuldy. Soňra: 134 uçar suraty awtonom aktiwler hökmünde goşuldy, täzeleme barlaýjysy bolan garşylama ekrany we Coil surat ýüklemesi goşuldy. |
| **Jemi** | **23,000+ setir** Kotlin, Python we XML. Enjamda täjirçilik uçarlarynyň we dronlarynyň hakyky dünýä kesgitlemeleri tassyklandy. |

### Emeli aň rollary

| Emeli aň | Roly |
|----------|------|
| **[Claude](https://claude.ai)** (Anthropic) | Esasy kodlaşdyrma agenti — arhitektura, durmuşa geçirmek, jübüt programmirleme |
| **[Grok](https://grok.com)** (xAI) | Dizaýn ugry we barlag |
| **[Codex](https://chatgpt.com)** (OpenAI) | Howpsuzlyk barlagy we maslahat beriş |
| **[Gemini](https://gemini.google.com)** (Google) | Tehnologiýa toplumy barlagy — kitaphanalary, çarçuwalary we çemeleşmeleri baha bermek |
| **[ML Kit](https://developers.google.com/ml-kit)** (Google) | Enjamda wizual obýekt kesgitlemek (gönüden-göni telefonda işleýär) |

**Vibe coding näme?** Bu adam we emeli aňyň hakyky wagtda bilelikde gurýan, hyzmatdaşlyk, söhbet esasly programma üpjünçiligini ösdürmek çemeleşmesidir. Her setiri el bilen ýazmagyň ýerine, näme isleýändigiňizi suratlandyrýarsyňyz, pikirleri gaýtalaýarsyňyz, bilelikde ýalňyşlyklary düzedýärsiňiz we siz görnüşe we arhitektura üns berýän wagtyňyz emeli aňyň şablon kody ýerine ýetirmegine ýol berýärsiňiz. Bu duýgy boýunça programmirleme — we işleýär.

Friend or Foe birnäçe emeli aň bilen guruldy, her biri iň güýçli ýerinde goşant goşdy. Claude ilkinji arhitekturadan sensor birleşme algoritmlerine, Baýes matematikasyna we wektor çyzgysy sungatyna çenli esasy kodlaşdyrma hyzmatdaşy bolup hyzmat etdi. Grok dizaýn ugruny kesgitlemäge kömek etdi. Codex açyk çeşme neşirinden öň howpsuzlyk auditini geçirdi. Gemini haýsy tehnologiýalary we çarçuwalary ulanmalydygyny bardy. ML Kit bolsa enjamda işleýär, hiç hili bulut garaşlylygy bolmazdan wizual kesgitlemäni güýçlendirýär. Bu ammardaky her faýl adam-emeli aň hyzmatdaşlygy bilen şekillendirildi.

**Programma üpjünçiligini ösdürmegiň geljeginiň dogry iş üçin dogry emeli aňy ulanmakdygyna ynanýarys** we muny amaly ýagdaýda görüp bilmegiňiz üçin bu taslamany açyk çeşme hökmünde çykarýarys.

---

## GAMECHANGERSai hakynda

**[GAMECHANGERSai](https://gamechangersai.org)** çagalar we maşgalalar üçin amaly, emeli aň esasly okuw tejribelerini döretmäge bagyşlanan 501(c)(3) peýdasyz guramadyr.

> *"Her öwrenijä gurmagy we remiks etmegi öwredýän oýunly emeli aň oýunlary."*

Platformamyz arkaly öwrenijiler hakyky, geçirip bolýan başarnyklary ösdürýärler:

- **Döredijilik kodlaşdyrma** — Emeli aňy ikinji pilot hökmünde ulanyp işleýän programmalary gurmak
- **Ulgam pikirlenmesi** — Çylşyrymly bölekleriň nähili täsir edýändigini we olary nähili düzetmelidigini düşünmek
- **Çeşme dolandyryşy** — Strategiki meýilleşdirmek we optimizasiýa
- **Ahlak pikirlenmesi** — Emeli aňy ulananda "edip bilerismi?" bilen birlikde "etmeli mi?" diýip soramagy öwrenmek

**Friend or Foe bu başarnyklaryň hakykydygynyň subutnamasydyr.** Çagalaryň platformamyzda öwrenýän şol döredijilik kodlaşdyrma, ulgam pikirlenmesi we emeli aň hyzmatdaşlygy bu doly işleýän uçar kesgitleme ulgamyny gurmak üçin ulanyldy. Oýun hökmünde başlaýan zat gurala öwrülýär. Oýun hökmünde başlaýan zat tejribä öwrülýär.

Açyk çeşme jemgyýetçiligine gaýtaryp bermäge borçlanýarys. Friend or Foe-ny köpçülige elýeterli edip neşir etmek bilen, umyt edýäris:

- Ösdürijileri we öwrenijileri emeli aň kömekli ösüş bilen nämäniň mümkindigini öwrenmäge **ylham bermek**
- Emeli aňyň ornuny tutujy däl-de, döredijilik hyzmatdaşydygyny **görkezmek** — bu programmadaky her karar adam pikiri bilen ýolbaşçylyk edildi
- Çagalarymyzyň öwrenýän başarnyklarynyň diňe oýunlar üçin däldigini — hakyky, peýdaly tehnologiýa gurmagyň esasydygyny **subut etmek**

Biziň wezipämiz barada has giňişleýin öwrenmek üçin **[gamechangersai.org](https://gamechangersai.org)** saýtyna girip görüň.

---

## Maglumat çeşmeleri we salgylanma

Friend or Foe ulanmak 100% mugt — aşakdaky her bir maglumat çeşmesi açyk we **API açarlary ýa-da tölegli hasaplar talap etmeýär**.

### ADS-B uçar ýeri maglumatlary

Programma, bir çeşme işlemese-de hemişe uçar maglumatlaryny almagyňyzy üpjün edýän üç derejeli ätiýaçlyk zynjyryny ulanýar:

| Ileri tutulýan | Çeşme | API gutarnykly nokady | Näme üpjün edýär | Tassyklama |
|---------------|-------|----------------------|-------------------|-----------|
| 1-nji | **[adsb.fi](https://adsb.fi)** | `opendata.adsb.fi/api` | Hakyky wagtda uçar ýerleri, çagyryş belgileri, beýiklik, tizlik, ugur, squawk kodlary | Mugt, açar ýok |
| 2-nji | **[airplanes.live](https://airplanes.live)** | `api.airplanes.live` | adsb.fi bilen meňzeş maglumatlar (laýyk ADSBx v2 formaty) | Mugt, açar ýok |
| 3-nji | **[OpenSky Network](https://opensky-network.org)** | `opensky-network.org/api` | ICAO ýagdaý wektorlary — ýeri, tizligi, ugry, wertikal tizligi | Mugt, açar ýok (has ýokary tizlik çäkleri üçin islege bagly hasap) |

Üç çeşmäniň hemmesi dünýädäki kabul ediji torlaryndan göni ADS-B transponder maglumatlaryny üpjün edýär. Programma GPS koordinatlary we radiusy boýunça sorag berýär we şowsuzlyk ýa-da wagt gutarmagynda awtomatiki indiki çeşmä geçýär.

### Baýlaşdyrma we goldaw maglumatlary

| Çeşme | Näme üpjün edýär | Tassyklama |
|-------|-------------------|-----------|
| **[Planespotters.net](https://planespotters.net)** | ICAO hex kody boýunça uçar suratlary — jikme-jik kartlarynda görkezilýär | Mugt, açar ýok |
| **[Open-Meteo](https://open-meteo.com)** | Häzirki howa (bulut örtügi, şemal tizligi, şertler) — kesgitleme parametrlerini sazlamak üçin ulanylýar | Mugt, açar ýok |
| **[OpenStreetMap](https://openstreetmap.org)** | 2D karta görnüşi üçin OSMDroid arkaly karta kaşlary | Mugt, açar ýok |

### Enjamda kesgitlemek (daşarky API ýok)

Bu kesgitleme usullary hiç hili tor çagyryşlary bolmazdan doly enjamda işleýär:

- **FAA Remote ID (Bluetooth LE)** — ~300m aralygynda laýyk dron ýaýlymlaryny skanirleýär
- **WiFi SSID deňeşdirmesi** — WiFi nusga boýunça DJI, Skydio, Parrot we 100-den gowrak beýleki dron öndürijilerini kesgitleýär
- **Wizual kesgitlemek (ML Kit)** — Enjamda işleýän kamera esasly obýekt tanamak
- **Harby klassifikasiýa** — Çagyryş belgisi nusga, squawk kodlary we operator maglumat bazasy (hemmesi ýerli goşulan)

---

## Ygtyýarnama

Bu taslama **MIT ygtyýarnamasy** astynda ygtyýarlandyryldy — jikme-jiklikler üçin [LICENSE](LICENSE) faýlyna serediň.

Bu programma üpjünçiligini islendik maksat üçin ulanmaga, üýtgetmäge we paýlamaga erkinsiňiz. Biz diňe awtorlyk hukugy habarnamasyny saklamagyňyzy we degişli ýerlerde hormat bildirmegiňizi soraýarys.

---

*Gyzyklanma, kod we eliň ýeten ähli emeli aň bilen ýasaldy.*
*[GAMECHANGERSai](https://gamechangersai.org) tarapyndan söýgi bilen çykaryldy.*
