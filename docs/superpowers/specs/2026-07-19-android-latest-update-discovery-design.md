# Android Latest Update Discovery Design

**Date:** 2026-07-19
**Status:** Approved by the user's explicit instruction to merge PR #3, increment the release, and do what is needed to make in-app updates visible.

## Problem

The Android app requests GitHub's `/repos/lnxgod/friendorfoe/releases/latest` endpoint and compares the returned tag, without its leading `v`, to `BuildConfig.VERSION_NAME`. The signed `v0.64.69-android-defcon34-badge-ui-r1` release is a prerelease and is not Latest, so that endpoint still returns `v0.64.68-live-follow`. An installed 0.64.68 build therefore reports that it is current.

The equality-only comparison has two adjacent defects: a newer app can offer an older Latest release as an update, and a release tag that does not exactly match the embedded version name can cause a permanent update prompt. The backend firmware catalog also selects the newest GitHub release blindly; an APK-only Android release can therefore temporarily replace the firmware catalog with an empty set.

## Considered Approaches

1. **Promote the existing r1 release.** Fast, but the tag and embedded version differ, so version 111 would permanently report itself as outdated. It also does not satisfy the requested +1 release.
2. **Teach only the new app to read prereleases.** Does not help already-installed apps because they continue calling `/releases/latest`.
3. **Roll forward with an exact stable tag and robust comparison.** Recommended. Existing apps discover the stable Latest release, while the new app handles ordering safely.

## Release Identity and Publication

- Android `versionCode`: `112`
- Android `versionName`: `0.64.70-android-defcon34-badge-ui`
- Git tag: `v0.64.70-android-defcon34-badge-ui`
- The tag minus `v` exactly equals the embedded version name.
- Android-only tags containing the exact `-android-` marker publish as normal, non-prerelease releases and become Latest.
- Beta, alpha, and release-candidate tags remain prereleases.
- The existing ESP32 `-android-` guard remains unchanged, so the release tag builds and publishes no firmware assets.
- The new tag must point to the merged `main` commit, not the feature-branch head.

## Android Update Policy

Extract a pure `AppUpdatePolicy` from `WelcomeScreen.kt`. It parses the first three numeric components after an optional leading `v`, compares them numerically, and fails closed for malformed remote tags.

- Remote newer than current: show `UpdateAvailable`.
- Remote equal to current, even with a differing descriptive suffix: show `UpToDate`.
- Remote older than current: show `UpToDate`; never offer a downgrade.
- Numeric components compare numerically, so `0.64.100` is newer than `0.64.99`.
- Network and JSON failures retain the existing user-facing error behavior.

The existing public GitHub release URL remains the download destination.

## Backend Firmware Catalog Safety

When refreshing GitHub releases, select the newest non-draft release that contains supported firmware `.bin` assets. Skip APK-only releases. If no release contains firmware assets, preserve the existing catalog rather than replacing it with an empty catalog.

This change does not alter firmware images, manifests, or badge update protocols.

## PR Integration

PR #3 is draft and its ESP32 check is red because six test functions are referenced by `test_runner.c` but are not present in the committed branch. Remove only those orphan declarations and `RUN_TEST` calls from the committed PR state. Preserve the user's uncommitted firmware work, where the matching implementations exist, without staging it.

After focused and full Android/backend tests pass and the exact PR head has green required checks:

1. Mark PR #3 ready.
2. Merge it into `main` with a merge commit so its reviewed history is retained.
3. Verify merged-main CI.
4. Tag the merged commit with the exact v0.64.70 Android tag.
5. Verify the signed public APK, checksum, signer, package/version metadata, two-asset release shape, firmware workflow skip, and `/releases/latest` response.

## Testing and Release Gates

- JVM tests cover newer, equal, older, suffix variation, numeric ordering, and malformed tags.
- Workflow contract tests require versionCode 112, exact version/tag-compatible name, stable/Latest Android-only release metadata, immutable action pins, and the unchanged ESP32 skip guard.
- Backend tests cover an APK-only release followed by a firmware-bearing release.
- Android unit tests and debug build pass on the exact release commit.
- The public APK verifies against production signer SHA-256 `3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95`.
- No firmware source, binary, manifest, or hardware is modified or flashed for this release.

## Self-Review

- No placeholders or deferred requirements remain.
- The embedded version and tag are identical after removing the leading `v`.
- Stable Latest publication and firmware isolation use separate conditions.
- Existing installed apps can discover the release without first installing new updater code.
- The design fixes downgrade/permanent-prompt behavior for subsequent checks.
