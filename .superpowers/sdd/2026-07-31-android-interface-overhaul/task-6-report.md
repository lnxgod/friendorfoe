# Task 6 report — explicit Capture → Review → Save

## Scope and outcome

- Android app only. No backend or ESP32 files were readied for commit or changed.
- Every AR entry route now remains read-only until the user explicitly taps **Save** from a review surface.
- Label taps open Object Peek with **Inspect**, **Capture**, and **Full details**. Exact-label and nearest-label hit paths share the same production interaction boundary.
- Zoom is inspect-only and states that no photo has been saved.
- Main shutter and Snap Photo capture one CameraX frame into an in-memory, platform-neutral `CaptureDraft`; `ImageProxy` is closed in `finally`.
- Review exposes **Save**, **Share**, and **Discard**. Failed saves expose **Retry save** and **Discard**.
- Share writes beneath `cacheDir/shared_captures`, returns a FileProvider URI, and launches ACTION_SEND with `FLAG_GRANT_READ_URI_PERMISSION`.
- `AndroidPhotoWriter` is the only Android production class that calls MediaStore/gallery output APIs. A failed partial write deletes its exact inserted URI.
- All legacy implicit/automatic capture functions, toggle/state, gallery helpers, old gallery-URI share bar, and unsupported database claim were removed.

## TDD evidence

### Reducer RED/GREEN

1. Added `CaptureReviewViewModelTest.onlyExplicitSaveWritesPhoto` first.
2. RED command:
   - `cd android && ./gradlew testDebugUnitTest --tests '*CaptureReviewViewModelTest'`
   - Failed in `compileDebugUnitTestKotlin` on unresolved `CaptureReviewViewModel`, `PhotoWriter`, `CaptureDraft`, `CapturePayload`, `SavedPhoto`, `ShareImageFactory`, and `ShareRequest`.
3. Minimal platform-neutral boundary/reducer implementation made the first contract GREEN.
4. Expanded coverage for save failure/retry, share isolation, one active save, discard during save, and cancellation. API changes to return/gate jobs produced the expected compile RED before the behavior was implemented.
5. Added delayed-share-after-discard regression. It failed at `CaptureReviewViewModelTest.kt:99` because a stale `LaunchShare` effect was emitted, then passed after review-generation gating.

### Android artifact RED/GREEN

1. Added `AndroidCaptureArtifactsTest` before Android implementations.
2. RED command:
   - `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.ar.AndroidCaptureArtifactsTest`
   - Failed in `compileDebugAndroidTestKotlin` on missing `AndroidPhotoWriter`, `MediaStoreSink`, and `AndroidShareImageFactory`.
3. Added writer/sink, cache-only share factory, FileProvider manifest/path, and Hilt bindings.
4. Added ACTION_SEND grant regression first; it failed to compile on missing `captureShareIntent`, then passed after the production consumer was extracted and used by `ArViewScreen`.

### Object Peek / entry-route RED/GREEN

1. Added `ObjectPeekTest` before the new UI contract.
2. RED command:
   - `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.ar.ObjectPeekTest`
   - Failed in `compileDebugAndroidTestKotlin` on missing `ObjectPeekState`, `ObjectPeek`, `CaptureReviewScreen`, `CaptureShutterButton`; private `ArOverlay`; and Snap Photo's legacy Boolean result/missing review callback.
3. Added the production-owned `ArCaptureInteractions` boundary and used it from both `ArViewScreen` and instrumentation tests so label/main-shutter assertions exercise the actual integration seam.
4. GREEN covers exact and nearest Canvas label hits, Inspect, inspect-only Zoom, Full details, Capture, Share, Discard, explicit Save, main shutter, Snap Photo, and failed-save retry.

## Changed files

- `android/app/src/main/AndroidManifest.xml`
- `android/app/src/main/res/xml/share_file_paths.xml`
- `android/app/src/main/java/com/friendorfoe/di/CaptureModule.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/CaptureArtifacts.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/CaptureReviewViewModel.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/AndroidPhotoWriter.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/AndroidShareImageFactory.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/ObjectPeek.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/CaptureReviewScreen.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewScreen.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/ArViewModel.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/ZoomViewSheet.kt`
- `android/app/src/main/java/com/friendorfoe/presentation/ar/SnapPhotoSheet.kt`
- `android/app/src/test/java/com/friendorfoe/presentation/ar/CaptureReviewViewModelTest.kt`
- `android/app/src/androidTest/java/com/friendorfoe/presentation/ar/ObjectPeekTest.kt`
- `android/app/src/androidTest/java/com/friendorfoe/presentation/ar/AndroidCaptureArtifactsTest.kt`

## Fresh verification after final production changes

- Focused JVM reducer:
  - `cd android && ./gradlew testDebugUnitTest --tests '*CaptureReviewViewModelTest' --rerun-tasks`
  - PASS — 7 tests, 0 failures/errors/skips; `BUILD SUCCESSFUL`.
