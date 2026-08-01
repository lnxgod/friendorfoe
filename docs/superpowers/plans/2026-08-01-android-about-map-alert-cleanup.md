# Android About, Badge Mark, Map, and Alert Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkboxes so progress survives handoffs.

**Goal:** Make every ordinary Android launch open a polished About landing, restore the real Triforce identity to Badge navigation and content, start the map instantly at the phone instead of visibly traveling across the world, and remove speculative possible-skimmer findings and alerts.

**Architecture:** Keep the stable seven-destination navigation shell and stable `info` route, but split its About landing from its secondary settings screen. Express map camera behavior as a pure reducer that the osmdroid bridge executes without animation. Remove serial-skimmer production at its analyzer source and add defensive filtering at both current Android privacy ingestion boundaries.

**Tech Stack:** Kotlin, Jetpack Compose, Navigation Compose, osmdroid, DataStore, coroutines, JUnit4, Compose UI tests

## Global Constraints

- Work only in `android/` plus planning documentation; do not change firmware or backend production code.
- Preserve all seven top-level destinations and the stable `info` route.
- Preserve privacy-notification deep links to exact findings; only ordinary launches default to About.
- Reuse `BadgeMarkIcon`; do not create a badge simulator or change badge/firmware commands.
- Never replace the removed possible-skimmer label with a different speculative warning.
- Follow strict red-green-refactor: write one failing regression, run it and inspect the expected failure, then make the smallest production change.
- Use behavior assertions in tests, not source-code grep assertions. Source grep is permitted only as a final verification check.
- Commit after each coherent task once its focused tests pass.

---

### Task 1: Restore the About-first launch and navigation identity

**Files:**

- Modify: `android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/Screen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutScreen.kt`
- Create: `android/app/src/main/java/com/friendorfoe/presentation/about/AboutLandingScreen.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/data/preferences/AppPreferenceRulesTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/data/preferences/AppPreferencesRepositoryTest.kt`
- Modify: `android/app/src/androidTest/java/com/friendorfoe/presentation/navigation/NavigationShellTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/badge/BadgeNavigationContractTest.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/about/AboutLandingScreenTest.kt`

- [ ] **Step 1: Write failing launch-route unit tests**

  Update `AppPreferenceRulesTest` so null and invalid persisted routes sanitize to `info`, and add an explicit assertion that the normal launch route is `info` even when a valid prior top-level route such as `privacy` is saved.

- [ ] **Step 2: Run the launch-route test and confirm the expected red state**

  Run:

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.preferences.AppPreferenceRulesTest' --console=plain
  ```

  Expected: failure showing the current `ar_view`/saved-route behavior instead of `info`.

- [ ] **Step 3: Implement one authoritative normal-launch rule**

  In `AppPreferencesRepository.kt`, introduce:

  ```kotlin
  internal const val DEFAULT_TOP_LEVEL_ROUTE = "info"

  internal fun normalLaunchRoute(): String = DEFAULT_TOP_LEVEL_ROUTE
  ```

  Make `sanitizeTopLevelRoute` fall back to that constant. Make repository initialization emit `LaunchState.Ready(normalLaunchRoute())` for an already-onboarded ordinary launch instead of restoring the previous destination. Keep storing the last route for in-session/state compatibility, and make onboarding completion enter `info`.

- [ ] **Step 4: Re-run the launch-route unit test**

  Run the command from Step 2. Expected: pass.

- [ ] **Step 5: Write the failing repository persistence regression**

  Update `AppPreferencesRepositoryTest` to save `privacy`, recreate the repository, and expect `LaunchState.Ready("info")`. Preserve an assertion that onboarding completion also yields `info`.

- [ ] **Step 6: Run the focused instrumentation test and confirm red, then make it green**

  Run:

  ```bash
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.data.preferences.AppPreferencesRepositoryTest --console=plain
  ```

  If the repository work in Step 3 already satisfies this newly written test, use `git show HEAD:android/app/src/main/java/com/friendorfoe/data/preferences/AppPreferencesRepository.kt` to temporarily confirm the test fails against the pre-change behavior; do not weaken the test. Restore the working implementation and re-run to green.

- [ ] **Step 7: Write failing navigation identity tests**

  In `NavigationShellTest`, assert the seven labels end in `About`, the stable top-level route remains `info`, and a secondary `info/settings` route exists. In `BadgeNavigationContractTest`, assert `TopLevelDestination.BADGE.icon` is the exact `BadgeMarkIcon` vector instance.

- [ ] **Step 8: Run the two navigation tests and confirm the expected failures**

  Run:

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.badge.BadgeNavigationContractTest' --console=plain
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.navigation.NavigationShellTest --console=plain
  ```

  Expected: `Info`, Tune icon, and absent settings-route assertions fail.

