# Android Signed GitHub Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the reviewed Android badge interface as a production-signed `v0.64.69-android-defcon34-badge-ui` GitHub prerelease with a directly downloadable APK and SHA-256 checksum, without building, modifying, publishing, or flashing badge firmware.

**Architecture:** A source-contract pytest first locks Android identity, signing checks, least-privilege job separation, release policy, and the exact Android-only firmware exclusion. The Android workflow then uses ordinary debug validation, secret-bearing release signing, and secret-free GitHub Release publication jobs. An immutable Android-only tag triggers the pipeline; the downloaded release APK is independently checked before handoff.

**Tech Stack:** Python/pytest contract tests, Gradle/Kotlin Android configuration, GitHub Actions YAML, Android SDK `apksigner` and `aapt`, GitHub CLI.

## Global Constraints

- Release tag: `v0.64.69-android-defcon34-badge-ui`.
- Android identity: `versionCode = 111`, `versionName = "0.64.69-defcon34-badge-ui"`.
- APK asset: `friendorfoe-v0.64.69-android-defcon34-badge-ui.apk`.
- Checksum asset: `friendorfoe-v0.64.69-android-defcon34-badge-ui.apk.sha256`.
- Production signer SHA-256: `3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95`.
- Android-only releases are prereleases and are never Latest.
- No file below `esp32/` may be changed, built, packaged, published, or flashed.
- The only firmware-related edit is the exact marker guard in `.github/workflows/esp32-web-flasher.yml`.
- Preserve unrelated dirty worktree changes and stage only explicit release paths.

---

### Task 1: Lock the Android Release Contract in a Failing Test

**Files:**
- Create: `backend/tests/test_android_release_workflow_contract.py`
- Read: `android/app/build.gradle.kts`
- Read: `.github/workflows/android-build.yml`
- Read: `.github/workflows/esp32-web-flasher.yml`

**Interfaces:**
- Consumes: repository files as plain text.
- Produces: pytest contract enforcing exact version, signer, permissions, assets, and firmware isolation.

- [ ] **Step 1: Write the failing contract test**

```python
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ANDROID_GRADLE = REPO_ROOT / "android" / "app" / "build.gradle.kts"
ANDROID_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "android-build.yml"
ESP32_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"

VERSION_NAME = "0.64.69-defcon34-badge-ui"
SIGNER_SHA256 = (
    "3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95"
)


def _job(workflow: str, name: str, next_name: str | None = None) -> str:
    marker = f"  {name}:"
    assert marker in workflow, f"missing workflow job: {name}"
    start = workflow.index(marker)
    if next_name is None:
        return workflow[start:]
    next_marker = f"  {next_name}:"
    assert next_marker in workflow, f"missing workflow job: {next_name}"
    return workflow[start : workflow.index(next_marker, start)]


def test_android_release_identity_is_exact():
    gradle = ANDROID_GRADLE.read_text()
    assert "versionCode = 111" in gradle
    assert f'versionName = "{VERSION_NAME}"' in gradle


def test_release_signing_is_test_gated_verified_and_secret_scoped():
    workflow = ANDROID_WORKFLOW.read_text()
    build = _job(workflow, "build", "sign-release")
    signing = _job(workflow, "sign-release", "publish-release")

    assert "contents: read" in build
    assert "./gradlew clean testDebugUnitTest assembleDebug" in build
    assert "needs: build" in signing
    assert "contents: read" in signing
    assert "contents: write" not in signing
    assert signing.index("- name: Validate signing secrets") < signing.index(
        "- name: Decode keystore"
    )
    for secret in (
        "KEYSTORE_BASE64",
        "KEYSTORE_PASSWORD",
        "KEY_ALIAS",
        "KEY_PASSWORD",
    ):
        assert f"secrets.{secret}" in signing

    assert "base64 --decode" in signing
    assert "./gradlew clean assembleRelease" in signing
    assert "apksigner" in signing
    assert "aapt" in signing
    assert SIGNER_SHA256 in signing
    assert "name='com.friendorfoe'" in signing
    assert "versionCode='111'" in signing
    assert f"versionName='{VERSION_NAME}'" in signing
    assert "sha256sum" in signing
    assert "rm -f android/app/release.keystore" in signing
    assert "actions/upload-artifact@v4" in signing
    assert ".apk.sha256" in signing


def test_release_publisher_has_write_access_without_signing_secrets():
    workflow = ANDROID_WORKFLOW.read_text()
    publish = _job(workflow, "publish-release")

    assert "needs: sign-release" in publish
    assert "contents: write" in publish
    assert "actions/download-artifact@v4" in publish
    assert "softprops/action-gh-release@v3" in publish
    assert "contains(github.ref_name, '-android-')" in publish
    assert "prerelease:" in publish
    assert "make_latest:" in publish
    assert "'false'" in publish
    assert "'legacy'" in publish
    assert ".apk.sha256" in publish
    for secret in (
        "KEYSTORE_BASE64",
        "KEYSTORE_PASSWORD",
        "KEY_ALIAS",
        "KEY_PASSWORD",
    ):
        assert f"secrets.{secret}" not in publish


def test_android_only_tag_skips_all_firmware_work():
    workflow = ESP32_WORKFLOW.read_text()
    build = _job(workflow, "build", "deploy")
    deploy = _job(workflow, "deploy")

    assert "contains(github.event.release.tag_name, '-android-')" in build
    assert "contains(github.ref_name, '-android-')" in build
    assert "contains(github.event.release.tag_name, 'android')" not in build
    assert "contains(github.ref_name, 'android')" not in build
    assert "Attach firmware to release" in build
    assert "needs.build.result == 'success'" in deploy
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
backend/.venv/bin/pytest backend/tests/test_android_release_workflow_contract.py -v
```

