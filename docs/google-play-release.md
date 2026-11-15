# Friend or Foe on Google Play

The Android application ID is `com.friendorfoe`. The first Play candidate in this branch uses version code 140 and targets API 36. The minimum remains Android 8 (API 26). [Google Play target API requirements](https://support.google.com/googleplay/android-developer/answer/11926878)

## Build an internal-test candidate

From `android/`, run `./gradlew testDebugUnitTest lintDebug bundlePlay` with JDK 17 and Android SDK 36. The output is `app/build/outputs/bundle/play/app-play.aab`. By default this is an unsigned bundle for inspection. To build an upload-signed bundle, supply the registered upload keystore through `KEYSTORE_PATH` (absolute path), `KEYSTORE_PASSWORD`, `KEY_ALIAS` and `KEY_PASSWORD` in the build environment. CI builds and checks the Play bundle on pull requests. The repository's tag workflow publishes a separate APK to GitHub.

Before the first Play release, confirm the package registration, the highest version code in Play Console and the app-signing certificate. Existing signed APK installations can upgrade in place only when Android accepts the Play app-signing identity and version. If continuity with existing users is required, enroll the corresponding existing signing key through Play Console. Keep signing and upload keys outside the repository. [Play App Signing](https://developer.android.com/studio/publish/app-signing)

## Test the update route

More → App updates asks Google Play which release the current installation can receive. If an update is available, Update opens Friend or Foe in the Play Store, which handles installation and auto-update settings. On a device without the Play Store app, the listing action opens Google's website when a browser is available. The GitHub button opens the source repository.

On an internal test track, use a tester account that owns the app, install the older signed version through Play, then make a higher version code available to that account. Verify the Update action, the installed version after returning, and the current-version result. Also exercise an offline or sideloaded installation and the retry action. Sideloading an emulator build only verifies the UI and recovery paths. [Testing Play update availability](https://developer.android.com/guide/playcore/in-app-updates/test)

## Store listing draft

**Title** — Friend or Foe

**Short description** — Find nearby recorder and camera broadcasts, trackers, beacons and aircraft.

**Opening copy** — Open Privacy to look for nearby personal recorders first. Android matches recognizable Bluetooth advertisements and camera setup networks. Grouped iBeacons, source filters and evidence details help you inspect a busy room. A match identifies a broadcast consistent with a device family. It cannot tell you whether anyone is recording. Devices with silent radios, including offline recorders and cameras, may be invisible.

The [Recorders view](screenshots/privacy-recorders/recorders-first.png) and [grouped iBeacons](screenshots/privacy-recorders/ibeacon-networks.png) show synthetic test observations from the Android emulator. Run the [native and packaging checks](google-play-native.md), complete the [publisher privacy worksheet](google-play-privacy-review.md), and link the publisher's public privacy policy from both the listing and the app before submission. The worksheet identifies the remaining publisher decisions about data recipients, retention and deletion.
