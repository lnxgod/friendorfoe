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
