# Badge Release Identity and Fail-Closed Web Flasher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every shipped ESP32 image self-identify as its real production or badge target and prevent an incomplete, stale, or cross-target web-flasher site from being deployed.

**Architecture:** A compile-selected target identity is the single source used by boot logs, USB status, HTTP status, heartbeats, OTA catalog lookups, and the embedded ESP app descriptor. A deterministic validator treats the five manifests and their binary parts as one release unit; GitHub Pages deployment runs only after builds and validation succeed.

**Tech Stack:** ESP-IDF CMake/C, PlatformIO, Python 3 standard library, pytest, GitHub Actions, ESP image app descriptors.

## Global Constraints

- The five release targets remain `scanner-s3-combo`, `scanner-s3-combo-seed`, `uplink-s3`, `scanner-s3-combo-fof_badge`, and `uplink-s3-fof_badge`.
- Runtime `firmware_name`, catalog target, manifest target, and embedded project identity must agree.
- Badge and production images must never share an ambiguous embedded project identity.
- Existing backend `board_type` compatibility is preserved for production; badge heartbeats use the badge-specific firmware target rather than pretending to be production.
- The validator is offline, deterministic, and fails on missing, empty, malformed, wrong-offset, wrong-version, or wrong-target artifacts.
- Pull requests build and validate but never deploy Pages. Branch and tag deployment happens only from a successful build job.

---

### Task 1: Compile-Selected Firmware Identity Everywhere

**Files:**
- Modify: `esp32/shared/version.h`
- Modify: `esp32/uplink/CMakeLists.txt`
- Modify: `esp32/scanner/CMakeLists.txt`
- Modify: `esp32/uplink/main/main.c`
- Modify: `esp32/scanner/main/main.c`
- Modify: `esp32/uplink/main/comms/http_upload.c`
- Modify: `esp32/uplink/main/core/serial_config.c`
- Modify: `esp32/uplink/main/network/http_status.c`
- Modify: `esp32/uplink/main/network/fw_auto_check.c`
- Modify: `esp32/scripts/firmware_version.py`
- Modify: `esp32/scripts/verify_firmware_versions.py`
- Modify: `backend/tests/test_firmware_build_version.py`
- Modify: `scripts/test_fof_badge_flash.py`

**Interfaces:**
- Produces: `FOF_FIRMWARE_TARGET`, `FOF_APP_PROJECT`, machine-readable `firmware_name`, and descriptor inspection that returns project plus version.
- Consumes: PlatformIO environment macros and the ESP app descriptor at image offset `0x20`.

- [ ] **Step 1: Write failing identity and descriptor tests**

Add Python tests that construct minimal app-descriptor fixtures and require both fields:

```python
def test_badge_uplink_descriptor_has_badge_project_and_version():
    info = parse_firmware_identity(make_app_image("fof_badge_uplink", "0.64.69"))
    assert info.project == "fof_badge_uplink"
    assert info.version == "0.64.69"

def test_status_contract_exposes_compile_selected_firmware_name():
    source = Path("esp32/uplink/main/core/serial_config.c").read_text()
    assert '"firmware_name"' in source
    assert "FOF_FIRMWARE_TARGET" in source
```

Extend `backend/tests/test_firmware_build_version.py` so all five target fixtures assert their expected embedded project and version. Add source-contract assertions preventing literal `#define FIRMWARE_NAME "uplink-s3"` and literal heartbeat `"board_type":"uplink-s3"` from returning.

- [ ] **Step 2: Run focused tests to verify RED**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_firmware_build_version.py -v
python3 -m unittest scripts.test_fof_badge_flash
```

Expected: descriptor identity/status assertions fail because only the version is authoritative today.

- [ ] **Step 3: Add target and project selection to the shared version contract**

Select target/project at compile time using the existing board, badge, and seed
environment defines:

```c
#if defined(UPLINK_BOARD)
# if defined(FOF_BADGE_VARIANT)
#  define FOF_FIRMWARE_TARGET "uplink-s3-fof_badge"
#  define FOF_APP_PROJECT "fof_badge_uplink"
# else
#  define FOF_FIRMWARE_TARGET "uplink-s3"
#  define FOF_APP_PROJECT "fof_uplink"
# endif
#elif defined(SCANNER_BOARD)
# if defined(FOF_BADGE_VARIANT)
#  define FOF_FIRMWARE_TARGET "scanner-s3-combo-fof_badge"
#  define FOF_APP_PROJECT "fof_badge_scanner"
# elif defined(SEED_SCANNER_PINS)
#  define FOF_FIRMWARE_TARGET "scanner-s3-combo-seed"
#  define FOF_APP_PROJECT "fof_scanner_seed"
# else
#  define FOF_FIRMWARE_TARGET "scanner-s3-combo"
#  define FOF_APP_PROJECT "fof_scanner"
# endif
#endif
```

Keep project names within the ESP app descriptor field limit. Configure each root
CMake project from the same five-entry target-to-project map, selected from
`PIOENV`; tests must prove the CMake and C maps cannot drift. Do not infer badge
identity at runtime.

- [ ] **Step 4: Replace hard-coded identity at every uplink boundary**

Use the shared target in the boot banner, `FOF_PRINT_IDENT`, heartbeat `board_type`, `FOF_STATUS`, `/api/badge/status`, `/api/ota/info`, and firmware-catalog/self-update lookup. Each machine-readable status includes:

```json
{"firmware_name":"uplink-s3-fof_badge","version":"0.64.69"}
```

Keep existing fields so the Android app, recovery tool, and backend remain backward compatible.

- [ ] **Step 5: Parse and verify both descriptor fields**

Extend `firmware_version.py` with a typed identity result while retaining the current version-only CLI behavior. Make `verify_firmware_versions.py` reject a target when its binary's embedded project does not match the target-to-project map, even when the version matches.

- [ ] **Step 6: Run focused tests and clean target builds**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest backend/tests/test_firmware_build_version.py -v
python3 -m unittest scripts.test_fof_badge_flash
cd esp32/scanner
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -t clean -e scanner-s3-combo-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e scanner-s3-combo-fof_badge
cd ../uplink
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -t clean -e uplink-s3-fof_badge
/Users/billh/gai/friendorfoe/esp32/.venv312/bin/pio run -e uplink-s3-fof_badge
```