- Combined API 35 instrumentation:
  - `cd android && ./gradlew connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.ar.ObjectPeekTest,com.friendorfoe.presentation.ar.AndroidCaptureArtifactsTest`
  - PASS on `Pixel8_API35(AVD) - 15` — 16 tests, 0 failures/errors/skips; `BUILD SUCCESSFUL`.
- Full Android JVM suite:
  - `cd android && ./gradlew testDebugUnitTest --rerun-tasks`
  - PASS — 278 tests, 0 failures/errors/skips; `BUILD SUCCESSFUL`.
- `git diff --check`
  - PASS — no output.

## Required source invariants

- Legacy-path search:
  - `rg -n "autoSaved|attemptAutoCapture|snapAndAutoCapture|NOT in any aircraft database|saveDetectionPhotos|capturePhotoToGallery|captureDualPhoto|saveBitmapToGallery" android/app/src/main/java/com/friendorfoe/presentation/ar`
  - No matches.
- Gallery sink search:
  - `rg -n "MediaStore|openOutputStream|ContentResolver.insert" android/app/src/main/java/com/friendorfoe/presentation/ar`
  - Every match is in `AndroidPhotoWriter.kt`.
- Share consumer search confirms ACTION_SEND and `FLAG_GRANT_READ_URI_PERMISSION` are both in `AndroidShareImageFactory.kt`; instrumentation also asserts the flag and stream URI.
- Stale UI search found no old Take Photo/Auto-capture/Auto-saved/Saved-to-Pictures/Capture-Photo text or toggle.
- Repository-wide Android production search for `MediaStore.Images`, `contentResolver.insert(`, and `openOutputStream(` found only `AndroidPhotoWriter.kt`.
- `TrainingDataCollector.saveLabeledCrop` remains a dormant app-private training-file method outside the AR presentation capture workflow. Search found only its definition and no reachable caller; it is not a MediaStore/gallery sink.

## Self-review and concerns

- Verified rapid Save suppression, save failure/retry, cancellation propagation, stale save/share completion suppression, cache-only sharing, exact partial-row cleanup, and one explicit row on Save.
- Verified exact and nearest AR label paths route through Object Peek and cannot reach a writer.
- Verified main shutter and Snap Photo use the same production `ArCaptureInteractions` review boundary as tests.
- Verified detail-sheet Zoom and tapped visual detections only open inspect-only `ZoomViewSheet`.
- Physical CameraX capture was not exercised on real hardware in this task; emulator coverage validates the UI/reducer/artifact boundaries, while production compilation validates `OnImageCapturedCallback` integration. A phone smoke test remains useful but is not required for this Android-only implementation commit.
- Existing project warnings remain: AGP 8.2.2/compileSdk 35 compatibility warning, Room schema export warning, and existing Android/Kotlin deprecation or unused-code warnings. No new dependency was added.

## Fix round — harden explicit photo saves

Commit target: `android: harden explicit photo saves`

### Corrected behavior

- Android 8–9 declares `WRITE_EXTERNAL_STORAGE` with `maxSdkVersion="28"`. The permission is not part of startup or AR entry gating: the production launcher runs only after an explicit **Save** tap, only on SDK 28 or earlier, and only while permission is absent. Denial preserves the draft, explains that Photos access was not granted, and offers **Retry save**. Both permission-denial retry and ordinary write-failure retry go through the same permission coordinator.
- MediaStore values branch on an injected SDK level. SDK 26–28 omits `RELATIVE_PATH`; SDK 29+ writes to `Pictures/FriendOrFoe`.
- All `AndroidPhotoWriter` instances share a process-wide mutex, and the production Hilt binding is also singleton-scoped. A newly created ViewModel therefore cannot overlap a still-unwinding write from an older ViewModel. Each inserted URI remains owned until the IO dispatch returns successfully; every failure or cancellation before that commit point deletes that exact URI once in `NonCancellable + Dispatchers.IO`, then preserves structured cancellation.
- An unfinished save remains the ViewModel operation gate until job completion. A Saving review cannot be dismissed, discarded, replaced by another capture, shared, or saved again. The modal's remembered state explicitly rejects a transition to `Hidden` while Saving, including swipe-to-hide. This replaces the initial implementation's unsafe cancel-and-replace behavior.
- Camera frame conversion uses `ImageProxy.imageInfo.rotationDegrees` and runs on a dedicated processing executor. JPEG and YUV/NV21 captures are physically normalized upright, 90°/270° dimensions are swapped, results return on the main executor, and the production-tested dispatcher closes the `ImageProxy` exactly once on success, conversion failure, or executor rejection.
- Object Peek **Capture** now directly calls the production `ArCaptureInteractions.captureWith` boundary, which requests `ArViewModel.capturePhotoDraft` and opens Review from that single tap. Snap Photo remains the distinct long-press route.
- Object Peek Inspect carries its exact evidence into Zoom. ADS-B and Remote ID evidence—including Wi-Fi NaN and Wi-Fi Beacon Remote ID transports—is no longer replaced by the contradictory “No radio match” statement.
- Inspect, Zoom, and Full details tests now exercise the production interaction owner and compare real API 35 MediaStore row counts before/after. The disconnected writer-only assertions were removed from those routes.