- [ ] **Step 9: Rename only the user-facing top-level identity**

  In `Screen.kt`, represent the stable route as `Screen.About("info")`, add `Screen.AboutSettings("info/settings")`, rename the enum entry to `ABOUT`, label it `About`, and assign `BadgeMarkIcon` to `BADGE`. Update exhaustive references throughout Android code and tests. Do not change the `info` route string or add an eighth top-level destination.

- [ ] **Step 10: Re-run the navigation tests**

  Run the command from Step 8. Expected: pass.

- [ ] **Step 11: Write the failing About landing Compose test**

  Create `AboutLandingScreenTest.kt`. Render the landing with no-op actions and assert these semantics/test tags and text:

  - `about_landing`
  - `about_triforce`
  - `Friend or Foe`
  - `Were you at our DEF CON talk? Thank you for coming—we're glad you're here.`
  - evidence language stating observations are not proof of identity, intent, or ownership
  - `about_app_settings`
  - `about_reference`
  - `about_contact`
  - `about_github`

  Click `about_app_settings` and assert its callback fires exactly once.

- [ ] **Step 12: Run the About landing test and confirm it fails because the composable does not exist**

  Run:

  ```bash
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.about.AboutLandingScreenTest --console=plain
  ```

- [ ] **Step 13: Build the About landing from existing visual primitives**

  Create `AboutLandingScreen.kt` with an `AboutLandingActions` value object containing `onOpenSettings`, `onOpenReference`, `onContactSupport`, and `onOpenGithub`. Use the real gold `BadgeMarkIcon`, the established dark surfaces/typography, concise supported-observation copy, the exact DEF CON sentence, evidence caveat, version row, and clear actions. Keep it compact and non-scrolling where it fits, while allowing a small-screen vertical scroll rather than clipping.

- [ ] **Step 14: Split landing from settings without losing working controls**

  Rename the current settings-oriented `AboutScreen` composable to `InfoSettingsScreen`. Keep `InfoContent` behavior and its existing test defaults intact, adding a flag only if needed to suppress the duplicate top-level header. Register `AboutLandingScreen` at `info` and `InfoSettingsScreen` behind the standard secondary header at `info/settings` in `FriendOrFoeNavGraph.kt`. Wire reference, support, GitHub, and safe URI actions through the existing mechanisms.

- [ ] **Step 15: Re-run About, navigation, and existing Info tests**

  Run:

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.data.preferences.AppPreferenceRulesTest' --tests 'com.friendorfoe.presentation.badge.BadgeNavigationContractTest' --console=plain
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.about.AboutLandingScreenTest,com.friendorfoe.presentation.about.InfoScreenTest,com.friendorfoe.data.preferences.AppPreferencesRepositoryTest,com.friendorfoe.presentation.navigation.NavigationShellTest --console=plain
  ```

  Expected: all pass, including existing settings behavior.

- [ ] **Step 16: Commit Task 1**

  ```bash
  git add android/app/src docs/superpowers/plans/2026-08-01-android-about-map-alert-cleanup.md
  git commit -m "android: restore About-first landing"
  ```

---

### Task 2: Restore the Triforce on the Badge screen

**Files:**

- Modify: `android/app/src/main/java/com/friendorfoe/presentation/badge/BadgeControlScreen.kt`
- Create: `android/app/src/androidTest/java/com/friendorfoe/presentation/badge/BadgeHeaderTest.kt`

- [ ] **Step 1: Write a failing Badge header test**

  Render the smallest accessible Badge header surface and assert the title `Badge` plus a visible node tagged `badge_triforce`.

- [ ] **Step 2: Run the test and verify the expected failure**

  ```bash
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.badge.BadgeHeaderTest --console=plain
  ```

- [ ] **Step 3: Add the compact Badge header**

  Add an internal `BadgeHeader()` composable as the first `LazyColumn` item before USB status. Render `BadgeMarkIcon` with `BadgeMarkGold` and the `Badge` title; do not add hardware simulation, settings, or new badge commands.

- [ ] **Step 4: Re-run Badge header and navigation contract tests**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.badge.BadgeNavigationContractTest' --console=plain
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.badge.BadgeHeaderTest --console=plain
  ```