Expected: four failures showing version code 110, missing `sign-release`, and broad `android` firmware guards.

- [ ] **Step 3: Commit only the proven failing test**

```bash
git add backend/tests/test_android_release_workflow_contract.py
git commit -m "test: lock signed Android release contract"
```

Expected: exactly one new test file in the commit.

---

### Task 2: Implement the Versioned, Least-Privilege Release Pipeline

**Files:**
- Modify: `android/app/build.gradle.kts`
- Modify: `.github/workflows/android-build.yml`
- Modify: `.github/workflows/esp32-web-flasher.yml`
- Test: `backend/tests/test_android_release_workflow_contract.py`

**Interfaces:**
- Consumes: `KEYSTORE_BASE64`, `KEYSTORE_PASSWORD`, `KEY_ALIAS`, and `KEY_PASSWORD` secrets.
- Produces: signed APK/checksum artifact consumed by `publish-release`.

- [ ] **Step 1: Set the exact Android version**

```kotlin
versionCode = 111
versionName = "0.64.69-defcon34-badge-ui"
```

- [ ] **Step 2: Keep the debug build read-only and test-gated**

```yaml
permissions:
  contents: read
```

```yaml
- name: Test and build debug APK
  run: cd android && ./gradlew clean testDebugUnitTest assembleDebug
```

- [ ] **Step 3: Replace `release` with secret-bearing `sign-release`**

