# Backend Sensor Release and Canary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the two isolated backend firmware builds into verifiable backend-named artifacts, prove the real firmware serializer against FastAPI, provide a separate backend-only web flasher, and migrate one three-board XIAO ESP32-S3 assembly through a fail-closed direct-USB canary without touching badge firmware.

**Architecture:** A local release verifier reads ESP image metadata and checks every identity, digest, marker, partition boundary, and artifact filename before packaging. A host-compiled fixture invokes the real C batch serializer and feeds its output to the backend test suite. Packaging and CI stay inside the backend firmware family. A stateful canary tool permits read-only inventory and backups first, requires exact per-board operator approval for writes, preserves the NVS range, and enforces scanners-first/uplink-last migration.

**Tech Stack:** Python 3.12, pytest, C11 host compiler, PlatformIO/ESP-IDF, GitHub Actions, ESP Web Tools manifests, esptool for ESP32-S3 read/write operations, FastAPI/httpx tests, POSIX `termios`/shell.

## Global Constraints

- This is plan 3 of 3. It consumes the API contract from plan 1 and the two binaries and serializer from plan 2.
- Keep every new firmware release/flasher/canary source under `backend-firmware/`; the only other planned changes are the cross-language backend fixture test, a new dedicated workflow, and backend-firmware documentation.
- Do not modify `esp32/uplink/`, `esp32/scanner/`, `esp32/shared/`, `esp32/web-flasher/`, badge scripts, or existing badge workflows.
- All directories, archives, workflow artifacts, manifests, HTML labels, and binary metadata use `backend` names.
- The only permitted tag namespace is `backend-fw-<version>` (for this plan,
  `backend-fw-0.1.0-backend`); backend tags never begin with `v`. Do not
  publish a GitHub Release while the existing badge release workflow responds
  to every release event. CI distributes only its private Actions artifact.
- Release identity is exact: version `0.1.0-backend`, hardware `seeed_xiao_esp32s3`, uplink target/project `uplink-s3-backend`/`fof_backend_uplink`, scanner target/project `scanner-s3-combo-backend`/`fof_backend_scanner`.
- “Lite” is only the operator-facing nickname for the physical no-screen three-board sensor assembly. It never appears in a firmware target, project, binary, manifest, catalog, release, or OTA identity; all such identifiers remain strictly backend-named.
- Initial migration of both scanners is direct USB. Never construct a legacy-identity bridge, never relax target/project/hardware checks, and never attempt badge/production-to-backend UART OTA.
- Inventory and full-flash/NVS backups of all three boards must succeed before any write. If either scanner cannot be reached over USB, stop and wait for physical access.
- Uplink is inventoried and backed up last, then deliberately left quiescent in
  USB ROM/reset state with its application not running. UART wiring remains
  physically fixed throughout initial migration—no live hot-plugging. Both
  scanners are flashed and provisionally verified before the uplink is flashed
  last. NVS at `0x9000..0xefff` is backed up and never written by the initial-
  flash command.
- No hardware write occurs merely because this software plan is executed. The operator must connect the named board and approve its exact role plus chip MAC immediately before each flash.
- The cross-plan binary identity contract is one and only one 164-byte record: little-endian `uint32_t magic=0x42464F46`, `uint16_t schema=1`, `uint16_t image_kind` (`0=uplink`, `1=scanner`), zero-padded `target[40]`, `project[40]`, `hardware[40]`, `version[32]`, and little-endian CRC32 over bytes `0..159`. It must agree with the ESP app descriptor and release index.
- The initial USB canary uses only `backend_canary.py`. The ESP Web Tools page is an unpublished, recovery/maintenance aid and is never an initial-migration or canary flashing path.
- Before Task 5, plan 2 must emit provisional boot identity before role-dependent rollback clearance and final health evidence after rollback clearance. If those exact records are absent, hardware work is blocked rather than weakened.
- Before Task 6 OTA evidence, plan 2 provides the uplink USB-only `FOF_BACKEND_OTA_PROBE`, `FOF_BACKEND_OTA_APPLY`, and `FOF_BACKEND_OTA_STATUS` maintenance contract. It pulls only through plan 1's backend catalog/download API and reports target/project/hardware/version/SHA/size/partition/recovery decisions plus image-write counters. Plan 3 consumes that exact contract; it never falls back to legacy `/api/ota`, `staged_legacy`, or `direct_legacy` routes.
- Hardware writes are refused when secure boot or flash encryption is enabled,
  flash size is not exactly 8 MB, or the live chip MAC differs from inventory.
  Before an initial migration, the decoded live table must exactly match the
  source-audited allowlist entry for that captured legacy target/project/
  hardware/version; it is not required to equal the backend table. Every
  packaged artifact must exactly match its backend table, and every provisional,
  final, baseline-backup, OTA, and recovery check after initial flashing must
  observe that exact backend table hash.

## Release File Map

```text
backend-firmware/
  tools/
    firmware_identity.py
    verify_backend_build.py
    pio_verify_backend_build.py
    emit_serializer_fixture.py
    verify_backend_release.py
    backend_canary.py
    backend_canary_evidence.py
    tests/
      test_firmware_identity.py
      test_verify_backend_build.py
      test_serializer_fixture.py
      test_backend_web_flasher.py
      test_verify_backend_release.py
      test_backend_canary.py
      test_backend_canary_evidence.py
  test/support/backend_serializer_fixture.c
  web-flasher/
    index.html
    build.sh
    manifest-uplink-s3-backend.json
    manifest-scanner-s3-combo-backend.json
    firmware/{uplink-s3-backend,scanner-s3-combo-backend}/.gitkeep
  release/backend-release-index.json     # generated atomically from both builds
  .canary/.gitkeep                 # empty marker; every other entry is ignored
backend/tests/fixtures/backend_firmware_detection_batch.json
backend/tests/test_backend_firmware_ingest.py
.github/workflows/backend-firmware.yml
docs/backend-firmware-canary.md
```

---

### Task 1: Verify Embedded Identity and Produce a Backend Release Index

**Files:**
- Create: `backend-firmware/tools/firmware_identity.py`
- Create: `backend-firmware/tools/verify_backend_build.py`
- Create: `backend-firmware/tools/pio_verify_backend_build.py`
- Create: `backend-firmware/tools/tests/test_firmware_identity.py`
- Create: `backend-firmware/tools/tests/test_verify_backend_build.py`
- Modify: `backend-firmware/scanner/platformio.ini`
- Modify: `backend-firmware/uplink/platformio.ini`

**Interfaces:**
- Consumes: each PlatformIO build directory and the exact expected backend identity.
- Produces: `parse_esp_app_identity`, `verify_backend_image`, `verify_artifact_set`, and a deterministic release index containing offsets, byte sizes, SHA-256, and CRC32.

- [ ] **Step 1: Write failing ESP image identity tests**

```python
from pathlib import Path
import hashlib
import struct
import zlib

import pytest

from tools.firmware_identity import (
    BACKEND_IDENTITY_MAGIC,
    ESP_APP_DESC_MAGIC,
    FirmwareIdentityError,
    parse_backend_identity_record,
    parse_esp_app_identity,
    verify_backend_image,
)


def c_field(value: str, size: int) -> bytes:
    encoded = value.encode("ascii")
    assert len(encoded) < size
    return encoded + bytes(size - len(encoded))


def identity_record(
    *, image_kind: int, target: str, project: str, hardware: str, version: str,
) -> bytes:
    prefix = struct.pack(
        "<IHH40s40s40s32s",
        BACKEND_IDENTITY_MAGIC,
        1,
        image_kind,
        c_field(target, 40),
        c_field(project, 40),
        c_field(hardware, 40),
        c_field(version, 32),
    )
    assert len(prefix) == 160
    return prefix + struct.pack("<I", zlib.crc32(prefix) & 0xFFFFFFFF)


def fake_app_image(
    project: str,
    version: str,
    *,
    image_kind: int,
    target: str,
    hardware: str,
) -> bytes:
    segment = bytearray(512)
    segment[0:4] = ESP_APP_DESC_MAGIC.to_bytes(4, "little")
    segment[16:48] = c_field(version, 32)
    segment[48:80] = c_field(project, 32)
    record = identity_record(
        image_kind=image_kind,
        target=target,
        project=project,
        hardware=hardware,
        version=version,
    )
    segment[256:256 + len(record)] = record

    # ESP32-S3 image: 24-byte header, one 8-byte segment header, segment,
    # checksum in the final byte of a 16-byte block, then appended SHA-256.
    common = struct.pack("<BBBBI", 0xE9, 1, 2, 0x3F, 0)
    extended = struct.pack(
        "<BBBBHBHHBBBBB",
        0xEE, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 1,
    )
    image = bytearray(common + extended)
    image += struct.pack("<II", 0x3FC88000, len(segment))
    image += segment
    checksum = 0xEF
    for value in segment:
        checksum ^= value
    while len(image) % 16 != 15:
        image.append(0)
    image.append(checksum)
    image += hashlib.sha256(image).digest()
    return bytes(image)


def test_parse_exact_scanner_descriptor_and_markers(tmp_path: Path):
    image = fake_app_image(
        "fof_backend_scanner",
        "0.1.0-backend",
        image_kind=1,
        target="scanner-s3-combo-backend",
        hardware="seeed_xiao_esp32s3",
    )
    path = tmp_path / "firmware.bin"
    path.write_bytes(image)
    result = verify_backend_image(
        path,
        target="scanner-s3-combo-backend",
        project="fof_backend_scanner",
        hardware="seeed_xiao_esp32s3",
        version="0.1.0-backend",
        partition_capacity=0x200000,
    )
    assert result.project == "fof_backend_scanner"
    assert result.version == "0.1.0-backend"
    assert result.size == len(image)
    assert len(result.sha256) == 64
    assert parse_backend_identity_record(image).image_kind == 1


@pytest.mark.parametrize("mutation", ["project", "version", "target", "hardware"])
def test_rejects_each_cross_family_or_missing_identity(tmp_path: Path, mutation: str):
    values = {
        "project": "fof_backend_scanner",
        "version": "0.1.0-backend",
        "target": "scanner-s3-combo-backend",
        "hardware": "seeed_xiao_esp32s3",
    }
    values[mutation] = {
        "project": "fof_badge_scanner",
        "version": "0.64.68-live-follow",
        "target": "scanner-s3-combo-fof_badge",
        "hardware": "seed_scanner",
    }[mutation]
    path = tmp_path / "firmware.bin"
    path.write_bytes(fake_app_image(
        values["project"], values["version"], image_kind=1,
        target=values["target"], hardware=values["hardware"],
    ))
    with pytest.raises(FirmwareIdentityError):
        verify_backend_image(
            path,
            target="scanner-s3-combo-backend",
            project="fof_backend_scanner",
            hardware="seeed_xiao_esp32s3",
            version="0.1.0-backend",
            partition_capacity=0x200000,
        )
```

Also test a bad image magic, bad app-description magic, non-NUL-terminated
project/version, structured-record magic/schema/image-kind mutations, nonzero
bytes after a string's first NUL, CRC mutation, a duplicate otherwise-valid
identity record, disagreement between the record and app descriptor, an empty
image, an invalid ESP segment checksum/digest, and a `0x200001`-byte image.
Loose target or hardware substrings are not identity evidence. Exactly one
valid 164-byte record must exist, and no second record or legacy/badge record
may coexist in the image.

- [ ] **Step 2: Run tests and observe missing modules**

Run:

```bash
cd backend-firmware
python -m pytest tools/tests/test_firmware_identity.py -q
```

Expected: FAIL because `firmware_identity.py` does not exist.

- [ ] **Step 3: Implement the strict app-image parser**

```python
ESP_IMAGE_MAGIC = 0xE9
ESP_IMAGE_HEADER_SIZE = 24
ESP_SEGMENT_HEADER_SIZE = 8
ESP_APP_DESC_OFFSET = ESP_IMAGE_HEADER_SIZE + ESP_SEGMENT_HEADER_SIZE
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP_APP_DESC_VERSION_OFFSET = 16
ESP_APP_DESC_PROJECT_OFFSET = 48
ESP_APP_DESC_STRING_SIZE = 32


@dataclass(frozen=True)
class VerifiedFirmwareImage:
    path: Path
    target: str
    project: str
    hardware: str
    version: str
    size: int
    sha256: str
    crc32: int
```

The public signatures are
`parse_esp_app_identity(image: bytes) -> tuple[str, str]` and
`parse_backend_identity_record(image: bytes) -> BackendIdentityRecord` and
`verify_backend_image(path: Path, *, target: str, project: str, hardware: str,
version: str, partition_capacity: int) -> VerifiedFirmwareImage`.

The implementation reads once and uses only Python's standard library to
validate the ESP32-S3 24-byte header, chip ID 9, 1..16 segment table, every
8-byte segment header and bounded payload, XOR checksum from initial `0xEF`,
16-byte checksum placement, `hash_appended == 1`, and the exact trailing
SHA-256 digest. It requires image and descriptor magic at the fixed offsets and
decodes the two fixed 32-byte app-description strings as strict ASCII C strings. It
locates exactly one 164-byte structured backend identity record by magic,
requires schema 1, validates zero-filled string tails and the CRC32 over bytes
`0..159`, maps image kind 0 to uplink and 1 to scanner, and compares every field
with the requested release identity and app descriptor. It rejects duplicate
records, record/descriptor disagreement, an empty image, or any image larger
than the partition. CRC32 value zero is retained as a valid computed value;
integrity comes from equality with the recomputed CRC, not a sentinel rule.

- [ ] **Step 4: Write failing artifact-set tests**

