# Android AR Overlay Range Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make valid ADS-B aircraft appear in Android AR out to 12 statute miles and stop approximate location from blanking the radio overlay.

**Architecture:** Put aircraft/drone range decisions in one pure sensor-layer policy consumed by both projection and off-screen arrow selection. Pass the complete location permission state into the AR screen and derive overlay eligibility from the existing `PermissionUiState.isUsable()` contract, so precise and approximate grants follow one rule.

**Tech Stack:** Kotlin, Jetpack Compose, Android location permissions, JUnit4, Gradle, GitHub Actions

## Global Constraints

- Work only in `android/`, the Android release workflow, and planning documentation; do not change firmware or backend production code.
- Aircraft AR range is exactly 12 statute miles, or 19,312.128 meters, measured as slant distance.
- In-view aircraft labels and off-screen aircraft arrows use the same aircraft range policy.
- Drone range remains exactly 2,000 meters.
- Approximate and precise location grants permit positional overlays; denied location or a missing fix does not.
- Preserve grounded, below-30-meter, and stale-aircraft filters.
- Preserve the 50-nautical-mile ADS-B acquisition radius and ADS-B-on default.
- Use only focused regression tests during implementation, followed by one final debug APK build.

---

### Task 1: Centralize and increase AR aircraft range

**Files:**

- Create: `android/app/src/main/java/com/friendorfoe/sensor/ArVisualRangePolicy.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/sensor/SkyPositionMapper.kt:30-107`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt:822-1048`
- Modify: `android/app/src/test/java/com/friendorfoe/sensor/SkyPositionMapperTest.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/ar/ArOverlayPolicyTest.kt`

**Interfaces:**

- Produces: `ArVisualRangePolicy.includes(skyObject: SkyObject, distanceMeters: Double): Boolean`.
- Produces: `selectOffScreenRadioPositions(screenPositions: List<ScreenPosition>): List<ScreenPosition>`.
- Consumes: `SkyObject`, `Aircraft`, `Drone`, and `ScreenPosition`.

- [ ] **Step 1: Write the failing 12-mile mapper regression**

  Add a test that maps two aircraft directly above the user with the camera pitched at 90 degrees: one at 19,200 meters altitude and one at 19,400 meters. Assert the first is in view and the second is not. The hand-checked boundary is independent of geographic-distance math.

  ```kotlin
  @Test
  fun `aircraft overlay includes twelve statute miles but excludes farther aircraft`() {
      val user = Position(40.0, -74.0, 0.0)
      val orientation = DeviceOrientation(0f, 90f, 0f)
      val results = mapper.mapToScreen(
          user,
          listOf(
              createTestAircraft("INSIDE", Position(40.0, -74.0, 19_200.0)),
              createTestAircraft("OUTSIDE", Position(40.0, -74.0, 19_400.0)),
          ),
          orientation,
          CameraFovCalculator(),
      )

      assertTrue(results.single { it.skyObject.id == "INSIDE" }.isInView)
      assertFalse(results.single { it.skyObject.id == "OUTSIDE" }.isInView)
  }
  ```

- [ ] **Step 2: Write the failing shared off-screen-selection regression**

  In `ArOverlayPolicyTest`, create out-of-view aircraft positions at 19,200 and 19,400 meters plus an out-of-view drone at 2,100 meters. Assert selection returns only the nearer aircraft. This catches the old hard-coded 13,000-meter arrow branch and protects the unchanged drone range.

- [ ] **Step 3: Run the two focused tests and verify red**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.sensor.SkyPositionMapperTest' --tests 'com.friendorfoe.presentation.ar.ArOverlayPolicyTest' --console=plain
  ```

  Expected: the 19,200-meter aircraft mapper assertion fails under the old 13-kilometer limit, and the new selection contract is absent or red.

- [ ] **Step 4: Implement the minimal shared range policy**

  Create:

  ```kotlin
  internal object ArVisualRangePolicy {
      private const val METERS_PER_STATUTE_MILE = 1_609.344
      private const val AIRCRAFT_MAX_DISTANCE_METERS = 12.0 * METERS_PER_STATUTE_MILE
      private const val DRONE_MAX_DISTANCE_METERS = 2_000.0

      fun includes(skyObject: SkyObject, distanceMeters: Double): Boolean =
          distanceMeters <= when (skyObject) {
              is Aircraft -> AIRCRAFT_MAX_DISTANCE_METERS
              is Drone -> DRONE_MAX_DISTANCE_METERS
          }
  }
  ```

  Replace the private mapper constants with this policy. Extract `selectOffScreenRadioPositions` in `ArViewScreen.kt`, preserving the positive-distance check, nearest-first ordering, and eight-arrow cap, and call it from `ArOverlay`.

- [ ] **Step 5: Re-run the two focused tests and verify green**

  Run the command from Step 3. Expected: both classes pass.