- [ ] **Step 5: Commit Task 2**

  ```bash
  git add android/app/src
  git commit -m "android: restore Badge Triforce treatment"
  ```

---

### Task 3: Make map startup local, instant, and respectful of user control

**Files:**

- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapPresentation.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewScreen.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapOverlayController.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/map/MapViewModel.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/map/MapPresentationTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/map/MapOverlayPresentationTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/map/MapScreenLifecycleActionsTest.kt`

- [ ] **Step 1: Replace the timer contract with failing pure camera-policy tests**

  Remove the obsolete ten-second pan-timeout expectation. Add tests for a reducer with these commands:

  ```kotlin
  internal enum class MapCameraAction {
      WaitForLocation,
      InitializeAtPhone,
      FollowPhone,
      KeepUserCamera,
  }
  ```

  Test that usable permission without a valid position waits; first valid position initializes; subsequent GPS updates keep the camera; follow explicitly centers; and any user-controlled state keeps the camera even when location changes.

- [ ] **Step 2: Run the map unit tests and confirm the new assertions fail**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.map.MapPresentationTest' --tests 'com.friendorfoe.presentation.map.MapOverlayPresentationTest' --tests 'com.friendorfoe.presentation.map.MapScreenLifecycleActionsTest' --console=plain
  ```

- [ ] **Step 3: Implement the pure camera reducer and reveal policy**

  In `MapPresentation.kt`, add `INITIAL_MAP_ZOOM = 13.0`, the command enum, and a pure `mapCameraAction(...)` function. Treat finite coordinates other than the uninitialized `(0, 0)` sentinel as valid. Reveal the actual map immediately when location permission is unavailable, but keep it behind the locating surface when usable permission exists and no valid fix has arrived.

- [ ] **Step 4: Re-run the pure map tests**

  Run the command from Step 2. Expected: reducer tests pass while any still-unimplemented bridge behavior remains red.

- [ ] **Step 5: Remove timed auto-resume from the Compose bridge**

  In `MapViewScreen.kt`, replace `mapPanActivity`/`MAP_PAN_TIMEOUT_MS` with saveable `userControlsCamera`. Set it on `ACTION_DOWN`, immediately call `viewModel.stopFollowingCompass()`, and never clear it on a timer. Clear it only when the user explicitly enables the follow control. Show a lightweight locating state instead of `AndroidView` while usable permission exists but location is invalid.

- [ ] **Step 6: Execute camera commands without animation**

  In `MapOverlayController.kt`, retain camera initialization state and apply reducer output. For `InitializeAtPhone`, call `setZoom(INITIAL_MAP_ZOOM)` followed by `setCenter(userGeoPoint)`. For `FollowPhone`, call only `setCenter(userGeoPoint)`. For other commands, leave camera state untouched. Delete all startup/follow `animateTo` and distance-triggered recentering.

- [ ] **Step 7: Add the explicit follow-stop ViewModel action**

  Add `stopFollowingCompass()` to `MapViewModel`; it must disable follow without mutating location or selected targets. Ensure the existing follow toggle is the only path that re-enables automatic centering.

- [ ] **Step 8: Re-run all focused map tests**

  Run the command from Step 2. Expected: pass.

- [ ] **Step 9: Commit Task 3**

  ```bash
  git add android/app/src
  git commit -m "android: center map instantly at phone location"
  ```