```yaml
sign-release:
  if: startsWith(github.ref, 'refs/tags/v')
  needs: build
  runs-on: ubuntu-latest
  permissions:
    contents: read

  steps:
    - uses: actions/checkout@v4

    - name: Set up JDK 17
      uses: actions/setup-java@v4
      with:
        java-version: '17'
        distribution: 'temurin'

    - name: Cache Gradle packages
      uses: actions/cache@v4
      with:
        path: |
          ~/.gradle/caches
          ~/.gradle/wrapper
        key: ${{ runner.os }}-gradle-${{ hashFiles('**/*.gradle*', '**/gradle-wrapper.properties') }}
        restore-keys: |
          ${{ runner.os }}-gradle-

    - name: Make gradlew executable
      run: chmod +x android/gradlew

    - name: Validate signing secrets
      env:
        KEYSTORE_BASE64: ${{ secrets.KEYSTORE_BASE64 }}
        KEYSTORE_PASSWORD: ${{ secrets.KEYSTORE_PASSWORD }}
        KEY_ALIAS: ${{ secrets.KEY_ALIAS }}
        KEY_PASSWORD: ${{ secrets.KEY_PASSWORD }}
      run: |
        test -n "$KEYSTORE_BASE64"
        test -n "$KEYSTORE_PASSWORD"
        test -n "$KEY_ALIAS"
        test -n "$KEY_PASSWORD"

    - name: Decode keystore
      env:
        KEYSTORE_BASE64: ${{ secrets.KEYSTORE_BASE64 }}
      run: printf '%s' "$KEYSTORE_BASE64" | base64 --decode > android/app/release.keystore

    - name: Build release APK
      run: cd android && ./gradlew clean assembleRelease
      env:
        KEYSTORE_PATH: release.keystore
        KEYSTORE_PASSWORD: ${{ secrets.KEYSTORE_PASSWORD }}
        KEY_ALIAS: ${{ secrets.KEY_ALIAS }}
        KEY_PASSWORD: ${{ secrets.KEY_PASSWORD }}

    - name: Verify and package release APK
      run: |
        BUILD_TOOLS_DIR="$(find "$ANDROID_SDK_ROOT/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
        APKSIGNER="$BUILD_TOOLS_DIR/apksigner"
        AAPT="$BUILD_TOOLS_DIR/aapt"
        APK="android/app/build/outputs/apk/release/app-release.apk"
        ASSET="friendorfoe-${GITHUB_REF_NAME}.apk"
        test -x "$APKSIGNER"
        test -x "$AAPT"
        "$APKSIGNER" verify --verbose --print-certs "$APK" | tee /tmp/apksigner-output.txt
        grep -F "Signer #1 certificate SHA-256 digest: 3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95" /tmp/apksigner-output.txt
        "$AAPT" dump badging "$APK" | tee /tmp/aapt-output.txt
        grep -F "package: name='com.friendorfoe' versionCode='111' versionName='0.64.69-defcon34-badge-ui'" /tmp/aapt-output.txt
        cp "$APK" "$ASSET"
        sha256sum "$ASSET" > "$ASSET.sha256"

    - name: Remove temporary keystore
      if: always()
      run: rm -f android/app/release.keystore

    - name: Upload signed release artifact
      uses: actions/upload-artifact@v4
      with:
        name: friendorfoe-signed-release
        path: |
          friendorfoe-${{ github.ref_name }}.apk
          friendorfoe-${{ github.ref_name }}.apk.sha256
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 4: Add secret-free `publish-release`**

```yaml
publish-release:
  if: startsWith(github.ref, 'refs/tags/v')
  needs: sign-release
  runs-on: ubuntu-latest
  permissions:
    contents: write

  steps:
    - name: Download signed release artifact
      uses: actions/download-artifact@v4
      with:
        name: friendorfoe-signed-release
        path: release-assets

    - name: Create GitHub Release
      uses: softprops/action-gh-release@v3
      with:
        draft: false
        prerelease: ${{ contains(github.ref_name, '-android-') || contains(github.ref_name, 'beta') || contains(github.ref_name, 'alpha') || contains(github.ref_name, 'rc') }}
        make_latest: ${{ contains(github.ref_name, '-android-') && 'false' || 'legacy' }}
        generate_release_notes: true
        files: |
          release-assets/friendorfoe-${{ github.ref_name }}.apk
          release-assets/friendorfoe-${{ github.ref_name }}.apk.sha256
