# Friend or Foe for iPhone

Friend or Foe by GameChangers AI runs on iPhone with iOS 17 or later. Open the recorder guide to try the classifier offline, or start a foreground Bluetooth scan to see nearby signals. The platform comparison describes which Android features work directly on iPhone and which need a scanner station. [iPhone feature parity](../docs/iphone-parity.md)

The public iPhone privacy policy is prepared for GitHub Pages. After the Pages workflow deploys from `main`, verify the proposed URL opens without signing in before using it in App Store Connect. [Policy source](../docs/iphone-privacy.html) · [Proposed public policy URL](https://lnxgod.github.io/friendorfoe/iphone-privacy.html)

## Run in Xcode

1. Check out the entire repository. The Xcode project bundles reference photos from the sibling Android source tree.
2. Open `ios/FriendOrFoe.xcodeproj` in Xcode. Xcode 26.4 and the iOS 26.4 Simulator are the tested combination. If the runtime is missing, install it from **Xcode → Settings → Components**.
3. Select the **FriendOrFoe** scheme, choose an iPhone simulator, and press **⌘R**. Simulator builds work without a signing team. [Apple’s guide to running apps](https://developer.apple.com/documentation/xcode/building-and-running-an-app)
4. Tap **Open Privacy**. The **Recorders** filter is selected. Open **Supported recorders & name example** to try the Plaud example without hardware or permissions. **More → Appearance** switches between System, Light and Dark.

To run the unit and UI tests, press **⌘U** or run these commands from the repository root. Substitute an installed destination shown by the first command if needed.

```sh
xcrun simctl list devices available
python3 ios/Tools/export_reference.py --check
xcodebuild test \
  -project ios/FriendOrFoe.xcodeproj \
  -scheme FriendOrFoe \
  -destination 'platform=iOS Simulator,name=iPhone 17 Pro,OS=26.4' \
  -parallel-testing-enabled NO
```

Changes to Android’s aircraft and drone catalogs or photo policy can require rebuilding the iPhone index. Run `python3 ios/Tools/export_reference.py` and include the updated `ios/FriendOrFoe/More/ReferenceIndex.json`. The iPhone GitHub Actions workflow checks the index and runs both test targets on a fresh simulator. [Workflow](../.github/workflows/ios-build.yml) · [GitHub’s macOS 26 runner image](https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md)

## Install on a test iPhone

1. Connect and unlock the iPhone. Trust the Mac when prompted. After pairing, enable **Settings → Privacy & Security → Developer Mode** on the iPhone, then restart and confirm. [Apple’s Developer Mode instructions](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device)
2. Add your Apple Account in Xcode Settings. In the **FriendOrFoe** target’s **Signing & Capabilities**, choose your development team and enable automatic signing. Use an identifier your team owns. The project specifies `org.gamechangersai.FriendOrFoe`. For a personal test, make any identifier change locally and keep the signing identity in your Mac keychain. Select the paired iPhone in Xcode and press **⌘R**. [Apple’s device and signing guide](https://developer.apple.com/documentation/xcode/building-and-running-an-app)
3. In **Privacy**, start a scan near a supported, advertising recorder you control. Allow Bluetooth when asked. Open its result to check the source, signal, observation time and match evidence. Repeat with the device’s Bluetooth advertising disabled. A matching broadcast identifies a possible recorder. Its recording state remains unknown.
4. Under **Beacons**, enter a test iBeacon’s known UUID, start ranging and allow location access. Verify the network group and its individual observations. Exercise permission denial and recovery for Bluetooth, location, camera and microphone. Lens inspection, sound check and radio reception need a physical phone for meaningful results.
5. If you have a scanner server, enter its HTTPS base address under **Station → Add server** and refresh. Check a reported Wi-Fi alert with a known SSID and the displayed last observation time. **Forget server** clears its configuration and reports. For a scanner badge, join its Wi-Fi and open **Station → Badge diagnostics** to read its status in the browser.

## TestFlight

1. Use the publisher’s Apple Developer Program team. Register the app’s final bundle identifier for that team, then create its iOS app record under **App Store Connect → Apps → New App** using the matching identifier. [Create an app record](https://developer.apple.com/help/app-store-connect/create-an-app-record/add-a-new-app)
2. In Xcode, verify the app target’s release signing team, marketing version and build number. Each subsequent upload for a version needs a new build number. Choose **Any iOS Device (arm64)** as the destination and **Product → Archive**. In Organizer, select the archive, then **Distribute App → App Store Connect** and upload it. Keep signing certificates, profiles and account credentials outside the repository. [Prepare an app for distribution](https://developer.apple.com/documentation/xcode/preparing-your-app-for-distribution) · [Upload an archive](https://developer.apple.com/documentation/xcode/distributing-your-app-for-beta-testing-and-releases)
3. After Apple processes the build, open **TestFlight** in the app record. Complete the applicable export-compliance questions, enter the features to test and add the build to an internal group. Install through TestFlight on the test iPhone. External testing adds beta review for the first build. [TestFlight setup](https://developer.apple.com/help/app-store-connect/test-a-beta-version/testflight-overview/)
4. For the public release, complete the store listing, screenshots, support and privacy-policy URLs, privacy disclosures and review notes. Tell reviewers how to open the offline recorder example without external hardware. Select the tested build and submit it to App Review. [App Store Connect workflow](https://developer.apple.com/help/app-store-connect/get-started/app-store-connect-workflow)