```python
EXPECTED_PART_OFFSETS = {
    "bootloader": 0x0000,
    "partition-table": 0x8000,
    "ota-data-initial": 0xF000,
    "firmware": 0x20000,
}


def expected_packaged_parts(target: str) -> dict[str, int]:
    return {
        f"{target}-{logical}.bin": offset
        for logical, offset in EXPECTED_PART_OFFSETS.items()
    }

EXPECTED_COMMON_PARTITIONS = {
    "nvs": ("data", "nvs", 0x9000, 0x6000),
    "otadata": ("data", "ota", 0xF000, 0x2000),
    "phy_init": ("data", "phy", 0x11000, 0x1000),
    "ota_0": ("app", "ota_0", 0x20000, 0x200000),
    "ota_1": ("app", "ota_1", 0x220000, 0x200000),
}

EXPECTED_SCANNER_TAIL = {
    "storage": ("data", "spiffs", 0x420000, 0x100000),
    "reserved": ("data", "fat", 0x520000, 0x2E0000),
}

EXPECTED_UPLINK_TAIL = {
    "fw_scanner_be": ("data", "0x40", 0x420000, 0x200000),
    "storage": ("data", "spiffs", 0x620000, 0x100000),
    "reserved": ("data", "fat", 0x720000, 0x0E0000),
}


def test_release_set_names_offsets_and_identity_are_exact(tmp_path: Path):
    build = make_fake_build(tmp_path, kind="uplink")
    release = verify_artifact_set(
        build,
        kind="uplink",
        version="0.1.0-backend",
    )
    assert {part.name: part.offset for part in release.parts} == (
        expected_packaged_parts("uplink-s3-backend")
    )
    assert release.target == "uplink-s3-backend"
    assert release.project == "fof_backend_uplink"
    assert release.hardware == "seeed_xiao_esp32s3"
    assert release.artifact_directory == "uplink-s3-backend"


def test_release_set_refuses_missing_or_non_backend_filename(tmp_path: Path):
    build = make_fake_build(tmp_path, kind="scanner")
    (build / "ota_data_initial.bin").unlink()
    with pytest.raises(BuildVerificationError, match="ota data"):
        verify_artifact_set(build, kind="scanner", version="0.1.0-backend")


@pytest.mark.parametrize("mutation", [
    "partition_offset", "partition_label", "rollback_disabled",
    "wrong_flash_size", "wrong_project", "wrong_flasher_offset",
    "wrong_flash_mode", "app_alias_differs", "ota_data_not_initial",
    "bootloader_crosses_partition_table", "partition_table_wrong_size",
    "part_intersects_nvs", "ota_data_wrong_size", "application_exceeds_slot",
    "part_overlap", "range_end_overflow", "flash_end_exceeds_8mb",
])
def test_release_set_rejects_layout_or_build_metadata_drift(
    tmp_path: Path, mutation: str,
):
    build = make_fake_build(tmp_path, kind="uplink")
    mutate_fake_build(build, mutation)
    with pytest.raises(BuildVerificationError):
        verify_artifact_set(build, kind="uplink", version="0.1.0-backend")
```

The test helper creates real build inputs named as ESP-IDF emits them:
`bootloader.bin`, `partitions.bin`, `ota_data_initial.bin`, and `firmware.bin`.
Those generic names are accepted only as unpublished inputs inside the exact
PlatformIO build directory. The packager maps all four inputs to exact target-
specific published basenames: `scanner-s3-combo-backend-bootloader.bin`,
`scanner-s3-combo-backend-partition-table.bin`,
`scanner-s3-combo-backend-ota-data-initial.bin`,
`scanner-s3-combo-backend-firmware.bin`, and the corresponding four
`uplink-s3-backend-*` names. No published, indexed, manifested, archived, or
canary-selected binary may retain a generic basename. No badge or generic
release directory is accepted. The helper also creates a real partition-table binary,
`sdkconfig.uplink-s3-backend`, `project_description.json`, and
`flasher_args.json`; mutations alter one real field at a time rather than
mocking the verifier.

- [ ] **Step 5: Implement deterministic build verification and release index**

```python
BACKEND_RELEASES = {
    "uplink": BackendReleaseSpec(
        environment="uplink-s3-backend",
        target="uplink-s3-backend",
        project="fof_backend_uplink",
        hardware="seeed_xiao_esp32s3",
        artifact_directory="uplink-s3-backend",
    ),
    "scanner": BackendReleaseSpec(
        environment="scanner-s3-combo-backend",
        target="scanner-s3-combo-backend",
        project="fof_backend_scanner",
        hardware="seeed_xiao_esp32s3",
        artifact_directory="scanner-s3-combo-backend",
    ),
}
```

`verify_backend_build.py` exposes single-build `verify_artifact_set(...)` for
post-build checks and pair-level `verify_release_pair(...)` for packaging. The
pair CLI is exact:

```text
verify_backend_build.py pair
  --scanner-build-dir PATH --scanner-partition-csv PATH --scanner-sdkconfig PATH
  --uplink-build-dir PATH --uplink-partition-csv PATH --uplink-sdkconfig PATH
  --output-dir PATH --index PATH [--check-only]
```

It validates both targets completely before copying either. It builds one
in-memory schema-1 index containing exactly scanner and uplink, writes it to a
`0600` temporary file, `fsync`s it, and uses `os.replace` only after both
target directories have been successfully materialized and verified. A
single-kind invocation can never write an index. The sorted indentation-two
index records version, target/project/hardware/image kind, the structured
identity-record CRC, partition capacity, and for every part its backend path,
offset, size, SHA-256, and CRC32. `--check-only` performs no writes.

The index shape is fixed so canary tooling never guesses where a digest lives:

```json
{
  "schema": 1,
  "version": "0.1.0-backend",
  "targets": {
    "scanner-s3-combo-backend": {
      "kind": "scanner",
      "target": "scanner-s3-combo-backend",
      "project": "fof_backend_scanner",
      "hardware": "seeed_xiao_esp32s3",
      "identity_crc32": 1,
      "partition_capacity": 2097152,
      "parts": [
        {"name": "scanner-s3-combo-backend-firmware.bin", "path": "scanner-s3-combo-backend/scanner-s3-combo-backend-firmware.bin", "offset": 131072, "size": 1, "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "crc32": 1}
      ]
    },
    "uplink-s3-backend": {
      "kind": "uplink",
      "target": "uplink-s3-backend",
      "project": "fof_backend_uplink",
      "hardware": "seeed_xiao_esp32s3",
      "identity_crc32": 1,
      "partition_capacity": 2097152,
      "parts": [
        {"name": "uplink-s3-backend-firmware.bin", "path": "uplink-s3-backend/uplink-s3-backend-firmware.bin", "offset": 131072, "size": 1, "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "crc32": 1}
      ]
    }
  }
}
```

The numeric `1` and `size:1` values above illustrate types only; generated
values are the verified record CRC, byte size, and binary CRC. Each target's
real `parts` array contains exactly the four target-specific results from
`expected_packaged_parts(target)`, sorted by offset, and the top-level
`targets` keys are sorted lexically. For every part the verifier additionally
requires `Path(path).name == name`, an exact target-directory parent, and the
target prefix in the basename.

For each build, decode `partitions.bin` entry-by-entry and require exact
equality with the matching backend CSV and the tables above. Parse generated
sdkconfig and require 8-MB flash, the exact custom partition filename,
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, octal PSRAM, and role-specific radio
settings. Parse `project_description.json` and require exact project/version
and repo-local component paths only under `backend-firmware`. Parse
`flasher_args.json` and require chip `esp32s3`, `dio`, `80m`, `8MB`, and exactly
the four offsets `0x0`, `0x8000`, `0xf000`, and `0x20000`. Require
the unpublished build input `firmware.bin` to be byte-identical to the
project-named application binary,
`ota_data_initial.bin` to be exactly 8192 bytes of `0xff`, each app to fit its
2-MB slot, and the scanner app to fit `fw_scanner_be`.

Range validation uses unbounded Python integers and proves the entire half-open
interval of every packaged part, not merely its start offset. Require positive
integer sizes and exact boundaries: bootloader `[0x0, end)` with
`end <= 0x8000`; partition table exactly `[0x8000, 0x9000)`; protected NVS
`[0x9000, 0xF000)` intersecting no packaged part; OTA data exactly
`[0xF000, 0x11000)`; and application `[0x20000, end)` with
`end <= 0x220000`. Require every end to be `<= 0x800000`, all four ranges to be
pairwise disjoint, no arithmetic wrap/truncation, and no range to cross its
decoded partition boundary. Tests mutate every start and size independently,
including an overlap, an oversized bootloader/application, a part ending past
8 MB, and a value that would wrap a fixed-width integer.

Plan 2's top-level CMake files read `shared/backend_version.h` and set
`PROJECT_VER` before `project(...)`, making the ESP app descriptor use that
single version source. `pio_verify_backend_build.py` invokes
`verify_backend_image` as a post-build action against `$BUILD_DIR/firmware.bin`
and aborts the build on mismatch. Add only these local scripts to the two
PlatformIO environments.

- [ ] **Step 6: Run verifier tests and commit**

Run:

```bash
cd backend-firmware
python -m pytest tools/tests/test_firmware_identity.py tools/tests/test_verify_backend_build.py -q
python tools/check_source_isolation.py --root .
```

Expected: PASS.

```bash
git add backend-firmware/tools backend-firmware/scanner/platformio.ini backend-firmware/uplink/platformio.ini
git commit -m "backend-fw: verify backend release identities"
```

---

### Task 2: Prove the Real C Serializer Against FastAPI

**Files:**
- Create: `backend-firmware/test/support/backend_serializer_fixture.c`
- Create: `backend-firmware/tools/emit_serializer_fixture.py`
- Create: `backend-firmware/tools/tests/test_serializer_fixture.py`
- Create: `backend/tests/fixtures/backend_firmware_detection_batch.json`
- Modify: `backend/tests/test_backend_firmware_ingest.py`

**Interfaces:**
- Consumes: the real `backend_upload_batch.c`, `backend_detection_codec.c`, `backend_json_writer.c`, and backend ingestion endpoint.
- Produces: one canonical complete payload generated by C and accepted losslessly by FastAPI.

- [ ] **Step 1: Add a failing Python wrapper test**

```python
def test_real_serializer_emits_complete_bounded_fixture(tmp_path: Path):
    payload = emit_fixture(REPO_ROOT, compiler="cc", build_dir=tmp_path)
    body = json.loads(payload)
    assert len(payload) <= 4096
    assert body["device_id"] == "uplink_CB77A4"
    assert body["firmware_target"] == "uplink-s3-backend"
    assert body["app_project"] == "fof_backend_uplink"
    assert body["hardware_type"] == "seeed_xiao_esp32s3"
    assert body["led_state"] == "drone_meta"
    assert len(body["scanners"]) == 2
    assert len(body["detections"]) == 2
    drone, meta = body["detections"]
    assert body["timestamp"] == 1_785_600_000
    assert drone["timestamp"] == 1_785_600_000_123
    assert meta["timestamp"] == 1_785_600_000_456
    assert drone["scanner_slots_seen"] == 3
    assert drone["freq_mhz"] == 2437
    assert drone["vertical_speed_mps"] == -1.5
    assert meta["ble_threat_kind"] == 2
    assert meta["ble_threat_evidence_mask"] == 49
```

Run: `cd backend-firmware && python -m pytest tools/tests/test_serializer_fixture.py -q`

Expected: FAIL because the fixture executable and wrapper are absent.

- [ ] **Step 2: Create the host fixture using only production serializer code**

`backend_serializer_fixture.c` defines a fixed `backend_batch_context_t`, one
full Remote ID/drone record, and one Meta/BLE-threat record. Populate every
optional scalar/string field from plan 1 at least once across the two records,
including raw/fused confidence, source, timestamps, coordinates, heading,
horizontal/vertical speed, altitude/height, operator and area fields,
frequency/channel/width/generation, scanner slot/mask, BLE service/evidence,
SSID probes, queue counters, time health, Wi-Fi health, and LED state.

Its `main` is exact in behavior:

```c
int main(void)
{
    backend_batch_context_t context = fixture_context();
    backend_detection_observation_t drone = {
        .detection = fixture_drone(),
        .timestamp_valid = true,
        .timestamp_epoch_ms = 1785600000123LL,
    };
    backend_detection_observation_t meta = {
        .detection = fixture_meta(),
        .timestamp_valid = true,
        .timestamp_epoch_ms = 1785600000456LL,
    };
    backend_upload_builder_t builder;
    backend_upload_batch_t batch = {0};

    backend_upload_builder_init(&builder, &context, 1000);
    if (backend_upload_builder_add(&builder, &drone, 1001) != BACKEND_ENCODE_OK ||
        backend_upload_builder_add(&builder, &meta, 1002) != BACKEND_ENCODE_OK ||
        !backend_upload_builder_finish(&builder, &batch) ||
        batch.json_len > BACKEND_UPLOAD_MAX_JSON) {
        return 2;
    }
    return fwrite(batch.json, 1, batch.json_len, stdout) == batch.json_len ? 0 : 3;
}
```

- [ ] **Step 3: Implement the deterministic compiler wrapper**

```python
PRODUCTION_SOURCES = (
    "shared/backend_upload_batch.c",
    "shared/backend_detection_codec.c",
    "shared/backend_json_writer.c",
)


def emit_fixture(repo_root: Path, *, compiler: str, build_dir: Path) -> bytes:
    backend_fw = repo_root / "backend-firmware"
    executable = build_dir / "backend-serializer-fixture"
    command = [
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DUNIT_TESTING", f"-I{backend_fw / 'shared'}",
        f"-I{backend_fw / 'test/stubs'}",
        str(backend_fw / "test/support/backend_serializer_fixture.c"),
        *(str(backend_fw / source) for source in PRODUCTION_SOURCES),
        "-o", str(executable),
    ]
    subprocess.run(command, check=True, cwd=repo_root)
    return subprocess.run(
        [str(executable)], check=True, cwd=repo_root, capture_output=True,
    ).stdout
```