- [ ] **Step 6: Commit the range behavior**

  ```bash
  git add android/app/src/main/java/com/friendorfoe/sensor android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt android/app/src/test/java/com/friendorfoe
  git commit -m "android: extend AR aircraft overlay range"
  ```

---

### Task 2: Restore approximate-location aircraft overlays

**Files:**

- Modify: `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt:139-262`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/navigation/FriendOrFoeNavGraph.kt:114-139`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/permissions/FeaturePermissions.kt:148-158`
- Modify: `android/app/src/test/java/com/friendorfoe/presentation/ar/ArOverlayPolicyTest.kt`

**Interfaces:**

- Produces: `displayedRadioPositions(screenPositions: List<ScreenPosition>, locationState: PermissionUiState): List<ScreenPosition>`.
- Consumes: `PermissionUiState.isUsable()`; `Granted` and `Approximate` are usable.

- [ ] **Step 1: Write the failing approximate-location regression**

  Add a pure test with one real `ScreenPosition` asserting that `displayedRadioPositions` retains the position for `Granted` and `Approximate`, but returns an empty list for `Denied`. The break it catches is the current exact equality check that treats approximate as unusable.

- [ ] **Step 2: Run only the overlay policy test and verify red**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.ar.ArOverlayPolicyTest' --console=plain
  ```

  Expected: failure because the display policy does not yet exist or approximate location produces an empty result.

- [ ] **Step 3: Make the AR screen consume permission state directly**

  Change `ArViewScreen` from `isPreciseLocation: Boolean` to `locationPermissionState: PermissionUiState`. Implement:

  ```kotlin
  internal fun displayedRadioPositions(
      screenPositions: List<ScreenPosition>,
      locationState: PermissionUiState,
  ): List<ScreenPosition> =
      if (locationState.isUsable()) screenPositions else emptyList()
  ```

  Use this result for labels, arrows, selection, and locks. Clear positional selection only when `!locationPermissionState.isUsable()`. Pass the full state from `FriendOrFoeNavGraph`.

- [ ] **Step 4: Correct the adjacent permission explanation**

  Replace the inaccurate claim that approximate access hides distance and bearing with concise text that says placement, distance, and bearing may be less accurate. Keep the existing non-blocking precise-location prompt.

- [ ] **Step 5: Re-run the focused permission/overlay tests and verify green**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.presentation.ar.ArOverlayPolicyTest' --tests 'com.friendorfoe.presentation.permissions.FeaturePermissionsTest' --console=plain
  ```

- [ ] **Step 6: Commit the permission fix**

  ```bash
  git add android/app/src
  git commit -m "android: restore approximate-location AR overlays"
  ```

---

### Task 3: Package and verify the Android patch release

**Files:**

- Modify: `android/app/build.gradle.kts:26-27`
- Modify: `.github/workflows/android-build.yml:114`

**Interfaces:**

- Produces: Android `versionCode = 116`, `versionName = "0.67.7-android-ar-overlay-range"`.
- Produces: Git tag `v0.67.7-android-ar-overlay-range` and matching signed APK asset.

- [ ] **Step 1: Update release identity**

  Set:

  ```kotlin
  versionCode = 116
  versionName = "0.67.7-android-ar-overlay-range"
  ```

  Update the workflow's `aapt` assertion to the same exact values.

- [ ] **Step 2: Run the final focused regression set once**

  ```bash
  cd android && ./gradlew testDebugUnitTest --tests 'com.friendorfoe.sensor.SkyPositionMapperTest' --tests 'com.friendorfoe.presentation.ar.ArOverlayPolicyTest' --tests 'com.friendorfoe.presentation.permissions.FeaturePermissionsTest' --console=plain
  ```

- [ ] **Step 3: Build one final debug APK**

  ```bash
  cd android && ./gradlew assembleDebug --console=plain
  ```

  Require exit code 0 and confirm APK identity with `aapt dump badging` when a local `aapt` binary is available.

- [ ] **Step 4: Inspect the complete diff and commit release identity**

  ```bash
  git diff --check
  git status --short
  git diff --stat origin/main...HEAD
  git add android/app/build.gradle.kts .github/workflows/android-build.yml docs/superpowers/plans/2026-08-01-android-ar-overlay-range-fix.md
  git commit -m "v0.67.7: prepare Android AR overlay release"
  ```

- [ ] **Step 5: Push, open the PR, merge after checks, tag, and verify release assets**

  Push `codex/android-ar-overlay-range`, open a ready PR against `main`, merge after required checks, then tag the merge commit `v0.67.7-android-ar-overlay-range`. Confirm the release is published and includes `friendorfoe-v0.67.7-android-ar-overlay-range.apk` plus its SHA-256 file before handing the install link to the user.