---

### Task 4: Remove speculative possible-skimmer detection and alerts

**Files:**

- Modify: `android/app/src/main/java/com/friendorfoe/detection/BleThreatAnalyzer.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/detection/GlassesDetector.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PhonePrivacySourceAdapter.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/BackendPrivacySourceAdapter.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicy.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/BleThreatAnalyzerTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/detection/GlassesDetectorBehavioralTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PhonePrivacySourceAdapterTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/BackendPrivacySourceAdapterTest.kt`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/privacy/PrivacyAlertPolicyTest.kt`

- [ ] **Step 1: Write the analyzer removal regression first**

  Replace one existing serial-skimmer-positive test with `serial UUID observations never produce a threat signal`. Feed the same qualifying sequence and assert no `BleThreatSignal` is emitted. Keep all pairing-spam tests unchanged.

- [ ] **Step 2: Run the analyzer test and confirm the expected failure**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.BleThreatAnalyzerTest' --console=plain
  ```

  Expected: a current `SerialSkimmer` emission violates the empty-result assertion.

- [ ] **Step 3: Stop producing the signal at its source**

  Remove `BleSerialEvidence`, `BleThreatSignal.SerialSkimmer`, serial tracking state, serial observation helpers, and serial constants from `BleThreatAnalyzer`. Make `observe()` return only supported signals such as pairing spam, and keep reset behavior for those signals. Delete obsolete tests that specify serial-skimmer scoring internals after the new removal regression is green.

- [ ] **Step 4: Remove downstream behavioral conversion and arbitration**

  Remove the `SerialSkimmer` branch and special arbitration/protection logic from `GlassesDetector`. Keep pairing-spam conversion intact and adjust exhaustive `when` expressions. Delete only obsolete serial-skimmer tests from `GlassesDetectorBehavioralTest`; pairing-spam regressions must remain and pass.

- [ ] **Step 5: Re-run analyzer and detector behavior tests**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.BleThreatAnalyzerTest' --tests 'com.friendorfoe.detection.GlassesDetectorBehavioralTest' --console=plain
  ```

- [ ] **Step 6: Write failing defensive phone and alert-policy tests**

  In `PhonePrivacySourceAdapterTest`, emit a legacy `GlassesDetection` with device type `Possible Serial Skimmer` and match reason `ble_behavioral:serial_skimmer`, then assert the adapter findings remain empty. In `PrivacyAlertPolicyTest`, pass the same legacy reason into `fromDetection` and assert it returns null. Preserve a nearby non-skimmer high-risk test so the filter cannot accidentally disable all alerts.

- [ ] **Step 7: Run the two tests and confirm they fail on the legacy finding**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.privacy.PhonePrivacySourceAdapterTest' --tests 'com.friendorfoe.presentation.privacy.PrivacyAlertPolicyTest' --console=plain
  ```

- [ ] **Step 8: Add one shared Android finding-support predicate**

  Add an internal predicate that rejects exactly `matchReason == "ble_behavioral:serial_skimmer"`. Apply it at the beginning of phone observation ingestion before mapping/storing/publishing, and again in `PrivacyAlertPolicy.fromDetection` as defense in depth. Do not use display-title matching as the primary rule.

- [ ] **Step 9: Re-run phone adapter and policy tests**

  Run the command from Step 7. Expected: pass.

- [ ] **Step 10: Write a failing backend-adapter suppression test**

  In `BackendPrivacySourceAdapterTest`, return a backend device whose `privacyKind` is `SKIMMER` and assert current Android findings are empty. Add or retain a supported backend-kind assertion to prove ordinary rows still map.

- [ ] **Step 11: Run the backend adapter test and confirm it fails**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.privacy.BackendPrivacySourceAdapterTest' --console=plain
  ```

- [ ] **Step 12: Filter explicit legacy backend skimmer rows in Android**

  Add an internal DTO support predicate based on case-insensitive `privacyKind != "SKIMMER"` and filter before `mapDevice`. Do not edit backend source or change its API.

- [ ] **Step 13: Run the complete focused privacy/detection suite**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.detection.BleThreatAnalyzerTest' --tests 'com.friendorfoe.detection.GlassesDetectorBehavioralTest' --tests 'com.friendorfoe.presentation.privacy.PhonePrivacySourceAdapterTest' --tests 'com.friendorfoe.presentation.privacy.BackendPrivacySourceAdapterTest' --tests 'com.friendorfoe.presentation.privacy.PrivacyAlertPolicyTest' --console=plain
  ```

