# Android CI GitHub Release Design

**Date:** 2026-07-19
**Status:** Approved for implementation
**Release commit base:** `204ea0dcf624232ddfbfb9272fa090a6e94bee24`

## Objective

Publish the reviewed DEF CON 34 Android badge interface as a directly downloadable, production-signed APK on GitHub Releases. GitHub Actions must build, verify, and publish the APK from an immutable tag. The Android release must not build, attach, deploy, publish, or flash ESP32 firmware.

## Selected Approach

Use the repository's existing tag-driven release path with a fresh Android-only `v*` tag:

`v0.64.69-android-defcon34-badge-ui`

The tag begins with `v`, so `.github/workflows/android-build.yml` runs its signed release job. It also contains `android`, so the existing exclusion in `.github/workflows/esp32-web-flasher.yml` skips the firmware build for both the tag-push event and the subsequent published-release event.

The Android release is a published GitHub prerelease, not an Actions-only artifact and not the repository's Latest release. Its APK and checksum are attached directly to the release.

## Alternatives Considered

### Actions artifact only

This is already available through draft PR #3, but GitHub wraps the APK in a ZIP, requires navigating through a workflow run, and expires the artifact after 30 days. It does not meet the requirement for a normal GitHub Release.

### Full product `v*` release

A product tag without the `android` marker would intentionally build and attach all scanner and uplink firmware targets. That is inappropriate for this Android-only delivery and would broaden the release beyond the approved scope.

### Separate rolling release publisher

A moving `android-latest` tag or a second workflow could provide a stable link, but moving tags weakens provenance and a second publisher duplicates signing and release logic. The immutable existing tag path is safer and easier to audit.

## Release Identity

The Android application identity changes to:

- `versionCode = 111`
- `versionName = "0.64.69-defcon34-badge-ui"`
- Git tag: `v0.64.69-android-defcon34-badge-ui`
- APK asset: `friendorfoe-v0.64.69-android-defcon34-badge-ui.apk`
- Checksum asset: `friendorfoe-v0.64.69-android-defcon34-badge-ui.apk.sha256`

The higher version code permits an update over the currently published version code 110 when the existing installation uses the same production signing identity.

## CI/CD Flow

1. Commit the version, workflow hardening, and contract test to `codex/defcon34-badge-final`.
2. Push the branch and require its pull-request Android check to pass.
3. Create the immutable annotated tag `v0.64.69-android-defcon34-badge-ui` at the verified commit.
4. Push that tag explicitly to `origin`.
5. The Android Build workflow runs `testDebugUnitTest` and `assembleDebug` in its build gate.
6. The release job runs only after that gate succeeds.
7. The release job fails before decoding a keystore if any signing secret is empty.
8. The release job builds `assembleRelease` with the repository's production signing secrets.
9. CI verifies the resulting APK signature, package name, version code, and version name.
10. CI renames the APK, generates its SHA-256 sidecar, and publishes both files in a GitHub prerelease.
11. CI marks Android-only tags as prereleases and never marks them Latest.
12. The ESP32 workflow sees `android` in the tag and skips its firmware build; its dependent Pages deployment is consequently skipped.

## Signing and Validation

The release job uses the existing secrets:

- `KEYSTORE_BASE64`
- `KEYSTORE_PASSWORD`
- `KEY_ALIAS`
- `KEY_PASSWORD`

The workflow must fail closed if any value is absent. It must not print secret values.

After `assembleRelease`, CI locates Android SDK `apksigner` and `aapt` from the runner's installed build tools. It verifies:

- APK Signature Scheme verification succeeds.
- Package name is exactly `com.friendorfoe`.
- Version code is exactly `111`.
- Version name is exactly `0.64.69-defcon34-badge-ui`.

The release contains the APK and a `sha256sum` sidecar generated from that exact file.

## Firmware Isolation

No file below `esp32/` is modified for this release.

The existing ESP32 workflow exclusion remains the enforcement point:

- Tag-push events whose `github.ref_name` contains `android` skip the firmware build.
- Published-release events whose `github.event.release.tag_name` contains `android` skip the firmware build.
- The Pages deployment requires a successful firmware build, so it also skips.
- No physical flashing mechanism is part of GitHub Actions.

A contract test locks both the Android publisher and the ESP32 exclusion so a future workflow edit cannot silently couple Android-only releases back to firmware publication.

## Failure Handling

- If Android tests or the debug build fail, the release job does not start.
- If signing secrets are missing, the release job stops before creating a keystore or release.
- If the release APK fails signature or identity verification, no release is published.
- If release publication fails, the pushed tag remains immutable; the failed tag is not moved or reused. A corrected build receives a new version and tag.
- The GitHub Release is not considered shipped until the published asset is downloaded and independently verified.

## Test Strategy

### Contract test first

Add `backend/tests/test_android_release_workflow_contract.py` before changing production configuration. The initial run must fail because version 111, the release verification steps, test-gated build, checksum asset, and Android prerelease/latest policy are not yet present.

The contract test will assert:

- Gradle contains the exact version code and name.
- Android CI runs unit tests before release publication.
- The release job is gated on the build job and has `contents: write` only where required.
- Signing secrets are checked before keystore decoding.
- The release APK is checked with `apksigner` and `aapt`.
- Both the APK and checksum are attached.
- Tags containing `android` publish as prerelease and not Latest.
- The ESP32 workflow skips Android tag-push and release-published events.

### Local verification

- Run the focused workflow contract test.
- Run the complete Android JVM suite.
- Build the debug APK locally.
- Run `git diff --check` for the exact implementation scope.
- Recompute the frozen ESP32 diff and status fingerprints to prove no firmware edits entered the Android release patch.

### Published-release verification

- Confirm the final Android workflow run succeeds.
- Confirm the ESP32 workflow is skipped for the Android tag and release.
- Confirm the GitHub Release is published as a prerelease and not Latest.
- Confirm exactly the APK and checksum are present as Android release assets.
- Download the APK from GitHub Releases.
- Compare it with the published checksum.
- Verify its production signature and exact package/version metadata locally.
- Install it on a real Android phone for the remaining USB-host hardware gate.

## Scope Boundaries

Included:

- Android version identity.
- Android GitHub Actions test, signing, verification, checksum, and release policy.
- A workflow contract regression test.
- Creation and explicit push of the fresh Android-only release tag.
- Verification of the published APK.

Excluded:

- Any ESP32 source, binary, manifest, or firmware version change.
- Any badge flashing.
- Merging draft PR #3.
- Publishing a full production firmware release.
- Reusing or moving an existing tag.

## Acceptance Criteria

The work is complete only when all of the following are true:

1. GitHub shows a published prerelease for `v0.64.69-android-defcon34-badge-ui`.
2. The release directly exposes the production-signed APK and its SHA-256 sidecar.
3. The downloaded APK reports package `com.friendorfoe`, version code 111, and version `0.64.69-defcon34-badge-ui`.
4. Local checksum and signature verification pass on the downloaded release asset.
5. Android unit tests and both debug and release builds are green in the required environments.
6. Android-only release handling does not publish firmware assets or deploy the firmware site.
7. The frozen ESP32 source/status fingerprints are unchanged by the implementation patch.
8. The release page URL and direct APK asset URL are handed to the user for installation.