The CLI supports `--output PATH` and `--check PATH`. `--output` atomically
writes exact bytes plus one final newline. `--check` regenerates in a temporary
directory and exits nonzero if the checked-in fixture differs. It never writes
object files or executables beneath the repository.

- [ ] **Step 4: Add the backend contract test using the checked fixture**

```python
@pytest.mark.asyncio
async def test_real_backend_firmware_serializer_contract(client):
    fixture = Path(__file__).parent / "fixtures/backend_firmware_detection_batch.json"
    body = json.loads(fixture.read_text(encoding="utf-8"))
    response = await client.post("/detections/drones", json=body)
    assert response.status_code == 200
    ack = response.json()
    assert set(ack) == {
        "status", "accepted", "processed", "deduplicated", "filtered",
        "device_id",
    }
    assert ack["status"] == "ok"
    assert ack["device_id"] == "uplink_CB77A4"
    assert ack["accepted"] == 2
    assert ack["processed"] + ack["deduplicated"] + ack["filtered"] == ack["accepted"]
```

Extend the test to query detection history and assert persisted frequency,
vertical speed, fused confidence, Remote ID accuracy/area fields, Wi-Fi
generation, and BLE threat evidence. Compare the stored operational
`device_id` to `uplink_CB77A4`; never register a new identity based on the
firmware target or hardware MAC.

- [ ] **Step 5: Generate, check, run, and commit**

Run:

```bash
cd backend-firmware
python tools/emit_serializer_fixture.py --output ../backend/tests/fixtures/backend_firmware_detection_batch.json
python tools/emit_serializer_fixture.py --check ../backend/tests/fixtures/backend_firmware_detection_batch.json
python -m pytest tools/tests/test_serializer_fixture.py -q

cd ../backend
pytest tests/test_backend_firmware_ingest.py -q
```

Expected: PASS; the checked fixture is at most 4096 bytes and is byte-for-byte
regenerated from the real C code.

```bash
git add backend-firmware/test/support backend-firmware/tools/emit_serializer_fixture.py backend-firmware/tools/tests/test_serializer_fixture.py backend/tests/fixtures/backend_firmware_detection_batch.json backend/tests/test_backend_firmware_ingest.py
git commit -m "test: prove backend firmware serializer contract"
```

---

### Task 3: Build a Separate Backend-Only Web Flasher

**Files:**
- Create: `backend-firmware/web-flasher/index.html`
- Create: `backend-firmware/web-flasher/build.sh`
- Create: `backend-firmware/web-flasher/manifest-uplink-s3-backend.json`
- Create: `backend-firmware/web-flasher/manifest-scanner-s3-combo-backend.json`
- Create: `backend-firmware/web-flasher/firmware/uplink-s3-backend/.gitkeep`
- Create: `backend-firmware/web-flasher/firmware/scanner-s3-combo-backend/.gitkeep`
- Create: `backend-firmware/tools/tests/test_backend_web_flasher.py`
- Modify: `backend-firmware/.gitignore`

**Interfaces:**
- Consumes: verified release sets from Task 1.
- Produces: two backend-only ESP Web Tools manifests and a local static page that cannot select badge/production images.

- [ ] **Step 1: Write failing manifest and isolation tests**

```python
EXPECTED_OFFSETS = [0, 32768, 61440, 131072]


@pytest.mark.parametrize("name,directory", [
    ("manifest-uplink-s3-backend.json", "uplink-s3-backend"),
    ("manifest-scanner-s3-combo-backend.json", "scanner-s3-combo-backend"),
])
def test_manifest_is_backend_named_and_complete(name: str, directory: str):
    manifest = json.loads((FLASHER / name).read_text(encoding="utf-8"))
    assert manifest["version"] == "0.1.0-backend"
    assert manifest["builds"][0]["chipFamily"] == "ESP32-S3"
    parts = manifest["builds"][0]["parts"]
    assert [part["offset"] for part in parts] == EXPECTED_OFFSETS
    assert [Path(part["path"]).parts[1] for part in parts] == [directory] * 4
    assert all("backend" in part["path"] for part in parts)


def test_backend_flasher_never_references_protected_firmware():
    text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in FLASHER.rglob("*")
        if path.is_file() and path.suffix in {".html", ".json", ".sh"}
    )
    for forbidden in (
        "esp32/web-flasher", "esp32/scanner", "esp32/uplink",
        "fof_badge", "badge-scanner", "badge-uplink",
        "scanner-s3-combo-seed", "uplink-s3/",
    ):
        assert forbidden not in text
```

Add a regression that first writes non-UTF-8 bytes to both target-specific
`*-backend-firmware.bin` paths and reruns the text isolation scan. The test must pass,
proving a second full release-gate run does not attempt to decode binaries.
Resolve every manifest part path and require that it is normalized, relative,
contains no `..`, and remains beneath the exact target firmware directory.

Run: `cd backend-firmware && python -m pytest tools/tests/test_backend_web_flasher.py -q`

Expected: FAIL because the backend flasher is absent.

- [ ] **Step 2: Create the exact manifests**

Uplink manifest:

```json
{
  "name": "Friend or Foe Backend Uplink (XIAO ESP32-S3)",
  "version": "0.1.0-backend",
  "builds": [{
    "chipFamily": "ESP32-S3",
    "parts": [
      {"path": "firmware/uplink-s3-backend/uplink-s3-backend-bootloader.bin", "offset": 0},
      {"path": "firmware/uplink-s3-backend/uplink-s3-backend-partition-table.bin", "offset": 32768},
      {"path": "firmware/uplink-s3-backend/uplink-s3-backend-ota-data-initial.bin", "offset": 61440},
      {"path": "firmware/uplink-s3-backend/uplink-s3-backend-firmware.bin", "offset": 131072}
    ]
  }]
}
```

Scanner manifest is structurally identical with name
`Friend or Foe Backend Scanner (XIAO ESP32-S3)`, directory
`scanner-s3-combo-backend`, and exact basenames
`scanner-s3-combo-backend-{bootloader,partition-table,ota-data-initial,firmware}.bin`.
Tests require that every manifest basename and release-index basename is one of
these eight target-specific names; a generic `bootloader.bin`,
`partition-table.bin`, `ota-data-initial.bin`, or `firmware.bin` anywhere in a
published package fails.

- [ ] **Step 3: Implement a backend-only build/package script**

`build.sh` uses `set -euo pipefail`, resolves only its own parent tree, chooses
`$PIO` or `pio`, and runs exactly:

```bash
cd "$BACKEND_FW_DIR/scanner"
"$PIO_BIN" run -e scanner-s3-combo-backend
cd "$BACKEND_FW_DIR/uplink"
"$PIO_BIN" run -e uplink-s3-backend
cd "$BACKEND_FW_DIR"
python tools/verify_backend_build.py pair \
  --scanner-build-dir scanner/.pio/build/scanner-s3-combo-backend \
  --scanner-partition-csv scanner/partitions_backend_scanner_8mb.csv \
  --scanner-sdkconfig scanner/sdkconfig.scanner-s3-combo-backend \
  --uplink-build-dir uplink/.pio/build/uplink-s3-backend \
  --uplink-partition-csv uplink/partitions_backend_uplink_8mb.csv \
  --uplink-sdkconfig uplink/sdkconfig.uplink-s3-backend \
  --output-dir web-flasher/firmware \
  --index release/backend-release-index.json
```

The script contains no `rm -rf`; the pair verifier validates both builds before
replacing either resolved exact backend target directory and writes the atomic
two-target index last. It then runs both manifest tests and prints artifact
paths, sizes, and SHA-256 values from the release index.

- [ ] **Step 4: Create the static flasher page**

`index.html` loads exactly
`https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module`
(never floating `@10`) and renders exactly two clearly separated recovery/
maintenance choices: Backend Uplink and Backend Scanner.
Each button points to one local manifest. The page says:

```text
UNPUBLISHED BACKEND RECOVERY/MAINTENANCE TOOL — NOT THE INITIAL CANARY PATH.
Use tools/backend_canary.py for inventory, backup, MAC binding, initial migration, and restore.
This page cannot distinguish a Lite sensor board from a badge because both use XIAO ESP32-S3.
Do not connect or select a badge. Do not use this page until all three Lite boards already have verified backend identities.
```

Do not add a badge option, automatic board guessing, firmware upload endpoint,
link to the existing web flasher, deployment configuration, or claim that the
page enforces board identity. The page and manifests remain CI-reviewed files;
the initial canary runbook never opens the page.

- [ ] **Step 5: Run tests, package both images, and commit**

Run:

```bash
cd backend-firmware
python -m pytest tools/tests/test_backend_web_flasher.py -q
bash web-flasher/build.sh
```

Expected: PASS and only backend-named files under the new flasher. Task 4 adds
and runs the independent release verifier after this package exists; do not
call that not-yet-created tool in this task.

```bash
git add backend-firmware/web-flasher backend-firmware/tools/tests/test_backend_web_flasher.py backend-firmware/.gitignore backend-firmware/release/backend-release-index.json
git commit -m "backend-fw: package backend-only web flasher"
```

---

### Task 4: Add a Dedicated Non-Deploying Release Workflow and Protected-Path Audit

**Files:**
- Create: `backend-firmware/tools/verify_backend_release.py`
- Create: `backend-firmware/tools/tests/test_verify_backend_release.py`
- Create: `.github/workflows/backend-firmware.yml`

**Interfaces:**
- Consumes: backend tests, native firmware tests, two builds, release index, fixture, manifests, and Git diff.
- Produces: one verified downloadable artifact named `friend-or-foe-backend-firmware-0.1.0-backend`; it does not deploy Pages or alter existing releases.

- [ ] **Step 1: Write failing release-audit tests**

```python
PROTECTED_PREFIXES = (
    "esp32/", "scripts/", "tools/badge_flasher/",
)
PROTECTED_FILES = frozenset({".github/workflows/esp32-web-flasher.yml"})


def test_release_index_and_flasher_are_identical(tmp_path: Path):
    index, flasher = make_valid_release(tmp_path)
    result = verify_release(index=index, flasher=flasher)
    assert result.targets == ("scanner-s3-combo-backend", "uplink-s3-backend")
    tampered = (
        flasher / "firmware/uplink-s3-backend"
        / "uplink-s3-backend-firmware.bin"
    )
    tampered.write_bytes(tampered.read_bytes() + b"x")
    with pytest.raises(ReleaseVerificationError, match="digest"):
        verify_release(index=index, flasher=flasher)


def test_protected_path_audit_reports_exact_changes():
    changed = [
        "backend/app/x.py", "esp32/scanner/main/main.c",
        "esp32/rid-simulator/main/main.c",
        "tools/badge_flasher/flash.py", ".github/workflows/esp32-web-flasher.yml",
    ]
    assert protected_changes(changed) == [
        ".github/workflows/esp32-web-flasher.yml",
        "esp32/rid-simulator/main/main.c",
        "esp32/scanner/main/main.c",
        "tools/badge_flasher/flash.py",
    ]
```

- [ ] **Step 2: Implement the release and Git audit CLI**

```python
def protected_changes(paths: Iterable[str]) -> list[str]:
    return sorted(
        path for path in paths
        if path in PROTECTED_FILES or path.startswith(PROTECTED_PREFIXES)
    )
```

The other public signature is
`verify_release(*, index: Path, flasher: Path) -> VerifiedRelease`.

`verify_release` requires schema 1, exactly two distinct targets, exact shared
version/hardware, exact target/project pairs, the exact target-specific four
basenames and four-offset set for each target, matching file size/SHA-256/
CRC32, app partition capacity, and matching manifests. It recomputes the same
half-open range proof as Task 1: positive integer sizes, exact bootloader/table/
OTA-data boundaries, no NVS intersection, application end `<= 0x220000`, every
end `<= 0x800000`, pairwise non-overlap, and containment in the decoded target
partition. It rejects duplicate offsets, arithmetic overflow/truncation,
generic basenames, path/name disagreement, and extra binary files beneath
`web-flasher/firmware`. Release-audit tests independently mutate each size and
offset so a bug in the Task-1 verifier cannot bless an unsafe index.

CLI option `--audit-protected BASE` requires `BASE` to resolve to a commit and
to be an ancestor of `HEAD`. It runs `git diff --name-status
--find-renames BASE HEAD --`, includes additions, copies, modifications,
renames, type changes, and deletions, and checks both old and new paths for
renames/copies. `VENDOR_BASE` remains independently fixed as detector-source
provenance; it is not the evolving protected-diff baseline. The workflow uses
the pull request base SHA or push event `before` SHA so unrelated legitimate
main history cannot make all future backend builds fail. It separately parses `git status --porcelain=v1
--untracked-files=all` so an untracked protected path also fails. It invokes
`check_source_isolation.py`, verifies `VENDOR_BASE` remains the pinned commit,
and prints every protected violation with status. Tests cover deletion,
protected-to-backend rename, backend-to-protected rename, type change, and an
untracked file; none may pass silently.

- [ ] **Step 3: Add the dedicated workflow**

Create `.github/workflows/backend-firmware.yml` with pull-request and branch-
push path filters for:

```yaml
on:
  pull_request:
    paths: *backend_paths
  push:
    branches: [main]
    paths: *backend_paths

paths:
  - "backend-firmware/**"
  - "backend/app/models/**"
  - "backend/app/routers/detections.py"
  - "backend/app/routers/nodes.py"
  - "backend/app/services/backend_node_status.py"
  - "backend/app/services/database.py"
  - "backend/app/services/firmware_manager.py"
  - "backend/app/services/node_commands.py"
  - "backend/tests/**"
  - ".github/workflows/backend-firmware.yml"
```

Set top-level workflow permissions exactly to read-only content access:

```yaml
permissions:
  contents: read
```

The concrete workflow may repeat the paths instead of using the explanatory
anchor above. It must have exactly `push.branches: [main]` and no
`push.tags`, `workflow_dispatch`, release event, or tag-ref trigger, so neither
a tag nor a manual dispatch can run or publish this workflow. Do not add a job-
level permission override or an `environment`. The single
Ubuntu job checks out full history, installs
`backend/requirements.txt`, PlatformIO, and pytest, then runs in this order:

```yaml
env:
  BACKEND_AUDIT_EVENT_BASE: ${{ github.event.pull_request.base.sha || github.event.before }}
```

Resolve `BACKEND_AUDIT_BASE` fail-closed before the audit. For a pull request,
use the exact base SHA and require it to resolve and be an ancestor of `HEAD`.
For an ordinary push, use the exact nonzero `before` SHA with the same checks.
For a branch-creation push whose `before` is exactly forty zeroes, fetch the
repository default branch with full history, resolve its unambiguous remote
tip, and set `BACKEND_AUDIT_BASE` to
`git merge-base <default-branch-tip> HEAD`; require that result to resolve and
be an ancestor of `HEAD`. Missing event data, a non-40-hex SHA, a zero SHA in
any other context, a missing/ambiguous default branch, fetch failure, or a base
that is not an ancestor fails the job. Never silently use `HEAD^`, an empty
diff, or a shallow-history guess. Pass only the resolved base to
`--audit-protected`. Workflow tests simulate PR, ordinary push, and zero-
`before` branch creation, plus each failure mode.

```text
backend full pytest suite
backend-firmware Python tool tests
backend-firmware native Unity suite
serializer fixture --check
scanner clean build
uplink clean build
backend-only flasher build/package
release verifier
source-isolation audit
protected-path audit from the PR base/push-before SHA through HEAD, including untracked files
git diff --check
```

Upload only `backend-firmware/web-flasher`,
`backend-firmware/release/backend-release-index.json`, and test logs as
`friend-or-foe-backend-firmware-0.1.0-backend` with a seven-day retention.
This private workflow artifact is not a publication channel and the page is
marked recovery/maintenance-only. It has only `contents: read`, with no Pages,
GitHub Release, tag creation, package, deployment, or repository-write step or
permission. The workflow never invokes `gh release`, `actions/create-release`,
`git tag`, or a release upload action.

- [ ] **Step 4: Test workflow contract and commit**

Extend `test_verify_backend_release.py` to parse the workflow as YAML and
assert both exact environments are present, `actions/upload-artifact` is
present, top-level permissions equal exactly `{"contents": "read"}`, no job
has `permissions` or `environment`, and the strings `deploy-pages`,
`pages: write`, `gh release`, `actions/create-release`, `git tag`, badge
environments, and existing `esp32/web-flasher` build script are absent. Assert
the path filter contains `backend-firmware/**`, does not contain `esp32/**`,
has only main-branch pushes with no tag/release/manual trigger, and contains the
tested zero-before base resolution. The separate Git audit protects every existing ESP32 path whenever
a backend/API PR runs.

Run:

```bash
cd backend-firmware
python -m pytest tools/tests/test_verify_backend_release.py -q
BACKEND_AUDIT_BASE="$(git merge-base origin/main HEAD)"
python tools/verify_backend_release.py --index release/backend-release-index.json --flasher web-flasher --audit-protected "$BACKEND_AUDIT_BASE"
```

Expected: PASS with no protected path changes.

```bash
git add backend-firmware/tools/verify_backend_release.py backend-firmware/tools/tests/test_verify_backend_release.py .github/workflows/backend-firmware.yml
git commit -m "ci: verify backend firmware artifacts"
```

---

### Task 5: Implement a Fail-Closed Inventory, Backup, Initial-Flash, and Restore Tool

**Files:**
- Create: `backend-firmware/tools/backend_canary.py`
- Create: `backend-firmware/tools/tests/test_backend_canary.py`
- Modify: `backend-firmware/.gitignore`

**Interfaces:**
- Consumes: the Lite assembly's three explicit USB serial ports, the running
  original uplink's read-only status evidence, verified backend artifacts, the
  PlatformIO executable, and one-use operator challenges.
- Produces: a durable local canary state file, structured installed identities,
  decoded partitions, security state, full-flash/NVS backups, safe initial and
  restore commands, provisional boot evidence, final rollback-clear evidence,
  and approval-gated backend-to-backend OTA maintenance commands.

- [ ] **Step 1: Write failing command-builder and state-order tests**

```python
BOARD_ROLES = ("scanner0", "scanner1", "uplink")


def test_backup_reads_full_flash_and_nvs_without_writing(tmp_path: Path):
    commands = build_backup_commands(
        esptool=Path("/opt/esptool.py"),
        port="/dev/cu.usbmodem1101",
        output_dir=tmp_path,
        role="scanner0",
        flash_size=0x800000,
    )
    joined = " ".join(" ".join(command) for command in commands)
    assert "read_flash 0x0 0x800000" in joined
    assert joined.count("read_flash 0x0 0x800000") == 2
    assert "read_flash 0x9000 0x6000" in joined
    assert joined.count("--after no_reset") == len(commands)
    assert "write_flash" not in joined
    assert "erase_flash" not in joined


def test_initial_flash_never_writes_nvs_or_erases_all():
    command = build_initial_flash_command(
        esptool=Path("/opt/esptool.py"),
        port="/dev/cu.usbmodem1101",
        artifact_dir=Path("scanner-s3-combo-backend"),
    )
    pairs = list(zip(command, command[1:]))
    assert ("0x9000", "nvs.bin") not in pairs
    assert "erase_flash" not in command
    assert "erase_all" not in command
    assert "--after" in command and "no_reset" in command
    assert "--verify" in command
    assert command[command.index("--flash_mode") + 1] == "dio"
    assert command[command.index("--flash_freq") + 1] == "80m"
    assert command[command.index("--flash_size") + 1] == "8MB"
    assert command[-8:] == [
        "0x0", "scanner-s3-combo-backend/scanner-s3-combo-backend-bootloader.bin",
        "0x8000", "scanner-s3-combo-backend/scanner-s3-combo-backend-partition-table.bin",
        "0xf000", "scanner-s3-combo-backend/scanner-s3-combo-backend-ota-data-initial.bin",
        "0x20000", "scanner-s3-combo-backend/scanner-s3-combo-backend-firmware.bin",
    ]


def test_uplink_is_refused_until_both_scanners_are_verified(state: CanaryState):
    state.record_backup("scanner0", "original", valid_backup("AA:00:00:00:00:01"))
    state.record_backup("scanner1", "original", valid_backup("AA:00:00:00:00:02"))
    state.record_backup("uplink", "original", valid_backup("AA:00:00:00:00:03"))
    state.record_provisional_backend_identity(
        "scanner0", scanner_identity("AA:00:00:00:00:01"),
    )
    with pytest.raises(CanaryOrderError, match="scanner1"):
        state.issue_challenge(
            role="uplink", port="/dev/cu.usbmodem-uplink",
            mac="AA:00:00:00:00:03", artifact_sha256="a" * 64,
            offsets_sha256="b" * 64, now=100,
        )


def test_flash_challenge_is_one_use_and_bound_to_live_inputs(state: CanaryState):
    ready_for_scanner0(state)
    challenge, token = state.issue_challenge(
        role="scanner0",
        port="/dev/cu.usbmodem1101",
        mac="AA:00:00:00:00:01",
        artifact_sha256="a" * 64,
        offsets_sha256="b" * 64,
        now=100,
    )
    assert state.consume_challenge(challenge.challenge_id, token, now=101)
    with pytest.raises(CanaryApprovalError, match="consumed"):
        state.consume_challenge(challenge.challenge_id, token, now=102)


def test_restore_is_only_allowed_after_partial_failure_and_exact_backup_source(
    state: CanaryState,
):
    failed_scanner0(state)
    challenge, token = state.issue_restore_challenge(
        "scanner0", source="original", full_backup_sha256="c" * 64, now=100,
    )
    command = state.authorize_restore(challenge.challenge_id, token, now=101)
    assert "--verify" in command
    assert command[-2:] == [
        "0x0", state.boards["scanner0"].backups["original"].full_path,
    ]


def test_ota_probe_is_read_only_and_apply_requires_one_use_challenge(
    state: CanaryState,
):
    ready_for_backend_ota(state)
    probe = state.record_ota_probe(valid_probe_evidence(
        component="scanner0", sha256="d" * 64,
        image_writes_before=7, image_writes_after=7,
    ))
    challenge, token = state.issue_ota_challenge(
        component="scanner0", artifact_sha256=probe.sha256,
        artifact_crc32=probe.crc32,
        mode="same-version-recovery", now=100,
    )
    assert state.consume_challenge(challenge.challenge_id, token, now=101)
    with pytest.raises(CanaryApprovalError, match="consumed"):
        state.consume_challenge(challenge.challenge_id, token, now=102)
```

Also test that no flash is authorized before all three inventories and backups
exist, wrong MAC/role approval tokens fail, the same port cannot be assigned to
two roles, a backup with wrong length or changed SHA fails, scanner1 cannot be
skipped, an uplink artifact cannot be selected for a scanner, and an
unverified artifact set fails before a subprocess starts. Add exact tests for
missing installed target/project/hardware/version, missing original scanner
role, missing updater-admission evidence, a non-8-MB or changed partition map,
secure boot, flash encryption, changed live MAC, expired/wrong-artifact/wrong-
port/replayed challenges, world-readable backups, existing backup filename,
an esptool subprocess prefix that does not use PlatformIO's Python, booting
before all reads finish, any initial NVS write, and attempting another board
while one role is `flash_failed` or `restore_required`.
Add migration-order tests proving the original uplink is backed up last, its
application is not restarted, `original_uplink_quiesced` is recorded only
after all verified reads complete with `--after no_reset`, UART wiring remains
fixed throughout migration, and an observed uplink application reboot
invalidates every outstanding write challenge. Test
that the exact captured legacy scanner table and the legacy uplink table with
`fw_scanner_s3` are recognized before initial flash, while any unrecognized
preflash table fails; then require the exact backend table (including
`fw_scanner_be` for uplink) for provisional/final/backend-baseline/OTA states.
Never require the recognized legacy preflash table to equal the backend table.
Test the Task-1 half-open range proof again before command construction so no
crafted index can overlap NVS, cross a partition boundary, or exceed 8 MB.
For OTA, test that a probe with unequal image-write counters, incomplete image
validation, changed boot ID, wildcard SHA on apply, non-backend identity, stale
probe, missing final health, or active restore requirement cannot issue an OTA
challenge; also reject missing or mismatched whole-image CRC32. Bind the
challenge to component, artifact SHA, artifact CRC32, mode, uplink port, MAC,
state generation, and five-minute expiry. For scanners, also bind physical
slot, scanner MAC, current scanner boot ID, and topology generation; mutation
of any bound field invalidates canonical token hashing/consume revalidation.
For every inventory/challenge/write operation, mutate the requested `--pio`,
resolved PlatformIO Python/core directory, esptool path/version, or canonical
toolchain hash and prove consumption fails before a write. Test that an OTA
challenge cannot be issued until all three `backend-baseline` backups exist and
rehash, that an initial failure can select only `original`, that a backend OTA
failure can select only `backend-baseline`, and that neither source can be
silently substituted for the other.

- [ ] **Step 2: Implement explicit state and safe command construction**

```python
@dataclass(frozen=True)
class BoardIdentity:
    role: Literal["scanner0", "scanner1", "uplink"]
    port: str
    chip: str
    mac: str
    flash_size: int
    secure_boot_enabled: bool
    flash_encryption_enabled: bool
    installed_target: str
    installed_project: str
    installed_hardware: str
    installed_version: str
    installed_role: str
    installed_partition_sha256: str
    updater_admission_evidence_sha256: str


@dataclass(frozen=True)
class ApprovalChallenge:
    challenge_id: str
    role: str
    port: str
    mac: str
    operation: Literal["flash-initial", "restore-full", "ota-apply"]
    component: Literal["uplink", "scanner0", "scanner1"] | None
    ota_mode: Literal["newer-only", "same-version-recovery"] | None
    artifact_sha256: str
    artifact_crc32: int | None
    offsets_sha256: str
    state_generation: int
    target_slot: int | None
    target_mac: str | None
    target_boot_id: int | None
    topology_generation: int | None
    pio_path: str
    toolchain_sha256: str
    restore_source: Literal["original", "backend-baseline"] | None
    expires_at: int
    consumed_at: int | None


class CanaryState:
    schema: int
    created_at: str
    boards: dict[str, BoardRecord]
```

Its methods are `record_installed_evidence(...) -> None`,
`record_inventory(identity: BoardIdentity) -> None`,
`record_backup(role: str, kind: Literal["original", "backend-baseline"],
backup: BackupRecord) -> None`,
`issue_challenge(...) -> tuple[ApprovalChallenge, str]`,
`consume_challenge(challenge_id: str, token: str, *, now: int) -> None`,
`record_provisional_backend_identity(...) -> None`, and
`record_final_backend_health(...) -> None`,
`record_ota_probe(evidence: OtaEvidence) -> OtaEvidence`, and
`issue_ota_challenge(...) -> tuple[ApprovalChallenge, str]`.