Inspect both built binaries with `firmware_version.py --json`; expected target/project/version triples match and the linker reports no IRAM overflow.

- [ ] **Step 7: Commit**

```bash
git add esp32/shared/version.h esp32/uplink esp32/scanner esp32/scripts backend/tests/test_firmware_build_version.py scripts/test_fof_badge_flash.py
git commit -m "firmware: make release target identity authoritative"
```

---

### Task 2: Deterministic Five-Target Web-Flasher Validator

**Files:**
- Create: `esp32/scripts/verify_web_flasher_site.py`
- Create: `esp32/scripts/test_verify_web_flasher_site.py`
- Modify: `esp32/web-flasher/manifest-scanner.json`
- Modify: `esp32/web-flasher/manifest-scanner-seed.json`
- Modify: `esp32/web-flasher/manifest-uplink.json`
- Modify: `esp32/web-flasher/manifest-badge-scanner.json`
- Modify: `esp32/web-flasher/manifest-badge-uplink.json`

**Interfaces:**
- Consumes: a staged web-flasher directory and the expected release version.
- Produces: exit zero plus a target/part summary only when the complete site is internally consistent.

- [ ] **Step 1: Write failing validator tests using temporary sites**

```python
def test_rejects_missing_manifest_part(tmp_path):
    site = valid_site(tmp_path)
    (site / "fof_badge_scanner.bin").unlink()
    with pytest.raises(ValidationError, match="missing"):
        validate_site(site, expected_version="0.64.69")

def test_rejects_cross_target_firmware(tmp_path):
    site = valid_site(tmp_path)
    replace_app_descriptor(site / "fof_badge_scanner.bin", project="fof_uplink")
    with pytest.raises(ValidationError, match="project"):
        validate_site(site, expected_version="0.64.69")
```

Cover malformed JSON, path traversal/absolute paths, duplicate offsets, non-integer or unexpected offsets, empty parts, missing bootloader/partition/app parts, version mismatch, target mismatch, and all five valid manifests.

- [ ] **Step 2: Run validator tests to verify RED**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest esp32/scripts/test_verify_web_flasher_site.py -v
```

Expected: import failure because the validator does not exist.

- [ ] **Step 3: Implement strict offline validation**

Allow only manifest-local regular files. Require exactly the repository's three
ESP Web Tools parts at offsets `0`, `32768`, and `131072`: bootloader, partition
table, and application. For each application image, parse the app descriptor and
compare its embedded project/version against the manifest target and requested
release version. Report every checked manifest and artifact SHA-256 for release
evidence.

- [ ] **Step 4: Align all five manifests and run the validator against staged artifacts**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest esp32/scripts/test_verify_web_flasher_site.py -v
python3 esp32/scripts/verify_web_flasher_site.py \
  --site _site \
  --version-header esp32/shared/version.h
```

Expected: unit tests pass; `_site` passes only after the workflow's assembly and
artifact-copy steps place current build artifacts at every manifest path.

- [ ] **Step 5: Commit**

```bash
git add esp32/scripts/verify_web_flasher_site.py esp32/scripts/test_verify_web_flasher_site.py esp32/web-flasher/manifest-*.json
git commit -m "release: validate every badge flasher target"
```

---

### Task 3: Fail-Closed Pages Build and Deployment

**Files:**
- Modify: `.github/workflows/esp32-web-flasher.yml`
- Modify: `docs/badge/README.md`

**Interfaces:**
- Consumes: successful artifacts from the matrix/build job and Task 2 validator.
- Produces: one validated Pages artifact; no deployment on partial build, pull request, or missing download.

- [ ] **Step 1: Add a failing workflow contract test**

Extend `esp32/scripts/test_verify_web_flasher_site.py` (or a focused workflow test) to parse the workflow and assert:

```python
assert requires_successful_build_and_non_pr_event(deploy_job["if"])
assert no_continue_on_error(deploy_job)
assert invokes_validator_before_pages_upload(deploy_job)
```

Also assert that every artifact download is required and that validation occurs before `actions/upload-pages-artifact`.

- [ ] **Step 2: Run the workflow contract test to verify RED**

Run Task 2's pytest command. Expected: current `always()` and `continue-on-error` violate the contract.

- [ ] **Step 3: Make the workflow fail closed**

Gate the deployment job on build success and non-PR events, remove all artifact-download error suppression, assemble the staged site, run the strict validator, and only then upload/deploy Pages. A missing artifact must terminate the job; do not replace it with a warning.

- [ ] **Step 4: Verify locally and inspect the workflow diff**

```bash
/Users/billh/gai/friendorfoe/backend/.venv/bin/pytest esp32/scripts/test_verify_web_flasher_site.py backend/tests/test_firmware_build_version.py -v
git diff --check
git diff -- .github/workflows/esp32-web-flasher.yml
```

Expected: tests pass; no `always()` deployment, no download `continue-on-error`, and validator precedes Pages upload.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/esp32-web-flasher.yml docs/badge/README.md esp32/scripts/test_verify_web_flasher_site.py
git commit -m "release: fail closed before badge flasher deploy"
```
