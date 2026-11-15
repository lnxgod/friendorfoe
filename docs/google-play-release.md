# Friend or Foe on Google Play

The Android application ID is `com.friendorfoe`. The first Play candidate in this branch uses version code 140 and targets API 36. The minimum remains Android 8 (API 26). [Google Play target API requirements](https://support.google.com/googleplay/android-developer/answer/11926878)

## Build an internal-test candidate

From `android/`, run `./gradlew testDebugUnitTest lintDebug bundlePlay` with JDK 17 and Android SDK 36. The output is `app/build/outputs/bundle/play/app-play.aab`. By default this is an unsigned bundle for inspection. To build an upload-signed bundle, supply the registered upload keystore through `KEYSTORE_PATH` (absolute path), `KEYSTORE_PASSWORD`, `KEY_ALIAS` and `KEY_PASSWORD` in the build environment. CI builds and checks the Play bundle on pull requests. The repository's tag workflow publishes a separate APK to GitHub.

Before the first Play release, confirm the package registration, the highest version code in Play Console and the app-signing certificate. Existing signed APK installations can upgrade in place only when Android accepts the Play app-signing identity and version. If continuity with existing users is required, enroll the corresponding existing signing key through Play Console. Keep signing and upload keys outside the repository. [Play App Signing](https://developer.android.com/studio/publish/app-signing)

After the release changes reach main, maintainers can run **Actions → Android Build → Run workflow** on main. It tests the app, signs the bundle with the existing repository Android release key, checks its public certificate fingerprint and creates the `friendorfoe-signed-release` workflow artifact. Download `friendorfoe-play.aab` and its SHA-256 file from that artifact. The manual run has no store upload or GitHub Release step. If Play has a different registered upload certificate, use its registered key with the local build instructions instead.

## Test the update route

More → App updates asks Google Play which release the current installation can receive. If an update is available, Update opens Friend or Foe in the Play Store, which handles installation and auto-update settings. On a device without the Play Store app, the listing action opens Google's website when a browser is available. The GitHub button opens the source repository.

On an internal test track, use a tester account that owns the app, install the older signed version through Play, then make a higher version code available to that account. Verify the Update action, the installed version after returning, and the current-version result. Also exercise an offline or sideloaded installation and the retry action. Sideloading an emulator build only verifies the UI and recovery paths. [Testing Play update availability](https://developer.android.com/guide/playcore/in-app-updates/test)

## Store listing

English (US). Proposed category: Tools.

**Title** — Friend or Foe

**Short description** — Find nearby recorder and camera broadcasts, trackers, beacons and aircraft.

**Full description**

Open Privacy to look for personal AI recorders first. Friend or Foe uses your Android phone to recognize nearby Bluetooth advertisements and Wi-Fi setup networks from supported devices. Explore recorder, camera, tracker and smart-glasses matches. Open a result to see the signal and name behind the match.

Search by name and narrow the list by device family or detection source. Busy iBeacon networks appear in groups. Expand a group to inspect individual beacons. Recent encounters show when your phone saw a signal and how its strength changed.

See nearby aircraft and compatible drone broadcasts, explore a map, or start Nearby Watch for aircraft notifications. You can change detection sources and permissions in Settings. An optional scanner badge or a sensor server you configure can add observations from other radios.

Detection depends on what a device broadcasts and what Android makes available to your phone. A nearby device can stay silent, and an advertising name can be reused. A match cannot tell you whether a microphone or camera is recording.

**Release notes**

Choose Light, Dark or System under App settings. Open Privacy to try a Bluetooth recorder name, filter nearby signals, and explore grouped iBeacons. Find GameChangers AI on Welcome and More, and read the privacy policy offline.

## Images for Play Console

Upload the [512 × 512 app icon](google-play-assets/app-icon.png), [1024 × 500 feature graphic](google-play-assets/feature-graphic.jpg) and the 1080 × 1920 phone screenshots below. Each screenshot is an opaque RGB PNG captured on Android 16. The graphic depicts the optional project badge.

- [Recorder observations, shown with synthetic test data](google-play-assets/recorder-observations.png)
- [Grouped iBeacons, shown with synthetic test data](google-play-assets/grouped-beacons.png)
- [Welcome in light mode](google-play-assets/welcome-light.png)
- [Interactive recorder-name example in dark mode](google-play-assets/recorder-example-dark.png)

## Internal testing and production

1. Verify the Play developer account and package registration. Use the registered upload key to sign the bundle. Select an internal tester list and upload a version code Play has not previously received. Manual Play Console upload suffices. The CI artifact and GitHub APK cannot register an app in Play Console. [Internal testing](https://support.google.com/googleplay/android-developer/answer/9845334)
2. In the current build, More → App settings → Privacy & Data → Privacy policy works offline. The GitHub Pages workflow copies the [same HTML](../android/app/src/main/res/raw/privacy_policy.html) to `_site/privacy-policy.html` when approved changes reach main. The planned public URL is `https://lnxgod.github.io/friendorfoe/privacy-policy.html`. After deployment, verify an anonymous HTTP 200 and the Friend or Foe Privacy Policy title in a private browser window before entering the URL in Play Console. The organization publicly identifies its contact email on its About page. Confirm the monitored support address for the listing and privacy inquiries. [Publisher contact details](https://gamechangersai.org/about.html) · [Google Play User Data policy](https://support.google.com/googleplay/android-developer/answer/10144311)
3. Complete the [Data safety and permissions worksheet](google-play-privacy-review.md) against the signed version. In App content, provide app access instructions, the ads declaration, target audience, content rating, and the foreground-service declaration and demonstration requested for location. Phone-only scanning has no app account. Describe optional hardware and sensor access to reviewers. If full review requires an operator server, supply reviewer-only access privately in Console. [App content requirements](https://support.google.com/googleplay/android-developer/answer/9859455)
4. Select intended countries, price and release track. Personal developer accounts created after November 13, 2023, must meet Google's closed-test rule before applying for production access. At least 12 testers must remain opted in continuously for 14 days. Check the account dashboard to see whether it applies. [Account-specific testing requirements](https://support.google.com/googleplay/android-developer/answer/14151465)
5. Exercise the signed Play installation and update route using an eligible tester. Verify native compatibility and conduct a physical Bluetooth/Wi-Fi spot check with known broadcasting devices, recording the phone, Android version and device mode. [Android native compatibility](google-play-native.md)

For foreground-service review, show the Nearby Watch explanation, permission choice, explicit Start control, ongoing notification while minimized, and Stop. In the same recording, demonstrate the user-visible consequence of stopping the watch. Describe the location purpose and the providers used. The reviewer should be able to follow those steps from a fresh install. [Foreground service review](https://support.google.com/googleplay/android-developer/answer/13392821)
