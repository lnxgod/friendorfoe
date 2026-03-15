# X (Twitter) Posts for Friend or Foe

## Main Post (Thread Starter)

Point your phone at the sky. Know what's up there.

I just open-sourced Friend or Foe -- an Android AR app that identifies aircraft and drones in real time.

ADS-B transponders. FAA Remote ID. WiFi drone detection. Visual ML. All fused with Bayesian math and overlaid on your camera in augmented reality.

The wild part? I vibe coded the entire thing with Claude in 72 hours.

13 commits. 8,500+ lines of code. 2 hours of AI pair-programming. First commit to working APK in a single afternoon.

Now it's 22,000+ lines, fully open-source, and MIT licensed.

No backend needed. No API keys. No accounts. Install the APK and start spotting.

https://github.com/lnxgod/friendorfoe

#ADSB #planespotting #drones #buildinpublic #vibecoding #opensource #ARCore #AndroidDev

---

## Reply 1 (Thread)

What it detects:

- Commercial flights via ADS-B (adsb.fi, airplanes.live, OpenSky fallback chain)
- Drones via FAA Remote ID (Bluetooth LE)
- DJI, Skydio, Parrot + 100 more via WiFi SSID fingerprinting
- Anything visible via ML Kit camera detection

It classifies into 10 categories: Commercial, Military, Helicopter, Drone, Government, Emergency, Cargo, GA, Ground Vehicle, Unknown.

120+ ICAO type codes mapped to aircraft silhouettes for instant visual ID.

---

## Reply 2 (Thread)

The tech under the hood:

- Kotlin + Jetpack Compose + Material 3
- ARCore with compass-math fallback (because ARCore hates featureless sky)
- Bayesian log-odds sensor fusion -- two weak signals agreeing beats one strong signal
- Room database for detection history
- OpenStreetMap with category-shaped markers and FOV cone overlay
- Hilt DI, CameraX, Retrofit, Coroutines + Flow

All built by a nonprofit (GAMECHANGERSai) to show what AI + human creativity can do.

Fork it. Build on it. Point it at the sky.

---

## Shorter Single-Post Version (if you prefer one tweet)

I vibe coded a 22,000-line Android app with Claude in 72 hours.

Friend or Foe: point your phone at the sky and it identifies aircraft + drones in real time using AR.

ADS-B + Remote ID + WiFi detection + visual ML. Bayesian sensor fusion. 120+ aircraft silhouettes.

Fully standalone. No API keys. Free. Open-source.

https://github.com/lnxgod/friendorfoe

#ADSB #drones #buildinpublic #vibecoding #opensource

---

*Yes, we used AI to write these posts too. It's AI all the way down.*