The committed implementation has complete method bodies. State writes use a
temporary file plus `os.replace`, file and parent-directory `fsync`, permissions
`0600`, schema 1, normalized uppercase MACs, absolute resolved backup paths,
byte sizes, and SHA-256. Backup/evidence directories are `0700`; every backup,
transcript, and evidence file is created with `O_CREAT|O_EXCL` and mode `0600`.
Never store or print Wi-Fi/AP passwords, API keys, Authorization/Cookie/
Set-Cookie values, session credentials, or raw BLE authentication values.
Backups may contain credentials and therefore never leave ignored `.canary/`.
Store only a SHA-256 of an approval token in durable state; generate the token
with `secrets.token_urlsafe(32)`, expire it after five minutes, consume it
atomically before starting a write subprocess, and delete its `0600` challenge
receipt immediately after consumption or expiry. Redact keys matching
`password|secret|credential|token|authorization|cookie|set-cookie|api_key`
case-insensitively from transcripts/evidence.

Resolve the requested `--pio` to an absolute executable and derive a canonical
toolchain receipt containing that path, PlatformIO version, resolved
`python_exe`, resolved `core_dir`, resolved esptool path, esptool version, and
SHA-256 of the esptool file. Hash the canonical receipt as `toolchain_sha256`.
Every inventory, backup, probe, challenge, flash, OTA, and restore command takes
`--pio`; challenges bind both `pio_path` and `toolchain_sha256`, and consume
re-resolves all fields immediately before a write. A symlink/path/version/hash
change is a no-write failure.

Subcommands are exact:

```text
capture-installed --state STATE --uplink-url URL --backend-base URL --output-dir DIR
inventory --role ROLE --port PORT --state STATE --pio PATH
backup --kind original|backend-baseline --role ROLE --state STATE --output-dir DIR --pio PATH
verify-backup --kind original|backend-baseline --role ROLE --state STATE
verify-uplink-quiesced --state STATE --pio PATH
challenge-flash --role ROLE --state STATE --artifact-dir DIR --index INDEX --pio PATH --output FILE
flash-initial --role ROLE --state STATE --artifact-dir DIR --index INDEX --challenge-id ID --token TOKEN --pio PATH
verify-provisional --role ROLE --state STATE --port PORT --timeout 30
verify-final --role ROLE --state STATE --port PORT --timeout 180
verify-catalog --backend-base URL --index INDEX --output FILE
ota-probe --component COMPONENT --catalog-name NAME --expected-sha SHA --catalog-evidence FILE --index INDEX --state STATE --port UPLINK_PORT --pio PATH --output FILE --timeout 300
challenge-ota --component COMPONENT --mode newer-only|same-version-recovery --probe FILE --catalog-evidence FILE --index INDEX --state STATE --pio PATH --output FILE
ota-apply --component COMPONENT --mode newer-only|same-version-recovery --index INDEX --state STATE --port UPLINK_PORT --challenge-id ID --token TOKEN --pio PATH --output FILE --timeout 600
challenge-restore --role ROLE --source original|backend-baseline --state STATE --pio PATH --output FILE
restore-full --role ROLE --source original|backend-baseline --state STATE --challenge-id ID --token TOKEN --pio PATH
status --state STATE
```

Serial subcommands use only Python's POSIX `os`, `termios`, and `select`
modules: open the exact recorded port with `O_RDWR|O_NOCTTY|O_NONBLOCK`, set raw
8N1 at 921600 baud, write one complete newline command, and accept only one
bounded `FOF_BACKEND_BOOT`, `FOF_BACKEND_HEALTH`, or
`FOF_BACKEND_OTA_ACCEPTED`/`FOF_BACKEND_OTA_EVIDENCE` JSON line with duplicate keys rejected. The port and
reported uplink MAC must match inventory. Timeouts close the descriptor and do
not retry a write command automatically.

`capture-installed` fetches the original uplink's read-only `/api/status`,
`/api/ota/info`, and `/api/fw/info` before any board enters ROM mode. It takes
the operational `device_id` from that source, then fetches read-only
`GET {backend_base}/nodes`, requires exactly one matching registry row, and
records its registered name/fixed lat/lon/alt/position mode. It then fetches
plan 1's read-only
`GET {backend_base}/detections/calibrate/continuity/{device_id}` and records
the exact nonsecret schema, device ID, trust status, session ID, applied time,
per-listener-model presence, model schema, and value digest. `GET /nodes` is
not treated as a calibration API and no nonexistent registry-row calibration
fields are required. It writes all raw responses and
parses both scanner slots/roles/MACs and each running target/project/
hardware/version. If the backend row is absent/ambiguous, stop rather than
pretending live GPS is the registered fixed location. It binds recognized installed identities to the source-
audited exact-target OTA admission contract and stores the source commit/path
hash as updater-admission evidence. Missing fields, an unknown identity, an
unmatched scanner MAC, or an unavailable updater contract blocks all writes;
there is no manual `--force` override.

The installed partition allowlist is backend-owned evidence committed in this
tool. Each entry binds the exact captured installed target/project/hardware/
version and source-audit hash to one decoded legacy table hash. The recognized
legacy uplink entry includes `fw_scanner_s3`; the backend uplink table instead
includes `fw_scanner_be`. `capture-installed` selects exactly one entry, and
`inventory` must match it before initial migration. No runtime build or canary
command imports, copies, or flashes a protected legacy/badge artifact. After an
initial write, every verifier switches to the exact backend table hash from the
release index; there is no pre/post-table equality shortcut.

`inventory` runs PlatformIO-pinned esptool `get_security_info`, `flash_id`, and
`read_mac`, requires ESP32-S3, exactly 8 MB, secure boot disabled, flash
encryption disabled, and matches each chip MAC with `capture-installed`.
Resolve esptool by running `<pio> system info --json-output`, taking its
`python_exe` and `core_dir`, and invoking
`<python_exe> <core_dir>/packages/tool-esptoolpy/esptool.py`; never execute a
`.py` path directly or with the system Python.

`backup` keeps the chip in ROM with `--after no_reset` while it reads two
independent temporary full `0x0 0x800000` images, two independent temporary NVS
`0x9000 0x6000` images, and the `0x8000 0x1000` partition-table sector. It
requires the two full-image hashes and two NVS hashes to agree; additionally,
each full image's NVS slice must equal both focused NVS reads and each full
image's partition-table slice must equal the focused sector. It decodes the
partition table, matches installed evidence, hashes all retained files,
atomically keeps one verified full image in the role/MAC-specific backup
directory, and removes only the explicit temporary duplicate. It never
overwrites an existing backup and keys records and paths by immutable kind:
`original` or `backend-baseline`. Tests force full-read mismatch, focused/full
slice mismatch, and partition mismatch and prove no role becomes backed up.

Original backups run in exact order scanner0, scanner1, uplink. After each
scanner's original backup succeeds, invoke esptool `run` so the still-legacy
assembly remains operational for the remaining capture. Back up the original
uplink only after both scanner backups verify; leave it in ROM/reset-quiescent
state after its final `--after no_reset` read and do **not** invoke `run`.
Record `old_uplink_quiescent=true` with MAC, port, security/flash receipt,
toolchain hash, state generation, and timestamp. UART wiring stays physically
unchanged. Immediately before every scanner challenge,
`verify-uplink-quiesced` repeats only no-write esptool security/flash/MAC probes
against the same uplink port, ends with `--after no_reset`, and requires the
same MAC and security state. Any observed original-app status/output, USB reset
that releases ROM, MAC change, or failed probe clears the flag and invalidates
all challenges. Stop and repeat the no-write quiescence verification before
issuing a new challenge; never continue with a stale challenge.

`backend-baseline` backups are a separate phase after all three backend final-
health checks, AP configuration/functional recovery, and catalog preflight,
but before any OTA challenge or write. Each must decode and match the exact
backend partition-table hash and repeat the same full/NVS/table duplicate-read
proof. The tool explicitly runs the backend application after each baseline
backup and re-runs final health. Original backups are retained unchanged for
aborted initial migration only; backend-baseline backups are used for failed
backend-to-backend OTA only.

- [ ] **Step 3: Implement exact initial-flash admission**

Before constructing `write_flash`, the tool:

1. Reloads state and requires complete installed evidence, security inventory,
   decoded source-allowlisted partitions, and rehashed verified `original`
   backups for all three roles. The original uplink must still be quiescent.
2. Re-runs `verify_backend_release.py`, matches scanner/uplink kind, and hashes
   the canonical role/port/MAC/artifact identity/four offsets into
   `offsets_sha256`.
3. `challenge-flash` re-runs live `get_security_info`, `flash_id`, and
   `read_mac`, displays role, port, MAC, installed identity, backup paths/
   hashes, backend identity-record fields/CRC, artifact SHA, and exact offsets,
   toolchain receipt, and old-uplink quiescence receipt, then returns a five-
   minute one-use challenge ID/token bound to those values. Scanner challenges
   revalidate that the old uplink remains quiescent before and after their live
   checks; UART wiring remains fixed.
4. `flash-initial` atomically consumes that challenge before any subprocess;
   a changed state, path, port, MAC, artifact, offsets, security result, expiry,
   or consumed token fails.
   After consumption but immediately before `write_flash`, it re-runs live
   MAC/security/flash-size checks on that exact port and requires identical
   values. A swapped board is a no-write admission failure, not
   `restore_required`. `restore-full` performs the same final TOCTOU check.
5. Requires scanner0 and scanner1 provisional backend boot verification before
   issuing an uplink challenge, while the old uplink remains quiescent. Final
   scanner role/radio/rollback proof is
   intentionally deferred until the backend uplink can command them.
6. Calls PlatformIO-pinned esptool with `--chip esp32s3 --port <exact port>
   --before default_reset --after no_reset write_flash --flash_mode dio
   --flash_freq 80m --flash_size 8MB --verify` and only offsets `0x0`,
   `0x8000`, `0xf000`, and `0x20000`. Built-in verification completes before
   any application boot, so mutable OTA data is never compared after reset.
7. After successful range verification but before any boot, read NVS
   `0x9000 0x6000` again with `--after no_reset` and require its SHA to equal
   the preflash verified NVS SHA. Only then record success and explicitly
   invoke esptool `run`. An NVS mismatch is `restore_required`. On any exception/nonzero/interrupt after challenge
   consumption, it records `restore_required`, prohibits all other board
   writes, and prints the exact `challenge-restore` command.

The tool contains no erase subcommand and refuses an artifact whose resolved
path escapes the verified backend release directory. A failed write remains
`restore_required`; it does not advance ordering or attempt another board.
Every read/write/restore command builder rejects `--force`, `--erase-all`,
`--encrypt`, `--encrypt-files`, and
`--ignore-flash-encryption-efuse-setting`; tests assert none can appear.

`challenge-restore` is legal only for `restore_required` or an explicitly
operator-selected already-flashed role. It requires an explicit `--source`,
rechecks live MAC/security/toolchain, rehashes the exact role/MAC/source full
backup, displays that restoring it will also restore NVS and OTA state, and
issues a separate five-minute one-use token bound to the source.
`restore-full` consumes the token, then runs `write_flash --flash_mode dio
--flash_freq 80m --flash_size 8MB --verify 0x0 <exact-8MB-backup>` with
`--after no_reset`, followed by esptool `run`. Verification occurs before boot
because NVS and OTA metadata may legitimately change once the restored image
runs. The tool clears `restore_required` only after the restored installed
identity and recorded chip MAC are observed again; a mismatch remains blocked
and requires physical recovery review. An initial-flash failure permits only
`--source original`; an OTA failure permits only `--source backend-baseline`.
No restore can select another role's backup, cross the failure phase/source
boundary, or use a backup whose MAC/hash/table differs from state.

- [ ] **Step 4: Verify provisional boot and final rollback-clear evidence**

Plan 2 must print this provisional line immediately after immutable identity,
NVS, LED, and UART-command-ingress initialization, before scanner role-dependent
rollback clearance:

```text
FOF_BACKEND_BOOT {"target":"scanner-s3-combo-backend","project":"fof_backend_scanner","hardware":"seeed_xiao_esp32s3","version":"0.1.0-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"nvs_erased":false,"uart_ingress":true,"ota_state":"valid"}
FOF_BACKEND_BOOT {"target":"uplink-s3-backend","project":"fof_backend_uplink","hardware":"seeed_xiao_esp32s3","version":"0.1.0-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"device_id":"uplink_CB77A4","config_state":"loaded","config_generation":9,"nvs_erased":false,"auto_update_enabled":false,"uart0_started":true,"uart1_started":true,"network_state":"ap","ota_state":"valid"}
```

`verify-provisional` requires one complete JSON object with exact role-specific
target/project, hardware/version, recorded chip MAC, nonzero current `boot_id`,
and the booleans above. It opens the recorded port, sends the read-only
`FOF_BACKEND_STATUS` line, and parses the re-emitted record; it never relies on
catching startup output. Scanner provisional verification deliberately does not
claim rollback clearance and requires actual initial `ota_state:"valid"`
read from the running partition; `pending` is rejected for direct USB. It is
only the admission needed to migrate the second scanner and then the uplink.
A timeout, malformed/duplicate/conflicting object, or an unexpected reboot
leaves the board unverified.

After the backend uplink is flashed and both UARTs are connected, plan 2 must
emit one final line per board, with scanner final evidence also present in the
uplink's scanner-status snapshot:

```text
FOF_BACKEND_HEALTH {"target":"scanner-s3-combo-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"nvs_erased":false,"role":"ble_primary","command_ingress_boot_id":305419896,"radio_healthy":true,"rollback_clear":true}
FOF_BACKEND_HEALTH {"target":"scanner-s3-combo-backend","mac":"AA:BB:CC:DD:EE:00","boot_id":2271560481,"nvs_erased":false,"role":"wifi_primary","command_ingress_boot_id":2271560481,"radio_healthy":true,"rollback_clear":true}
FOF_BACKEND_HEALTH {"target":"uplink-s3-backend","mac":"AA:BB:CC:DD:EE:11","boot_id":2882400001,"device_id":"uplink_CB77A4","config_state":"loaded","config_generation":9,"nvs_loaded":true,"nvs_erased":false,"auto_update_enabled":false,"uart0_started":true,"uart1_started":true,"coordinator_started":true,"network_state":"ap","rollback_clear":true}
```

`verify-final` binds every final line to the corresponding provisional MAC and
boot ID, requires scanner roles to be exactly one `ble_primary` and one
`wifi_primary`, current-boot command ingress, radio health, and rollback clear;
both scanner records require `nvs_erased:false`. The uplink requires the exact
captured pre-migration `device_id`, config state `loaded|migrated`, nonzero
config generation, NVS loaded, `nvs_erased:false`,
`auto_update_enabled:false`, both UARTs, coordinator, AP-or-STA, and rollback
clear. Final verification also uses `FOF_BACKEND_STATUS` and rejects a blank or
newly generated identity even if other health fields are true.
No OTA, functional acceptance, restore-route retirement, or soak starts until
all three final records pass. If plan 2 does not provide these exact records,
Task 5 is blocked and its health requirements may not be relaxed locally.

- [ ] **Step 5: Implement approval-gated backend OTA maintenance**

`verify-catalog` is a mandatory read-only availability preflight. For exactly
`scanner-s3-combo-backend` and `uplink-s3-backend`, it fetches
`GET {backend_base}/nodes/firmware/latest/{target}` and then the returned
`GET {backend_base}/nodes/firmware/download/{target}` from the same normalized
scheme/host/port. It rejects redirects or alternate hosts and verifies exact
target/project/hardware/version, target-specific basename, size, SHA-256,
CRC32, ETag/content headers, and downloaded bytes against the release index.
The evidence is fresh, `0600`, canonical, and bound to the backend base and
index hash. If either artifact is unavailable, stop. The canary never uploads,
side-loads, or substitutes a local/raw image; use an independently authorized
backend deployment or run the verified backend worktree, then repeat the
preflight.

`ota-probe` sends plan 2's exact `FOF_BACKEND_OTA_PROBE` line. It validates the
returned schema/operation/component/catalog and exact target/project/hardware/
version/SHA/CRC32/size/capacity fields, including the legitimate CRC32 value
zero, against the release index. An admitted probe is recordable only when the
complete image was validated, `image_writes_before == image_writes_after`, the
boot IDs are unchanged, and no rollback transition occurred. It also records
the exact selected target MAC, target boot ID, physical slot (scanner only),
and topology generation (`0` for uplink). The hardware canary accepts only the
two backend catalog names and exact 64-hex SHA values. Cross-family rejection
remains covered by native/API unit tests; no badge/generic catalog path, raw
upload, wildcard SHA, or legacy `/api/ota` request is sent to canary hardware.

The parser requires exact component strings `uplink|scanner0|scanner1`, probe
mode `probe`, apply modes `newer-only|same-version-recovery`, and exactly the
ten decision spellings `admit`, `no_update`, `reject_identity`,
`reject_version`, `reject_digest`, `reject_size`, `reject_capacity`,
`reject_busy`, `applied`, and `failed`. Parameterized tests accept every legal
mapping and reject unknown/case variants or a probe carrying an apply mode.
For apply admission, parse the firmware contract's exact keys
`component_slot`, `uplink_mac`, `expected_target_mac`, `actual_target_mac`,
`expected_target_boot_id`, `actual_target_boot_id`,
`expected_topology_generation`, `actual_topology_generation`, and
`boot_id_before`. Reject a missing, duplicate, extra, renamed, or unequal
expected/actual binding before accepting the operation receipt.

`challenge-ota` requires the fresh catalog preflight, verified backend-baseline
backups and final health for all three boards, no pending restore, a fresh
admitted probe for the same component, SHA, CRC32, target fields, and an exact
backend release-index match. Its one-use challenge is bound to operation
`ota-apply`, component, uplink port/MAC, artifact SHA, artifact CRC32, apply
mode, state generation, PlatformIO/toolchain receipt, target MAC, target boot
ID, topology generation, and expiry. A scanner challenge additionally binds
the recorded physical slot; uplink uses no target slot and exact topology
generation `0`. Immediately before sending apply, re-read the latest validated
snapshot and refuse with zero writes if any binding changed or a scanner slot
now reports another MAC. `ota-apply` consumes it before writing exactly one
newline-terminated command:

```text
FOF_BACKEND_OTA_APPLY <component> <64hex-sha> <newer-only|same-version-recovery> <target-mac> <target-boot-id> <topology-generation>
```

No field may be omitted, inferred after approval, or replaced with a wildcard.
The command uses the challenge's exact canonical values and is never retried
after a timeout. It first
requires the flushed `FOF_BACKEND_OTA_ACCEPTED` record and stores its operation
ID. For uplink self-OTA, it expects USB disconnect/re-enumeration, repeatedly
reopens only the same recorded path for read-only polling until timeout, sends
only `FOF_BACKEND_OTA_STATUS` after reopen, and requires the same MAC,
operation ID, SHA, and CRC32. It never resends `FOF_BACKEND_OTA_APPLY` or
accepts another serial path. Scanner relay keeps the same uplink USB session
but follows the identical operation-ID checks. Success
requires both accepted and final evidence to echo the exact target MAC,
pre-apply target boot ID, topology generation, SHA, mode, and operation ID.
Final evidence must show `decision:"applied"`, the expected positive image-
write delta, a changed post-apply boot ID for the updated component, exact
identity/version/SHA/CRC32, rollback clearance, and convergence. It then reruns the
same final-health checks used by `verify-final`. Failure records
`ota_recovery_required`, blocks further OTA/flash operations, and prints the
MAC-bound `challenge-restore --source backend-baseline` command for direct USB
recovery. Tests parse the exact command fields and reject reordered, missing,
extra, stale, or noncanonical values before any image-write counter changes.

- [ ] **Step 6: Run canary tool tests and commit**

Run:

```bash
cd backend-firmware
python -m pytest tools/tests/test_backend_canary.py -q
python tools/backend_canary.py --help
```

Expected: PASS. The help output warns that hardware writes need exact approval,
scanners are direct USB first, uplink is last, the web flasher is not the
canary path, and restore is the only permitted next write after a partial
failure.

Add these exact rules to `backend-firmware/.gitignore`:

```gitignore
.canary/*
!.canary/.gitkeep
```

Keep `.canary/.gitkeep` empty. Tests run `git check-ignore` against state,
original and backend-baseline backups, transcripts, catalog/probe/evidence,
ready files, and challenge receipts; each must be ignored. `git ls-files
backend-firmware/.canary` must return only `.canary/.gitkeep`, and the runbook
must never use `git add -f`. All canary commands run under `umask 077`; the
tool rechecks `0700` directories and `0600` files rather than relying only on
the shell setting.

Full flash/NVS images, state, approval receipts, serial transcripts, and raw
canary logs are never committed and never included in an Actions artifact.
PR evidence contains only hashes and explicitly redacted JSON summaries; tests
inspect workflow upload paths and fail if `.canary` or a backup/log glob can be
uploaded.

```bash
git add backend-firmware/tools/backend_canary.py backend-firmware/tools/tests/test_backend_canary.py backend-firmware/.gitignore backend-firmware/.canary/.gitkeep
git commit -m "backend-fw: gate direct USB canary migration"
```

---

### Task 6: Write the Exact Three-Board Canary Runbook

**Files:**
- Create: `backend-firmware/tools/backend_canary_evidence.py`
- Create: `backend-firmware/tools/tests/test_backend_canary_evidence.py`
- Create: `docs/backend-firmware-canary.md`
- Modify: `backend-firmware/README.md`

**Interfaces:**
- Consumes: one Lite uplink and two Lite scanner XIAO ESP32-S3 boards, the
  verified release, a pre-provisioned operator-owned known-good Remote ID lab
  transmitter whose firmware is not changed during canary, a physical Meta
  Glasses device or explicitly configured lab advertiser, and backend HTTP
  access.
- Produces: a reproducible inventory, backup, flash, AP provisioning,
  functional, OTA/rejection, recovery, and 24-hour JSONL evidence record.

- [ ] **Step 1: Write failing evidence-recorder tests**

```python
def test_evidence_snapshot_is_canonical_redacted_and_bound_to_device(tmp_path):
    fetcher = FakeBackend(
        nodes_status=backend_status_fixture("uplink_CB77A4"),
        drone_history=history_fixture("uplink_CB77A4", kinds=["drone", "meta"]),
    )
    path = tmp_path / "canary.jsonl"
    recorder = CanaryEvidenceRecorder(
        backend_base="http://127.0.0.1:8000",
        device_id="uplink_CB77A4",
        output=path,
        fetch_json=fetcher,
        now=lambda: 1785600000.0,
    )
    recorder.snapshot("drone")
    record = json.loads(path.read_text().splitlines()[0])
    assert record["device_id"] == "uplink_CB77A4"
    assert record["phase"] == "drone"
    assert record["backend"]["scanner_profiles"] == [
        "ble_primary", "wifi_primary",
    ]
    assert not contains_secret_key(record)


def test_soak_rejects_gap_reset_schema_error_or_unfinished_queue(tmp_path):
    path = write_soak_fixture(
        tmp_path, heartbeat_gap_s=91, unexpected_resets=1,
        schema_errors=1, final_queue_depth=2,
    )
    with pytest.raises(EvidenceError):
        verify_soak(path, expected_duration_s=86400, max_heartbeat_gap_s=90)


def test_command_evidence_requires_ordered_begin_and_exact_terminal(tmp_path):
    history = command_history_fixture(
        states=["ble_inv_begin", "ble_inv_progress", "ble_inv_end"],
        terminal_state="cancelled",
    )
    record = validate_command_history(
        history, device_id="uplink_CB77A4",
        command_id="0123456789abcdef0123456789abcdef",
        terminal_state="cancelled",
    )
    assert record["terminal"] is True
    assert [event["sequence"] for event in record["events"]] == [0, 1, 2]
```

Also test wrong `device_id`, missing scanner final-health evidence, duplicate
scanner profiles, non-backend target/project/hardware, secret-key redaction,
changed/missing calibration-continuity status/session/presence/schema/digest,
out-of-order FIFO sequence, unexplained drop/quarantine increments, a duration
of 86399 seconds, and a passing 86400-second fixture. For command history,
test missing begin, skipped/duplicate sequence, unexpected terminal state,
wrong device/command ID, nonterminal timeout, and raw `value_hex` redaction.
For detection waits, test exact source/identity matching, manufacturer matching
for Meta, and readiness ordering. History `timestamp` is epoch seconds and is
converted exactly with `timestamp * 1000`; fields whose names end in `_ms` are
already milliseconds and must be `>= 1_700_000_000_000`. Reject ambiguous or
mixed units, negative/pre-epoch values, overflow, and an unrelated detection
after the cutoff. Include fixtures at `1785600000` seconds and
`1785600000000` milliseconds and require normalized time `>= after_ms`.
Add cutoff-boundary cases proving a seconds row is accepted exactly when
`timestamp_s >= ceil(after_ms / 1000)`, including `after_ms` endings 999 and
1000; this is equivalent to the checked integer multiplication but documents
the database's seconds precision explicitly.
Test exact `profile`, `role_generation`, `role_acked`, and `radio_healthy`
fields for both slots; a display `role` string never substitutes for `profile`.

Run: `cd backend-firmware && python -m pytest tools/tests/test_backend_canary_evidence.py -q`

Expected: FAIL because `backend_canary_evidence.py` is absent.

- [ ] **Step 2: Implement the deterministic evidence recorder**

Expose `CanaryEvidenceRecorder.snapshot(phase: str) -> dict` and
`verify_soak(path: Path, *, expected_duration_s: int,
max_heartbeat_gap_s: int) -> SoakResult`. The CLI is exact:

```text
backend_canary_evidence.py snapshot --backend-base URL --device-id ID --phase PHASE --output FILE
backend_canary_evidence.py wait-detection --backend-base URL --device-id ID --kind drone|meta --source SOURCE --identity-field drone_id|bssid --identity-value VALUE [--manufacturer VALUE --service-uuid-token VALUE] --after-ms EPOCH_MS --ready-file FILE --timeout-s 120 --output FILE
backend_canary_evidence.py wait-led --backend-base URL --device-id ID --expected healthy|drone|meta|drone_meta|network_degraded|uart_lost|fatal --after-ms EPOCH_MS --timeout-s 120 --output FILE
backend_canary_evidence.py wait-command --backend-base URL --device-id ID --command-id ID --terminal-state complete|failed|cancelled --timeout-s 120 --output FILE
backend_canary_evidence.py monitor --backend-base URL --device-id ID --duration-s 86400 --interval-s 30 --serial-log-dir DIR --output FILE
backend_canary_evidence.py verify-soak --input FILE --duration-s 86400 --max-heartbeat-gap-s 90
```

