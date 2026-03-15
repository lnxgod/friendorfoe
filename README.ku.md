🌍 [English](README.md) | [עברית](README.he.md) | [Українська](README.uk.md) | [العربية](README.ar.md) | [Türkçe](README.tr.md) | [Azərbaycan](README.az.md) | [Türkmen](README.tk.md) | [پښتو](README.ps.md) | [اردو](README.ur.md) | **[Kurdî](README.ku.md)** | [Հայերեն](README.hy.md) | [ქართული](README.ka.md) | [فارسی](README.fa.md)

# Friend or Foe — Naskirandina Balafiran û Dronan di Dema Rast de

[![Android Build](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml/badge.svg)](https://github.com/lnxgod/friendorfoe/actions/workflows/android-build.yml)

**Telefona xwe berbi ezmên ve bikin. Bizanin li wir çi heye.**

Friend or Foe sepaneke Android-ê ya çavkaniya vekirî ye ku bi prensîba **saz bike û dest pê bike** dixebite û bi karanîna rastiya zêdekirî balafir û dronan di dema rast de nas dike. Ew daneyên transponderên ADS-B, weşanên FAA Remote ID yên dronan, analîza sînyalên WiFi, û naskirandina dîtbarî ya li ser amûrê dike yek ji bo ku labelên herikbar li ser dîmena kamerayê bicîh bike — ji we re dibêje ka li ser serê we çi difirre, kî dixebitîne, ku diçe, û gelo ev heval e an dijmin. Bê hesab, bê tomarkirin, bê mifteyên API — tenê saz bikin û dest pê bikin.

Ev proje **bi zîrekiya destkirî hate avakirin** — ne tenê bi yekê, lê bi hemûyan. Claude kod nivîsand, Grok sêwiran diyar kir, Codex ewlehiyê kontrol kir, û Gemini alîkariya hilbijartina berhevoka teknolojiyê kir. Ji hêla [GAMECHANGERSai](https://gamechangersai.org) ve hat weşandin da ku nîşan bide dema ku zîrekiya destkirî bi afirîneriya mirovî re dicive çi mumkin e. Ji bo dîroka guhertoyê [CHANGELOG](CHANGELOG.md) bibînin.

### Bezê 72-Saetî

> **Ji sifirê heta naskirandina pejirandî ya balafir û dronan li ser amûreke rast — di bin 72 saetan de.**
>
> Di 12ê Adara 2025-an de, commit-a yekem wê êvarê hat çêkirin, 13 commit û **8,500+ rêzikên kodê** di ~2 saetan programkirina cot bi AI-ê de bi Claude re hatin nivîsandin — APK-yek amadekirî bi dîtbarê AR, naskirandina pirçavkanî, berhevkirina sensorên Bayesî, û dîmena nexşeyê çêkir. Heya 14ê Adarê, sepan bi sîluetên balafiran, nîşaneyên nexşeyê yên xemilandî, û cilakirinê re wek çavkaniya vekirî hat weşandin — bi tevahî **22,000+ rêzik** di Kotlin, Python û XML de.
>
> Sepan **saz bike û dest pê bike** ye — APK-ê saz bikin, destûran bidin û dest bi naskirandina balafiran bikin. Bê tomarkirin, bê miftê, bê hesab rasterast bi API-yên giştî yên belaş ên ADS-B ve girêdayî ye. Servera paşîn a Python a vebijarkî tenê dewlemendkirina zêde zêde dike (navên firokevaniyê, agahdariya rêyê).

---

## Çi Dike

- **Dîtbarê AR** — Dîmena kamerayê ya zindî bi labelên herikbar ên bi rengên kodkirî li ser balafir û dronên naskirî. Label çêjnav, cureyê balafirê, bilindahî û dûrahiyê nîşan didin.
- **Naskirandina Pirçavkanî** — Ji bo hişmendiya berfireh a ezmên çar rêbazên naskirandinê yên serbixwe dike yek:
  - Daneyên transponderên ADS-B (firînên bazirganî, hewanoriya giştî)
  - FAA Remote ID bi Bluetooth LE (dronên lihevhatî)
  - Lihevhatina nimûneyên WiFi SSID (DJI, Skydio, Parrot, 100+ çêker)
  - Naskirandina dîtbarî bi ML Kit (naskirandina tiştan bi kamerê)
- **Kategorîkirina Jîr** — Her tiştî di 10 cureyan de kategorîze dike: Bazirganî, Hewanoriya Giştî, Leşkerî, Helîkopter, Hikûmetî, Acîl, Barkêş, Dron, Wesayîta Erdî, û Nenas. Naskirandina leşkerî nimûneyên çêjnav, kodên squawk, û databasên operatoran bikar tîne.
- **Berhevkirina Sensorên Bayesî** — Dema ku çend sensor heman tiştî nas dikin, nirxên baweriyê bi karanîna log-odds ên Bayesî tên berhev kirin — ne tenê "yê herî bilind hilbijêre." Du sînyalên lawaz ên ku li hev dikin dikarin ji sînyalek bihêz girantir bin.
- **Sîluet û Wêneyên Balafiran** — 120+ kodên cureyê ICAO bi 10 kategoriyên sîluetên vektorî ve (laşê teng, laşê fireh, herêmî, turboprop, jetê karsaziyê, helîkopter, şerker, barkêş, balafirek sivik, dron) hatine nexşekirin û ji bo naskirandina dîtbarî ya tavilê 134 wêneyên balafiran ên pêvekirî — tor ne pêwîst e.
- **Dîmena Nexşeya 2D** — OpenStreetMap bi şiklên nîşanan ên cuda ji bo her kategoriyê, zencîreyên dûrahiyê, moda şopandina qibleyê, û pêvekirina konê FOV.
- **Kartên Hûrgilî** — Ji bo agahdariya tevahî li ser her tiştekî bixin: tomarkirin, operator, rê (dest pêk/armancek), bilindahî, lez, ser, kodê squawk, çavkaniya naskirandinê, û asta baweriyê.
- **Loga Dîrokê** — Databasa mayînde ya her tiştê ku we nas kiriye, ku dikare were lêgerîn û rêzkirin.
- **Rêbera Referansa Dronan** — Databasa pêvekirî ya 30+ cureyên dronan bi wêne, taybetmendî û ravekirinan — ji dronên xerîdar ên DJI heta UCAV-ên leşkerî yên wekî Bayraktar TB2, MQ-9 Reaper, û Shahed-136. Li gorî kategoriyê bigerin, bi nav bigerin, an rasterast ji naskirandina droneke nenas biçin bo naskirandina tiştê ku hûn dibînin.
- **Dewlemendkirina Servera Paşîn (Vebijarkî)** — Serverek API ya Python a vebijarkî dikare navên firokevaniyê, hejmarên tomarkirinê, û agahdariya rêyê lê zêde bike. Sepan bê wê bi tevahî dixebite — wêneyên balafir û referansên dronan di APK de hene.

---

## Çawa Dixebite

### Çavkaniyên Naskirandinê

| Çavkanî | Çi Nas Dike | Mesafe | Bawerî |
|---------|------------|--------|--------|
| **ADS-B** | Balafirên bazirganî û GA yên bi transponderan | ~250 NM | Pir bilind (0.95) |
| **Remote ID (BLE)** | Dronên bi FAA re lihevhatî (250g+) | ~300m | Bilind (0.85) |
| **WiFi SSID** | DJI, Skydio, Parrot, dronên erzan | ~100m | Navîn (0.3-0.85) |
| **Dîtbarî (ML Kit)** | Her tiştê ku di kamerayê de xuya dike | Xeta dîtinê | Guhêrbar |

### Berhevkirina Sensoran û AR

Sepan daneyên akselerometer, magnetometre û jîroskopê dike yek da ku tam bizanibe telefon li ku dixe. Cihên balafiran (firehî/dirêjahî/bilindahî) bi karanîna hesabkirinên dûrahiya haversine û geometriya FOV ya kamerayê li ser dîmena kamerayê tên projeksiyonkirin.

**Hîbrîdê ARCore + Qible**: ARCore dema ku kamera taybetmendiyên erdê dibîne şopandina hêja peyda dike, lê dema ku ber bi ezmana bê taybetmendî ve tê nîşankirin tengahî dikişîne — tam dema ku herî zêde hewce ye. Dema ARCore şopandinê winda dike sepan bixweber vedigere ser hêlkirina qible-matematîkê, di hemû şertan de labelên bênavber peyda dike.

### Mîmarî

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
        (serekî)     (yedek)         (yedek)
```

---

## Destpêkirin

### Pêwîstî

- **Android Studio** Hedgehog (2023.1.1) an paşîn
- **JDK 17**
- **Android SDK 35** (compileSdk) bi minSdk 26 (Android 8.0+)
- Amûreke Android bi GPS, qible û kamerayê (emulator ji bo dîmenên lîste/nexşe dixebite lê ne ji bo AR)

> **Hesab an tomarkirin ne pêwîst e.** Sepan rasterast bi API-yên giştî yên belaş ên ADS-B ve girêdayî ye — sazkirina serverê, mifteyên API, hesab ne pêwîst e.

### Destpêkirina Bilez — Tenê Android

Sepan rasterast bi API-yên giştî yên belaş ên ADS-B ve girêdayî ye — sazkirina serverê an hesab ne pêwîst e. Wêneyên balafir û rêbera referansa dronan di APK de hatine pêvekirin. Ji bo çêkirin û xebitandinê:

```bash
cd android
./gradlew assembleDebug
# Li ser amûra girêdayî saz bikin:
adb install app/build/outputs/apk/debug/app-debug.apk
```

An jî APK-a herî dawî ya pêşçêkirî ji [**GitHub Releases**](https://github.com/lnxgod/friendorfoe/releases) daxînin.

### Sazkirina Servera Paşîn (Vebijarkî — dewlemendkirinê çalak dike)

Servera paşîn wêneyên balafir, navên firokevaniyê, lêgerîna tomarkirinê, û agahdariya rêyê zêde dike. **Python 3.11+ û vebijarkî Redis ji bo cachkirinê hewce ye.**

```bash
cd backend

# Jîngeha virtual biafirînin
python -m venv .venv
source .venv/bin/activate  # an li ser Windows .venv\Scripts\activate

# Girêdayiyan saz bikin
pip install -r requirements.txt

# Veavakirina jîngehê kopî bikin
cp .env.example .env
# Heke hûn dixwazin nasnameyên OpenSky lê zêde bikin .env biguherînin (vebijarkî)

# Serverê bixebitînin
uvicorn app.main:app --host 0.0.0.0 --port 8000
```

**Bi Docker** (Redis + PostgreSQL tê de ye):

```bash
cd backend
docker compose up
```

API dê li `http://localhost:8000` peyda bibe. Kontrola tenduristiyê: `http://localhost:8000/health`

### Girêdana Sepanê bi Servera Xwe

URL-ya servera paşîn di veavakirina torê ya sepana Android de nûve bikin da ku ber bi navnîşana IP ya serverê ve nîşan bide.

---

## Berhevoka Teknolojiyê

### Sepana Android

| Hêman | Pirtûkxane | Guherto |
|-------|-----------|---------|
| Ziman | Kotlin | 1.9.22 |
| UI | Jetpack Compose + Material 3 | 2024.02.00 |
| AR | ARCore | 1.41.0 |
| Kamera | CameraX | 1.3.1 |
| ML | ML Kit Object Detection | 17.0.1 |
| DI | Hilt | 2.50 |
| HTTP | Retrofit + OkHttp | 2.9.0 / 4.12.0 |
| Databas | Room | 2.6.1 |
| Wêne | Coil | 2.5.0 |
| Nexşe | OSMDroid | 6.1.18 |
| Asynckron | Coroutines + Flow | 1.7.3 |

### Servera Paşîn

| Hêman | Pirtûkxane | Guherto |
|-------|-----------|---------|
| Çarçove | FastAPI | 0.115.6 |
| Server | Uvicorn | 0.34.0 |
| Xerîdarê HTTP | httpx | 0.28.1 |
| Cache | Redis | 5.2.1 |
| Databas | SQLAlchemy + asyncpg | 2.0.36 |
| Pejirandin | Pydantic | 2.10.4 |

---

## Avahiya Projeyê

```
friendorfoe/
├── android/                           # Sepana Android
│   └── app/src/main/java/com/friendorfoe/
│       ├── data/
│       │   ├── local/                 # Databasa Room (dîrok, şopandin)
│       │   ├── remote/                # Xizmetên API yên Retrofit û DTO
│       │   └── repository/            # Depoyên daneyê
│       ├── detection/                 # Motorên naskirandinê
│       │   ├── AdsbPoller.kt          # Lêpirsîna daneyên ADS-B
│       │   ├── RemoteIdScanner.kt     # Skankirina BLE Remote ID
│       │   ├── WifiDroneScanner.kt    # Naskirandina WiFi SSID
│       │   ├── BayesianFusionEngine.kt # Berhevkirina baweriya pirsenor
│       │   ├── MilitaryClassifier.kt  # Naskirandina balafirên leşkerî
│       │   └── VisualDetection*.kt    # Naskirandina dîtbarî ya ML Kit
│       ├── domain/model/              # Modelên domain (Aircraft, Drone, SkyObject)
│       ├── di/                        # Derzîkirina girêdayiyên Hilt
│       ├── presentation/
│       │   ├── ar/                    # Ekrana dîtbarê AR
│       │   ├── map/                   # Dîmena OpenStreetMap
│       │   ├── list/                  # Lîsteya tiştên rêzkirî
│       │   ├── detail/                # Kartên hûrgilî yên tevahî
│       │   ├── drones/                # Geroka rêbera referansa dronan
│       │   ├── history/               # Dîroka naskirandinê
│       │   ├── welcome/               # Ekrana xêrhatinê/destpêkirinê
│       │   ├── about/                 # Agahdariya sepanê
│       │   └── util/                  # AircraftPhotos, CategoryColors, DroneDatabase
│       └── sensor/                    # Berhevkirina sensoran û cihkirin
├── backend/                           # Servera Python FastAPI
│   └── app/
│       ├── main.py                    # Ketina FastAPI
│       ├── config.py                  # Veavakirina jîngehê
│       ├── routers/aircraft.py        # Nuqteyên dawî yên API
│       └── services/
│           ├── adsb.py                # Kişandina ADS-B ya pirçavkanî
│           └── enrichment.py          # Dewlemendkirina daneyên balafirê
├── images/                            # Wêneyên referansa balafiran (Wikimedia CC)
├── scripts/                           # Skrîptên alîkar (daxistina wêneyan)
├── macos/                             # Sepana hevkar a macOS (qonaxa destpêkê)
└── docs/                              # Belgeyên sêwiranê
```

---

## Nuqteyên Dawî yên API

| Rêbaz | Nuqteya Dawî | Rave |
|-------|-------------|------|
| `GET` | `/health` | Kontrola tenduristiyê (rewşa Redis, DB) |
| `GET` | `/aircraft/nearby?lat=&lon=&radius_nm=` | Balafirên li nêzî cihekî |
| `GET` | `/aircraft/{icao_hex}/detail` | Hûrgiliyên balafirê yên dewlemendkirî |

---

## Beşdarî

Beşdarî bi xêr tên! Gelo rastkirinên çewtî, rêbazên nû yên naskirandinê, başkirinên UI, an belgehkirin — em alîkariya we dixwazin.

1. Depoyê fork bikin
2. Şaxeke taybetmendiyê biafirînin (`git checkout -b feature/amazing-feature`)
3. Guherandinên xwe commit bikin (`git commit -m 'Add amazing feature'`)
4. Şaxê push bikin (`git push origin feature/amazing-feature`)
5. Pull Request vekin

### Ramanên ji bo Beşdaran

- Nexşekirinên zêde yên kodên cureyê balafirê
- Şopên çêkerên dronên WiFi yên başkirî
- Moda şevê / temaya tarî ji bo pêvekirina AR
- Pêşbîniya rêya firînê û dîtbarîkirina rêgezê
- Taybetmendiyên civakî (parvekirina dîtinan, raporên civakê)
- Portkirina iOS bi karanîna bingeha macOS Swift
- Yekkirina bi çavkaniyên zêde yên daneyên ADS-B

---

## Bi Zîrekiya Destkirî Hatiye Avakirin

Ev proje ne tenê bi zîrekiya destkirî hatiye avakirin — bi **hemûyan** hatiye avakirin. Me her platformek sereke ya AI-ê ceriband û her yek li cihê ku herî bihêz bû bi kar anî. Dîroka git çîrokê vedibêje:

| Dîrok | Çi Qewimî |
|-------|-----------|
| **12ê Adarê** | 13 commit hatin kirin — di ~2 saetan programkirina cot bi AI de **8,500+ rêzikên kodê** hatin nivîsandin. Encam: APK-yek amadekirî bi dîtbarê AR, çar çavkaniyên naskirandinê, berhevkirina sensorên Bayesî, dîmena nexşeyê û şopandina dîrokê. |
| **14ê Adarê** | Weşana çavkaniya vekirî. 120+ sîluetên balafiran, nîşaneyên nexşeyê yên xemilandî bi şiklên kategoriyê, cilakirina destûran, û kontrola ewlehiyê hatin zêdekirin. Paşê: 134 wêneyên balafiran wekî çavkaniyên offline hatin pêvekirin, ekrana xêrhatinê bi kontrolkera nûvekirinê û barkirin wêneya Coil hatin zêdekirin. |
| **Tevahî** | **23,000+ rêzik** Kotlin, Python û XML. Naskirandinên rastî yên balafirên bazirganî û dronan li ser amûrê hatin pejirandin. |

### Rolên AI

| AI | Rol |
|----|-----|
| **[Claude](https://claude.ai)** (Anthropic) | Nûnerê kodkirinê yê serekî — mîmarî, bicihanîn, programkirina cot |
| **[Grok](https://grok.com)** (xAI) | Rêberiya sêwiranê û lêkolîn |
| **[Codex](https://chatgpt.com)** (OpenAI) | Kontrola ewlehiyê û şêwirmendî |
| **[Gemini](https://gemini.google.com)** (Google) | Lêkolîna berhevoka teknolojiyê — nirxandina pirtûkxane, çarçove û nêzîkatiyan |
| **[ML Kit](https://developers.google.com/ml-kit)** (Google) | Naskirandina dîtbarî ya tiştan li ser amûrê (rasterast li ser telefonê dixebite) |

**Vibe coding çi ye?** Ew nêzîkatiyeke hevkar û axaftin-bingehîn a pêşvebirina nermalava ye ku mirov û AI bi hev re di dema rast de ava dikin. Li şûna ku her rêzikê bi destan binivîsin, hûn rave dikin ka hûn çi dixwazin, li ser ramanan dubare dikin, bi hev re çewtiyê dibînin, û dihêlin ku AI kodê şablonê birêve bibe dema ku hûn li ser dîtinê û mîmariyê balê didin. Ev programkirina bi hestê ye — û dixebite.

Friend or Foe bi çend AI hatin avakirin, her yek li cihê ku tê de herî baş e beşdarî bû. Claude wekî hevkarê kodkirinê yê serekî xizmet kir — ji mîmariya destpêkê heta algorîtmayên berhevkirina sensoran, matematîka Bayesî, û hunera vektora çîçek. Grok alîkariya teşkilkirina rêberiya sêwiranê kir. Codex beriya weşana çavkaniya vekirî kontrola ewlehiyê pêk anî. Gemini lêkolîn kir ka kîjan teknolojî û çarçove bikar bînin. Û ML Kit li ser amûrê dixebite, bê girêdayîbûna ewr naskirandina dîtbarî dikare. Her pelê di vê depoyê de bi hevkariya mirov-AI hatiye şekil kirin.

**Em bawer dikin ku paşeroja pêşvebirina nermalava karanîna AI-ya rast ji bo karê rast e**, û em vê projeyê wek çavkaniya vekirî diweşînin da ku hûn bibînin ev di pratîkê de çawa xuya dike.

---

## Derbarê GAMECHANGERSai

**[GAMECHANGERSai](https://gamechangersai.org)** rêxistinek nehesabker a 501(c)(3) ye ku ji bo çêkirina ezmûnên fêrbûnê yên pratîkî yên bi AI-ê ji bo zarok û malbatan hatiye terxankirin.

> *"Lîstikên AI yên dilşad ku ji her xwendekaran re hîn dikin ku ava bikin û ji nû ve bikin."*

Bi rêya platforma me, xwendekar jêhatîyên rastîn ên veguhêzbar pêşve dixin:

- **Kodkirina Afirîner** — Avahîkirina bernameyên fonksiyonelî bi AI wekî pîlota duyemîn
- **Ramana Pergalê** — Têgihîştina ka parçeyên tevlihev çawa li hev dikin û çawa çewtiyê bibînin
- **Birêvebirina Çavkaniyan** — Plansazî û optimîzekirina stratejîk
- **Ramana Exlaqî** — Fêrbûna pirskirina "gelo divê em?" li gel "gelo em dikarin?" dema ku AI bikar tînin

**Friend or Foe delîlek e ku ev jêhatî rastîn in.** Heman kodkirina afirîner, ramana pergalê, û hevkariya bi AI-ê ku zarok li ser platforma me hîn dibin ji bo avahîkirina vê pergala naskirandina balafiran a bi tevahî fonksiyonelî hate bikaranîn. Tiştê ku wekî lîstikê dest pê dike dibe amûr. Tiştê ku wekî lîstinê dest pê dike dibe pisporî.

Em soz didin ku ji civata çavkaniya vekirî re vegerînin. Bi weşandina Friend or Foe ji bo giştî, em hêvî dikin:

- Pêşvebir û xwendekaran **îlham bikin** ku bi pêşvebirina bi alîkariya AI re çi mumkin e bikolin
- **Nîşan bidin** ku AI hevkareke afirîner e, ne şûngir — her biryarek di vê sepanê de bi dadweriziya mirovî hatiye rêberkirin
- **Nîşan bidin** ku jêhatîyên ku zarokên me hîn dibin ne tenê ji bo lîstikan in — ew bingeha avahîkirina teknolojiya rast û kêrhatî ne

Ji bo ku bêtir li ser mîsyona me hîn bibin li **[gamechangersai.org](https://gamechangersai.org)** serdana me bikin.

---

## Çavkaniyên Daneyê û Referans

Friend or Foe 100% belaş e — her çavkaniya daneyê ya jêrîn vekirî ye û **mifteyên API an hesabên bi pere** hewce nake.

### Daneyên Cihê Balafirên ADS-B

Sepan zencîreya yedekê ya sê-astî bikar tîne da ku hûn her dem daneyên balafirê bistînin heke çavkaniyek nexebite jî:

| Pêşanî | Çavkanî | Nuqteya Dawî ya API | Çi Peyda Dike | Nasname |
|--------|---------|---------------------|---------------|---------|
| 1-emîn | **[adsb.fi](https://adsb.fi)** | `opendata.adsb.fi/api` | Cihên balafiran ên dema rast, çêjnav, bilindahî, lez, ser, kodên squawk | Belaş, bê miftê |
| 2-emîn | **[airplanes.live](https://airplanes.live)** | `api.airplanes.live` | Heman daneyên wekî adsb.fi (formata ADSBx v2 ya lihevhatî) | Belaş, bê miftê |
| 3-emîn | **[OpenSky Network](https://opensky-network.org)** | `opensky-network.org/api` | Vektorên rewşa ICAO — cih, lez, ser, rêjeya vertîkal | Belaş, bê miftê (hesabê vebijarkî ji bo sînorên rêjeyê yên bilindtir) |

Her sê çavkanî daneyên transponderên ADS-B ên zindî ji torên wergirên li çaraliyê cîhanê peyda dikin. Sepan bi koordînatên GPS û tîrêjê dipirse û di şikestinê an derbasbûna demê de bixweber diçe çavkaniya din.

### Dewlemendkirin û Daneyên Piştgir

| Çavkanî | Çi Peyda Dike | Nasname |
|---------|---------------|---------|
| **[Planespotters.net](https://planespotters.net)** | Wêneyên balafiran li gorî koda ICAO hex — li ser kartên hûrgilî têne nîşandan | Belaş, bê miftê |
| **[Open-Meteo](https://open-meteo.com)** | Hewa heyî (pêçandina ewran, leza bayê, şert) — ji bo sazkirina parametreyên naskirandinê tê bikaranîn | Belaş, bê miftê |
| **[OpenStreetMap](https://openstreetmap.org)** | Qalikên nexşeyê bi OSMDroid ji bo dîmena nexşeya 2D | Belaş, bê miftê |

### Naskirandina li ser Amûrê (Bê API-yên Derve)

Van rêbazên naskirandinê bi tevahî li ser telefonê bê bangên torê dixebitin:

- **FAA Remote ID (Bluetooth LE)** — Di nav ~300m de weşanên dronên lihevhatî diskanîne
- **Lihevhatina WiFi SSID** — Bi nimûneyên WiFi DJI, Skydio, Parrot, û 100+ çêkerên dronên din nas dike
- **Naskirandina Dîtbarî (ML Kit)** — Naskirandina tiştan bi kamerê ku li ser amûrê dixebite
- **Kategorîkirina Leşkerî** — Nimûneyên çêjnav, kodên squawk, û databasa operatoran (hemû bi awayê herêmî hatine pêvekirin)

---

## Lîsans

Ev proje di bin **Lîsansa MIT** de hatiye lîsanskirin — ji bo hûrgiliyan pelê [LICENSE](LICENSE) bibînin.

Hûn azad in ku vê nermalava ji bo her armancê bikar bînin, biguherînin û belav bikin. Em tenê dixwazin ku hûn daxuyaniya mafê kopiyê biparêzin û li cihê ku pêwîst e kredît bidin.

---

*Bi meraq, kod, û her AI-ya ku dest me lê girt hatiye çêkirin.*
*Ji hêla [GAMECHANGERSai](https://gamechangersai.org) ve bi evînê hat berdan.*
