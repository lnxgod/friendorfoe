import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ANDROID_GRADLE = REPO_ROOT / "android" / "app" / "build.gradle.kts"
ANDROID_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "android-build.yml"
ESP32_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "esp32-web-flasher.yml"

VERSION_NAME = "0.67.5-android-privacy-repair"
VERSION_CODE = 114
SIGNER_SHA256 = (
    "3a1581ba5d10df59fdb28e09987851d6c7d79ce26df4eb69b9f6d262b9b68e95"
)
APKSIGNER_V2_CERT_OUTPUT = (
    "V2 Signer: certificate SHA-256 digest: " + SIGNER_SHA256
)
ACTION_PINS = {
    "actions/checkout": ("34e114876b0b11c390a56381ad16ebd13914f8d5", "v4", 2),
    "actions/setup-java": ("c1e323688fd81a25caa38c78aa6df2d33d3e20d9", "v4", 2),
    "actions/cache": ("0057852bfaa89a56745cba8c7296529d2fc39830", "v4", 2),
    "actions/upload-artifact": ("ea165f8d65b6e75b540449e92b4886f43607fa02", "v4", 2),
    "actions/download-artifact": ("d3f86a106a0bac45b974a628896c90dbdf5c8093", "v4", 1),
    "softprops/action-gh-release": ("3d0d9888cb7fd7b750713d6e236d1fcb99157228", "v3", 1),
}
PRERELEASE_EXPRESSION = (
    "${{ contains(github.ref_name, 'beta') || contains(github.ref_name, 'alpha') "
    "|| contains(github.ref_name, 'rc') }}"
)
MAKE_LATEST_EXPRESSION = (
    "${{ contains(github.ref_name, '-android-') && !contains(github.ref_name, 'beta') "
    "&& !contains(github.ref_name, 'alpha') && !contains(github.ref_name, 'rc') "
    "&& 'true' || 'legacy' }}"
)
ESP32_BUILD_GUARD = (
    "if: ${{ !((github.event_name == 'release' && "
    "contains(github.event.release.tag_name, '-android-')) || "
    "(github.event_name != 'release' && contains(github.ref_name, '-android-'))) }}"
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
    assert f"versionCode = {VERSION_CODE}" in gradle
    assert f'versionName = "{VERSION_NAME}"' in gradle


def test_android_workflow_actions_are_immutable_and_release_fails_closed():
    workflow = ANDROID_WORKFLOW.read_text()
    uses_entries = re.findall(r"^\s*(?:-\s+)?uses:\s*([^\s#]+)", workflow, re.MULTILINE)

    assert len(uses_entries) == sum(count for _, _, count in ACTION_PINS.values())
    for action, (sha, version, count) in ACTION_PINS.items():
        pinned_reference = f"{action}@{sha}"
        assert workflow.count(pinned_reference) == count
        assert workflow.count(f"{pinned_reference} # {version}") == count

    for reference in uses_entries:
        assert re.fullmatch(r"[^@]+@[0-9a-f]{40}", reference), (
            f"mutable or malformed action reference: {reference}"
        )

    publish = _job(workflow, "publish-release")
    assert "fail_on_unmatched_files: true" in publish


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
    assert f"versionCode='{VERSION_CODE}'" in signing
    assert f"versionName='{VERSION_NAME}'" in signing
    assert "sha256sum" in signing
    assert "rm -f android/app/release.keystore" in signing
    assert "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02" in signing
    assert ".apk.sha256" in signing


def test_signer_assertion_accepts_current_apksigner_v2_output():
    workflow = ANDROID_WORKFLOW.read_text()
    signing = _job(workflow, "sign-release", "publish-release")
    assertion = re.search(
        r'grep -F "([^"]+)" /tmp/apksigner-output\.txt', signing
    )

    assert assertion is not None
    assert assertion.group(1).endswith(SIGNER_SHA256)
    assert assertion.group(1) in APKSIGNER_V2_CERT_OUTPUT


def test_release_publisher_has_write_access_without_signing_secrets():
    workflow = ANDROID_WORKFLOW.read_text()
    publish = _job(workflow, "publish-release")

    assert "needs: sign-release" in publish
    assert "contents: write" in publish
    assert "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093" in publish
    assert "softprops/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228" in publish
    assert "contains(github.ref_name, '-android-')" in publish
    assert f"prerelease: {PRERELEASE_EXPRESSION}" in publish
    assert f"make_latest: {MAKE_LATEST_EXPRESSION}" in publish
    assert "make_latest: ${{ contains(github.ref_name, '-android-') && 'true'" not in publish
    assert "fail_on_unmatched_files: true" in publish
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

    assert ESP32_BUILD_GUARD in build
    assert "contains(github.event.release.tag_name, 'android')" not in build
    assert "contains(github.ref_name, 'android')" not in build
    assert "Attach firmware to release" in build
    assert "needs.build.result == 'success'" in deploy