Use only `GET /detections/nodes/status`,
`GET /detections/drones/history?hours=1&limit=2000`, and plan 1's read-only
`GET /nodes/{device_id}/commands/{command_id}` and
`GET /detections/calibrate/continuity/{device_id}` evidence routes. Every request
has a 10-second timeout. Append canonical compact JSON plus newline through a
`0600` file descriptor opened in the ignored `0700` evidence directory; call
`fsync` after every phase transition. Persist server timestamps, device ID,
firmware/scanner identities, roles, boot IDs, rollback state, queue counters,
LED state, detection fields, reset/health counters, and response SHA-256. Drop
keys matching `password`, `secret`, `credential`, `token`, `authorization`,
`cookie`, `set-cookie`, or `api_key`, plus raw BLE value payloads, before
writing. Require exact scanner target/project/hardware/version and derive
`scanner_profiles` only from each heartbeat scanner object's `profile` key,
with exact slot/MAC/generation/ACK/radio-health binding. `monitor` exits nonzero immediately on identity drift,
unexpected reset/rollback, schema/quarantine growth, or both scanners becoming
unusable, and `verify-soak` also requires a final queue depth of zero after the
network-recovery phase.

Every `snapshot` also stores the nonsecret calibration-continuity response and
requires its exact device ID, status, session/applied values, presence flag,
model schema, and model SHA-256 to equal the pre-migration receipt from
`capture-installed`. A mismatch blocks final acceptance; the raw calibration
coefficients are never returned or recorded.

`wait-detection` performs a successful initial backend poll, then atomically
creates and fsyncs the ignored `0600` `--ready-file`; only that file authorizes
the operator to enable the RF source. A drone match requires exact device ID,
`source == wifi_beacon_rid`, and the requested `drone_id`. A Meta match requires
exact device ID, `source == ble_fingerprint`, requested `bssid`, and
`manufacturer == "Meta Glasses"`, with case-normalized UUID token `fd5f` in
the parsed service-UUID set. It ignores every other row. It normalizes the history
field named `timestamp` from epoch seconds by multiplying by 1000 exactly;
fields explicitly named `*_ms` are treated as milliseconds only. Both must
normalize to `>= after_ms`; unit ambiguity or overflow is an error.

Run: `cd backend-firmware && python -m pytest tools/tests/test_backend_canary_evidence.py -q`

Expected: PASS.

- [ ] **Step 3: Document wiring and preconditions**

The runbook states that “Lite” is only the physical no-screen assembly nickname
and is never a firmware identity. It begins with this table:

| Role | Image | UART wiring | Primary radio | LED |
|---|---|---|---|---|
| Lite scanner0 | `scanner-s3-combo-backend` | TX GPIO1 → uplink RX GPIO2; RX GPIO2 ← uplink TX GPIO1 | BLE | active-low GPIO21 yellow |
| Lite scanner1 | `scanner-s3-combo-backend` | TX GPIO1 → uplink RX GPIO4; RX GPIO2 ← uplink TX GPIO3 | Wi-Fi | active-low GPIO21 yellow |
| Lite uplink | `uplink-s3-backend` | slot0 RX2/TX1; slot1 RX4/TX3 | infrastructure Wi-Fi/HTTP | active-low GPIO21 yellow |

All UART links are 921600 baud with common ground. State explicitly that
GPIO21 is the XIAO board's single yellow/orange user LED; “purple,” “red,” and
“blue” requirements are represented by the approved temporal patterns because
the board cannot render those colors.

- [ ] **Step 4: Document the no-write inventory and backup phase**

While the original Lite uplink and scanners are still running and wired,
export the original uplink URL and the three USB device paths shown by
`pio device list`. Resolve esptool through PlatformIO's recorded package paths:

```bash
cd backend-firmware
umask 077
BACKEND_PIO_BIN="$(command -v pio)"
: "${BACKEND_PIO_BIN:?PlatformIO pio is required}"
: "${BACKEND_ORIGINAL_UPLINK_URL:?export the original Lite uplink URL, for example http://192.168.1.50}"
: "${BACKEND_BASE_URL:?export the FastAPI base URL that owns the registered node}"
: "${BACKEND_SCANNER0_PORT:?export the scanner0 USB path from pio device list}"
: "${BACKEND_SCANNER1_PORT:?export the scanner1 USB path from pio device list}"
: "${BACKEND_UPLINK_PORT:?export the uplink USB path from pio device list}"

python tools/backend_canary.py capture-installed --state .canary/canary-state.json --uplink-url "$BACKEND_ORIGINAL_UPLINK_URL" --backend-base "$BACKEND_BASE_URL" --output-dir .canary/installed
python tools/backend_canary.py inventory --role scanner0 --port "$BACKEND_SCANNER0_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py inventory --role scanner1 --port "$BACKEND_SCANNER1_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py inventory --role uplink --port "$BACKEND_UPLINK_PORT" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py backup --kind original --role scanner0 --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py backup --kind original --role scanner1 --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
# UPLINK MUST BE LAST: this successful command leaves the original uplink quiescent in ROM/reset state and does not run its app.
python tools/backend_canary.py backup --kind original --role uplink --state .canary/canary-state.json --output-dir .canary/backups/original --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind original --role scanner0 --state .canary/canary-state.json
python tools/backend_canary.py verify-backup --kind original --role scanner1 --state .canary/canary-state.json
python tools/backend_canary.py verify-backup --kind original --role uplink --state .canary/canary-state.json
python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py status --state .canary/canary-state.json
```

The gate requires the preserved operational device ID/location and exact
read-only calibration-continuity receipt,
three structured installed identities, the two original roles, updater-
admission evidence, three unique MACs, disabled secure boot/encryption, exact
decoded partition maps, three 8-MB full backups, matching NVS rereads, and
recorded SHA-256 values. The old uplink must be quiescent and UART wiring must
remain physically fixed throughout initial migration. If any item is missing, any board cannot enter USB ROM,
or either scanner cannot be read directly, stop. Do not use UART OTA or the web
flasher as a substitute.

- [ ] **Step 5: Document per-board approval and flash order**

The runbook never includes or derives a reusable approval value. For each
board, `challenge-flash` revalidates live state and writes one `0600` challenge
receipt. Show the user that receipt and stop. Only after the user approves that
exact receipt, read its one-use ID/token and run the paired flash command:

```bash
python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py challenge-flash --role scanner0 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-challenge.json
# STOP: show .canary/scanner0-challenge.json and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -r .challenge_id .canary/scanner0-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -r .token .canary/scanner0-challenge.json)"
python tools/backend_canary.py flash-initial --role scanner0 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/scanner0-challenge.json
python tools/backend_canary.py verify-provisional --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 30

python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py challenge-flash --role scanner1 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner1-challenge.json
# STOP: show .canary/scanner1-challenge.json and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -r .challenge_id .canary/scanner1-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -r .token .canary/scanner1-challenge.json)"
python tools/backend_canary.py flash-initial --role scanner1 --state .canary/canary-state.json --artifact-dir web-flasher/firmware/scanner-s3-combo-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/scanner1-challenge.json
python tools/backend_canary.py verify-provisional --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 30

python tools/backend_canary.py verify-uplink-quiesced --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py challenge-flash --role uplink --state .canary/canary-state.json --artifact-dir web-flasher/firmware/uplink-s3-backend --index release/backend-release-index.json --pio "$BACKEND_PIO_BIN" --output .canary/uplink-challenge.json
# STOP: show .canary/uplink-challenge.json and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -r .challenge_id .canary/uplink-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -r .token .canary/uplink-challenge.json)"
python tools/backend_canary.py flash-initial --role uplink --state .canary/canary-state.json --artifact-dir web-flasher/firmware/uplink-s3-backend --index release/backend-release-index.json --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN"
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
test ! -e .canary/uplink-challenge.json
python tools/backend_canary.py verify-provisional --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 30
```

Request fresh user approval before each of the three flash commands. Do not run
the next command after a failure or unexpected identity. Challenge files expire
after five minutes and are deleted after consumption. If the original uplink
application is ever observed running before its migration, stop, repeat only
the no-write quiescence command, and obtain a fresh challenge. UART wiring has
remained fixed; after the backend uplink assigns roles, run:

```bash
python tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
python tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
python tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
python tools/backend_canary.py status --state .canary/canary-state.json
```

All three must show final backend health and `rollback_clear=true` before any
functional or OTA step.

- [ ] **Step 6: Document AP provisioning and functional acceptance**

With the unchanged UART wiring and all three verified backend boards healthy:

1. Join `FriendOrFoe-Backend-XXXXXX` using default password `friendorfoe`.
2. Open `http://192.168.4.1` and enter one to four ordered Wi-Fi credentials,
   backend URL, node name, optional fixed location, and a replacement AP password.
3. Run the portal connectivity test; verify no password appears in status or logs.
4. Set and validate the evidence variables, then capture the zero-detection
   heartbeat, healthy LED state, and final scanner identities:

   ```bash
   : "${BACKEND_BASE_URL:?export the FastAPI base URL, for example http://127.0.0.1:8000}"
   : "${BACKEND_DEVICE_ID:?export the preserved uplink_XXXXXX device ID shown by canary status}"
   : "${BACKEND_TEST_DRONE_ID:?export the exact pre-provisioned lab transmitter Remote ID}"
   : "${BACKEND_RID_SOURCE_DESCRIPTION:?export its operator-owned model/serial/MAC description}"
   : "${BACKEND_META_MAC:?export the exact Meta device or lab-advertiser BLE MAC}"
   mkdir -p -m 700 .canary/evidence
   python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase baseline --output .canary/evidence/canary.jsonl
   ```

   The snapshot must contain both exact backend scanner identities, recorded
   slot/MACs, `scanner_profiles == ["ble_primary","wifi_primary"]` sourced
   from the heartbeat `profile` keys, current `role_generation`,
   `role_acked:true`, `radio_healthy:true`, valid time, and queue depth zero.
   Visually time at least two healthy cycles on all three LEDs: 80 ms on and
   2920 ms off.

5. For Remote ID, use only the pre-provisioned operator-owned lab transmitter
   identified above. Its firmware is not built, flashed, or modified in this
   canary, and no repository simulator path is used. Begin with the transmitter
   powered/advertising **off**. Capture the cutoff, start the exact waiter in
   the background, and do not enable the source until its ready file exists:

   ```bash
   BACKEND_DRONE_AFTER_MS="$(python -c 'import time; print(int(time.time()*1000))')"
   BACKEND_DRONE_READY=".canary/evidence/drone-$BACKEND_DRONE_AFTER_MS.ready"
   python tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind drone --source wifi_beacon_rid --identity-field drone_id --identity-value "$BACKEND_TEST_DRONE_ID" --after-ms "$BACKEND_DRONE_AFTER_MS" --ready-file "$BACKEND_DRONE_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
   BACKEND_DRONE_WAIT_PID=$!
   until test -s "$BACKEND_DRONE_READY"; do kill -0 "$BACKEND_DRONE_WAIT_PID" || { wait "$BACKEND_DRONE_WAIT_PID"; exit 1; }; sleep 0.1; done
   # Only now may the operator enable the identified Remote ID source.
   wait "$BACKEND_DRONE_WAIT_PID"
   unset BACKEND_DRONE_WAIT_PID
   # Operator disables the Remote ID source immediately after wait succeeds.
   python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase drone --output .canary/evidence/canary.jsonl
   ```

   Confirm at least two exact drone cycles on all three boards: 400 ms on,
   120 ms off, 120 ms on, 1360 ms off. Evidence is accepted only for the exact
   device ID, `source:wifi_beacon_rid`, and exact Remote ID. Record the source
   description; do not represent lab data as a real aircraft.

6. For Meta evidence, use a physical Meta Glasses test device whose BLE MAC is
   recorded in the canary notes. If that device is unavailable, use a dedicated
   lab advertiser configured in nRF Connect with local name `Ray-Ban Meta` and
   advertised 16-bit service UUID `0xFD5F`; record the advertiser hardware/MAC
   and label the evidence simulated. If neither source is available, this gate
   is blocked—UART or HTTP injection is not RF evidence. Begin with the source
   off, then use the same ready-before-enable ordering:

   ```bash
   BACKEND_META_AFTER_MS="$(python -c 'import time; print(int(time.time()*1000))')"
   BACKEND_META_READY=".canary/evidence/meta-$BACKEND_META_AFTER_MS.ready"
   python tools/backend_canary_evidence.py wait-detection --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --kind meta --source ble_fingerprint --identity-field bssid --identity-value "$BACKEND_META_MAC" --manufacturer "Meta Glasses" --service-uuid-token fd5f --after-ms "$BACKEND_META_AFTER_MS" --ready-file "$BACKEND_META_READY" --timeout-s 120 --output .canary/evidence/canary.jsonl &
   BACKEND_META_WAIT_PID=$!
   until test -s "$BACKEND_META_READY"; do kill -0 "$BACKEND_META_WAIT_PID" || { wait "$BACKEND_META_WAIT_PID"; exit 1; }; sleep 0.1; done
   # Only now may the operator enable the identified Meta source.
   wait "$BACKEND_META_WAIT_PID"
   unset BACKEND_META_WAIT_PID
   # Operator disables the Meta source immediately after wait succeeds.
   python tools/backend_canary_evidence.py snapshot --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --phase meta --output .canary/evidence/canary.jsonl
   ```

   Confirm at least two exact Meta cycles on all three boards: four 100-ms
   pulses separated by 100-ms gaps, then 1000 ms off. Evidence must match exact
   device ID, `source:ble_fingerprint`, BSSID,
   `manufacturer:"Meta Glasses"`, and UUID token `fd5f`.

   For combined state, turn both sources off, capture one shared `AFTER_MS`,
   start one exact drone waiter and one exact Meta waiter with separate ready
   files, and wait until **both** ready files exist before enabling either
   source. Wait for both PIDs, turn both sources off, and capture phase
   `drone-meta`. Confirm two complete concatenated cycles on all three boards:
   the drone timing followed by the Meta timing. Reusing either earlier cutoff
   or enabling one source before both waiters are ready invalidates the phase.