- [ ] **Step 14: Commit Task 4**

  ```bash
  git add android/app/src
  git commit -m "android: remove speculative skimmer alerts"
  ```

---

### Task 5: Verify the complete Android experience and build an installable APK

**Files:**

- Verify: all changed Android source and tests
- Artifact: `android/app/build/outputs/apk/debug/app-debug.apk`

- [ ] **Step 1: Check patch hygiene and exact scope**

  ```bash
  git diff --check origin/main...HEAD
  git diff --name-only origin/main...HEAD
  ```

  Expected: no whitespace errors; production changes appear only under `android/`; documentation appears only under `docs/superpowers/`; no `esp32/` or `backend/` files.

- [ ] **Step 2: Verify the removed copy/path is absent from Android production source**

  ```bash
  rg -n -i 'Possible Serial Skimmer|ble_behavioral:serial_skimmer' android/app/src/main
  ```

  Expected: no matches. Legacy strings may remain in regression test fixtures only.

- [ ] **Step 3: Run all Android JVM tests and assemble from clean state**

  ```bash
  cd android && ./gradlew clean testDebugUnitTest assembleDebug --console=plain
  ```

  Expected: `BUILD SUCCESSFUL` and a fresh `app-debug.apk`.

- [ ] **Step 4: Run the focused Compose instrumentation suite**

  ```bash
  cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.about.AboutLandingScreenTest,com.friendorfoe.presentation.about.InfoScreenTest,com.friendorfoe.presentation.badge.BadgeHeaderTest,com.friendorfoe.data.preferences.AppPreferencesRepositoryTest --console=plain
  ```

- [ ] **Step 5: Install the fresh APK on the emulator**

  ```bash
  adb -s emulator-5554 install -r android/app/build/outputs/apk/debug/app-debug.apk
  adb -s emulator-5554 emu geo fix -115.1398 36.1699
  ```

- [ ] **Step 6: Perform Android emulator QA using the Android emulator QA skill**

  Verify and capture screenshots/UI trees for:

  1. Complete onboarding if needed; About appears after completion.
  2. Navigate elsewhere, fully stop/relaunch the app, and confirm an ordinary launch returns to About.
  3. Confirm the exact DEF CON message, evidence caveat, real Triforce, and App settings action.
  4. Open App settings and use Back to return to About.
  5. Confirm the Badge nav icon and Badge header both use the Triforce.
  6. Open Map with the mocked Las Vegas location and compare immediate and delayed screenshots; there must be no visible country/world travel.
  7. Pan the map, wait longer than ten seconds, and confirm GPS updates do not steal the camera. Explicit follow may recenter instantly.
  8. Confirm Privacy contains no `Possible Serial Skimmer` copy.
  9. Send a valid privacy-finding notification intent fixture, if the existing test harness supports it, and confirm it still opens that exact finding rather than About.

- [ ] **Step 7: Inspect logcat for new crashes or navigation errors**

  ```bash
  adb -s emulator-5554 logcat -d -t 600 AndroidRuntime:E FriendOrFoe:E '*:S'
  ```

  Expected: no crash or navigation exception from the verified flow.

- [ ] **Step 8: Review the final diff and commit only necessary QA fixes**

  ```bash
  git status --short
  git diff origin/main...HEAD -- android
  ```

  If QA required code changes, repeat the relevant focused tests plus Steps 1–7, then commit with a concise Android-scoped subject. Do not claim completion until the fresh verification output is green.

- [ ] **Step 9: Report the installable artifact**

  Report the absolute APK path and its checksum. Do not tag, push, merge, or create a GitHub release unless the user explicitly asks for publication after reviewing this build.
