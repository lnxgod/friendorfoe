# Android Latest Update Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge PR #3 and publish a signed Android version 112 release that existing installs discover through GitHub's Latest endpoint without publishing firmware.

**Architecture:** A pure Android update policy owns numeric version ordering; `WelcomeScreen` retains network/UI ownership. The tag-driven Android workflow publishes exact `-android-` production tags as stable Latest releases while the independent ESP32 guard skips firmware. The backend firmware manager selects the newest firmware-bearing release instead of assuming the newest GitHub release contains firmware.

**Tech Stack:** Kotlin/JVM, Jetpack Compose, OkHttp/Gson, Python/pytest/httpx, GitHub Actions YAML, GitHub Releases, PlatformIO native tests.

## Global Constraints

- Release identity is exactly versionCode `112`, versionName `0.64.70-android-defcon34-badge-ui`, tag `v0.64.70-android-defcon34-badge-ui`.
- The tag must point to the merged `main` commit.
- The release must be non-draft, non-prerelease, Latest, and contain exactly the APK plus checksum.
- The existing exact `-android-` ESP32 build/deploy skip guard remains unchanged.
- Do not stage firmware production files, binaries, manifests, or the user's unrelated dirty worktree changes.
- Do not flash hardware.

---

### Task 1: Numeric Android Update Policy

**Files:**
- Create: `android/app/src/main/java/com/friendorfoe/presentation/welcome/AppUpdatePolicy.kt`
- Create: `android/app/src/test/java/com/friendorfoe/presentation/welcome/AppUpdatePolicyTest.kt`
- Modify: `android/app/src/main/java/com/friendorfoe/presentation/welcome/WelcomeScreen.kt:237-269`

**Interfaces:**
- Produces: `AppUpdatePolicy.isRemoteNewer(currentVersion: String, remoteTag: String): Boolean`
- Consumes: optional leading `v` and semantic core `major.minor.patch` from both strings.

- [ ] **Step 1: Write failing JVM tests**

Cover `.68 -> .70`, `.69 -> .70`, equal `.70` with differing suffixes, `.70 -> .68`, `.99 -> .100`, and malformed remote input.

```kotlin
assertTrue(AppUpdatePolicy.isRemoteNewer("0.64.68-live-follow", "v0.64.70-android-defcon34-badge-ui"))
assertFalse(AppUpdatePolicy.isRemoteNewer("0.64.70-local", "v0.64.70-remote"))
assertFalse(AppUpdatePolicy.isRemoteNewer("0.64.70", "v0.64.68"))
```

- [ ] **Step 2: Run RED**

Run: `cd android && ./gradlew testDebugUnitTest --tests '*AppUpdatePolicyTest'`

Expected: compilation failure because `AppUpdatePolicy` does not exist.

- [ ] **Step 3: Implement the minimal pure policy**

Parse exactly three leading numeric components after optional `v`; compare lexicographically as integers; return false when either value is malformed.

- [ ] **Step 4: Route WelcomeScreen through the policy**

Replace equality-only branching with:

```kotlin
if (AppUpdatePolicy.isRemoteNewer(currentVersion, tagName)) {
    UpdateState.UpdateAvailable(latestVersion, htmlUrl)
} else {
    UpdateState.UpToDate(currentVersion)
}
```

- [ ] **Step 5: Run GREEN**

Run the focused test, then `cd android && ./gradlew testDebugUnitTest`.

### Task 2: Version 112 Stable-Latest Release Contract

**Files:**
- Modify: `android/app/build.gradle.kts:20-21`
- Modify: `.github/workflows/android-build.yml:106-156`
- Modify: `backend/tests/test_android_release_workflow_contract.py`

**Interfaces:**
- Produces exact release metadata and signed APK assertions for version 112.
- Preserves the ESP32 `-android-` guard byte-for-byte.

- [ ] **Step 1: Change contract expectations first**

Set the expected name to `0.64.70-android-defcon34-badge-ui`, code to `112`, and require:

```yaml
prerelease: ${{ contains(github.ref_name, 'beta') || contains(github.ref_name, 'alpha') || contains(github.ref_name, 'rc') }}
make_latest: ${{ contains(github.ref_name, '-android-') && 'true' || 'legacy' }}
```

- [ ] **Step 2: Run RED**

Run: `/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest -q backend/tests/test_android_release_workflow_contract.py`