```

- [ ] **Step 5: Narrow the firmware marker without touching `esp32/`**

```yaml
if: ${{ !((github.event_name == 'release' && contains(github.event.release.tag_name, '-android-')) || (github.event_name != 'release' && contains(github.ref_name, '-android-'))) }}
```

- [ ] **Step 6: Run the focused test and verify GREEN**

```bash
backend/.venv/bin/pytest backend/tests/test_android_release_workflow_contract.py -v
```

Expected: `4 passed`.

- [ ] **Step 7: Check and commit the exact implementation paths**

```bash
git diff --check -- android/app/build.gradle.kts .github/workflows/android-build.yml .github/workflows/esp32-web-flasher.yml backend/tests/test_android_release_workflow_contract.py
git add android/app/build.gradle.kts .github/workflows/android-build.yml .github/workflows/esp32-web-flasher.yml
git commit -m "v0.64.69-android: publish signed badge UI APK"
```

Expected: no whitespace errors; the production commit contains the two workflows and Gradle identity only because the failing contract test was committed separately.

---

### Task 3: Verify the Android Patch and Preserve Firmware State

**Files:**
- Verify: `android/`
- Verify: `backend/tests/test_android_release_workflow_contract.py`
- Verify only: `esp32/`

**Interfaces:**
- Consumes: implementation commit from Task 2.
- Produces: a locally proven commit safe to push and tag.

- [ ] **Step 1: Run the contract test fresh**

```bash
backend/.venv/bin/pytest backend/tests/test_android_release_workflow_contract.py -v
```

Expected: `4 passed`.

- [ ] **Step 2: Run the complete Android JVM tests**

```bash
cd android && ./gradlew testDebugUnitTest
```

Expected: `BUILD SUCCESSFUL` and no failing tests.

- [ ] **Step 3: Build the debug APK**

```bash
cd android && ./gradlew assembleDebug
```

Expected: `BUILD SUCCESSFUL` and `android/app/build/outputs/apk/debug/app-debug.apk` exists.

- [ ] **Step 4: Recompute frozen firmware fingerprints**

Run the same sorted status and binary-diff commands that established the baselines. Expected binary diff SHA-256:

```text
82bb2e31817ca7ec3afefc0e403cf34c62b7b62bc446042b92d90f96179bb968
```

Expected status SHA-256:

```text
5824bde974b6c960cc16d5114309ccf92c6c53e05dae67c8d41fe56981b476ef
```

- [ ] **Step 5: Push the verified branch**

```bash
git push origin codex/defcon34-badge-final
```

Expected: the remote branch advances to the implementation commit.

- [ ] **Step 6: Require the branch workflow to pass**

```bash
gh run list --repo lnxgod/friendorfoe --branch codex/defcon34-badge-final --workflow "Android Build" --limit 5
```

Expected: the pushed implementation commit's run concludes `success`.

---

### Task 4: Publish and Independently Verify the Signed Release

**Files:**
- Create remotely: tag and GitHub Release `v0.64.69-android-defcon34-badge-ui`.
- Download temporarily: `/tmp/fof-android-release-v0.64.69/`.

**Interfaces:**
- Consumes: verified pushed implementation commit.
- Produces: immutable prerelease with signed APK and checksum URLs.

- [ ] **Step 1: Prove the tag and release do not exist**

```bash
git ls-remote --tags origin refs/tags/v0.64.69-android-defcon34-badge-ui
gh release view v0.64.69-android-defcon34-badge-ui --repo lnxgod/friendorfoe
```

Expected: no remote tag and `release not found`.

- [ ] **Step 2: Create and push the annotated tag**

```bash
git tag -a v0.64.69-android-defcon34-badge-ui -m "v0.64.69 Android DEF CON 34 badge UI"
git push origin v0.64.69-android-defcon34-badge-ui
```

Expected: the new tag is accepted by `origin`.

- [ ] **Step 3: Watch Android CI through publication**

```bash
gh run list --repo lnxgod/friendorfoe --workflow "Android Build" --limit 10
gh run list --repo lnxgod/friendorfoe --workflow "Android Build" --limit 20 --json databaseId,headBranch --jq '.[] | select(.headBranch == "v0.64.69-android-defcon34-badge-ui") | .databaseId' | head -n 1 | xargs gh run watch --repo lnxgod/friendorfoe --exit-status
```

Expected: `build`, `sign-release`, and `publish-release` succeed.

- [ ] **Step 4: Confirm firmware CI skipped the tag**

```bash
gh run list --repo lnxgod/friendorfoe --workflow "ESP32 Web Flasher" --limit 10
gh run list --repo lnxgod/friendorfoe --workflow "ESP32 Web Flasher" --limit 20 --json databaseId,headBranch --jq '.[] | select(.headBranch == "v0.64.69-android-defcon34-badge-ui") | .databaseId' | head -n 1 | xargs -I{} gh run view {} --repo lnxgod/friendorfoe --json status,conclusion,jobs
```

Expected: firmware `build` and dependent `deploy` are skipped, with no firmware assets attached.

- [ ] **Step 5: Download the release assets**

```bash
mkdir -p /tmp/fof-android-release-v0.64.69
gh release download v0.64.69-android-defcon34-badge-ui --repo lnxgod/friendorfoe --pattern 'friendorfoe-v0.64.69-android-defcon34-badge-ui.apk*' --dir /tmp/fof-android-release-v0.64.69
```

Expected: exactly the APK and `.sha256` sidecar.

- [ ] **Step 6: Verify checksum, signature, signer, and identity**

```bash
cd /tmp/fof-android-release-v0.64.69 && shasum -a 256 -c friendorfoe-v0.64.69-android-defcon34-badge-ui.apk.sha256
/Users/billh/Library/Android/sdk/build-tools/36.0.0/apksigner verify --verbose --print-certs /tmp/fof-android-release-v0.64.69/friendorfoe-v0.64.69-android-defcon34-badge-ui.apk
/Users/billh/Library/Android/sdk/build-tools/36.0.0/aapt dump badging /tmp/fof-android-release-v0.64.69/friendorfoe-v0.64.69-android-defcon34-badge-ui.apk
```

Expected: checksum `OK`; signature verifies; signer SHA-256 is `3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95`; package is `com.friendorfoe`; version code is `111`; version name is `0.64.69-defcon34-badge-ui`.

- [ ] **Step 7: Verify final release policy and assets**

```bash
gh release view v0.64.69-android-defcon34-badge-ui --repo lnxgod/friendorfoe --json tagName,isDraft,isPrerelease,assets,url
```

Expected: published, not draft, prerelease true, and exactly the APK plus checksum, with no firmware binaries or archives.

- [ ] **Step 8: Hand off the release**

Report the release URL, direct APK URL, checksum, signer fingerprint, package/version, workflow links, and the explicit statement that no physical Android USB-host test or badge firmware action occurred during CI publication.
