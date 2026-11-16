# Friend or Foe on Google Play

The Android application ID is `com.friendorfoe`. This Play candidate uses version code 140, targets API 36 and supports Android 8 (API 26) or newer. [Google Play target API requirements](https://support.google.com/googleplay/android-developer/answer/11926878)

## Build an internal-test candidate

From `android/`, run `./gradlew testDebugUnitTest lintDebug bundlePlay` with JDK 17 and Android SDK 36. The output is `app/build/outputs/bundle/play/app-play.aab`. By default this is an unsigned bundle for inspection. To build an upload-signed bundle, supply the registered upload keystore through `KEYSTORE_PATH` (absolute path), `KEYSTORE_PASSWORD`, `KEY_ALIAS` and `KEY_PASSWORD` in the build environment. CI builds and checks the Play bundle on pull requests. The repository's tag workflow publishes a separate APK to GitHub.

Before uploading, check this package’s highest version code and its registered upload and app-signing certificates in Play Console. Use an unused version code higher than the version testers will upgrade from. For a package already enrolled in Play App Signing, use its registered upload key. When enrolling a package already distributed elsewhere, choose the corresponding app-signing key to support an in-place update. Keep signing and upload keys outside the repository. [Android app versioning](https://developer.android.com/studio/publish/versioning) · [Play App Signing](https://developer.android.com/studio/publish/app-signing)

After the release changes reach main, maintainers can run **Actions → Android Build → Run workflow** on main. It tests the app, signs the bundle with the existing repository Android release key, checks its public certificate fingerprint and creates the `friendorfoe-signed-release` workflow artifact. Download `friendorfoe-play.aab` and its SHA-256 file from that artifact. The manual run has no store upload or GitHub Release step. If Play has a different registered upload certificate, use its registered key with the local build instructions instead.

## Test the update route

More → App updates asks Google Play which release the current installation can receive. If an update is available, Update opens Friend or Foe in the Play Store, which handles installation and auto-update settings. On a device without the Play Store app, the listing action opens Google's website when a browser is available. The GitHub button opens the source repository.

On an internal test track, use a tester account that owns the app, install the older signed version through Play, then make a higher version code available to that account. Verify the Update action, the installed version after returning, and the current-version result. Also exercise an offline or sideloaded installation and the retry action. Sideloading an emulator build only verifies the UI and recovery paths. [Testing Play update availability](https://developer.android.com/guide/playcore/in-app-updates/test)

## Store listing

English (US). Proposed category: Tools.

**Title** — Friend or Foe

**Short description** — Find nearby recorder and camera signals and see why your phone flagged them.

**Full description**

Use your Android phone to look for nearby personal AI recorders, cameras, smart glasses and trackers. Open Privacy to see supported recorder signals first. Tap a finding to see the Bluetooth or Wi-Fi evidence behind it.

Search by name or filter by device family. AirPods activity and other common Bluetooth signals are grouped under Everyday devices. iBeacons are grouped by network. Wi-Fi alerts show the network name when available and explain what triggered the alert. Recent encounters show when a signal appeared and how its strength changed.

You can also explore nearby aircraft and compatible drone broadcasts on the map. Start Nearby Watch for aircraft notifications. The phone can scan on its own. An optional scanner badge or a sensor server you configure can supply additional observations.

Detection depends on what a device broadcasts and what Android can see. A device can stay silent or reuse another device’s name. A signal match cannot tell you whether a microphone or camera is recording.

Friend or Foe is an open-source app published by GameChangers AI.

**Release notes**

Choose Light, Dark or System under App settings. Open Privacy to try a Bluetooth recorder name, inspect grouped everyday devices and iBeacons, or see why a Wi-Fi network was flagged. Find GameChangers AI on Welcome and More, and read the privacy policy offline.

## Images for Play Console

Upload the phone screenshots in the numbered order below. Each is a 1080 × 1920 opaque RGB PNG from the Android interface. The nearby signals in these examples are synthetic. Copy the English alt text into the matching asset field in Play Console. [Google Play preview asset guidance](https://support.google.com/googleplay/android-developer/answer/9866151)

| Asset | File | Alt text |
| --- | --- | --- |
| App icon, 512 × 512 | [App icon](google-play-assets/app-icon.png) | Cyan finder ring and white dot on navy. |
| Feature graphic, 1024 × 500 | [Feature graphic](google-play-assets/feature-graphic.jpg) | Friend or Foe by GameChangers AI with its optional triangular scanner badge. |
| Phone 1 | [Recorder findings](google-play-assets/recorder-observations.png) | Privacy shows sample Plaud and Omi recorder signals, the Bluetooth evidence and the recording-status limitation. |
| Phone 2 | [Wi-Fi alert](google-play-assets/wifi-alert.png) | A sample Wi-Fi alert names the Demo guest network and explains that its access points advertise different security settings. |
| Phone 3 | [Everyday devices](google-play-assets/everyday-devices.png) | A sample Plaud finding appears above a closed Everyday devices group containing AirPods activity and smart-home signals. |
| Phone 4 | [Grouped iBeacons](google-play-assets/grouped-beacons.png) | Twelve sample beacon observations appear under one iBeacon network in the Beacons tab. |
| Phone 5 | [Bluetooth name example](google-play-assets/recorder-example-dark.png) | In dark mode, the Bluetooth name example shows how a sample name matches the Plaud family. |
| Phone 6 | [Welcome](google-play-assets/welcome-light.png) | The GameChangers AI welcome screen introduces personal recorder and camera detection, nearby aircraft, and flight paths. |

## Internal testing and production

1. Verify the Play developer account and package registration. Use the registered upload key to sign the bundle. Select an internal tester list and upload a version code Play has not previously received. Manual Play Console upload suffices. The CI artifact and GitHub APK cannot register an app in Play Console. [Internal testing](https://support.google.com/googleplay/android-developer/answer/9845334)
2. In the current build, More → App settings → Privacy & Data → Privacy policy works offline. The GitHub Pages workflow copies the [same HTML](../android/app/src/main/res/raw/privacy_policy.html) to `_site/privacy-policy.html` when approved changes reach main. The planned public URL is `https://lnxgod.github.io/friendorfoe/privacy-policy.html`. After deployment, verify an anonymous HTTP 200 and the Friend or Foe Privacy Policy title in a private browser window before entering the URL in Play Console. The organization publicly identifies its contact email on its About page. Confirm the monitored support address for the listing and privacy inquiries. [Publisher contact details](https://gamechangersai.org/about.html) · [Google Play User Data policy](https://support.google.com/googleplay/android-developer/answer/10144311)
3. Complete the [Data safety and permissions worksheet](google-play-privacy-review.md) against the signed version. In App content, provide app access instructions, the ads declaration, target audience, content rating, and the foreground-service declaration and demonstration requested for location. Phone-only scanning has no app account. Describe optional hardware and sensor access to reviewers. If full review requires an operator server, supply reviewer-only access privately in Console. [App content requirements](https://support.google.com/googleplay/android-developer/answer/9859455)
4. Select intended countries, price and release track. Personal developer accounts created after November 13, 2023, must meet Google's closed-test rule before applying for production access. At least 12 testers must remain opted in continuously for 14 days. Check the account dashboard to see whether it applies. [Account-specific testing requirements](https://support.google.com/googleplay/android-developer/answer/14151465)
5. Exercise the signed Play installation and update route using an eligible tester. Verify native compatibility and conduct a physical Bluetooth/Wi-Fi spot check with known broadcasting devices, recording the phone, Android version and device mode. [Android native compatibility](google-play-native.md)

For foreground-service review, show the Nearby Watch explanation, permission choice, explicit Start control, ongoing notification while minimized, and Stop. In the same recording, demonstrate the user-visible consequence of stopping the watch. Describe the location purpose and the providers used. The reviewer should be able to follow those steps from a fresh install. [Foreground service review](https://support.google.com/googleplay/android-developer/answer/13392821)