Expected: failures on version identity and release classification.

- [ ] **Step 3: Apply the minimal Gradle/workflow change**

Update the embedded version, aapt verification line, prerelease expression, and make-latest expression only.

- [ ] **Step 4: Run GREEN and YAML parse**

Run focused pytest and parse both workflow YAML files with Ruby `YAML.safe_load`.

### Task 3: Preserve Firmware Catalog Across APK-Only Releases

**Files:**
- Modify: `backend/app/services/firmware_manager.py:152-213`
- Modify: `backend/tests/test_firmware_catalog.py`

**Interfaces:**
- Produces: selection of the newest non-draft release with at least one supported `.bin` asset.
- Preserves existing assets if no qualifying firmware release exists.

- [ ] **Step 1: Write failing async regression tests**

Mock GitHub releases with an APK-only Android release first and a firmware-bearing release second. Assert the second tag and firmware asset are selected. Add a no-firmware case that preserves an existing catalog.

- [ ] **Step 2: Run RED**

Run: `/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest -q backend/tests/test_firmware_catalog.py`

Expected: the manager selects the APK-only release or clears the catalog.

- [ ] **Step 3: Implement a small release selector**

Filter releases in API order for non-draft entries containing an asset whose lowercase name ends in `.bin` and matches a configured firmware asset pattern. Return without mutation when no release qualifies.

- [ ] **Step 4: Run GREEN**

Run the focused catalog tests, then the full backend suite.

### Task 4: Restore PR Check Integrity Without Firmware Production Changes

**Files:**
- Modify committed form only: `esp32/test/test_runner.c`

**Interfaces:**
- Removes six declarations and six `RUN_TEST` calls that reference implementations absent from PR head.
- Does not stage the matching user-owned dirty firmware/test work.

- [ ] **Step 1: Record the dirty-file hash and diff**

Capture `git diff --binary -- esp32/test/test_runner.c | shasum -a 256` before modification.

- [ ] **Step 2: Remove only the six orphan references**

Names:

```text
test_ble_threat_serial_skimmer_requires_minus_45_or_stronger
test_badge_radio_boot_order_prioritizes_assigned_primary_and_safe_fallback
test_badge_scanner_health_rejects_dead_primary_radio
test_badge_scanner_health_requires_opposing_radio_quiescence
test_badge_skimmer_display_policy_is_lowest_priority
test_relay_policy_post_reboot_proof_requires_a_new_nonzero_boot_id
```

- [ ] **Step 3: Stage and commit the committed-state cleanup, then restore the user's working copy**

Verify the staged diff contains only those twelve removed lines. Commit it, then re-add the lines to the working copy so the user's uncommitted matching implementations remain intact.

- [ ] **Step 4: Verify the clean committed tree in CI**

Push the PR head and require the exact-head ESP32, Android, and backend checks to finish successfully before merge.

### Task 5: Merge, Tag, and Verify the Public Release

**Files:** None beyond prior tasks.

- [ ] **Step 1: Run release gates**

Run full Android JVM tests + debug build, full backend tests, focused release contracts, `git diff --check`, action-pin checks, and frozen ESP32 fingerprint checks.

- [ ] **Step 2: Push and verify PR head**

Mark PR #3 ready only after exact-head CI is green.

- [ ] **Step 3: Merge with a merge commit**

Merge PR #3 into `main`, verify the returned merge commit, and wait for merged-main Android/backend checks. Do not tag the feature head.

- [ ] **Step 4: Create and push the annotated tag**

Tag the merged commit `v0.64.70-android-defcon34-badge-ui` and confirm the ESP32 tag workflow skips build/deploy.

- [ ] **Step 5: Verify the release independently**

Download the two public assets. Verify checksum, production signer, `com.friendorfoe`, versionCode 112, exact versionName, release Latest/non-prerelease metadata, and `/releases/latest` exact tag.

- [ ] **Step 6: Record receipts**

Update `.superpowers/sdd/progress.md` with commit, PR merge, tag, workflow runs, APK SHA-256, signer, release URLs, and the remaining physical Android USB-host gate.

## Plan Self-Review

- Every design requirement maps to one task.
- Version, tag, package, signer, and firmware-isolation values are exact and consistent.
- TDD RED/GREEN steps precede each production change.
- No placeholder steps remain.
- The merge and tag sequence prevents another branch-only release.