7. With both RF sources off, physically disconnect the Lite uplink from its
   infrastructure network for ten minutes while leaving both scanner UARTs and
   power intact. At minute five confirm AP startup and two exact network-
   degraded cycles on all three LEDs: 300 ms on/off/on, then 1800 ms off.
   Capture `outage-start` and `outage-end` snapshots; after reconnecting, poll
   snapshots until queue depth returns to zero and verify FIFO sequence order.

8. Leave scanner0 powered by USB but disconnect only its UART TX/RX conductors.
   Confirm scanner0 `uart_lost` with two 1000-ms on/off cycles, scanner1 hybrid
   failover, continued detections, and no uplink reboot. Capture
   `scanner0-disconnected`, reconnect both wires, wait for exact profile/
   generation/ACK/radio-health convergence and final health, and capture
   `scanner0-reconnected`.

   Finally, with the fully backend assembly healthy and both RF sources off,
   disconnect both scanner UART pairs for the bounded fatal test. The uplink
   must enter `fatal` and show two cycles of 120-ms on/off/on/off/on followed by
   800 ms off. Each isolated scanner cannot receive mirrored fatal state and
   therefore must independently show `uart_lost`; do not claim all three mirror
   fatal while links are absent. Capture `both-scanners-disconnected`, promptly
   reconnect both pairs, require exact profiles/final health, capture
   `fatal-recovered`, and re-observe healthy timing on all three boards.

9. Exercise real backend BLE command polling with exact requests. First cancel
   one passive command, then allow a second command to complete:

   ```bash
   BACKEND_CANCEL_JSON="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"target_mac":null,"mode":"passive_capture","timeout_ms":12000}' "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/ble-investigate")"
   BACKEND_CANCEL_ID="$(printf '%s' "$BACKEND_CANCEL_JSON" | jq -er .command_id)"
   curl -fsS -X POST "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/$BACKEND_CANCEL_ID/cancel" | jq -e --arg id "$BACKEND_CANCEL_ID" 'keys == ["command_id","mode","next_sequence","request_id","result_state","target","timeout_ms","type"] and .type == "ble_investigate_cancel" and .command_id == $id and .request_id == $id and .mode == "passive_capture" and .target == null and .timeout_ms == 12000 and .next_sequence == 0 and .result_state == null'
   python tools/backend_canary_evidence.py wait-command --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --command-id "$BACKEND_CANCEL_ID" --terminal-state cancelled --timeout-s 120 --output .canary/evidence/canary.jsonl
   BACKEND_COMPLETE_JSON="$(curl -fsS -X POST -H 'Content-Type: application/json' -d '{"target_mac":null,"mode":"passive_capture","timeout_ms":12000}' "$BACKEND_BASE_URL/nodes/$BACKEND_DEVICE_ID/commands/ble-investigate")"
   BACKEND_COMPLETE_ID="$(printf '%s' "$BACKEND_COMPLETE_JSON" | jq -er .command_id)"
   python tools/backend_canary_evidence.py wait-command --backend-base "$BACKEND_BASE_URL" --device-id "$BACKEND_DEVICE_ID" --command-id "$BACKEND_COMPLETE_ID" --terminal-state complete --timeout-s 120 --output .canary/evidence/canary.jsonl
   unset BACKEND_CANCEL_JSON BACKEND_CANCEL_ID BACKEND_COMPLETE_JSON BACKEND_COMPLETE_ID
   ```

   The evidence recorder requires ordered/idempotent begin/progress/end events,
   a terminal cancel for the first ID, terminal completion for the second, and
   absence of raw authentication or characteristic values.

- [ ] **Step 7: Document OTA, rollback, restore, and soak gates**

Only after direct-USB migration, functional acceptance, and all three final-
health checks, prove that the backend can serve the exact release bytes, then
capture the separate known-good backend recovery baseline. These operations
remain read-only with respect to application/NVS contents:

```bash
BACKEND_CATALOG_EVIDENCE=".canary/evidence/backend-catalog-preflight.json"
python tools/backend_canary.py verify-catalog --backend-base "$BACKEND_BASE_URL" --index release/backend-release-index.json --output "$BACKEND_CATALOG_EVIDENCE"

python tools/backend_canary.py backup --kind backend-baseline --role scanner0 --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind backend-baseline --role scanner0 --state .canary/canary-state.json
python tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180
python tools/backend_canary.py backup --kind backend-baseline --role scanner1 --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind backend-baseline --role scanner1 --state .canary/canary-state.json
python tools/backend_canary.py verify-final --role scanner1 --state .canary/canary-state.json --port "$BACKEND_SCANNER1_PORT" --timeout 180
python tools/backend_canary.py backup --kind backend-baseline --role uplink --state .canary/canary-state.json --output-dir .canary/backups/backend-baseline --pio "$BACKEND_PIO_BIN"
python tools/backend_canary.py verify-backup --kind backend-baseline --role uplink --state .canary/canary-state.json
python tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
python tools/backend_canary.py status --state .canary/canary-state.json
```

Each baseline command performs and cross-checks the same two independent 8-MB
reads and two independent NVS reads as the original backup, then requires the
exact backend partition table, MAC, identity, preserved device configuration,
and returned final health. A failure blocks OTA. OTA recovery may select only
these `backend-baseline` images; original images remain reserved for a separate
whole-assembly rollback.

Now extract the verified application digests and run the no-write probes:

```bash
BACKEND_RELEASE_INDEX="release/backend-release-index.json"
BACKEND_SCANNER_SHA="$(jq -er '.targets["scanner-s3-combo-backend"].parts[] | select(.name == "scanner-s3-combo-backend-firmware.bin") | .sha256' "$BACKEND_RELEASE_INDEX")"
BACKEND_UPLINK_SHA="$(jq -er '.targets["uplink-s3-backend"].parts[] | select(.name == "uplink-s3-backend-firmware.bin") | .sha256' "$BACKEND_RELEASE_INDEX")"

python tools/backend_canary.py ota-probe --component scanner0 --catalog-name scanner-s3-combo-backend --expected-sha "$BACKEND_SCANNER_SHA" --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner0-backend-probe.json --timeout 300
python tools/backend_canary.py ota-probe --component scanner1 --catalog-name scanner-s3-combo-backend --expected-sha "$BACKEND_SCANNER_SHA" --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner1-backend-probe.json --timeout 300
python tools/backend_canary.py ota-probe --component uplink --catalog-name uplink-s3-backend --expected-sha "$BACKEND_UPLINK_SHA" --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/uplink-backend-probe.json --timeout 300
```

Cross-family rejection is proven by the native/API software gate and is never
sent to canary hardware. Every backend probe must be `admit`, validate the
complete image, match the index identity/SHA/CRC32/size/
capacity, and leave write counters and boot IDs unchanged. Any other result
blocks OTA acceptance.

Exercise one scanner relay and uplink self-OTA using explicit same-version
recovery so release `0.1.0-backend` can test the real write/reboot path without
inventing an unshipped `0.1.1` artifact. Each challenge is a separate user
approval boundary:

```bash
python tools/backend_canary.py challenge-ota --component scanner0 --mode same-version-recovery --probe .canary/evidence/scanner0-backend-probe.json --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/scanner0-ota-challenge.json
# STOP: show .canary/scanner0-ota-challenge.json and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -r .challenge_id .canary/scanner0-ota-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -r .token .canary/scanner0-ota-challenge.json)"
python tools/backend_canary.py ota-apply --component scanner0 --mode same-version-recovery --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/scanner0-ota-apply.json --timeout 600
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
python tools/backend_canary.py verify-final --role scanner0 --state .canary/canary-state.json --port "$BACKEND_SCANNER0_PORT" --timeout 180

python tools/backend_canary.py challenge-ota --component uplink --mode same-version-recovery --probe .canary/evidence/uplink-backend-probe.json --catalog-evidence "$BACKEND_CATALOG_EVIDENCE" --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --pio "$BACKEND_PIO_BIN" --output .canary/uplink-ota-challenge.json
# STOP: show .canary/uplink-ota-challenge.json and obtain explicit approval.
BACKEND_CHALLENGE_ID="$(jq -r .challenge_id .canary/uplink-ota-challenge.json)"
BACKEND_CHALLENGE_TOKEN="$(jq -r .token .canary/uplink-ota-challenge.json)"
python tools/backend_canary.py ota-apply --component uplink --mode same-version-recovery --index "$BACKEND_RELEASE_INDEX" --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --challenge-id "$BACKEND_CHALLENGE_ID" --token "$BACKEND_CHALLENGE_TOKEN" --pio "$BACKEND_PIO_BIN" --output .canary/evidence/uplink-ota-apply.json --timeout 600
unset BACKEND_CHALLENGE_ID BACKEND_CHALLENGE_TOKEN
python tools/backend_canary.py verify-final --role uplink --state .canary/canary-state.json --port "$BACKEND_UPLINK_PORT" --timeout 180
python tools/backend_canary.py status --state .canary/canary-state.json
```

Successful apply evidence requires a positive image-write delta, changed boot
ID for the selected component, exact backend identity/SHA/CRC32, rollback clearance,
and full convergence. If an initial flash or OTA write fails, do not try the
web flasher or another board. Run `challenge-restore`, show its exact MAC-bound
full-backup receipt to the user, obtain separate approval, and then run
`restore-full` with that one-use challenge. The tool verifies the full 8-MB
restore before boot and requires the original installed identity afterward.

Soak acceptance is 24 continuous hours with:

- no watchdog reset, rollback, or fatal health;
- both scanner UART links current except during an induced recovery test;
- empty heartbeats no more than 90 seconds apart while network is available;
- FIFO order preserved and no unexplained drop/quarantine counters;
- no AP password, Wi-Fi password, BLE authentication payload, or secret in logs;
- backend detections containing all complete sensor fields and preserved node identity;
- protected badge/production paths still byte-identical to the branch base.

Unset `BACKEND_SCANNER_SHA`, `BACKEND_UPLINK_SHA`,
`BACKEND_CATALOG_EVIDENCE`, and `BACKEND_RELEASE_INDEX` after collecting the
OTA records.

- [ ] **Step 8: Commit evidence tooling and documentation**

```bash
git add docs/backend-firmware-canary.md backend-firmware/README.md backend-firmware/tools/backend_canary_evidence.py backend-firmware/tools/tests/test_backend_canary_evidence.py
git commit -m "docs: add backend sensor canary runbook"
```

---

### Task 7: Run the Software Release Gate, Then Pause for Hardware Approval

**Files:**
- Verify only; no new files before the hardware checkpoint.

**Interfaces:**
- Consumes: all three implementation plans.
- Produces: a clean, reviewable backend-firmware branch and a user decision at the first hardware write boundary.

- [ ] **Step 1: Run all software verification**

```bash
cd backend
pytest tests -q

cd ../backend-firmware
python -m pytest tools/tests -q
pio test -e backend-native
python tools/emit_serializer_fixture.py --check ../backend/tests/fixtures/backend_firmware_detection_batch.json
python tools/check_source_isolation.py --root .

cd scanner
pio run -e scanner-s3-combo-backend -t clean
pio run -e scanner-s3-combo-backend

cd ../uplink
pio run -e uplink-s3-backend -t clean
pio run -e uplink-s3-backend

cd ..
bash web-flasher/build.sh
BACKEND_AUDIT_BASE="$(git merge-base origin/main HEAD)"
python tools/verify_backend_release.py --index release/backend-release-index.json --flasher web-flasher --audit-protected "$BACKEND_AUDIT_BASE"

cd ..
git diff --check
```

Expected: every suite/build/audit passes and neither existing badge nor original
sensor firmware path appears in the changed-file audit.

- [ ] **Step 2: Review the branch before hardware**

Use superpowers:requesting-code-review. Resolve correctness findings, rerun the
entire gate, and show the user:

```text
branch and base commit
changed-path list
backend API test result
native firmware test result
scanner/uplink build result and sizes
release index identities and SHA-256 values
protected-path audit result
the three-board inventory/backup commands that are read-only
```

- [ ] **Step 3: Inventory and back up all three boards**

With the user's three USB ports available, run only the Task-6 inventory and
backup commands. Report the three exact role/MAC mappings and backup hashes.
If any board is inaccessible or mismatched, stop; do not flash another board.

- [ ] **Step 4: Pause and request exact scanner0 flash approval**

No hardware write is authorized until the user sees scanner0's exact MAC,
verified backup hashes, exact backend artifact identity/SHA, offsets, and NVS
preservation statement, then explicitly approves. Continue with scanner1 and
uplink only through the separate approval checkpoints in Task 6.

- [ ] **Step 5: Complete functional/soak verification and tag only on request**

After all three per-board approvals and successful flashes, perform Task-6
functional, recovery, OTA, and 24-hour soak gates. Record evidence in the PR.
Do not merge, tag, publish a release, deploy the flasher, or roll out beyond the
canary unless the user separately requests that action. If a tag is later
approved, it must be exactly `backend-fw-0.1.0-backend`; a GitHub Release stays
prohibited until the badge workflow's global release trigger is separately
redesigned and reviewed.

## Plan 3 Completion Gate

Software completion requires deterministic backend artifacts, strict embedded
identity verification, a checked real-C/FastAPI fixture, backend-only manifests,
passing CI-equivalent checks, and a protected-path audit with no changes. System
completion additionally requires all three original boards inventoried and
backed up, two scanners direct-USB migrated and verified first, uplink migrated
last with NVS preserved, functional recovery/OTA tests, and the 24-hour soak.
The plan must stop at each hardware approval boundary and whenever direct USB
access is unavailable.