### Fix-round TDD evidence

1. Legacy permission/path and save lifecycle:
   - RED JVM compilation failed on missing `CaptureSavePermissionDecision`, `CaptureSaveInteractions`, and `SavePermissionDenied`.
   - RED Android compilation failed because `AndroidPhotoWriter` had no injectable SDK branch.
   - GREEN focused JVM coverage proves SDK 28 denial requests permission without writing, denial preserves the draft, grant writes once, and SDK 29+ never requests legacy access.
   - GREEN API 35 instrumentation proves SDK 28 values omit `RELATIVE_PATH`, SDK 29 values include the intended Pictures path, partial writes delete the exact row, and a cancelled non-cooperative blocking write prevents a second writer instance from inserting until exact cleanup completes (`maximumConcurrentWrites == 1`).
2. Orientation and frame ownership:
   - RED Android compilation failed on missing JPEG/YUV normalizers.
   - GREEN asymmetric-image tests prove JPEG 90° and 270° rotations occur exactly once with swapped dimensions; a YUV/NV21 90° test proves the final pixel orientation and dimensions.
   - RED JVM compilation failed on missing `dispatchCapturedFrame`.
   - GREEN dispatcher tests prove conversion is queued off the callback thread and the frame closes exactly once on converter throw and processing-executor rejection.
3. Object Peek, evidence, and read-only routes:
   - RED Android compilation failed on the missing direct capture/inspect/detail route methods and Zoom evidence input.
   - GREEN Compose coverage proves one Capture tap requests the displayed object label, dismisses Peek, opens Review, and performs no gallery write.
   - A strengthened RED required Inspect to pass the complete `ObjectPeekState`; GREEN proves `ADS-B radio match` survives the production route and Zoom omits the contradictory no-radio text.
   - Inspect, Zoom, and Full details each retain the real API 35 MediaStore row count. Saving hides Discard, and a denied legacy save renders the honest explanation plus retry action.
4. Independent-review follow-up:
   - RED two-instance instrumentation failed `expected:<1> but was:<2>`, proving an instance-local writer mutex was insufficient; GREEN passed after moving the gate process-wide and singleton-scoping the binding.
   - RED Android compilation failed on missing `CaptureReviewModal`; GREEN focused instrumentation performs a real `swipeDown` and proves a Saving modal remains displayed without invoking Discard.
   - RED JVM compilation failed on missing `objectPeekEvidence`; GREEN proves Wi-Fi NaN/Beacon retain truthful Remote ID evidence while generic Wi-Fi remains a Wi-Fi observation.

### Fresh fix-round verification

- `cd android && ./gradlew testDebugUnitTest`
  - PASS — 285 tests, 0 failures, 0 errors, 0 skipped.
- `cd android && ./gradlew connectedDebugAndroidTest '-Pandroid.testInstrumentationRunnerArguments.class=com.friendorfoe.presentation.ar.AndroidCaptureArtifactsTest,com.friendorfoe.presentation.ar.ObjectPeekTest'`
  - PASS on `Pixel8_API35(AVD) - 15` — 24 tests, 0 failures, 0 errors, 0 skipped.
- `cd android && ./gradlew assembleDebug`
  - PASS — `BUILD SUCCESSFUL`.
- Merged-manifest inspection confirms `WRITE_EXTERNAL_STORAGE` and `android:maxSdkVersion="28"`.
- `git diff --check`
  - PASS — no output.
- Repository-wide production search for `MediaStore.Images`, `contentResolver.insert(`, and `openOutputStream(` still finds only `AndroidPhotoWriter.kt`.
- `PermissionHandler.kt` contains no storage permission, and production search finds no direct `captureReviewViewModel.retrySave` bypass.
- Source inspection confirms Object Peek Capture uses `captureObjectPeek`, while `snapToObject` remains attached only to `onLabelLongPressed`.

### Environment limits and explicit follow-ups

- No API 28 emulator was already installed: both available AVDs (`PeopleNotes` and `Pixel8_API35`) use Android 35. Per task constraints, no system image was downloaded and no AVD was created. Legacy behavior is covered by the injected pure policy and SDK-specific writer-value tests; an API 28 device/AVD smoke test remains useful when one is available.
- Full-resolution Review preview decoding remains synchronous in Compose. This minor performance follow-up is explicitly deferred to Task 17; capture conversion itself is now off main.
- If a user has permanently denied legacy storage access on Android 8–9, the honest denial/retry state remains usable for Share or Discard, but a direct app-settings recovery shortcut is deferred to the broader permission-recovery work in Task 13.
- Physical CameraX capture was not exercised because no phone was attached during this fix round. Emulator coverage validates rotation math, frame ownership, UI routing, MediaStore semantics, and permission policy; production CameraX integration compiles in the final APK.
