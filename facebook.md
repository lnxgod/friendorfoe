# Facebook Post for Friend or Foe

---

## Main Post

Ever wonder what that plane is flying over your house? Or spot a drone and want to know who's flying it?

I just released an app called **Friend or Foe** -- and it does exactly that.

Open your camera, point it at the sky, and the app overlays floating labels on aircraft and drones in real time. It tells you the callsign, aircraft type, altitude, how far away it is, and what category it falls into -- commercial flight, military, helicopter, drone, or something else entirely.

**How does it work?**

It pulls from multiple sources at once:
- **ADS-B transponder data** from public flight tracking networks (the same data that powers sites like Flightradar24)
- **FAA Remote ID** -- picks up compliant drones broadcasting via Bluetooth within about 300 meters
- **WiFi detection** -- identifies DJI, Skydio, Parrot, and 100+ other drone brands by their WiFi signals
- **Camera-based detection** -- uses on-device machine learning to spot objects in the sky

All of this gets combined using math (Bayesian sensor fusion) to give you a confidence score for each identification. It even shows aircraft silhouettes so you can visually match what you're seeing.

**The backstory is kind of wild:**

I built this with an AI called Claude in about 72 hours. The first working version -- 8,500 lines of code -- was written in a single 2-hour session on March 12. By March 14 it was polished and open-sourced. The entire codebase is now over 22,000 lines.

**It's completely free:**
- No subscriptions
- No API keys or accounts to create
- No backend server to set up
- Just install the app and go

It's also fully open-source under the MIT license, released by GAMECHANGERSai -- a nonprofit focused on AI education for kids and families.

If you're into aviation, plane spotting, drone awareness, or just want to know what's flying overhead -- give it a try. Android only for now.

Download the APK or check out the code: https://github.com/lnxgod/friendorfoe

---

## Suggested Facebook Groups to Share In

- **Aviation/Plane Spotting Groups** -- Lead with the AR plane identification angle. Great for airshows and airport spotting.
- **Drone Pilot Groups** -- "See what other drones are in your area." Remote ID + WiFi detection is genuinely useful for drone operators.
- **ADS-B / Flight Tracking Groups** -- Technical community that will appreciate the multi-source fallback chain and ICAO type code mappings.
- **Android Enthusiast Groups** -- Cool open-source Android app showcase.
- **AI / Tech Groups** -- The 72-hour vibe coding story is the hook here.
- **Neighborhood / Community Groups** -- "Want to know what that drone over your house is?" Very relatable angle for local communities.
- **Ham Radio / Scanner Groups** -- Overlap with the ADS-B community. Many scanner enthusiasts also track aircraft.
- **UFO / Sky Watcher Groups** -- "Identify what's actually up there before you speculate." Genuine utility for the sky-watching community.

---

*Yes, we used AI to write these posts too. The app, the README, and now the marketing -- it's AI all the way down.*
