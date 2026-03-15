# Reddit Posts for Friend or Foe

---

## r/ADSB Post

**Title:** I built an open-source Android AR app that overlays ADS-B data on your camera view -- plus drone detection via Remote ID and WiFi

**Body:**

Hey everyone -- I just open-sourced an Android app called **Friend or Foe** that does real-time aircraft and drone identification using augmented reality.

**What it does:**

Point your phone at the sky and floating labels appear on detected aircraft and drones showing callsign, type, altitude, distance, and category. It's like Flightradar24 meets a heads-up display, but running entirely on your phone.

**Detection sources:**

- **ADS-B** via a three-tier fallback chain: adsb.fi (primary) -> airplanes.live -> OpenSky Network. Queries by GPS coordinates + radius, auto-falls through on failure/timeout
- **FAA Remote ID** via Bluetooth LE -- picks up compliant drones within ~300m
- **WiFi SSID pattern matching** -- fingerprints for DJI, Skydio, Parrot, and 100+ other drone manufacturers
- **Visual detection** via ML Kit for anything visible in the camera

**The ADS-B nerd details:**

- ICAO hex code lookups with enrichment (registration, operator, photos via Planespotters)
- Squawk code decoding (7500/7600/7700 alerts + military squawk ranges)
- Military classification using callsign patterns and operator databases
- 120+ ICAO type codes mapped to 10 vector silhouette categories (narrowbody, widebody, regional, turboprop, bizjet, helicopter, fighter, cargo, lightplane, drone)
- Smart categorization: Commercial, GA, Military, Helicopter, Government, Emergency, Cargo, Drone, Ground Vehicle, Unknown
- Bayesian log-odds sensor fusion when multiple sources detect the same object

**Map view:**

OpenStreetMap with distinct marker shapes per category, distance rings, compass-follow mode, and a FOV cone showing where your camera is pointed.

**Fully standalone** -- no backend server needed. The app hits the public ADS-B APIs directly. No API keys, no accounts, no server to run. There's an optional Python/FastAPI backend if you want aircraft photos and airline enrichment, but it's not required.

22,000+ lines of Kotlin, Python, and XML. MIT licensed. Built by GAMECHANGERSai (501c3 nonprofit).

**GitHub:** https://github.com/lnxgod/friendorfoe

APK available on GitHub Releases if you just want to install and go. Would love to hear what you think -- especially ideas for additional type code mappings or detection improvements.

---

## r/BuildInPublic Post

**Title:** From zero to 22,000 lines in 72 hours -- I vibe coded an aircraft/drone identification app with AI and just open-sourced it

**Body:**

I want to share a build that I think demonstrates where AI-assisted development is heading.

**The speed run:**

On March 12, 2025, I sat down with Claude (Anthropic's AI) and started building an Android app from scratch. The first commit landed at 12:13 PM. By 2:22 PM that same day -- just over 2 hours later -- I had 13 commits and **8,500+ lines of production code**:

- AR viewfinder with floating labels
- Four independent detection systems (ADS-B, Bluetooth drone scanning, WiFi fingerprinting, visual ML)
- Bayesian sensor fusion engine
- Map view with OpenStreetMap
- Detection history database
- Full MVVM architecture with Hilt dependency injection

By March 14, the app was open-sourced with 120+ aircraft silhouettes, styled map markers, permission handling polish, and a security review. Total: **22,000+ lines** across Kotlin, Python, and XML.

**What it actually does:**

The app is called **Friend or Foe**. You point your phone at the sky and it identifies aircraft and drones in real time using augmented reality. ADS-B data, FAA Remote ID, WiFi drone detection, and camera-based ML -- all fused together with Bayesian math and overlaid on your camera view.

It confirmed real-world detections of commercial aircraft and drones on a physical device within 72 hours of the first line of code.

**The vibe coding part:**

This wasn't "AI writes some boilerplate." Claude was a full pair-programming partner -- architecture design, sensor fusion algorithms, Bayesian math, vector drawable artwork, ARCore compass-math hybrid fallback, the works. Every file was shaped by human-AI collaboration. I focused on vision and architecture, Claude handled the implementation at speed.

**It's fully standalone** -- no backend, no API keys, no accounts. Install the APK and point it at the sky.

MIT licensed. Released by GAMECHANGERSai (501c3 nonprofit focused on AI education).

**GitHub:** https://github.com/lnxgod/friendorfoe

Happy to answer questions about the process, the architecture, or the AI workflow.

---

## Suggested Subreddits and Angles

### r/drones
**Angle:** Drone detection tool -- "See what drones are flying near you." Lead with Remote ID Bluetooth scanning + WiFi SSID fingerprinting for 100+ drone manufacturers. Mention that it detects DJI, Skydio, Parrot, and budget drones. Useful for drone pilots who want to see who else is in the air, or anyone curious about drone activity in their area. The AR overlay shows detected drones on your camera view with distance and confidence level.

### r/androiddev
**Angle:** Technical architecture showcase. Kotlin + Jetpack Compose + Material 3, ARCore with compass-math hybrid fallback, CameraX, ML Kit, Hilt DI, Room, Retrofit, Coroutines + Flow, OSMDroid. Clean Architecture (data/domain/presentation/sensor/detection). Bayesian log-odds sensor fusion. Open-source, good reference project. The ARCore fallback pattern is particularly interesting -- ARCore loses tracking when pointed at featureless sky, so the app automatically falls back to compass-math orientation.

### r/aviation
**Angle:** AR plane spotting tool. "Point your phone at the sky and see what's flying." Floating labels with callsign, type, altitude, distance. 120+ aircraft silhouettes for visual recognition. Military detection via callsign patterns and squawk codes. Category colors for instant identification. Great for airshows, airport spotting, or just backyard sky watching.

### r/UFOs
**Angle:** "Before you post 'what is this in the sky?' -- try this app." Tongue-in-cheek but genuinely useful. The app identifies commercial flights, drones, helicopters, military aircraft, and more. If it CAN be identified, this app will tell you what it is. What's left after you've ruled everything out... that's the interesting part. Four independent detection methods plus Bayesian fusion means high confidence in identifications.

### r/sideproject
**Angle:** Weekend project that escalated. Zero to working app in 72 hours, now 22,000+ lines and open-source. Built with AI pair programming. Free, no API keys, standalone Android app. Great example of a side project with real-world utility.

### r/artificial
**Angle:** What AI-assisted development actually looks like in practice. Not a toy demo -- a production Android app with AR, sensor fusion, ML, and four detection systems. 8,500+ lines in 2 hours of pair-programming. The git history proves it. Open-sourced so people can see every line.

### r/ClaudeAI
**Angle:** Vibe coding showcase. Built entirely with Claude as a pair-programming partner. Architecture design through implementation, sensor fusion math, vector drawable artwork, security audit. 22,000+ lines in 72 hours. Show the git timeline. This is what Claude Code can do with a motivated human driving.

### r/opensource
**Angle:** New MIT-licensed project from a 501(c)(3) nonprofit. Full-featured Android AR app for aircraft/drone identification. 22,000+ lines of Kotlin + Python. No API keys required, connects to free public data sources. Looking for contributors -- ideas for additional type code mappings, drone manufacturer fingerprints, iOS port, night mode.

---

*Yes, we used AI to write these posts too. The app, the README, and now the marketing -- it's AI all the way down.*
