# Android CI GitHub Release Design

**Date:** 2026-07-19
**Status:** Approved direction; awaiting written-spec review
**Release commit base:** `204ea0dcf624232ddfbfb9272fa090a6e94bee24`

## Objective

Publish the reviewed DEF CON 34 Android badge interface as a directly downloadable, production-signed APK on GitHub Releases. GitHub Actions must build, verify, and publish the APK from an immutable tag. The Android release must not build, attach, deploy, publish, or flash ESP32 firmware.

## Selected Approach

Use the repository's existing tag-driven release path with a fresh Android-only `v*` tag:

`v0.64.69-android-defcon34-badge-ui`

The tag begins with `v`, so `.github/workflows/android-build.yml` runs its signed release path. It also contains the reserved marker `-android-`, so `.github/workflows/esp32-web-flasher.yml` skips the firmware build for both tag-push and published-release events. The firmware guard is tightened from a broad `android` substring check to the exact `-android-` release marker so unrelated tag names cannot accidentally suppress a firmware release.

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
6. A `sign-release` job runs only after that gate succeeds and has `contents: read` permission.
7. `sign-release` fails before decoding a keystore if any signing secret is empty.
8. `sign-release` builds `assembleRelease` with the repository's production signing secrets.
9. CI verifies the resulting APK signature, pinned signer certificate, package name, version code, and version name.
10. `sign-release` renames the APK, generates its SHA-256 sidecar, deletes the decoded keystore, and uploads only the APK and checksum as a short-lived workflow artifact.
11. A separate `publish-release` job downloads that artifact. It has `contents: write` permission but receives no signing secrets or keystore.
12. `publish-release` attaches the APK and checksum to a GitHub prerelease with `make_latest: false` using the repository-scoped `GITHUB_TOKEN`.
13. The ESP32 workflow sees the exact `-android-` marker and skips its firmware build; its dependent Pages deployment is consequently skipped.

## Signing and Validation

The `sign-release` job uses the existing secrets:

- `KEYSTORE_BASE64`
- `KEYSTORE_PASSWORD`
- `KEY_ALIAS`
- `KEY_PASSWORD`

The workflow must fail closed if any value is absent. It must not print secret values, and the `publish-release` job must not expose or reference them.

After `assembleRelease`, CI locates Android SDK `apksigner` and `aapt` from the runner's installed build tools. It verifies:

- APK Signature Scheme verification succeeds.
- The signing-certificate SHA-256 digest exactly matches `3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95`, independently read with `apksigner` from the published `friendorfoe-v0.64.68-live-follow.apk`. The workflow pins this non-secret value and must not merely accept whatever certificate came from the supplied keystore.
- Package name is exactly `com.friendorfoe`.
- Version code is exactly `111`.
- Version name is exactly `0.64.69-defcon34-badge-ui`.

The release contains the APK and a `sha256sum` sidecar generated from that exact file. The decoded keystore is removed before the signed artifact is uploaded between jobs.

## Workflow Permissions and Event Behavior

- `build` and `sign-release` use `contents: read`.
- Only `publish-release` uses `contents: write`.
- `publish-release` receives no signing secrets and uses only GitHub's repository-scoped `GITHUB_TOKEN`; no personal access token is introduced.
- The publisher uses `softprops/action-gh-release@v3`, explicitly sets `prerelease: true`, and explicitly sets `make_latest: false` for the reserved Android-only tag.
- A release created with `GITHUB_TOKEN` does not recursively launch another workflow run. The ESP32 published-release guard remains mandatory because a maintainer or external automation could later publish a release with a personal access token or through the GitHub UI.

## Firmware Isolation

No file below `esp32/` is modified for this release.

The ESP32 workflow exclusion remains the enforcement point, with its marker made exact:

- Tag-push events whose `github.ref_name` contains `-android-` skip the firmware build.
- Published-release events whose `github.event.release.tag_name` contains `-android-` skip the firmware build.
- The Pages deployment requires a successful firmware build, so it also skips.
- No physical flashing mechanism is part of GitHub Actions.

A contract test locks both the Android publisher and the ESP32 exclusion so a future workflow edit cannot silently couple Android-only releases back to firmware publication.

## Failure Handling

- If Android tests or the debug build fail, `sign-release` and `publish-release` do not start.
- If signing secrets are missing, `sign-release` stops before creating a keystore or release.
- If the release APK fails signature or identity verification, no release is published.
- If the decoded certificate differs from the pinned production signer, no signed artifact leaves `sign-release` and no release is published.
- If release publication fails, the pushed tag remains immutable; the failed tag is not moved or reused. A corrected build receives a new version and tag.
- The GitHub Release is not considered shipped until the published asset is downloaded and independently verified.

## Test Strategy

### Contract test first

Add `backend/tests/test_android_release_workflow_contract.py` before changing production configuration. The initial run must fail because version 111, the release verification steps, test-gated build, checksum asset, and Android prerelease/latest policy are not yet present.

The contract test will assert:

- Gradle contains the exact version code and name.
- Android CI runs unit tests before release publication.
- The signing job is gated on the build job and has only `contents: read`.
- The publishing job is gated on the signing job, has `contents: write`, and has no signing-secret references.
- Signing secrets are checked before keystore decoding.
- The release APK is checked with `apksigner` and `aapt`, including the exact pinned production signer digest.
- Both the APK and checksum are attached.
- Tags containing the exact `-android-` marker publish as prerelease and not Latest.
- The ESP32 workflow skips exact-marker Android tag-push and release-published events while preserving normal firmware handling for other tags.

### Local verification

- Run the focused workflow contract test.
- Run the complete Android JVM suite.
- Build the debug APK locally.
- Re-verify the reference APK asset digest (`4d71294cf00d782ac22378f6e7c960bd9caf39f8c0af5004a091ddd0d28797a7`) before relying on its pinned signer certificate.
- Run `git diff --check` for the exact implementation scope.
- Recompute the frozen ESP32 diff and status fingerprints to prove no firmware source edits entered the Android release patch. The one permitted firmware-workflow edit is `.github/workflows/esp32-web-flasher.yml`, which only narrows the Android-only tag guard and never builds or flashes firmware.

### Published-release verification

- Confirm the final Android workflow run succeeds.
- Confirm the ESP32 workflow is skipped for the Android tag. Also inspect its event policy to confirm an independently published Android release would be skipped.
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
- Narrowing the ESP32 GitHub Actions Android-only marker from `android` to `-android-`.
- A workflow contract regression test.
- Creation and explicit push of the fresh Android-only release tag.
- Verification of the published APK.

Excluded:

- Any file below `esp32/`, including source, binary, manifest, or firmware version changes.
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
