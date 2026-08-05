# Backend Sensor Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build two isolated backend-only XIAO ESP32-S3 firmware images—one uplink and one dual-role scanner—that preserve badge-era sensing while replacing all display behavior with reliable HTTP uplink, AP provisioning, and synchronized yellow-LED alerts.

**Architecture:** Vendor only portable detector/protocol code into `backend-firmware/`, then place new focused backend policies around it. Pure C modules are host-tested first; thin ESP-IDF adapters own GPIO21, UART, Wi-Fi/AP, NVS, HTTP, and OTA, and two small application roots compose them without compiling any existing badge/production tree.

**Tech Stack:** C11, ESP-IDF through PlatformIO `espressif32@6.13.0`, Seeed XIAO ESP32-S3R8, FreeRTOS, NimBLE, ESP Wi-Fi, a backend-owned bounded JSON tokenizer/writer, mbedTLS SHA-256, Unity native tests, Python isolation tooling.

## Global Constraints

- All firmware source, build inputs, tests, partitions, and tools live under `backend-firmware/`.
- Vendor provenance is pinned to commit `2cca5ad8df17ebd8d5f48dc72051441e30df1b8f`.
- No source/include/component path may resolve through `esp32/uplink/`, `esp32/scanner/`, `esp32/shared/`, `esp32/web-flasher/`, or badge scripts.
- Existing protected firmware paths must not be edited.
- Version is `FOF_VERSION_BACKEND "0.1.0-backend"`.
- Uplink identity is target `uplink-s3-backend`, project `fof_backend_uplink`, hardware `seeed_xiao_esp32s3`.
- Scanner identity is target `scanner-s3-combo-backend`, project `fof_backend_scanner`, hardware `seeed_xiao_esp32s3`.
- Every CRC32 in identity records, whole images, release indexes, journals,
  UART OTA, and evidence is unsigned IEEE/zlib CRC-32: reflected polynomial
  `0xEDB88320`, initial/final XOR `0xFFFFFFFF`. C and Python tests require the
  known vector `"123456789" == 0xCBF43926`.
- Documentation may call the physical no-screen three-board assembly the
  **Lite** hardware version. “Lite” is a human nickname only: it must never
  appear in a binary target, project, manifest key, catalog name, artifact
  filename, USB identity, OTA identity, or release channel; every such
  machine-readable identifier remains strictly `backend`.
- Operational `device_id` remains the existing NVS value or legacy `uplink_XXXXXX` MAC-suffix identity.
- Uplink/scanner UART is 921600 baud; slot 0 uses uplink RX GPIO2/TX GPIO1 and slot 1 uses uplink RX GPIO4/TX GPIO3; both scanners use TX GPIO1/RX GPIO2.
- Both scanners run the same image; slot 0 is BLE-primary, slot 1 Wi-Fi-primary, and one surviving scanner becomes hybrid failover.
- Uplink Bluetooth remains disabled in normal operation.
- All three active-low user LEDs use GPIO21 and mirror the approved yellow patterns.
- Encoded HTTP batches are at most 4096 bytes; the volatile PSRAM FIFO holds 512 complete batches and drops only the oldest whole batch when full.
- A non-empty upload batch is closed after 80 ms without a new detection; a heartbeat closes immediately. HTTP JSON response bodies are at most 4096 bytes and response headers are at most 2048 bytes. Firmware bodies are streamed and are never buffered in those JSON buffers.
- Empty heartbeat upload interval is 60 seconds; scanner time/role/LED enforcement is every 10/10/2 seconds respectively, with LED TTL 6000 ms.
- The AP is configuration/status only; it must expose no firmware mutation route and must redact all passwords.
- Initial scanner migration is direct USB only. Do not create a badge/production-identity bridge or relax OTA identity checks.
- Use test-driven development and commit after every task.

## Plan Order and Dependencies

This is plan 2 of 3. It consumes the API contract in plan 1. The release plan consumes its binaries and C serializer to create manifests, backend fixtures, and the hardware canary gate.

## File Map

```text
backend-firmware/
  README.md
  .gitignore
  VENDOR_BASE
  VENDOR_MANIFEST.json
  platformio.ini
  vendor/                       # immutable pinned donor blobs; never compiled
    shared/
    scanner_detection/
    shared_reference/
    scanner_reference/
  shared/                       # pure backend policies and adapted portable code
  scanner/
    CMakeLists.txt
    platformio.ini
    sdkconfig.defaults
    partitions_backend_scanner_8mb.csv
    main/{core,comms,detection,hw}/
  uplink/
    CMakeLists.txt
    platformio.ini
    sdkconfig.defaults
    partitions_backend_uplink_8mb.csv
    main/{core,comms,network,storage,ota,hw}/
  test/
    test_<suite>/test_main.c    # one independently discoverable Unity suite
    support/                    # shared fixtures/assertions; never a test suite
    stubs/                      # host-only ESP stubs; never a test suite
  tools/                        # vendoring, fixture, and isolation helpers
  web-flasher/                  # populated by plan 3
```

---

### Task 1: Create the Isolated Build Skeleton and Backend Identities

**Files:**
- Create: `backend-firmware/VENDOR_BASE`
- Create: `backend-firmware/VENDOR_MANIFEST.json`
- Create: `backend-firmware/.gitignore`
- Create: `backend-firmware/platformio.ini`
- Create: `backend-firmware/shared/backend_version.h`
- Create: `backend-firmware/shared/backend_identity.h`
- Create: `backend-firmware/shared/backend_identity.c`
- Create: `backend-firmware/shared/backend_embedded_identity.c`
- Create: `backend-firmware/test/test_backend_identity/test_main.c`
- Create: `backend-firmware/test/support/backend_test_main.h`
- Create: `backend-firmware/tools/vendor_snapshot.py`
- Create: `backend-firmware/tools/check_source_isolation.py`
- Create: `backend-firmware/tools/tests/test_source_isolation.py`
- Create: `backend-firmware/tools/tests/test_embedded_identity_object.py`

**Interfaces:**
- Consumes: immutable source commit `2cca5ad8df17ebd8d5f48dc72051441e30df1b8f`.
- Produces: `backend_identity_for_image`, `backend_identity_matches`, and native environment `backend-native`.

- [ ] **Step 1: Write failing identity tests**

```c
void test_backend_identities_are_exact_and_distinct(void)
{
    const backend_firmware_identity_t *uplink =
        backend_identity_for_image(BACKEND_IMAGE_UPLINK);
    const backend_firmware_identity_t *scanner =
        backend_identity_for_image(BACKEND_IMAGE_SCANNER);

    TEST_ASSERT_EQUAL_STRING("uplink-s3-backend", uplink->target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_uplink", uplink->project);
    TEST_ASSERT_EQUAL_STRING("scanner-s3-combo-backend", scanner->target);
    TEST_ASSERT_EQUAL_STRING("fof_backend_scanner", scanner->project);
    TEST_ASSERT_EQUAL_STRING("seeed_xiao_esp32s3", uplink->hardware);
    TEST_ASSERT_EQUAL_STRING("0.1.0-backend", scanner->version);
    TEST_ASSERT_FALSE(backend_identity_matches(
        scanner, "scanner-s3-combo-fof_badge", "fof_badge_scanner",
        "seeed_xiao_esp32s3"));

    backend_embedded_identity_record_t record = {0};
    TEST_ASSERT_TRUE(backend_identity_record_build(
        BACKEND_IMAGE_SCANNER, &record));
    TEST_ASSERT_EQUAL_HEX32(FOF_BACKEND_IDENTITY_MAGIC, record.magic);
    TEST_ASSERT_EQUAL_UINT16(1, record.schema);
    TEST_ASSERT_EQUAL_STRING("fof_backend_scanner", record.project);
    TEST_ASSERT_TRUE(backend_identity_record_validate(&record));
}
```

- [ ] **Step 2: Create the native harness and verify the test fails**

Use this root environment:

```ini
[platformio]
src_dir = .
test_dir = test

[env:backend-native]
platform = native
test_framework = unity
test_build_src = yes
build_flags =
    -Ishared
    -Itest/stubs
    -DUNIT_TESTING
    -DUNITY_INCLUDE_DOUBLE
    -fsanitize=address
    -fno-omit-frame-pointer
build_src_filter =
    +<shared/backend_identity.c>
```

PlatformIO discovers native suites by directory, not by a flat C filename.
Every C suite in this plan therefore lives at
`test/test_<suite>/test_main.c`. Each `test_main.c` owns exactly one
`main()` which calls `UNITY_BEGIN()`, `RUN_TEST(...)` for every test in that
directory, and `UNITY_END()`. `test/support/backend_test_main.h` contains only
small shared macros; there is no global `test_runner.c`. Directories
`test/support` and `test/stubs` intentionally do not begin with `test_`.
Consequently `pio test -f test_backend_identity` selects and runs the named
suite instead of silently selecting PlatformIO's catch-all `*` suite.

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_identity`

Expected: FAIL because the identity headers and implementation are missing.

- [ ] **Step 3: Implement the backend-only identity contract**

```c
#define FOF_VERSION_BACKEND "0.1.0-backend"
#define FOF_BACKEND_UPLINK_TARGET "uplink-s3-backend"
#define FOF_BACKEND_UPLINK_PROJECT "fof_backend_uplink"
#define FOF_BACKEND_SCANNER_TARGET "scanner-s3-combo-backend"
#define FOF_BACKEND_SCANNER_PROJECT "fof_backend_scanner"
#define FOF_BACKEND_HARDWARE "seeed_xiao_esp32s3"

typedef enum {
    BACKEND_IMAGE_UPLINK = 0,
    BACKEND_IMAGE_SCANNER = 1,
} backend_image_kind_t;

typedef struct {
    const char *target;
    const char *project;
    const char *hardware;
    const char *version;
} backend_firmware_identity_t;

#define FOF_BACKEND_IDENTITY_MAGIC UINT32_C(0x42464F46)
#define FOF_BACKEND_IDENTITY_SCHEMA UINT16_C(1)

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t image_kind;
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    uint32_t crc32;
} backend_embedded_identity_record_t;

const backend_firmware_identity_t *
backend_identity_for_image(backend_image_kind_t kind);

bool backend_identity_matches(
    const backend_firmware_identity_t *expected,
    const char *target,
    const char *project,
    const char *hardware);
bool backend_identity_record_build(
    backend_image_kind_t kind,
    backend_embedded_identity_record_t *out);
bool backend_identity_record_validate(
    const backend_embedded_identity_record_t *record);
```

Use two file-static immutable structs; invalid kinds return `NULL`; matching
requires exact non-null strings for all three dimensions. Assert at compile
time that the packed-layout-independent record is 164 bytes and every field
offset is the declared natural layout; calculate CRC32 over bytes from `magic`
through the byte before `crc32`, with all string tails zero-filled. Device
builds define exactly one of `FOF_BACKEND_UPLINK` or `FOF_BACKEND_SCANNER`.
`backend_embedded_identity.c` uses that define to initialize one `const
backend_embedded_identity_record_t fof_backend_embedded_identity` in a `used`,
four-byte-aligned `.fof_backend_identity` section. Its exact CRC constants for
version `0.1.0-backend` are `0xF08BCDE4` for uplink and `0x9DD382FF` for
scanner; absence/both image defines is a compile error. This is static image
data—runtime initialization is forbidden.

`test_embedded_identity_object.py` invokes the host C compiler twice with the
two image defines, dumps only `.fof_backend_identity` from each object using
the discovered `objcopy`, and independently requires exactly 164 bytes,
zero-filled string tails, the expected kind/identity, and recomputed CRC32.
The actual ESP builds list `backend_embedded_identity.c` exactly once; the
scanner/uplink boot code references and validates the exported object before
printing identity, preventing linker garbage collection. Link-map tests require
exactly one `.fof_backend_identity` input, and the post-build identity verifier
repeats the same assertion on each final ELF and `firmware.bin`.
Native tests also exercise runtime record building explicitly by kind.
Both suites first assert the shared IEEE CRC32 known vector
`"123456789" == 0xCBF43926`.
Release/backend/OTA validators require exactly one valid record and require it
to agree with the ESP app descriptor; arbitrary string markers are not
identity evidence.

- [ ] **Step 4: Pin provenance and create the explicit vendor manifest**

`VENDOR_BASE` contains exactly the 40-character commit plus newline. The JSON
manifest maps only these portable files from that commit to local destinations:

```json
{
  "base": "2cca5ad8df17ebd8d5f48dc72051441e30df1b8f",
  "shared": [
    "constants.h", "detection_types.h", "detection_policy.c", "detection_policy.h",
    "badge_ble_rssi_policy.h",
    "privacy_rf_signatures.c", "privacy_rf_signatures.h", "psram_alloc.c", "psram_alloc.h",
    "rssi_distance.h", "time_sync_policy.c", "time_sync_policy.h",
    "scanner_uart_line_framer.c", "scanner_uart_line_framer.h",
    "ble_investigation_types.h", "ble_investigation_protocol.c", "ble_investigation_protocol.h",
    "firmware_version_order.c", "firmware_version_order.h",
    "firmware_image_contract.c", "firmware_image_contract.h",
    "firmware_json_schema.c", "firmware_json_schema.h",
    "firmware_operation_token.c", "firmware_operation_token.h"
  ],
  "scanner_detection": [
    "bayesian_fusion.c", "bayesian_fusion.h", "ble_fingerprint.c", "ble_fingerprint.h",
    "ble_investigator.c", "ble_investigator.h", "ble_ja3.c", "ble_ja3.h",
    "ble_remote_id.c", "ble_remote_id.h", "ble_threat_detector.c", "ble_threat_detector.h",
    "dji_drone_id_parser.c", "dji_drone_id_parser.h", "french_dri_parser.c", "french_dri_parser.h",
    "glasses_detector.c", "glasses_detector.h", "open_drone_id_parser.c", "open_drone_id_parser.h",
    "wifi_beacon_rid_parser.c", "wifi_beacon_rid_parser.h",
    "wifi_oui_database.c", "wifi_oui_database.h", "wifi_scanner.c", "wifi_scanner.h",
    "wifi_ssid_patterns.c", "wifi_ssid_patterns.h"
  ],
  "shared_reference": [
    "badge_threat_policy.c", "badge_threat_policy.h", "uart_protocol.h"
  ],
  "scanner_reference": [
    "comms/uart_ota.c", "comms/uart_ota.h",
    "comms/uart_tx.c", "comms/uart_tx.h",
    "core/calibration_mode.c", "core/calibration_mode.h",
    "core/scanner_rollback.c", "core/scanner_rollback.h",
    "core/task_priorities.h"
  ]
}
```

`vendor_snapshot.py` reads the exact `base` value from the manifest, requires
`git cat-file -e <base>^{commit}` to succeed, reads blobs with
`git show <base>:<path>`, refuses symlinks/non-files, writes only under
`backend-firmware/vendor/{shared,scanner_detection,shared_reference,scanner_reference}`,
and records
SHA-256 values back into a generated `VENDORED_SHA256.json`. It is a manual
provenance tool, not a build step. Files under `vendor/` are immutable donor
evidence and never appear in a CMake source list, PlatformIO source filter, or
compile database.

Manifest keys have fixed source roots: `shared` and `shared_reference` resolve
below `esp32/shared/`; `scanner_detection` resolves below
`esp32/scanner/main/detection/`; `scanner_reference` resolves below
`esp32/scanner/main/`. An entry containing `..`, an absolute path, a symlink,
or a source outside its declared root is rejected. The reference groups pin
the later threat, UART OTA, rollback, UART emission, calibration-removal, and
task-priority adaptations; they are copied as evidence but never compiled.

- [ ] **Step 5: Implement and test source isolation**

`check_source_isolation.py` walks CMake, PlatformIO, C/C++ headers/sources, and
generated compile databases beneath `backend-firmware`. It canonicalizes
every referenced path before classification. Relative paths may not escape the
subtree. Absolute paths are allowed only when they resolve under the backend
firmware subtree or under explicitly discovered PlatformIO/compiler/ESP-IDF
package roots; an absolute repository path elsewhere, any protected firmware
path, a repository symlink, or a compiled `vendor/` path fails. It excludes
`tools/vendor_snapshot.py`,
`VENDOR_MANIFEST.json`, `vendor/`, and provenance JSON because those are
read-only source records, not build inputs. A separate compile-database check
fails if any `vendor/` file is compiled.

```python
def test_rejects_protected_include(tmp_path):
    root = tmp_path / "backend-firmware"
    root.mkdir(parents=True)
    (root / "CMakeLists.txt").write_text(
        'target_sources(app PRIVATE "../esp32/scanner/main/main.c")\n',
        encoding="utf-8",
    )
    assert audit_tree(root) == [
        "CMakeLists.txt: protected or escaping build path ../esp32/scanner/main/main.c"
    ]

def test_compile_database_accepts_canonical_absolute_local_path(tmp_path):
    root = make_backend_tree(tmp_path)
    local = (root / "shared/backend_identity.c").resolve()
    write_compile_database(root, file=local, include=root / "shared")
    assert audit_tree(root, allowed_tool_roots=[]) == []

def test_compile_database_rejects_absolute_protected_and_vendor_paths(tmp_path):
    root = make_backend_tree(tmp_path)
    repo = root.parent
    write_compile_database(root, file=repo / "esp32/scanner/main/main.c")
    assert "protected" in "\n".join(audit_tree(root, allowed_tool_roots=[]))
    write_compile_database(root, file=root / "vendor/shared/constants.c")
    assert "vendor source compiled" in "\n".join(
        audit_tree(root, allowed_tool_roots=[]))
```

- [ ] **Step 6: Run tests and commit**

Run:

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_identity
python -m pytest tools/tests/test_embedded_identity_object.py -q
python -m pytest tools/tests/test_source_isolation.py -q
python tools/check_source_isolation.py --root .
```

Expected: PASS with no isolation findings.

```bash
git add backend-firmware
git commit -m "backend-fw: isolate backend firmware identities"
```

---

### Task 2: Vendor the Portable Detector and Parser Baseline

**Files:**
- Create: immutable files listed in `VENDOR_MANIFEST.json` under `backend-firmware/vendor/`
- Create: adapted portable files under `backend-firmware/shared/`
- Create: adapted scanner files under `backend-firmware/scanner/main/detection/`
- Create: `backend-firmware/scanner/main/core/backend_task_priorities.h`
- Create: `backend-firmware/scanner/main/core/backend_detection_sink.h`
- Create: `backend-firmware/scanner/main/core/backend_detection_sink.c`
- Create: `backend-firmware/scanner/main/core/backend_investigation_sink.h`
- Create: `backend-firmware/scanner/main/core/backend_investigation_sink.c`
- Create: `backend-firmware/scanner/main/detection/backend_glasses_classifier.h`
- Create: `backend-firmware/scanner/main/detection/backend_glasses_classifier.c`
- Create: `backend-firmware/scanner/main/detection/backend_glasses_settings.h`
- Create: `backend-firmware/scanner/main/detection/backend_glasses_settings.c`
- Create: `backend-firmware/shared/backend_uart_protocol.h`
- Create: `backend-firmware/test/stubs/esp_log.h`
- Create: `backend-firmware/test/stubs/esp_timer.h`
- Create: `backend-firmware/test/support/backend_test_clock.h`
- Create: `backend-firmware/test/support/backend_test_clock.c`
- Create: `backend-firmware/BACKEND_PORT_NOTES.md`
- Create: `backend-firmware/test/test_ported_detectors/test_main.c`
- Modify: `backend-firmware/platformio.ini`
- Modify: `backend-firmware/README.md`

**Interfaces:**
- Consumes: exact source blobs at `VENDOR_BASE`.
- Produces: local detection types, BLE/Wi-Fi parsers, privacy signatures,
  Bayesian fusion, time primitives, OTA primitives, and stable backend sink
  boundaries that later UART/command tasks implement without forward includes.

- [ ] **Step 1: Add a failing parity smoke test before copying source**

```c
void test_ported_open_drone_id_and_meta_detection_baseline(void)
{
    odid_state_t state;
    odid_state_init(&state, "AA:BB:CC:DD:EE:FF", 1000);
    uint8_t rid_payload[25] = {0};
    rid_payload[0] = 0x00;
    rid_payload[1] = (1U << 4) | 2U;
    memcpy(&rid_payload[2], "BACKEND-RID-1", 13);
    odid_parse_message(rid_payload, sizeof(rid_payload), &state, 0);
    TEST_ASSERT_TRUE(state.has_basic_id);
    TEST_ASSERT_EQUAL_STRING("BACKEND-RID-1", state.drone_id);

    const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    glasses_detection_t glasses = {0};
    TEST_ASSERT_TRUE(backend_glasses_classify_advertisement(
        mac, "Ray-Ban Meta", 12, NULL, 0, NULL, 0, 0, -47, 1000, &glasses));
    TEST_ASSERT_EQUAL_STRING("Meta", glasses.manufacturer);
    TEST_ASSERT_EQUAL_STRING("Smart Glasses", glasses.device_type);
    TEST_ASSERT_TRUE(glasses.has_camera);
    TEST_ASSERT_TRUE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_BLE_FINGERPRINT));
    TEST_ASSERT_FALSE(fof_policy_scan_profile_allows_source(
        "ble_primary", DETECTION_SRC_WIFI_AP_INVENTORY));
}
```

- [ ] **Step 2: Run and observe missing vendored headers**

Run: `cd backend-firmware && pio test -e backend-native -f test_ported_detectors`

Expected: FAIL at compilation because the detector baseline is not present.

- [ ] **Step 3: Materialize the pinned snapshot**

Run:

```bash
cd backend-firmware
python tools/vendor_snapshot.py --repo-root .. --manifest VENDOR_MANIFEST.json
```

Expected: all declared blobs are copied into immutable `vendor/` directories
and every digest appears in `VENDORED_SHA256.json`. Confirm the declared set
contains none of
`badge_con_observer`, `badge_display`, `badge_theme`, `badge_button`,
`badge_power`, `badge_easter`, OLED/LCD drivers, WS2812 code, or game assets.

- [ ] **Step 4: Materialize and adapt the backend-owned build copies**

Copy pure parsers and policies from `vendor/` into `shared/` and
`scanner/main/detection/`. For ESP runtime donors `wifi_scanner.c`,
`ble_remote_id.c`, `ble_investigator.c`, and `ble_ja3.c`, make local build
copies and perform these explicit edits only in the local copies:

- replace `core/task_priorities.h` with `core/backend_task_priorities.h`;
- replace `comms/uart_tx.h` calls with the Task-2
  `backend_detection_sink_emit` callback;
- replace donor `uart_protocol.h` with Task-2 `backend_uart_protocol.h`,
  which copies only the BLE-investigation property bits, OTA framing constants,
  compact detection key strings, and backend command names actually consumed
  by local code; it contains no badge include, badge message, pin selection, or
  variant branch;
- remove `calibration_mode.h`, `badge_easter_egg.h`,
  `badge_con_observer.h`, their conditionals, and their call sites;
- retain passive BLE Remote ID, fingerprints, JA3 structure, company/service/
  name evidence, Meta Glasses, trackers, venue/privacy devices, and both
  pairing-spam and serial-skimmer behavioral results;
- retain Wi-Fi Remote ID, DJI/French DRI, SSID/OUI, AP inventory, probes,
  association, anomaly, and lock-on evidence;
- make both behavioral BLE threat kinds enter `backend_detection_sink` even
  though the donor badge build's demo constant suppressed serial-skimmer rows;
- replace investigator UART result calls with Task-2
  `backend_investigation_sink_emit`, without importing badge
  display/investigation state; Task 11 registers the uplink-facing bounded
  result callback against this already-defined interface;
- keep NimBLE/ESP-Wi-Fi dependencies in device builds only.

The stable Task-2 sink contracts are complete before those local runtime files
are compiled:

```c
typedef bool (*backend_detection_consumer_fn)(
    void *context,
    const drone_detection_t *detection,
    int64_t observed_monotonic_ms);

void backend_detection_sink_register(
    backend_detection_consumer_fn consumer, void *context);
bool backend_detection_sink_emit(
    const drone_detection_t *detection, int64_t observed_monotonic_ms);

typedef bool (*backend_investigation_consumer_fn)(
    void *context, const ble_investigation_chunk_t *chunk);

void backend_investigation_sink_register(
    backend_investigation_consumer_fn consumer, void *context);
bool backend_investigation_sink_emit(
    const ble_investigation_chunk_t *chunk);
```

Both sinks fail closed when no consumer is registered, copy input before
cross-task enqueue, and never retain caller pointers. The scanner application
registers its UART encoder in Task 13; native tests register capture callbacks.

Do not compile the donor `glasses_detector.c` directly on either host or
device. Move its immutable signature tables and matching logic into pure
`backend_glasses_classifier.c` with this interface:

```c
bool backend_glasses_classify_advertisement(
    const uint8_t mac[6],
    const char *name, size_t name_len,
    const uint8_t *manufacturer_data, size_t manufacturer_data_len,
    const uint16_t *service_uuids, size_t service_uuid_count,
    uint16_t appearance, int8_t rssi, int64_t observed_ms,
    glasses_detection_t *out);
```

It has no NVS, ESP timer, or ESP logging include and uses `observed_ms` for
first/last timestamps. Device-only `backend_glasses_settings.c` owns the NVS
enabled flag. The BLE runtime checks settings and calls the pure classifier.
Native sources that otherwise need only donor logging/timing include local
no-op `test/stubs/esp_log.h` and `test/stubs/esp_timer.h`; the timer stub calls
the explicitly settable `backend_test_clock_now_us()`. No NVS, FreeRTOS,
NimBLE, ESP Wi-Fi, ESP-IDF JSON, or undeclared host library is linked into
`backend-native`.

`BACKEND_PORT_NOTES.md` records each donor path, pinned SHA-256, local build
path, removed dependency, replacement backend interface, and parity tests.
Never edit `vendor/` after materialization.

- [ ] **Step 5: Add only local pure source paths to the native build**

Extend `build_flags` with `-Iscanner/main/detection`,
`-Iscanner/main/core`, and `-Itest/support` and append every portable
`.c` file required by the ported tests to `build_src_filter`. Do not compile
the ESP runtime donors, `backend_glasses_settings.c`, or anything under
`vendor/` in the native environment. Compile the pure glasses classifier,
sink capture implementations, and `test/support/backend_test_clock.c`.
Do not use a wildcard that could compile a later UI/game file.

- [ ] **Step 6: Port the exact existing native vectors**

Copy test functions and their local fixture helpers—not includes or production
sources—from these pinned files at `VENDOR_BASE`:

```text
esp32/test/test_open_drone_id_parser.c
esp32/test/test_dji_drone_id_parser.c
esp32/test/test_ssid_patterns.c
esp32/test/test_ble_threat_detector.c
esp32/test/test_bayesian_fusion.c
esp32/test/test_detection_policy.c
```

From `test_detection_policy.c`, copy only the BLE fingerprint, Luxottica/Meta,
Wi-Fi OUI, and Wi-Fi policy cases whose callees are in `VENDOR_MANIFEST.json`;
do not copy badge threat/display cases. Add one explicit French DRI fixture and
one Wi-Fi Beacon Remote ID fixture using the public parser functions in
`french_dri_parser.h` and `wifi_beacon_rid_parser.h`, asserting ID, position,
altitude, source, and RSSI. Change includes only to local header names and keep
every original expected decoded field and confidence value unchanged.

Add a backend feature-matrix test that feeds representative BLE fingerprint,
Meta, tracker, venue/privacy, pairing-spam, serial-skimmer, Wi-Fi AP, probe,
association, anomaly, and lock-on observations through the adapted sink and
asserts each produces a complete `drone_detection_t`. The serial-skimmer case
must emit under `FOF_BACKEND_FIRMWARE=1`.

- [ ] **Step 7: Run detector tests and isolation audit**

Run:

```bash
cd backend-firmware
pio test -e backend-native -f test_ported_detectors
python tools/check_source_isolation.py --root .
python tools/vendor_snapshot.py --repo-root .. --manifest VENDOR_MANIFEST.json --check
```

Expected: PASS; the build has no protected include/source path.

- [ ] **Step 8: Commit**

```bash
git add backend-firmware/vendor backend-firmware/shared backend-firmware/scanner/main/detection backend-firmware/scanner/main/core backend-firmware/test backend-firmware/platformio.ini backend-firmware/README.md backend-firmware/BACKEND_PORT_NOTES.md backend-firmware/VENDORED_SHA256.json
git commit -m "backend-fw: vendor portable sensing baseline"
```

---

### Task 3: Encode and Decode the Complete Scanner UART Contract

**Files:**
- Create: `backend-firmware/shared/backend_json_writer.h`
- Create: `backend-firmware/shared/backend_json_writer.c`
- Create: `backend-firmware/shared/backend_json_reader.h`
- Create: `backend-firmware/shared/backend_json_reader.c`
- Create: `backend-firmware/shared/backend_detection_codec.h`
- Create: `backend-firmware/shared/backend_detection_codec.c`
- Create: `backend-firmware/test/support/backend_detection_assert.h`
- Create: `backend-firmware/test/support/backend_detection_assert.c`
- Create: `backend-firmware/test/test_backend_detection_codec/test_main.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: local `drone_detection_t` and scanner slot metadata.
- Produces: fail-closed newline JSON encoding/decoding with maximum 4095-byte UART lines.

- [ ] **Step 1: Write failing full-record and boundary tests**

```c
void test_detection_codec_round_trips_full_record(void)
{
    drone_detection_t input = fixture_full_detection();
    backend_scanner_stamp_t stamp = {
        .sequence = 44,
        .time_valid = true,
        .observed_epoch_ms = 1785600000123LL,
    };
    char line[4096] = {0};
    size_t length = backend_detection_uart_encode(
        &input, &stamp, line, sizeof(line));
    TEST_ASSERT_GREATER_THAN(0, length);
    TEST_ASSERT_LESS_THAN(4096, length);

    drone_detection_t output = {0};
    backend_scanner_stamp_t decoded = {0};
    TEST_ASSERT_EQUAL(BACKEND_DECODE_OK, backend_detection_uart_decode(
        line, length, BACKEND_SCANNER_SLOT_WIFI, &output, &decoded));
    drone_detection_t expected = input;
    expected.scanner_slot = BACKEND_SCANNER_SLOT_WIFI;
    expected.scanner_slots_seen = 1U << BACKEND_SCANNER_SLOT_WIFI;
    backend_assert_detection_equal(&expected, &output);
    TEST_ASSERT_EQUAL_INT64(1785600000123LL, decoded.observed_epoch_ms);
}

void test_detection_codec_never_returns_partial_json(void)
{
    drone_detection_t input = fixture_full_detection();
    char too_small[80];
    memset(too_small, 'X', sizeof(too_small));
    TEST_ASSERT_EQUAL_UINT(0, backend_detection_uart_encode(
        &input, NULL, too_small, sizeof(too_small)));
    TEST_ASSERT_EQUAL_CHAR('\0', too_small[0]);
}
```

- [ ] **Step 2: Run and observe missing codec symbols**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_detection_codec`

Expected: FAIL at compilation.

- [ ] **Step 3: Implement a bounded JSON writer**

```c
typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
    bool failed;
} backend_json_writer_t;

void backend_json_writer_init(
    backend_json_writer_t *writer, char *buffer, size_t capacity);
bool backend_json_append(backend_json_writer_t *writer, const char *text);
bool backend_json_append_format(
    backend_json_writer_t *writer, const char *format, ...);
bool backend_json_append_escaped(
    backend_json_writer_t *writer, const char *value);
size_t backend_json_writer_finish(backend_json_writer_t *writer);
```

All appends reserve one byte for NUL. The first failure sets `buffer[0]='\0'`
and makes later calls no-ops. `finish` returns zero unless the complete object
fits.

Implement a backend-owned, allocation-free JSON tokenizer instead of cJSON:

```c
#define BACKEND_JSON_MAX_TOKENS 256
#define BACKEND_JSON_MAX_DEPTH 4

typedef enum {
    BACKEND_JSON_OK = 0,
    BACKEND_JSON_MALFORMED,
    BACKEND_JSON_TOO_MANY_TOKENS,
    BACKEND_JSON_TOO_DEEP,
    BACKEND_JSON_DUPLICATE_KEY,
    BACKEND_JSON_RANGE,
} backend_json_result_t;

typedef enum {
    BACKEND_JSON_OBJECT,
    BACKEND_JSON_ARRAY,
    BACKEND_JSON_STRING,
    BACKEND_JSON_NUMBER,
    BACKEND_JSON_BOOL,
    BACKEND_JSON_NULL,
} backend_json_token_kind_t;

typedef struct {
    backend_json_token_kind_t kind;
    int16_t parent;
    uint16_t start;
    uint16_t end;
    uint16_t child_count;
} backend_json_token_t;

backend_json_result_t backend_json_parse(
    const char *json, size_t length,
    backend_json_token_t *tokens, size_t capacity, size_t *out_count);
bool backend_json_object_find(
    const char *json, const backend_json_token_t *tokens, size_t token_count,
    size_t object_index, const char *key, size_t *out_value_index);
bool backend_json_copy_string(
    const char *json, const backend_json_token_t *token,
    char *output, size_t capacity);
bool backend_json_get_bool(
    const char *json, const backend_json_token_t *token, bool *out);
bool backend_json_get_i64(
    const char *json, const backend_json_token_t *token, int64_t *out);
bool backend_json_get_u64(
    const char *json, const backend_json_token_t *token, uint64_t *out);
bool backend_json_get_double(
    const char *json, const backend_json_token_t *token, double *out);
```

Parsing consumes an explicit length, never calls `strlen` on transport input,
rejects embedded NUL, duplicate object keys, malformed UTF-8/escapes, excessive
depth/tokens, integer overflow, NaN/Infinity, and trailing non-whitespace.
String copy decodes JSON escapes and fails rather than truncating. These two
local JSON modules are used by UART, config portal, ACK, command, status, time,
and OTA code on both host and ESP-IDF builds; no cJSON header, component, or
host package is required.

- [ ] **Step 4: Implement the exact codec interface**

```c
typedef enum {
    BACKEND_SCANNER_SLOT_BLE = 0,
    BACKEND_SCANNER_SLOT_WIFI = 1,
} backend_scanner_slot_t;

typedef struct {
    uint32_t sequence;
    bool time_valid;
    int64_t observed_epoch_ms;
} backend_scanner_stamp_t;

typedef struct {
    drone_detection_t detection;
    bool timestamp_valid;
    int64_t timestamp_epoch_ms;
} backend_detection_observation_t;

typedef enum {
    BACKEND_DECODE_OK,
    BACKEND_DECODE_MALFORMED,
    BACKEND_DECODE_SCHEMA_MISMATCH,
    BACKEND_DECODE_TOO_LARGE,
} backend_detection_decode_result_t;

size_t backend_detection_uart_encode(
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *stamp,
    char *output,
    size_t capacity);

backend_detection_decode_result_t backend_detection_uart_decode(
    const char *json,
    size_t length,
    backend_scanner_slot_t slot,
    drone_detection_t *out_detection,
    backend_scanner_stamp_t *out_stamp);
```

Encode every field in local `detection_types.h`, using existing compact UART
key names. Decoder validation rejects overlong strings, invalid enum/range
values, uptime masquerading as epoch time, missing required identity/source,
and lines longer than 4095 bytes. It annotates `scanner_slot` and
`scanner_slots_seen = 1U << slot` locally rather than trusting wire values.
The decoder returns the scanner stamp separately; the uplink coordinator—not
the wire decoder—constructs `backend_detection_observation_t` using the exact
Task-10 scanner-time/fallback policy. This keeps the detector's
`first_seen_ms` and `last_updated_ms` evidence unchanged while giving the HTTP
serializer one explicit per-item observation timestamp.

Task 3 also freezes the scanner-struct-to-HTTP contract so the two codecs
cannot accidentally agree on the same wrong or missing key. Task 8 must use
this table verbatim:

| `drone_detection_t` member(s) | HTTP detection key | Exact encoding and presence rule |
|---|---|---|
| `drone_id` | `drone_id` | JSON string; required and nonempty. |
| `source` | `source` | Exact strings: `0=ble_rid`, `1=wifi_ssid`, `2=wifi_dji_ie`, `3=wifi_beacon_rid`, `4=wifi_oui`, `5=wifi_probe_request`, `6=ble_fingerprint`, `7=wifi_assoc`, `8=wifi_ap_inventory`; every other value is invalid, never `unknown`. |
| `confidence`, `fused_confidence` | same names | Finite JSON numbers with no rescaling; `confidence` is required and `fused_confidence` is emitted when valid detector fusion evidence is present. |
| `latitude`, `longitude`, `altitude_m`, `heading_deg`, `speed_mps`, `vertical_speed_mps`, `rssi`, `estimated_distance_m` | same names | Exact numeric value; latitude/longitude are emitted as a pair. Zero is retained when the applicable source explicitly supplies it rather than being used as an omission shortcut. |
| `manufacturer`, `model`, `operator_id`, `self_id_text`, `ssid`, `bssid`, `ble_name`, `class_reason` | same names | JSON string with normal escaping; omit only an empty C string. |
| `operator_lat`, `operator_lon` | same names | Exact numeric pair; never substitute device location. |
| `ua_type`, `id_type`, `self_id_desc_type`, `height_agl_m`, `geodetic_alt_m`, `h_accuracy_m`, `v_accuracy_m`, `area_count`, `area_radius`, `area_ceiling`, `area_floor`, `classification_type` | same names | Exact numeric value with the Task-3 range checks; preserve zero for applicable Remote-ID evidence. |
| `freq_mhz` | `freq_mhz` | Exact integer MHz when positive; it is never written into `channel`. |
| `freq_mhz` | `channel` | Derived independently: 2412..2472 in 5-MHz steps maps to 1..13, 2484 maps to 14, and a 5005..5895 value divisible by 5 maps to `(freq_mhz - 5000) / 5`; omit for every other value. |
| `channel_width_mhz` | `channel_width_mhz` | Exact positive integer MHz; omit zero. |
| `wifi_auth_mode` | `auth_m` | Exact integer 0..10 for a Wi-Fi source, including meaningful value 0; omit only sentinel `0xFF`. |
| `wifi_generation` | `wifi_generation` | Exact integer for every Wi-Fi source, including legacy value 0; omit for non-Wi-Fi sources. |
| `probed_ssids` plus `ssid` | `probed_ssids` | For `wifi_probe_request`, split the bounded legacy CSV on ASCII comma, discard empty tokens, preserve token bytes/order, and emit a JSON string array. If the CSV is empty, use one nonempty `ssid` element. Reject an overlong token instead of truncating it. Omit for other sources. |
| `probe_ie_hash` | `ie_hash` | Exactly eight lowercase hex digits; omit only zero. |
| `ble_company_id`, `ble_apple_type`, `ble_ad_type_count`, `ble_payload_len` | same names | Exact unsigned integer; omit zero because it is the detector's absent sentinel. |
| `ble_addr_type` | `ble_addr_type` | Exact integer 0..3 for a BLE source, including public-address value 0; omit for non-BLE sources. |
| `ble_ja3_hash` | `ble_ja3` | Exactly eight lowercase hex digits; omit only zero. |
| `ble_apple_auth[3]` | `ble_apple_auth` | Exactly six lowercase hex digits in array order; omit only when all three bytes are zero. |
| `ble_apple_activity` | `ble_activity` | Exact integer including 0 whenever Apple evidence is present (`ble_company_id == 0x004c`, nonzero `ble_apple_type`, or nonzero auth tag); otherwise omit. |
| `ble_apple_flags` | `ble_apple_flags` | Always emit the exact integer, including 0, so zero and absent cannot collapse. |
| `ble_raw_mfr[0..ble_raw_mfr_len)` | `ble_raw_mfr` | Two lowercase hex digits per byte in array order; require length 0..20 and omit length 0. |
| `ble_adv_interval_us` | `ble_adv_interval` | Floating-point milliseconds computed as `ble_adv_interval_us / 1000.0`; omit values `<= 0` and preserve fractions such as `125500 -> 125.5`. |
| `ble_svc_uuids_raw`, or the two UUID arrays/counts | `ble_svc_uuids` | Prefer a syntactically validated nonempty raw comma-separated string. Otherwise join up to four 16-bit UUIDs as four lowercase hex digits, then up to two 128-bit UUIDs as lowercase canonical UUIDs after reversing the stored little-endian bytes; preserve order and separate tokens with one comma. Omit when both counts are zero. |
| `first_seen_ms`, `last_updated_ms` | same names | Exact validated epoch-millisecond detector evidence; never substitute `timestamp` or monotonic uptime. Omit a zero/invalid sentinel. |
| `scanner_slot`, `scanner_slots_seen` | same names | Exact uplink-annotated integers; never accept either value from scanner JSON. |
| `ble_threat_kind`, `ble_prompt_family_mask`, `ble_unique_macs`, `ble_observation_count`, `ble_serial_service_uuid`, `ble_threat_evidence_mask` | same names | Emit the complete six-key group with exact integers when `ble_threat_kind != 0`; omit the entire group when kind is 0. |

The remaining direct members use their same-named HTTP keys with the exact C
numeric value. Per-item `timestamp` is not a `drone_detection_t` mapping: it
comes only from the validated `backend_detection_observation_t` policy below.
The hand-written JSON fixture must use values that expose every special case:
open Wi-Fi auth (`auth_m:0`), legacy Wi-Fi generation 0, public BLE address 0,
Apple activity/flags 0, `probe_ie_hash=0x00abcdef`,
`ble_ja3_hash=0x0123abcd`, raw bytes containing `00`/`ff`, a 125500-us
advertisement interval, all four 16-bit and both 128-bit UUID slots, and a
frequency whose channel differs numerically from MHz. Native tests assert the
exact keys, types, values, lowercase hex, UUID byte order, source strings,
zero-vs-absent rules, and the 2412/2472/2484/5180/invalid frequency boundaries.

`backend_assert_detection_equal` is test-only and independently enumerates
every member; it must not call the production encoder, decoder, a production
`memcmp` helper, or compare the padded struct as one blob. It compares strings
by value, float/double members with declared test tolerances, scalar members
exactly, and the fixed numeric/byte arrays element-by-element. Its explicit
coverage list is:

```text
drone_id, source, confidence,
latitude, longitude, altitude_m,
heading_deg, speed_mps, vertical_speed_mps,
rssi, estimated_distance_m,
manufacturer, model,
operator_lat, operator_lon, operator_id,
ua_type, id_type, self_id_desc_type, self_id_text,
height_agl_m, geodetic_alt_m, h_accuracy_m, v_accuracy_m,
area_count, area_radius, area_ceiling, area_floor, classification_type,
ssid, bssid, freq_mhz, channel_width_mhz,
ble_company_id, ble_apple_type,
ble_service_uuids[0..3], ble_svc_uuid_count,
ble_service_uuids_128[0..1][0..15], ble_svc_uuid_128_count,
ble_svc_uuids_raw, ble_ad_type_count, ble_payload_len, ble_addr_type,
ble_ja3_hash, ble_name, class_reason,
ble_apple_auth[0..2], ble_apple_activity, ble_apple_flags,
ble_raw_mfr[0..19], ble_raw_mfr_len, ble_adv_interval_us,
first_seen_ms, last_updated_ms, fused_confidence,
probed_ssids, probe_ie_hash, wifi_generation, wifi_auth_mode,
scanner_slot, scanner_slots_seen,
ble_threat_kind, ble_prompt_family_mask, ble_unique_macs,
ble_observation_count, ble_serial_service_uuid, ble_threat_evidence_mask
```

`fixture_full_detection()` deliberately sets every scalar nonzero, fills every
array slot, uses escaped characters in strings, and sets both 128-bit UUID
slots. A second test toggles each field group and proves the helper detects the
difference. Add malformed, duplicate-key, token-limit, string-overflow, maximum
4095-byte, and 4096-byte rejection vectors for the local reader/decoder.
The same test-support file exports
`backend_assert_detection_json_equal(expected, json, tokens, token_count,
object_index)`; it independently looks up every corresponding HTTP key from
the list above and is first exercised against a hand-written full JSON fixture
before Task 8 uses it against production batch output.

- [ ] **Step 5: Run codec and sanitizer tests**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_detection_codec`

Expected: PASS under AddressSanitizer.

- [ ] **Step 6: Commit**

```bash
git add backend-firmware/shared backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: preserve scanner detection contract"
```

---

### Task 4: Enforce Scanner Roles, Health, Time, and Failover

**Files:**
- Create: `backend-firmware/shared/backend_scanner_topology.h`
- Create: `backend-firmware/shared/backend_scanner_topology.c`
- Create: `backend-firmware/shared/backend_scanner_role.h`
- Create: `backend-firmware/shared/backend_scanner_role.c`
- Create: `backend-firmware/shared/backend_scanner_control_codec.h`
- Create: `backend-firmware/shared/backend_scanner_control_codec.c`
- Create: `backend-firmware/shared/backend_scanner_status_codec.h`
- Create: `backend-firmware/shared/backend_scanner_status_codec.c`
- Create: `backend-firmware/shared/backend_flow_policy.h`
- Create: `backend-firmware/shared/backend_flow_policy.c`
- Create: `backend-firmware/shared/backend_recovery_policy.h`
- Create: `backend-firmware/shared/backend_recovery_policy.c`
- Create: `backend-firmware/test/test_backend_scanner_topology/test_main.c`
- Create: `backend-firmware/test/test_backend_scanner_role/test_main.c`
- Create: `backend-firmware/test/test_backend_scanner_wire/test_main.c`
- Create: `backend-firmware/test/test_backend_flow_policy/test_main.c`
- Create: `backend-firmware/test/test_backend_recovery_policy/test_main.c`
- Create: `backend-firmware/scanner/main/core/backend_scanner_runtime.h`
- Create: `backend-firmware/scanner/main/core/backend_scanner_runtime.c`
- Create: `backend-firmware/uplink/main/comms/backend_uart_slot.h`
- Create: `backend-firmware/uplink/main/comms/backend_uart_slot.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: two scanner heartbeat/link snapshots and the complete detection codec.
- Produces: deterministic fixed roles, single-scanner hybrid failover,
  boot-quiescent scanner runtime, complete control/status schemas, bounded flow
  and link recovery, and dual UART adapters.

- [ ] **Step 1: Write failing topology tests**

```c
void test_two_healthy_scanners_get_fixed_profiles(void)
{
    backend_scanner_health_t health[2] = {
        healthy_scanner(10), healthy_scanner(20),
    };
    backend_scanner_plan_t plan = {0};
    backend_scanner_plan_compute(health, 0, 1000, &plan);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, plan.desired[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.converged_mask);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
}

void test_one_scanner_becomes_hybrid_and_zero_is_fatal(void)
{
    backend_scanner_health_t one[2] = { healthy_scanner(10), {0} };
    backend_scanner_plan_t plan = {0};
    backend_scanner_plan_compute(one, 0, 1000, &plan);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_HYBRID_FAILOVER, plan.desired[0]);
    TEST_ASSERT_TRUE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);

    backend_scanner_health_t none[2] = {{0}, {0}};
    backend_scanner_plan_compute(none, 0, 20000, &plan);
    TEST_ASSERT_TRUE(plan.fatal);
}

void test_cold_boot_assigns_roles_before_radios_can_report_healthy(void)
{
    backend_scanner_health_t cold[2] = {
        transport_ready_scanner(10), transport_ready_scanner(20),
    };
    cold[0].radio_healthy = false;
    cold[1].radio_healthy = false;
    cold[0].role_acked = false;
    cold[1].role_acked = false;
    backend_scanner_plan_t plan = {0};
    backend_scanner_plan_compute(cold, 0, 1000, &plan);
    TEST_ASSERT_EQUAL_HEX8(0x03, plan.eligible_mask);
    TEST_ASSERT_EQUAL_HEX8(0x00, plan.converged_mask);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY, plan.desired[0]);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_WIFI_PRIMARY, plan.desired[1]);
    TEST_ASSERT_TRUE(plan.converging);
    TEST_ASSERT_FALSE(plan.degraded);
    TEST_ASSERT_FALSE(plan.fatal);
}
```

- [ ] **Step 2: Define and implement topology types**

```c
typedef enum {
    BACKEND_SCAN_PROFILE_QUIESCENT = 0,
    BACKEND_SCAN_PROFILE_BLE_PRIMARY,
    BACKEND_SCAN_PROFILE_WIFI_PRIMARY,
    BACKEND_SCAN_PROFILE_HYBRID_FAILOVER,
} backend_scan_profile_t;

typedef struct {
    bool connected;
    bool identity_valid;
    bool command_healthy;
    bool radio_healthy;
    bool role_acked;
    uint32_t boot_id;
    uint32_t acknowledged_generation;
    backend_scan_profile_t reported_profile;
    int64_t convergence_started_ms;
} backend_scanner_health_t;

typedef struct {
    backend_scan_profile_t desired[2];
    uint8_t eligible_mask;
    uint8_t converged_mask;
    bool converging;
    bool degraded;
    bool fatal;
} backend_scanner_plan_t;

bool backend_scanner_required_radio_healthy(
    backend_scan_profile_t profile,
    bool ble_healthy,
    bool wifi_healthy);

void backend_scanner_plan_compute(
    const backend_scanner_health_t health[2],
    int64_t system_boot_ms,
    int64_t now_ms,
    backend_scanner_plan_t *out);
int backend_scanner_ble_owner(const backend_scanner_plan_t *plan);
```

`backend_scanner_required_radio_healthy` is the sole derivation of aggregate
`radio_healthy`: `QUIESCENT -> false`, `BLE_PRIMARY -> ble_healthy`,
`WIFI_PRIMARY -> wifi_healthy`, and
`HYBRID_FAILOVER -> ble_healthy && wifi_healthy`; an unknown profile also
returns false. The uplink computes it from the latest validated status and
never accepts an aggregate value from the wire. The scanner heartbeat bridge,
topology convergence, rollback readiness, and OTA convergence all use this
same helper. Table-driven native tests cover every profile and both boolean
inputs, including quiescent-with-both-healthy and each one-radio-down hybrid
case.

Transport eligibility is deliberately separate from convergence. A scanner is
eligible for a role when it is connected, has the exact backend identity and a
nonzero boot ID, and its command ingress is healthy. It is converged only after
it ACKs the current generation, reports the desired profile, and reports that
profile's required radio healthy. A newly eligible scanner with no
`convergence_started_ms` must receive a role immediately; the coordinator sets
that timestamp when it sends the first role command. It remains an assignment
candidate for a 15000-ms convergence grace. Only after that grace may a
non-converged scanner be removed and the survivor changed to hybrid. Thus the
quiescent-radio boot state can never prevent the role command needed to start
the radios. `degraded`/`fatal` are withheld during the grace;
`backend_scanner_ble_owner` returns only a converged BLE/hybrid owner, or `-1`.
If no transport is eligible during the first 15000 ms after
`system_boot_ms`, the plan is `converging` rather than fatal; zero eligible
slots becomes fatal only at that deadline. Tests cover 14999 and 15000 ms.

- [ ] **Step 3: Test and implement monotonic role application**

```c
void test_scanner_stays_quiescent_until_current_boot_generation(void)
{
    backend_scanner_role_state_t state;
    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_QUIESCENT,
                      backend_scanner_role_effective(&state));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, backend_scanner_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_SCAN_PROFILE_BLE_PRIMARY,
                      backend_scanner_role_effective(&state));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_STALE, backend_scanner_role_apply(
        &state, 77, 3, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
    TEST_ASSERT_EQUAL(BACKEND_ROLE_INVALID_BOOT, backend_scanner_role_apply(
        &state, 76, 5, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
}

void test_equal_role_generation_is_an_idempotent_ack_refresh(void)
{
    backend_scanner_role_state_t state;
    backend_scanner_role_init(&state, 77);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_APPLIED, backend_scanner_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    uint32_t transitions = state.radio_transition_count;
    TEST_ASSERT_EQUAL(BACKEND_ROLE_REFRESHED, backend_scanner_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_BLE_PRIMARY));
    TEST_ASSERT_EQUAL_UINT32(transitions, state.radio_transition_count);
    TEST_ASSERT_TRUE(state.ack_pending);
    TEST_ASSERT_EQUAL(BACKEND_ROLE_CONFLICT, backend_scanner_role_apply(
        &state, 77, 4, BACKEND_SCAN_PROFILE_WIFI_PRIMARY));
}
```

Role commands carry `boot_id`, monotonic `generation`, and profile. The scanner
must halt both radios on boot until a valid current-boot command is applied.
```c
typedef enum {
    BACKEND_ROLE_APPLIED = 0,
    BACKEND_ROLE_REFRESHED,
    BACKEND_ROLE_STALE,
    BACKEND_ROLE_CONFLICT,
    BACKEND_ROLE_INVALID_BOOT,
    BACKEND_ROLE_INVALID_PROFILE,
} backend_scanner_role_result_t;

typedef struct {
    uint32_t boot_id;
    uint32_t generation;
    backend_scan_profile_t effective;
    uint32_t radio_transition_count;
    bool ack_pending;
} backend_scanner_role_state_t;
```

A larger generation applies
and increments `radio_transition_count` only if the effective profile changes.
The exact same boot/generation/profile returns `REFRESHED` and queues the same
ACK without stopping or restarting a radio. Equal generation with different
content is `CONFLICT`; lower generation is `STALE`. Every `APPLIED` or
`REFRESHED` result makes `backend_scanner_role_take_ack()` return one ACK
containing boot ID, generation, effective profile, and current radio health.

- [ ] **Step 4: Define and round-trip every scanner control/status line**

`backend_scanner_control_t` is a tagged union for `role`, `time`, `flow`,
`led_state`, `health_request`, `recovery`, `investigate`, `cancel`,
`ota_begin`, `ota_end`, and `ota_abort`. To avoid forward dependencies, Task 4
defines all wire payload structs using only fixed-width integers, booleans, and
bounded arrays: LED `state[16]/generation/ttl_ms`; investigation
`command_id[33]/has_mac/mac[18]/mode/timeout_ms`; cancel `command_id[33]`; OTA begin
`session_id/generation/component_slot/expected_mac[18]/expected_boot_id/
expected_topology_generation/target[40]/project[40]/hardware[40]/version[32]/
image_size/crc32/sha256[65]/allow_same_version/dry_run`; and OTA end/abort
`session_id/generation/reason[48]`. Tasks 5, 11, and 12 validate and translate
those structs into their domain types; they do not change this Task-4 union or
introduce circular includes. Unknown types fail closed.
The Task-4 payloads and exact JSON forms are:

```text
{"type":"role","boot_id":77,"generation":4,"profile":"ble_primary"}
{"type":"time","generation":5,"valid":true,"epoch_ms":1785600000123,"source":"sntp"}
{"type":"flow","generation":6,"paused":true}
{"type":"health_request","sequence":8}
{"type":"recovery","boot_id":77,"generation":9,"action":"restart_radios"}
{"type":"investigate","command_id":"0123456789abcdef0123456789abcdef","mac":"AA:BB:CC:DD:EE:FF","mode":"gatt","timeout_ms":12000}
{"type":"investigate","command_id":"0123456789abcdef0123456789abcdef","mac":null,"mode":"passive_capture","timeout_ms":12000}
{"type":"cancel","command_id":"0123456789abcdef0123456789abcdef"}
{"type":"ota_begin","session_id":7,"generation":12,"component_slot":0,"expected_mac":"AA:BB:CC:DD:EE:01","expected_boot_id":305419896,"expected_topology_generation":4,"target":"scanner-s3-combo-backend","project":"fof_backend_scanner","hardware":"seeed_xiao_esp32s3","version":"0.1.1-backend","image_size":1048576,"crc32":305419896,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","allow_same_version":false,"dry_run":false}
```

The uplink translates API `request_id`/`target` to scanner
`command_id`/`mac` exactly once; scanner output translates back by emitting
API event `request_id` equal to that same command ID. Passive investigate uses
JSON null `mac`; cancel contains no invented target/mode fields because those
remain in uplink state. Round-trip tests cover both modes and cancellation.

`backend_scanner_status_t` is the following complete host/ESP shared type; it
does not carry a wire-supplied aggregate `radio_healthy`:

```c
typedef struct {
    uint8_t schema;
    uint32_t sequence;
    uint32_t boot_id;
    char mac[18];
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    backend_scan_profile_t profile;
    uint32_t role_generation;
    bool role_acked;
    bool command_ingress;
    bool ble_healthy;
    bool wifi_healthy;
    bool flow_paused;
    char ota_state[24];
    char rollback_state[24];
    uint32_t rx_errors;
    uint32_t tx_drops;
    uint64_t uptime_ms;
} backend_scanner_status_t;
```

Its exact compact form is:

```text
{"type":"scanner_status","schema":1,"sequence":12,"boot_id":77,"mac":"AA:BB:CC:DD:EE:FF","target":"scanner-s3-combo-backend","project":"fof_backend_scanner","hardware":"seeed_xiao_esp32s3","version":"0.1.0-backend","profile":"ble_primary","role_generation":4,"role_acked":true,"command_ingress":true,"ble_healthy":true,"wifi_healthy":false,"flow_paused":false,"ota_state":"idle","rollback_state":"valid","rx_errors":0,"tx_drops":0,"uptime_ms":9000}
```

Encode/decode functions consume explicit lengths and a 4096-byte destination,
use the local bounded JSON modules, and reject duplicate/missing fields,
overlong identity/MAC values, unknown enums, zero boot/status sequence, or
lines over 4095 bytes. Uplink accepts increasing status sequence within a boot;
an exact duplicate refreshes liveness without reapplying state, a lower
sequence is stale, and a changed nonzero boot resets sequence/role convergence.
Add round-trip, duplicate, conflict, max-line, wrong-identity, and changed-boot
tests in `test_backend_scanner_wire`.

- [ ] **Step 5: Add the exact ESP-IDF UART adapters**

`backend_uart_slot.c` owns two line framers and these pin records:

```c
typedef struct {
    int uart;
    int rx_gpio;
    int tx_gpio;
} backend_uart_slot_config_t;

static const backend_uart_slot_config_t SLOT_CONFIG[2] = {
    { .uart = UART_NUM_1, .rx_gpio = 2, .tx_gpio = 1 },
    { .uart = UART_NUM_2, .rx_gpio = 4, .tx_gpio = 3 },
};
```

Both use 921600 baud, 8N1, no flow control. Scanner UART uses UART1 TX GPIO1,
RX GPIO2. Status/command lines remain accepted during normal output flow
control and OTA quiet mode.

- [ ] **Step 6: Add bounded flow, recovery, and watchdog policies**

The scanner detection TX queue is exactly 64 complete lines. Uplink sends
`flow paused=true` when its per-slot decoded queue reaches 48 and sends
`paused=false` only after it falls to 24. Flow commands use monotonic generation
with the same equal-generation/idempotent rules as role commands. Pausing stops
only new detection-line enqueue; UART RX plus status, role/time/flow ACK,
investigation terminal result, recovery, and OTA control always have a
four-entry reserved control queue and remain serviceable. The pure
`backend_flow_policy_update(depth, current_state)` returns `PAUSE`, `RESUME`,
or `NO_CHANGE` and boundary tests cover 47/48 and 24/25.

Status is emitted at boot, on state change, and every 2000 ms. Missing valid
status for 6000 ms enters `PROBE`; send three health requests one second apart,
then reinitialize that UART driver once. If no status arrives by 15000 ms,
mark the slot unavailable. A recovered changed boot re-enters the 15000-ms
role-convergence grace. The only remote recovery action is
`restart_radios`: it is bound to current boot and monotonic generation, never
erases NVS or changes OTA partitions, is deferred during active OTA, and
restarts only radios required by the effective role. One unavailable scanner
drives the survivor to hybrid; two unavailable scanners are fatal. Uplink STA
reconnect is an independent policy and never marks a scanner unhealthy.
`backend_recovery_policy_tick` returns `NONE`, `SEND_PROBE`,
`REINIT_LOCAL_UART`, `MARK_UNAVAILABLE`, or `SEND_RESTART_RADIOS`; tests cover
every exact deadline, changed-boot recovery, deferred OTA recovery, one-slot
hybrid, and two-slot fatal behavior.

Every long-running scanner/uplink worker registers itself with ESP task WDT and
feeds only its own subscription after a successful bounded loop iteration.
UART RX/control, coordinator, radio workers, uploader, command client, and OTA
worker each have an explicit 30-second budget; no common timer task may feed on
their behalf. OTA erases/writes feed only between bounded chunks. A host-tested
watchdog readiness mask prevents rollback-clear until every required worker
has completed at least one iteration.

- [ ] **Step 7: Add time validity and command cadence tests**

Use vendored `time_sync_policy` to reject epoch values below
`1700000000000`. Coordinator policy emits role and time commands immediately
on boot/link change and at most every ten seconds thereafter; the time command
contains epoch milliseconds, validity, and source (`sntp`, `backend`, or
`none`).

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_scanner_topology -f test_backend_scanner_role -f test_backend_scanner_wire -f test_backend_flow_policy -f test_backend_recovery_policy`

Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add backend-firmware/shared backend-firmware/scanner/main/core backend-firmware/uplink/main/comms backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: coordinate scanner roles and failover"
```

---

### Task 5: Implement the Exact Yellow-LED Contract

**Files:**
- Create: `backend-firmware/shared/backend_led_pattern.h`
- Create: `backend-firmware/shared/backend_led_pattern.c`
- Create: `backend-firmware/shared/backend_led_protocol.h`
- Create: `backend-firmware/shared/backend_led_protocol.c`
- Create: `backend-firmware/test/test_backend_led_pattern/test_main.c`
- Create: `backend-firmware/test/test_backend_led_protocol/test_main.c`
- Create: `backend-firmware/scanner/main/hw/backend_yellow_led.h`
- Create: `backend-firmware/scanner/main/hw/backend_yellow_led.c`
- Create: `backend-firmware/uplink/main/hw/backend_yellow_led.h`
- Create: `backend-firmware/uplink/main/hw/backend_yellow_led.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: local threat liveness, network/scanner health, and mirrored UART commands.
- Produces: exact active-low GPIO21 timing without heap, RMT, or a second hardware timer.

- [ ] **Step 1: Write failing priority and timing tests**

```c
void test_led_priority_and_exact_patterns(void)
{
    backend_led_inputs_t inputs = {
        .network_degraded = true, .drone_live = true, .meta_live = true,
    };
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE_META, backend_led_select(&inputs));
    inputs.fatal = true;
    TEST_ASSERT_EQUAL(BACKEND_LED_FATAL, backend_led_select(&inputs));

    size_t count = 0;
    const backend_led_step_t *drone = backend_led_pattern(BACKEND_LED_DRONE, &count);
    const backend_led_step_t expected[] = {
        {true, 400}, {false, 120}, {true, 120}, {false, 1360},
    };
    TEST_ASSERT_EQUAL_UINT(sizeof(expected) / sizeof(expected[0]), count);
    for (size_t i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL(expected[i].on, drone[i].on);
        TEST_ASSERT_EQUAL_UINT16(expected[i].duration_ms, drone[i].duration_ms);
    }
}
```

Assert these immutable arrays verbatim; `drone_meta` is the drone array followed
by the meta array:

```c
static const backend_led_step_t HEALTHY[] = {
    {true, 80}, {false, 2920},
};
static const backend_led_step_t NETWORK_DEGRADED[] = {
    {true, 300}, {false, 300}, {true, 300}, {false, 1800},
};
static const backend_led_step_t DRONE[] = {
    {true, 400}, {false, 120}, {true, 120}, {false, 1360},
};
static const backend_led_step_t META[] = {
    {true, 100}, {false, 100}, {true, 100}, {false, 100},
    {true, 100}, {false, 100}, {true, 100}, {false, 1000},
};
static const backend_led_step_t DRONE_META[] = {
    {true, 400}, {false, 120}, {true, 120}, {false, 1360},
    {true, 100}, {false, 100}, {true, 100}, {false, 100},
    {true, 100}, {false, 100}, {true, 100}, {false, 1000},
};
static const backend_led_step_t FATAL[] = {
    {true, 120}, {false, 120}, {true, 120}, {false, 120},
    {true, 120}, {false, 800},
};
static const backend_led_step_t UART_LOST[] = {
    {true, 1000}, {false, 1000},
};
```

- [ ] **Step 2: Implement selection and immutable patterns**

```c
typedef enum {
    BACKEND_LED_HEALTHY = 0,
    BACKEND_LED_NETWORK_DEGRADED,
    BACKEND_LED_DRONE,
    BACKEND_LED_META,
    BACKEND_LED_DRONE_META,
    BACKEND_LED_FATAL,
    BACKEND_LED_UART_LOST,
} backend_led_state_t;

typedef struct {
    bool fatal;
    bool network_degraded;
    bool drone_live;
    bool meta_live;
} backend_led_inputs_t;

typedef struct {
    bool on;
    uint16_t duration_ms;
} backend_led_step_t;

backend_led_state_t backend_led_select(const backend_led_inputs_t *inputs);
const backend_led_step_t *backend_led_pattern(
    backend_led_state_t state, size_t *step_count);
```

Selection order is fatal, both, drone, meta, network degraded, healthy.
`uart_lost` is scanner-local TTL fallback, not a normal uplink selection.

- [ ] **Step 3: Write failing generation/TTL tests**

```c
void test_led_mirror_refreshes_equal_generation_without_restarting_pattern(void)
{
    backend_led_mirror_t mirror;
    backend_led_mirror_init(&mirror);
    backend_led_command_t command = {
        .state = BACKEND_LED_DRONE, .generation = 42, .ttl_ms = 6000,
    };
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_NEW,
        backend_led_mirror_accept(&mirror, &command, 1000));
    uint32_t transitions = mirror.pattern_transition_count;
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_REFRESH,
        backend_led_mirror_accept(&mirror, &command, 3000));
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_REFRESH,
        backend_led_mirror_accept(&mirror, &command, 5000));
    TEST_ASSERT_EQUAL(BACKEND_LED_ACCEPTED_REFRESH,
        backend_led_mirror_accept(&mirror, &command, 7000));
    TEST_ASSERT_EQUAL_UINT32(transitions, mirror.pattern_transition_count);
    TEST_ASSERT_EQUAL(BACKEND_LED_DRONE,
                      backend_led_mirror_effective(&mirror, 12999));
    TEST_ASSERT_EQUAL(BACKEND_LED_UART_LOST,
                      backend_led_mirror_effective(&mirror, 13000));

    backend_led_command_t conflict = command;
    conflict.state = BACKEND_LED_META;
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_CONFLICT,
        backend_led_mirror_accept(&mirror, &conflict, 8000));
    conflict = command;
    conflict.generation = 41;
    TEST_ASSERT_EQUAL(BACKEND_LED_REJECTED_STALE,
        backend_led_mirror_accept(&mirror, &conflict, 8000));
}
```

- [ ] **Step 4: Implement the exact wire command**

```c
typedef struct {
    backend_led_state_t state;
    uint32_t generation;
    uint32_t ttl_ms;
} backend_led_command_t;

typedef enum {
    BACKEND_LED_ACCEPTED_NEW = 0,
    BACKEND_LED_ACCEPTED_REFRESH,
    BACKEND_LED_REJECTED_STALE,
    BACKEND_LED_REJECTED_CONFLICT,
    BACKEND_LED_REJECTED_INVALID,
} backend_led_accept_result_t;

typedef struct {
    backend_led_command_t accepted;
    int64_t accepted_monotonic_ms;
    uint32_t pattern_transition_count;
    bool has_accepted;
} backend_led_mirror_t;

size_t backend_led_command_encode(
    const backend_led_command_t *command, char *output, size_t capacity);
bool backend_led_command_decode(
    const char *json, size_t length, backend_led_command_t *out);
void backend_led_mirror_init(backend_led_mirror_t *mirror);
backend_led_accept_result_t backend_led_mirror_accept(
    backend_led_mirror_t *mirror,
    const backend_led_command_t *incoming,
    int64_t now_ms);
backend_led_state_t backend_led_mirror_effective(
    const backend_led_mirror_t *mirror, int64_t now_ms);
```

Encoder output is exactly
`{"type":"led_state","state":"drone","generation":42,"ttl_ms":6000}`.
Reject unknown names, generation zero, and TTL outside 2000-30000 ms.
An uninitialized mirror reports `UART_LOST` until its first accepted command.
A larger generation changes state, refreshes acceptance time, and increments
`pattern_transition_count` only if the logical state changed. The exact same
generation/state/TTL is an idempotent refresh: it updates acceptance time but
does not restart the pattern. Equal generation with different state or TTL is
a conflict; lower generation is stale. The coordinator therefore increments
generation only on a logical state change and safely retransmits that same
generation every two seconds for the 6000-ms TTL.

- [ ] **Step 5: Add thin active-low GPIO adapters**

Both adapters configure GPIO21 output/high at boot and drive it only with:

```c
gpio_set_level(GPIO_NUM_21, on ? 0 : 1);
```

A single FreeRTOS task walks immutable steps with `vTaskDelay`; state changes
restart at step zero. Do not include `led_strip`, RMT, WS2812, GPIO48, or RGB
types.

- [ ] **Step 6: Run tests and commit**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_led_pattern -f test_backend_led_protocol`

Expected: PASS.

```bash
git add backend-firmware/shared backend-firmware/scanner/main/hw backend-firmware/uplink/main/hw backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: add mirrored yellow LED alerts"
```

---

### Task 6: Validate, Migrate, and Atomically Persist AP Configuration

**Files:**
- Create: `backend-firmware/shared/backend_config.h`
- Create: `backend-firmware/shared/backend_config.c`
- Create: `backend-firmware/test/test_backend_config/test_main.c`
- Create: `backend-firmware/test/test_backend_config_migration/test_main.c`
- Create: `backend-firmware/test/test_backend_nvs_safety/test_main.c`
- Create: `backend-firmware/uplink/main/storage/backend_nvs_config.h`
- Create: `backend-firmware/uplink/main/storage/backend_nvs_config.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: up to four ordered Wi-Fi networks, backend URL, legacy NVS keys, optional location, and existing `device_id`.
- Produces: one CRC-protected `backend_config` record no larger than 1024 bytes.

- [ ] **Step 1: Write failing validation and migration tests**

```c
void test_config_accepts_four_networks_and_preserves_device_id(void)
{
    backend_config_record_t record = valid_config_fixture();
    record.network_count = 4;
    strcpy(record.device_id, "uplink_CB77A4");
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_VALID, backend_config_validate(&record));
    backend_config_blob_t blob = {0};
    TEST_ASSERT_TRUE(backend_config_encode_canonical(&record, &blob));
    backend_config_record_t decoded = {0};
    TEST_ASSERT_EQUAL(BACKEND_CONFIG_VALID,
        backend_config_decode_canonical(blob.bytes, blob.length, &decoded));
    TEST_ASSERT_EQUAL_STRING("uplink_CB77A4", record.device_id);
}

void test_legacy_migration_accepts_wifi_pass_alias_and_never_overwrites_id(void)
{
    backend_legacy_config_t legacy = {
        .wifi_ssid = "FieldNet",
        .wifi_pass = "secret-value",
        .backend_url = "http://10.0.0.2:8000",
        .device_id = "uplink_CB77A4",
        .ap_pass = "friendorfoe",
    };
    backend_config_record_t migrated = {0};
    TEST_ASSERT_TRUE(backend_config_migrate_legacy(&legacy, 9, &migrated));
    TEST_ASSERT_EQUAL_UINT8(1, migrated.network_count);
    TEST_ASSERT_EQUAL_STRING("secret-value", migrated.networks[0].password);
    TEST_ASSERT_EQUAL_STRING("uplink_CB77A4", migrated.device_id);
    TEST_ASSERT_EQUAL_UINT32(9, migrated.generation);
}
```

- [ ] **Step 2: Define the bounded record and pure validation**

```c
#define BACKEND_CONFIG_MAX_NETWORKS 4
#define BACKEND_CONFIG_SCHEMA_VERSION 1

typedef struct {
    char ssid[33];
    char password[65];
} backend_wifi_network_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char wifi_pass[65];
    char backend_url[192];
    char device_id[33];
    char ap_pass[65];
} backend_legacy_config_t;

typedef struct {
    uint16_t schema_version;
    uint32_t generation;
    uint8_t network_count;
    backend_wifi_network_t networks[BACKEND_CONFIG_MAX_NETWORKS];
    char backend_url[192];
    char device_id[33];
    char display_name[65];
    char ap_password[65];
    bool auto_update_enabled;
    bool has_location;
    double latitude;
    double longitude;
    float altitude_m;
} backend_config_record_t;

#define BACKEND_CONFIG_MAGIC UINT32_C(0x47464342)
#define BACKEND_CONFIG_BLOB_MAX 1024
#define BACKEND_CONFIG_PAYLOAD_MAX 1008

typedef struct {
    size_t length;
    uint8_t bytes[BACKEND_CONFIG_BLOB_MAX];
} backend_config_blob_t;

typedef enum {
    BACKEND_CONFIG_VALID,
    BACKEND_CONFIG_INVALID_LENGTH,
    BACKEND_CONFIG_INVALID_CRC,
    BACKEND_CONFIG_INVALID_FIELD,
} backend_config_result_t;

backend_config_result_t backend_config_validate(
    const backend_config_record_t *record);
bool backend_config_encode_canonical(
    const backend_config_record_t *record, backend_config_blob_t *out);
backend_config_result_t backend_config_decode_canonical(
    const uint8_t *bytes, size_t length, backend_config_record_t *out);
bool backend_config_migrate_legacy(
    const backend_legacy_config_t *legacy,
    uint32_t generation,
    backend_config_record_t *out);
```

Validate URL scheme `http://`, non-empty host, four-network bound, SSID/password
termination, AP password length 8-63, latitude -90..90, longitude -180..180,
finite altitude, schema version, and nonzero generation. Passwords are never
logged. `auto_update_enabled` defaults false for a missing/legacy record and is
changed only by an authenticated AP configuration save; it is never inferred
from catalog availability.

Never CRC or persist the native C struct. Canonical bytes are exactly:
`magic:u32-le, schema:u16-le, payload_len:u16-le, generation:u32-le`,
followed by payload, then `crc32:u32-le`. Payload encodes network count,
each active SSID/password, backend URL, device ID, display name, AP password,
auto-update-enabled, location-present, and location values in that order. Strings are
`length:u16-le + exactly length bytes` with no uninitialized tail. Integers
are little-endian; finite IEEE-754 doubles/floats are encoded by their declared
bit pattern through `memcpy`, never an unaligned cast. Inactive network slots
are not serialized. CRC covers every canonical byte from magic through the end
of payload, excluding only the CRC itself. Decoder requires
`length == 12 + payload_len + 4`, payload no larger than 1008, exact schema,
valid CRC, complete consumption, and then logical validation.

Add a regression that initializes two logical records with `0xAA` and `0x55`,
sets identical logical values while leaving different struct/string tail
padding, and proves canonical length, bytes, and CRC are identical. Mutating
each header/payload/CRC byte class must fail decode without modifying the
caller's output.

- [ ] **Step 3: Implement atomic NVS storage and legacy import**

`backend_nvs_config.c` uses namespace `fof_config`, key `backend_config`, one
canonical bounded byte blob, and `nvs_commit`. It logically validates then
canonical-encodes before every write and canonical-decodes after every read.
It never uses `sizeof(backend_config_record_t)` as persistent format. On
missing record it reads `wifi_ssid`, both `wifi_password` and the real
legacy key `wifi_pass`, `backend_url`, `device_id`, and `ap_pass`; it imports
once and writes the new blob. A valid existing `device_id` always wins; only a
missing ID is generated as `uplink_XXXXXX` from the uplink STA MAC suffix.
If both legacy password aliases are present and differ, migration fails closed
without writing; if one is present it is used. Migration always sets
`auto_update_enabled=false`.

Backend device code never calls `nvs_flash_erase`, including recovery from
`ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`. Add an injected
NVS-init adapter test for both errors that proves zero erase/write/commit
hooks, fatal recovery health, unchanged output, and rollback remains uncleared.
The source-isolation audit rejects the token `nvs_flash_erase` in every
backend-owned C/C++ build source. Recovery requires the verified direct-USB
backup path; firmware may not silently destroy preserved identity/config.

```c
bool backend_config_load(backend_config_record_t *out);
bool backend_config_commit(const backend_config_record_t *record);
bool backend_config_load_or_migrate(backend_config_record_t *out);
```

The NVS adapter accepts injected read/write hooks under `UNIT_TESTING` so
power-failure and failed-commit tests prove the prior record remains readable.

- [ ] **Step 4: Run configuration tests**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_config -f test_backend_config_migration -f test_backend_nvs_safety`

Expected: PASS with no secret values in captured log output.

- [ ] **Step 5: Commit**

```bash
git add backend-firmware/shared backend-firmware/uplink/main/storage backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: persist backend AP configuration"
```

---

### Task 7: Implement Ordered Wi-Fi Selection and the Configuration-Only AP

**Files:**
- Create: `backend-firmware/shared/backend_ap_policy.h`
- Create: `backend-firmware/shared/backend_ap_policy.c`
- Create: `backend-firmware/shared/backend_portal_contract.h`
- Create: `backend-firmware/shared/backend_portal_contract.c`
- Create: `backend-firmware/test/test_backend_ap_policy/test_main.c`
- Create: `backend-firmware/test/test_backend_portal_routes/test_main.c`
- Create: `backend-firmware/test/test_backend_wifi_manager/test_main.c`
- Create: `backend-firmware/test/test_backend_portal_contract.py`
- Create: `backend-firmware/uplink/main/network/backend_wifi_manager.h`
- Create: `backend-firmware/uplink/main/network/backend_wifi_manager.c`
- Create: `backend-firmware/uplink/main/network/backend_config_portal.h`
- Create: `backend-firmware/uplink/main/network/backend_config_portal.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: validated config, backend success time, AP USB request, Wi-Fi events.
- Produces: ordered reconnect policy and setup portal at `192.168.4.1`.

- [ ] **Step 1: Write failing AP lifecycle tests**

```c
void test_ap_starts_for_invalid_config_usb_or_five_minute_outage(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(BACKEND_AP_START, backend_ap_policy_tick(
        &policy, ap_input(false, 0, false, false), 1));
    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(BACKEND_AP_START, backend_ap_policy_tick(
        &policy, ap_input(true, 7, true, true), 2000));
    backend_ap_policy_init(&policy, 0);
    TEST_ASSERT_EQUAL(BACKEND_AP_START, backend_ap_policy_tick(
        &policy, ap_input(true, 7, false, false), 300000));
}

void test_ap_stops_only_for_current_generation_success_after_this_ap_start(void)
{
    backend_ap_policy_t policy;
    backend_ap_policy_init(&policy, 0);
    backend_ap_policy_tick(&policy, ap_input(true, 8, true, true), 1000);
    backend_ap_policy_note_backend_success(&policy, 7, 2000);
    TEST_ASSERT_EQUAL(BACKEND_AP_NO_CHANGE, backend_ap_policy_tick(
        &policy, ap_input(true, 8, false, false), 40000));
    backend_ap_policy_note_backend_success(&policy, 8, 100000);
    TEST_ASSERT_EQUAL(BACKEND_AP_NO_CHANGE, backend_ap_policy_tick(
        &policy, ap_input(true, 8, false, false), 129999));
    TEST_ASSERT_EQUAL(BACKEND_AP_STOP, backend_ap_policy_tick(
        &policy, ap_input(true, 8, false, false), 130000));
}
```

- [ ] **Step 2: Implement AP and ordered-network policies**

```c
typedef enum {
    BACKEND_AP_NO_CHANGE,
    BACKEND_AP_START,
    BACKEND_AP_STOP,
} backend_ap_action_t;

typedef struct {
    bool config_valid;
    uint32_t config_generation;
    bool backend_connected;
    bool usb_start_requested;
} backend_ap_input_t;

typedef struct {
    bool running;
    int64_t boot_ms;
    int64_t ap_started_ms;
    uint32_t ap_started_config_generation;
    uint32_t last_success_generation;
    int64_t last_success_ms;
    int64_t outage_started_ms;
} backend_ap_policy_t;

void backend_ap_policy_init(backend_ap_policy_t *policy, int64_t boot_ms);
void backend_ap_policy_note_config_commit(
    backend_ap_policy_t *policy, uint32_t new_generation, int64_t now_ms);
void backend_ap_policy_note_backend_success(
    backend_ap_policy_t *policy, uint32_t config_generation, int64_t now_ms);
backend_ap_action_t backend_ap_policy_tick(
    backend_ap_policy_t *policy,
    backend_ap_input_t input,
    int64_t now_ms);
```

The policy stores `running`, `ap_started_ms`,
`ap_started_config_generation`, `last_success_generation`,
`last_success_ms`, and `outage_started_ms`. Starting for USB or invalid config
records the current generation. A config commit changes generation, clears
success eligibility, and restarts the outage clock. AP stop requires one
validated backend response whose generation equals the current config, whose
timestamp is at or after this AP start, and a full 30000-ms grace since that
response. A stale success from before manual AP start or from a prior
generation can never close the portal. Add those two regressions plus exact
299999/300000-ms outage and 29999/30000-ms grace boundaries.

`backend_wifi_manager` tries configured networks in order, advances after an
authentication/no-AP timeout, and returns to index zero after all four fail
with bounded exponential reconnect delay. It never changes scanner Wi-Fi
promiscuous channels because only the uplink joins infrastructure Wi-Fi.

```c
typedef enum {
    BACKEND_WIFI_EVENT_TICK = 0,
    BACKEND_WIFI_EVENT_CONNECTED,
    BACKEND_WIFI_EVENT_AUTH_FAILED,
    BACKEND_WIFI_EVENT_NO_AP,
    BACKEND_WIFI_EVENT_DISCONNECTED,
} backend_wifi_event_t;

typedef struct {
    uint32_t config_generation;
    uint8_t network_count;
    uint8_t network_index;
    uint8_t retry_exponent;
    int64_t attempt_started_ms;
    int64_t retry_after_ms;
    bool connected;
} backend_wifi_policy_t;

backend_wifi_action_t backend_wifi_policy_update(
    backend_wifi_policy_t *state,
    const backend_config_record_t *config,
    backend_wifi_event_t event,
    int64_t now_ms);
```

`test_backend_wifi_manager` proves indices 0→1→2→3 on exact auth/no-AP or
attempt-timeout events, wraps to 0 only after all configured entries, applies
the declared capped backoff, resets index/backoff on a new config generation,
and reconnects from index 0 only after the portal commit succeeds. Boundary
tests cover one and four networks and prove SSIDs/passwords are never logged.

- [ ] **Step 3: Write the portal allowlist/redaction contract test**

```python
def test_portal_is_config_only_and_redacts_secrets():
    source = PORTAL_SOURCE.read_text(encoding="utf-8")
    for route in (
        'HTTP_GET, "/"',
        'HTTP_GET, "/api/status"',
        'HTTP_GET, "/api/config"',
        'HTTP_POST, "/api/config"',
        'HTTP_POST, "/api/backend/test"',
    ):
        assert route in source
    assert "/api/ota" not in source
    assert "/firmware" not in source
    assert '"password":"%s"' not in source
    assert '"ap_password":"%s"' not in source
```

- [ ] **Step 4: Implement AP identity and endpoints**

The ESP-IDF portal creates APSTA mode, static AP address `192.168.4.1`, SSID
`FriendOrFoe-Backend-XXXXXX`, default password `friendorfoe`, channel 1, and
four clients maximum. Endpoints are exactly:

```text
GET  /
GET  /api/status
GET  /api/config
POST /api/config
POST /api/backend/test
```

`backend_portal_contract.c` owns the runtime route registry:

```c
typedef enum { BACKEND_PORTAL_GET, BACKEND_PORTAL_POST } backend_portal_method_t;
typedef enum {
    BACKEND_PORTAL_ROOT, BACKEND_PORTAL_STATUS, BACKEND_PORTAL_CONFIG_GET,
    BACKEND_PORTAL_CONFIG_POST, BACKEND_PORTAL_BACKEND_TEST
} backend_portal_route_id_t;

const backend_portal_route_t *backend_portal_routes(size_t *out_count);
bool backend_portal_route_lookup(
    backend_portal_method_t method, const char *path,
    backend_portal_route_id_t *out);
size_t backend_portal_render_redacted_config(
    const backend_config_record_t *config, char *output, size_t capacity);
```

The native route test asserts the registry contains exactly those five
method/path pairs, rejects `/api/ota` and `/firmware` at runtime, renders a
fixture whose literal Wi-Fi/AP secrets do not occur in output, parses the JSON,
and asserts SSID plus `password_set` booleans. The ESP-IDF adapter registers
routes only by iterating this registry; the Python source scan is a second
isolation check, not the primary route test.

`GET /api/config` returns SSID names and `password_set` booleans, never secret
values, plus the non-secret `auto_update_enabled` boolean. POST parses into a
candidate record, including an explicit boolean `auto_update_enabled`, validates, atomically commits,
and only then asks the Wi-Fi manager to reconnect. Backend test performs a
bounded `GET /health` and reports status without storing response secrets.
Scanner detection remains active while AP is running. USB line
`FOF_AP_START` sets the policy request flag.

The HTML labels automatic updates as a future firmware-write authorization,
defaults its checkbox off, and requires a separate confirmation checkbox when
turning it on. The POST contract rejects an enable transition unless both
booleans are explicitly true in the same authenticated AP request. Turning it
off needs no extra confirmation. Native/Python tests prove omitted fields keep
the prior value, fresh/legacy config stays false, and catalog reachability can
never toggle it.

- [ ] **Step 5: Run policy and portal tests**

Run:

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_ap_policy
pio test -e backend-native -f test_backend_portal_routes
pio test -e backend-native -f test_backend_wifi_manager
python -m pytest test/test_backend_portal_contract.py -q
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add backend-firmware/shared backend-firmware/uplink/main/network backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: add headless AP provisioning"
```

---

### Task 8: Build Complete 4096-Byte Batches, FIFO, Retry, and ACK Policies

**Files:**
- Create: `backend-firmware/shared/backend_upload_batch.h`
- Create: `backend-firmware/shared/backend_upload_batch.c`
- Create: `backend-firmware/shared/backend_upload_fifo.h`
- Create: `backend-firmware/shared/backend_upload_fifo.c`
- Create: `backend-firmware/shared/backend_http_policy.h`
- Create: `backend-firmware/shared/backend_http_policy.c`
- Create: `backend-firmware/shared/backend_ingest_ack.h`
- Create: `backend-firmware/shared/backend_ingest_ack.c`
- Create: `backend-firmware/test/test_backend_upload_batch/test_main.c`
- Create: `backend-firmware/test/test_backend_upload_fifo/test_main.c`
- Create: `backend-firmware/test/test_backend_http_policy/test_main.c`
- Create: `backend-firmware/test/test_backend_ingest_ack/test_main.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: full detections plus identity, scanner, time, queue, Wi-Fi, and LED telemetry.
- Produces: bounded JSON, FIFO ordering, retry/quarantine classification, and strict transport ACK validation.

- [ ] **Step 1: Write failing batch-boundary tests**

```c
void test_batch_flushes_before_4096_and_never_truncates(void)
{
    backend_upload_builder_t builder;
    backend_batch_context_t context = fixture_batch_context();
    backend_upload_builder_init(&builder, &context, 1000);
    backend_detection_observation_t item = fixture_full_observation();
    backend_encode_result_t result;
    do {
        result = backend_upload_builder_add(&builder, &item, 1001);
    } while (result == BACKEND_ENCODE_OK);
    TEST_ASSERT_EQUAL(BACKEND_ENCODE_NEEDS_FLUSH, result);
    backend_upload_batch_t batch = {0};
    TEST_ASSERT_TRUE(backend_upload_builder_finish(&builder, &batch));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(4096, batch.json_len);
    TEST_ASSERT_EQUAL_CHAR('}', batch.json[batch.json_len - 1]);
    TEST_ASSERT_EQUAL_CHAR('\0', batch.json[batch.json_len]);
}
```

The context fixture uses exact backend identities, legacy `device_id`, two
scanner snapshots, queue counters, Wi-Fi RSSI, LED state, and time health.

- [ ] **Step 2: Define the immutable complete-batch type and builder**

```c
#define BACKEND_UPLOAD_MAX_JSON 4096
#define BACKEND_UPLOAD_FIFO_CAPACITY 512

typedef struct {
    uint32_t sequence;
    uint16_t item_count;
    uint16_t json_len;
    uint32_t json_crc32;
    char json[BACKEND_UPLOAD_MAX_JSON + 1];
} backend_upload_batch_t;

typedef enum {
    BACKEND_ENCODE_OK,
    BACKEND_ENCODE_NEEDS_FLUSH,
    BACKEND_ENCODE_ITEM_TOO_LARGE,
    BACKEND_ENCODE_INVALID,
} backend_encode_result_t;

typedef struct {
    uint16_t depth_batches;
    uint16_t capacity_batches;
    uint32_t overflow_dropped_batches;
    uint32_t quarantined_batches;
} backend_upload_queue_telemetry_t;

typedef struct {
    uint32_t ok;
    uint32_t failed;
    uint32_t retry_count;
    bool has_last_success_age;
    uint32_t last_success_age_s;
} backend_upload_telemetry_t;

typedef struct {
    char device_id[33];
    char firmware_version[32];
    char firmware_target[40];
    char app_project[40];
    char hardware_type[40];
    char hardware_mac[18];
    char node_name[65];
    char capabilities[16][41];
    uint8_t capability_count;
    bool has_device_location;
    double device_lat;
    double device_lon;
    double device_alt;
    backend_scanner_status_t scanners[2];
    bool scanner_present[2];
    bool clock_valid;
    int64_t epoch_ms;
    char wifi_ssid[33];
    int8_t wifi_rssi;
    bool ap_active;
    uint32_t config_generation;
    uint32_t command_success_count;
    uint32_t command_failure_count;
    uint64_t uptime_ms;
    backend_led_state_t led_state;
    backend_upload_queue_telemetry_t upload_queue;
    backend_upload_telemetry_t upload;
    uint32_t sequence;
} backend_batch_context_t;

typedef struct {
    backend_batch_context_t context;
    char json[BACKEND_UPLOAD_MAX_JSON + 1];
    size_t json_len;
    uint16_t item_count;
    int64_t opened_ms;
    int64_t last_item_ms;
    bool active;
    bool failed;
} backend_upload_builder_t;

void backend_upload_builder_init(
    backend_upload_builder_t *builder,
    const backend_batch_context_t *context,
    int64_t now_ms);
backend_encode_result_t backend_upload_builder_add(
    backend_upload_builder_t *builder,
    const backend_detection_observation_t *observation,
    int64_t now_ms);
bool backend_upload_builder_tick(
    backend_upload_builder_t *builder,
    int64_t now_ms,
    backend_upload_batch_t *out);
bool backend_upload_builder_finish(
    backend_upload_builder_t *builder,
    backend_upload_batch_t *out);
```

The builder copies the context at `init`, validates
`capability_count <= 16`, and owns all partial JSON in its fixed array; no
context pointer or detection pointer may outlive the call that supplied it.
`finish` copies one immutable complete object to `out`, computes CRC32 over
exactly `json_len` bytes, then resets the builder while retaining no partial
item. Sequence is the nonzero `context.sequence` and advances only after a
successful finish.

Use `backend_json_writer`; serialize every field added in API plan 1. Emit
`node_name` plus optional `device_lat`, `device_lon`, and `device_alt` from
the validated config context. Emit actual Wi-Fi `channel` derived with the
tested 2.4/5-GHz frequency-to-channel policy, separate `freq_mhz`, separate
`channel_width_mhz`, raw and fused confidence, first/last timestamps, every
Remote ID identity/position/kinematic/accuracy/type/area/operator field, every
BLE UUID/raw/manufacturer/timing/threat field, every Wi-Fi probe/auth field,
scanner slot/mask, identity, capabilities, `upload_queue`, and health. Never
write partial JSON or silently omit a present field.

Each `scanners` member uses this exact backend heartbeat bridge (no direct
dump of the compact UART-status key names):

```json
{"uart":"ble","slot":0,"firmware_target":"scanner-s3-combo-backend","app_project":"fof_backend_scanner","hardware_type":"seeed_xiao_esp32s3","firmware_version":"0.1.0-backend","mac":"AA:BB:CC:DD:EE:01","boot_id":305419896,"profile":"ble_primary","status_sequence":12,"role_generation":4,"role_acked":true,"command_ingress":true,"radio_healthy":true,"ble_healthy":true,"wifi_healthy":false,"ota_state":"idle","rollback_state":"valid"}
```

Physical slot 0 always maps to legacy selector `uart:"ble"`; slot 1 maps to
`uart:"wifi"`, regardless of a temporary hybrid profile. All other members
come from the latest validated `backend_scanner_status_t`; never trust scanner
slot/uart text from the wire. A parsed parity test serializes both slots and
asserts every key/value, exact slot mapping, backend identity, MAC/boot/status
sequence, current role generation/ACK, command/radio health, OTA state, and
rollback state. This is the object consumed by plan 1's OTA-family gate and by
canary final-health evidence.

For each item, emit API `timestamp` from
`observation.timestamp_epoch_ms` only when `timestamp_valid` and the value is
at least `1700000000000`; do not substitute monotonic uptime. The batch-level
`timestamp` is whole epoch seconds from the validated uplink clock, or absent
when that clock is invalid. Add parsed tests proving a valid scanner
millisecond timestamp survives exactly, an invalid observation omits the item
key, and an invalid uplink clock omits the batch key so the backend's
server-received fallback applies. `first_seen_ms` and `last_updated_ms` remain
separate detector evidence and are never rewritten by this serializer.

The builder records `last_item_ms`. `backend_upload_builder_tick` returns false
through 79 ms of idleness and atomically finishes/resets a non-empty batch at
80 ms; an empty heartbeat is built and finished immediately by its dedicated
60-second caller. Adding an item that does not fit first returns
`NEEDS_FLUSH` without consuming it; the caller finishes the old batch and
retries that exact item into a fresh builder.

Add a parsed evidence-parity test. It locates the first detection object in the
batch with `backend_json_reader` and calls a test-only
`backend_assert_detection_json_equal` that names and checks every key in the
Task-3 explicit field list, including each UUID/raw array element. It also
asserts node/device location fields independently. The helper never calls the
HTTP production serializer or UART decoder, so a field omitted by either wire
path fails rather than agreeing through shared omission. Add exact 79/80-ms
idle-flush and oversized-single-item preservation cases.

The parity test additionally uses the hand-written Task-3 Wi-Fi and BLE
objects, not only `fixture_full_detection()`. It asserts `freq_mhz:2437` and
derived `channel:6` are distinct; `auth_m:0`, `wifi_generation:0`, and a JSON
array `probed_ssids` survive; and every BLE hex/unit/UUID conversion and
zero-vs-absent rule matches the table exactly. A wrong-but-schema-ignored key
such as `wifi_auth_mode`, `probe_ie_hash`, `ble_ja3_hash`,
`ble_adv_interval_us`, or `ble_service_uuids` must fail the native test.

- [ ] **Step 3: Write and implement FIFO ordering/overflow tests**

```c
void test_fifo_drops_oldest_whole_batch_and_never_reorders(void)
{
    backend_upload_batch_t storage[3];
    backend_upload_fifo_t fifo;
    backend_upload_fifo_init(&fifo, storage, 3);
    bool dropped = false;
    for (uint32_t sequence = 1; sequence <= 4; ++sequence) {
        backend_upload_batch_t batch = fixture_batch(sequence);
        TEST_ASSERT_TRUE(backend_upload_fifo_push(&fifo, &batch, &dropped));
    }
    TEST_ASSERT_TRUE(dropped);
    TEST_ASSERT_EQUAL_UINT32(1, fifo.dropped_batches);
    TEST_ASSERT_EQUAL_UINT32(2, backend_upload_fifo_peek(&fifo)->sequence);
    TEST_ASSERT_FALSE(backend_upload_fifo_pop_acked(&fifo, 3));
    TEST_ASSERT_TRUE(backend_upload_fifo_pop_acked(&fifo, 2));
    TEST_ASSERT_EQUAL_UINT32(3, backend_upload_fifo_peek(&fifo)->sequence);
}
```

Production allocates `512 * sizeof(backend_upload_batch_t)` with strict PSRAM;
if that allocation fails, health becomes fatal rather than silently reducing
the contract.

The FIFO type and ownership contract are exact:

```c
typedef struct {
    backend_upload_batch_t *storage;
    uint16_t capacity;
    uint16_t head;
    uint16_t count;
    uint32_t dropped_batches;
} backend_upload_fifo_t;

void backend_upload_fifo_init(
    backend_upload_fifo_t *fifo,
    backend_upload_batch_t *storage,
    uint16_t capacity);
bool backend_upload_fifo_push(
    backend_upload_fifo_t *fifo,
    const backend_upload_batch_t *batch,
    bool *out_dropped_oldest);
const backend_upload_batch_t *backend_upload_fifo_peek(
    const backend_upload_fifo_t *fifo);
bool backend_upload_fifo_pop_acked(
    backend_upload_fifo_t *fifo,
    uint32_t expected_sequence);
```

`init` requires non-null storage and capacity 1..512. Push copies a whole
batch; when full it removes exactly the oldest whole batch, increments
`dropped_batches`, and sets `*out_dropped_oldest=true` for that call. Peek is
borrowed only while the caller holds the queue lock. Pop succeeds only when
the current head sequence equals `expected_sequence` and never searches or
removes a later entry.

- [ ] **Step 4: Write and implement send/retry/quarantine tests**

```c
void test_http_policy_classifies_retry_and_permanent_errors(void)
{
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(false, 0, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 429, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_RETRY,
        backend_http_classify(true, 503, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_QUARANTINE,
        backend_http_classify(true, 400, false));
    TEST_ASSERT_EQUAL(BACKEND_HTTP_ACK,
        backend_http_classify(true, 200, true));
}
```

```c
typedef enum {
    BACKEND_HTTP_ACK = 0,
    BACKEND_HTTP_RETRY,
    BACKEND_HTTP_QUARANTINE,
} backend_http_disposition_t;

backend_http_disposition_t backend_http_classify(
    bool transport_complete,
    int status_code,
    bool exact_ack_valid);

typedef ssize_t (*backend_http_send_fn)(
    void *context, const void *data, size_t length);

bool backend_http_send_all(
    backend_http_send_fn send_fn,
    void *context,
    const uint8_t *data,
    size_t length);

uint32_t backend_retry_delay_ms(uint8_t exponent, uint32_t random_value);
```

The send-all test hook returns 1, 3, and remaining bytes across calls and must
still succeed. Backoff starts at 500 ms, doubles to a 60000 ms cap, and adds
deterministic jitter `random_value % (base / 4 + 1)`.

- [ ] **Step 5: Validate exact ingest acknowledgments**

```c
void test_ack_requires_device_and_transport_count(void)
{
    const char ok[] =
        "{\"status\":\"ok\",\"accepted\":2,\"processed\":1,"
        "\"deduplicated\":1,\"filtered\":0,"
        "\"device_id\":\"uplink_CB77A4\"}";
    TEST_ASSERT_TRUE(backend_ingest_ack_validate(
        ok, strlen(ok), "uplink_CB77A4", 2));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        ok, strlen(ok), "uplink_OTHER", 2));
    TEST_ASSERT_FALSE(backend_ingest_ack_validate(
        ok, strlen(ok), "uplink_CB77A4", 1));
}
```

The ACK reader requires exactly the six members `status`, `accepted`,
`processed`, `deduplicated`, `filtered`, and `device_id`, rejects duplicate or
unknown members, requires `status="ok"`, exact device/item count, nonnegative
integer counters, and `processed + deduplicated + filtered == accepted`.
Missing `filtered` is malformed rather than silently treated as zero.

Malformed 2xx responses are retryable. Permanent 4xx batches are popped into a
bounded quarantine summary `{sequence,item_count,crc32,status}` and increment
`schema_error_count`; raw credential-bearing bodies are not retained.

- [ ] **Step 6: Run upload policy tests and commit**

Run:

```bash
cd backend-firmware
pio test -e backend-native -f test_backend_upload_batch -f test_backend_upload_fifo -f test_backend_http_policy -f test_backend_ingest_ack
```

Expected: PASS under AddressSanitizer.

```bash
git add backend-firmware/shared backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: add reliable backend upload queue"
```

---

### Task 9: Implement HTTP Transport, Time, Heartbeat, and AP Success Tracking

**Files:**
- Create: `backend-firmware/uplink/main/network/backend_http_transport.h`
- Create: `backend-firmware/uplink/main/network/backend_http_transport.c`
- Create: `backend-firmware/uplink/main/network/backend_uploader.h`
- Create: `backend-firmware/uplink/main/network/backend_uploader.c`
- Create: `backend-firmware/uplink/main/network/backend_time_sync.h`
- Create: `backend-firmware/uplink/main/network/backend_time_sync.c`
- Create: `backend-firmware/uplink/main/storage/backend_firmware_buffer.h`
- Create: `backend-firmware/uplink/main/storage/backend_firmware_buffer.c`
- Create: `backend-firmware/test/test_backend_uploader_state/test_main.c`
- Create: `backend-firmware/test/test_backend_firmware_buffer/test_main.c`
- Create: `backend-firmware/test/test_backend_http_transport/test_main.c`
- Create: `backend-firmware/test/test_backend_time_sync/test_main.c`
- Create: `backend-firmware/test/test_backend_heartbeat/test_main.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: ordered Wi-Fi state, FIFO policies, `/detections/drones`, SNTP, and `/detections/time`.
- Produces: FIFO upload/dequeue, bounded retry, 60-second heartbeat, and backend-success timestamps.

- [ ] **Step 1: Write a failing uploader-state test**

```c
void test_uploader_pops_only_after_exact_ack_and_never_age_clears(void)
{
    backend_uploader_state_t state;
    backend_uploader_state_init(&state);
    backend_upload_batch_t batch = fixture_batch(8);
    backend_uploader_note_queued(&state, &batch, 1000);
    backend_uploader_note_response(
        &state, 8, BACKEND_HTTP_RETRY, 0, 2000);
    TEST_ASSERT_EQUAL_UINT32(1, state.queue_depth);
    backend_uploader_tick(&state, 3600000);
    TEST_ASSERT_EQUAL_UINT32(1, state.queue_depth);
    backend_uploader_note_response(
        &state, 8, BACKEND_HTTP_ACK, 200, 3601000);
    TEST_ASSERT_EQUAL_UINT32(0, state.queue_depth);
    TEST_ASSERT_EQUAL_INT64(3601000, state.last_backend_success_ms);
}

void test_firmware_buffer_allocates_once_and_serializes_generations(void)
{
    counting_allocator_t allocator = {0};
    backend_firmware_buffer_t buffer = {0};
    TEST_ASSERT_TRUE(backend_firmware_buffer_init_once(
        &buffer, counting_psram_alloc, &allocator));
    TEST_ASSERT_EQUAL_UINT32(1, allocator.calls);
    TEST_ASSERT_EQUAL_UINT32(
        BACKEND_FIRMWARE_BUFFER_CAPACITY, allocator.last_size);
    TEST_ASSERT_TRUE(backend_firmware_buffer_acquire(&buffer, 7));
    TEST_ASSERT_FALSE(backend_firmware_buffer_acquire(&buffer, 8));
    backend_firmware_buffer_release(&buffer, 6);
    TEST_ASSERT_FALSE(backend_firmware_buffer_acquire(&buffer, 8));
    backend_firmware_buffer_release(&buffer, 7);
    TEST_ASSERT_TRUE(backend_firmware_buffer_acquire(&buffer, 8));
    TEST_ASSERT_EQUAL_UINT32(1, allocator.calls);
}

void test_backend_time_requires_exact_single_ms_object(void)
{
    int64_t epoch_ms = 0;
    TEST_ASSERT_TRUE(backend_time_parse_response(
        "{\"ms\":1785600000123}", 20, &epoch_ms));
    TEST_ASSERT_EQUAL_INT64(1785600000123LL, epoch_ms);
    TEST_ASSERT_FALSE(backend_time_parse_response(
        "{\"ms\":1785600000123,\"x\":1}", 26, &epoch_ms));
    TEST_ASSERT_FALSE(backend_time_parse_response("1785600000123", 13, &epoch_ms));
}

void test_empty_heartbeat_is_due_only_at_60000_ms(void)
{
    backend_heartbeat_state_t heartbeat = {0};
    backend_heartbeat_init(&heartbeat, 1000);
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 60999));
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 61000));
    backend_heartbeat_mark_queued(&heartbeat, 61000);
    TEST_ASSERT_FALSE(backend_heartbeat_due(&heartbeat, 120999));
    TEST_ASSERT_TRUE(backend_heartbeat_due(&heartbeat, 121000));
}
```

`test_backend_http_transport` uses an injected scripted socket/monotonic clock
to split the status line, every header delimiter, chunk-size line, chunk body,
and terminating chunk at every byte boundary. It proves exact Host/path for
`http://host:8080/base`, partial-send completion, Content-Length and chunked
success, and rejects conflicting framing, duplicate/oversized headers,
truncated/trailing bodies, invalid chunks, JSON over 4096, binary length
mismatch, 4999/5000-ms JSON timeout, 59999/60000-ms binary total timeout, and
4999/5000-ms binary no-progress timeout. No test uses a real network.

`test_backend_time_sync` also covers duplicate/unknown `ms`, float, overflow,
pre-epoch, embedded NUL, whitespace/trailing bytes, and fallback source
selection. `test_backend_heartbeat` verifies empty batches contain the exact
identity/scanner/time/Wi-Fi/AP/queue/command/LED health fields and proves only
a validated exact ingest ACK calls the generation-bound AP-success hook.

- [ ] **Step 2: Implement URL-aware HTTP transport**

Parse the configured `http://host[:port][/base]`, resolve that host, and send
the same host/port in the HTTP `Host` header. Loop partial writes through
`backend_http_send_all`, read bounded headers/body, require a complete status
line and content length/chunk completion, and return transport status plus body
to the policy layer. JSON attempts have a five-second total timeout. Binary
downloads have a 60-second total timeout plus a five-second no-progress
timeout. Do not hardcode
`fof-server.local`.

```c
typedef bool (*backend_http_body_sink_fn)(
    void *context, const uint8_t *bytes, size_t length);

typedef enum {
    BACKEND_HTTP_ERROR_NONE = 0,
    BACKEND_HTTP_ERROR_INVALID_URL,
    BACKEND_HTTP_ERROR_DNS,
    BACKEND_HTTP_ERROR_CONNECT,
    BACKEND_HTTP_ERROR_TIMEOUT,
    BACKEND_HTTP_ERROR_SEND,
    BACKEND_HTTP_ERROR_HEADERS_TOO_LARGE,
    BACKEND_HTTP_ERROR_BODY_TOO_LARGE,
    BACKEND_HTTP_ERROR_FRAMING,
    BACKEND_HTTP_ERROR_SINK,
} backend_http_error_t;

typedef struct {
    bool transport_complete;
    int status_code;
    size_t body_length;
    backend_http_error_t error;
} backend_http_result_t;

backend_http_result_t backend_http_get_json(
    const char *base_url,
    const char *endpoint,
    char *response_body,
    size_t response_capacity);
backend_http_result_t backend_http_post_json(
    const char *base_url,
    const char *endpoint,
    const char *json,
    size_t json_length,
    char *response_body,
    size_t response_capacity);
backend_http_result_t backend_http_get_binary(
    const char *base_url,
    const char *endpoint,
    size_t expected_length,
    backend_http_body_sink_fn sink,
    void *sink_context);
```

Create the single download arena in this task so later OTA code consumes an
already-tested primitive rather than defining a forward dependency:

```c
#define BACKEND_FIRMWARE_BUFFER_CAPACITY 0x200000U

typedef void *(*backend_firmware_alloc_fn)(size_t size, void *context);

typedef struct {
    uint8_t *bytes;
    size_t capacity;
    uint32_t owner_generation;
    bool initialized;
    bool acquired;
} backend_firmware_buffer_t;

bool backend_firmware_buffer_init_once(
    backend_firmware_buffer_t *buffer,
    backend_firmware_alloc_fn psram_alloc,
    void *alloc_context);
bool backend_firmware_buffer_acquire(
    backend_firmware_buffer_t *buffer, uint32_t owner_generation);
void backend_firmware_buffer_release(
    backend_firmware_buffer_t *buffer, uint32_t owner_generation);
uint8_t *backend_firmware_buffer_data(backend_firmware_buffer_t *buffer);
size_t backend_firmware_buffer_capacity(
    const backend_firmware_buffer_t *buffer);
```

The ESP adapter calls `init_once` exactly once during uplink boot with a
strict PSRAM allocator and never allocates per request. Native tests inject a
counting allocator and prove there is exactly one 2-MiB allocation, ownership
is generation-bound, a second owner cannot acquire it, stale release cannot
unlock it, and release permits the next serialized owner without reallocating.
Allocation failure marks firmware maintenance unavailable while leaving
sensing and ordinary JSON upload operational.

JSON calls reject headers over 2048 bytes and bodies over 4096 bytes including
decoded chunked bodies. They NUL-terminate only after a complete body and never
expose partial data as success. Binary GET accepts both valid Content-Length
and chunked framing, streams each decoded chunk to the sink while updating
received length, aborts if it would exceed the manifest's exact expected
length, and succeeds only at exact length. It never allocates a buffer per
request: every firmware download sinks to this task's one fixed 2-MiB PSRAM
validation arena while incrementally computing SHA-256 and CRC32. Only after
the complete structured image validates may Task 12 call `esp_ota_write` for
self-update or copy a scanner image into `fw_scanner_be`; network receipt alone
never erases or writes a firmware partition. All three operations share URL/
Host/status/framing parsing and enforce the operation-specific deadlines above.

- [ ] **Step 3: Implement FIFO-first uploader behavior**

Define the host-testable uploader and heartbeat state instead of leaving the
test fixtures to invent private layouts:

```c
typedef struct {
    uint32_t queue_depth;
    int64_t last_backend_success_ms;
    uint32_t queued_count;
    uint32_t ack_count;
    uint32_t retry_count;
    uint32_t quarantine_count;
    uint32_t in_flight_sequence;
    uint32_t in_flight_crc32;
    int64_t next_attempt_ms;
    uint8_t retry_exponent;
    bool in_flight;
    bool in_flight_orphaned;
} backend_uploader_state_t;

typedef struct {
    int64_t last_queued_ms;
    bool initialized;
} backend_heartbeat_state_t;

void backend_uploader_state_init(backend_uploader_state_t *state);
void backend_uploader_note_queued(
    backend_uploader_state_t *state,
    const backend_upload_batch_t *batch,
    int64_t now_ms);
void backend_uploader_note_response(
    backend_uploader_state_t *state,
    uint32_t sequence,
    backend_http_disposition_t disposition,
    int status_code,
    int64_t now_ms);
void backend_uploader_tick(
    backend_uploader_state_t *state, int64_t now_ms);
void backend_heartbeat_init(
    backend_heartbeat_state_t *state, int64_t now_ms);
bool backend_heartbeat_due(
    const backend_heartbeat_state_t *state, int64_t now_ms);
void backend_heartbeat_mark_queued(
    backend_heartbeat_state_t *state, int64_t now_ms);
```

The state object is a policy mirror, not a second FIFO. Queue depth changes
only on whole-batch queue/pop/quarantine events. A response whose sequence
does not match the immutable in-flight copy is ignored; an orphaned in-flight
response updates attempt counters but cannot pop or reinsert the current head.
`backend_uploader_tick` only advances retry timing and never age-clears queue
depth. Heartbeat due is the inclusive `last_queued_ms + 60000` boundary and
`mark_queued` is called only after the empty batch actually enters the FIFO.

Detection producers can enqueue while an HTTP attempt is in flight. A short
FIFO critical section copies the current head into a dedicated 4097-byte send
buffer and records its sequence/CRC, then releases the lock before DNS/connect/
send/read. Producers use that same lock only for whole-batch push/drop-oldest.
After an ACK, the uploader reacquires the lock and pops only if the head still
has the recorded sequence/CRC; new tail work is never lost or reordered.
Retry network/timeout/408/429/5xx/malformed-2xx with the
bounded jitter policy. Quarantine 400 and other permanent 4xx summaries. Never
move a failed head batch to the tail, clear on staleness, or reboot solely for
upload failure. Expose success/fail/retry/schema/overflow counters.

The native mock-send regression blocks after its first partial write, invokes a
producer hook that pushes a second batch, resumes and ACKs the first, then
asserts sequence 1 was popped and sequence 2 remains at the head. A companion
test fills the queue during the attempt and proves the required drop-oldest
policy may remove the in-flight head from the FIFO while its immutable send
copy continues. That sequence is marked orphaned; its later ACK or failure
never pops/reinserts the new head. The newly enqueued tail remains ordered, and
the overflow counter identifies exactly the dropped oldest whole batch.

- [ ] **Step 4: Implement time and heartbeat cadence**

Start SNTP on STA connection. If SNTP remains invalid, call
`GET /detections/time`; accept exactly a JSON object
`{"ms":1785600000123}` with one integer `ms` member (unknown or duplicate
members, a bare number, floating value, seconds-scale value, overflow, or
trailing bytes fail) and then apply the vendored epoch-ms policy. `GET /health`
and command polling also use `backend_http_get_json`; firmware downloads alone
use `backend_http_get_binary`. Broadcast time to scanners every ten seconds. Build an
empty batch every 60 seconds containing identity, hardware MAC, scanner role
health, time health, Wi-Fi SSID/RSSI, AP state, queue counters, command
counters, uptime, and LED state.

Any validated ingest acknowledgment—including an empty heartbeat—sets
`last_backend_success_ms` and calls
`backend_ap_policy_note_backend_success(current_config_generation, now_ms)`;
AP policy then grants its generation-bound 30-second client grace. Backend
outage does not stop scanning, local LED classification, UART recovery, or
command ingress.

- [ ] **Step 5: Run uploader-state/buffer tests and commit**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_uploader_state -f test_backend_firmware_buffer -f test_backend_http_transport -f test_backend_time_sync -f test_backend_heartbeat`

Expected: PASS.

```bash
git add backend-firmware/uplink/main/network backend-firmware/uplink/main/storage backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: upload detections and heartbeats"
```

---

### Task 10: Integrate Deduplication, Threat Liveness, Health, and LED Mirroring

**Files:**
- Create: `backend-firmware/shared/backend_threat_policy.h`
- Create: `backend-firmware/shared/backend_threat_policy.c`
- Create: `backend-firmware/shared/backend_detection_router.h`
- Create: `backend-firmware/shared/backend_detection_router.c`
- Create: `backend-firmware/uplink/main/core/backend_health.h`
- Create: `backend-firmware/uplink/main/core/backend_health.c`
- Create: `backend-firmware/uplink/main/core/backend_coordinator.h`
- Create: `backend-firmware/uplink/main/core/backend_coordinator.c`
- Create: `backend-firmware/test/test_backend_detection_router/test_main.c`
- Create: `backend-firmware/test/test_backend_threat_policy/test_main.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: decoded scanner detections and scanner/network health.
- Produces: cross-slot dedupe, independent upload and local-threat paths, chosen LED state, and mirrored command generation.

- [ ] **Step 1: Write the non-filtering router test**

```c
void test_every_valid_detection_reaches_upload_regardless_of_led_policy(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t low_rank =
        fixture_low_rank_privacy_observation();
    backend_detection_route_result_t result =
        backend_detection_router_ingest(&router, &low_rank, 1000);
    TEST_ASSERT_TRUE(result.accepted_for_upload);
    TEST_ASSERT_TRUE(result.update_local_threat);
}

void test_cross_slot_copy_is_merged_with_slot_mask(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t first =
        fixture_drone_observation(0, -60, 1785600000100LL);
    backend_detection_observation_t second =
        fixture_drone_observation(1, -40, 1785600000200LL);
    backend_detection_route_result_t accepted =
        backend_detection_router_ingest(&router, &first, 1000);
    TEST_ASSERT_TRUE(accepted.accepted_for_upload);
    backend_detection_route_result_t merged =
        backend_detection_router_ingest(&router, &second, 1100);
    TEST_ASSERT_TRUE(merged.accepted_for_upload);
    TEST_ASSERT_EQUAL_UINT32(1,
        backend_detection_router_tick(&router, 1500));
    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(
        backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_HEX8(
        0x03, upload.detection.scanner_slots_seen);
    TEST_ASSERT_EQUAL_INT8(-40, upload.detection.rssi);
    TEST_ASSERT_EQUAL_INT64(
        1785600000100LL, upload.timestamp_epoch_ms);
    TEST_ASSERT_FALSE(
        backend_detection_router_next_upload(&router, &upload));
}

void test_scanner_observation_time_wins_and_invalid_scanner_uses_uplink(void)
{
    drone_detection_t detection = fixture_drone_detection(0, -60);
    backend_scanner_stamp_t scanner = {
        .time_valid = true,
        .observed_epoch_ms = 1785600000123LL,
    };
    backend_detection_observation_t observed = {0};
    backend_observation_resolve(
        &detection, &scanner, 1785600000999LL, &observed);
    TEST_ASSERT_TRUE(observed.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(
        1785600000123LL, observed.timestamp_epoch_ms);

    scanner.time_valid = false;
    scanner.observed_epoch_ms = 9000;
    backend_observation_resolve(
        &detection, &scanner, 1785600000999LL, &observed);
    TEST_ASSERT_TRUE(observed.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(
        1785600000999LL, observed.timestamp_epoch_ms);

    backend_observation_resolve(&detection, &scanner, 12000, &observed);
    TEST_ASSERT_FALSE(observed.timestamp_valid);
    TEST_ASSERT_EQUAL_INT64(0, observed.timestamp_epoch_ms);
}

void test_last_dedupe_bucket_flushes_at_exact_500_ms_boundary(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t item =
        fixture_drone_observation(0, -60, 1785600000100LL);
    backend_detection_route_result_t result =
        backend_detection_router_ingest(&router, &item, 1000);
    TEST_ASSERT_TRUE(result.accepted_for_upload);
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_detection_router_tick(&router, 1499));
    TEST_ASSERT_EQUAL_UINT32(
        1, backend_detection_router_tick(&router, 1500));
    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(
        backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_FALSE(
        backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_UINT32(
        0, backend_detection_router_tick(&router, 1501));
}

void test_interleaved_identities_merge_by_key_not_arrival_order(void)
{
    backend_detection_router_t router;
    backend_detection_router_init(&router);
    backend_detection_observation_t a0 = fixture_named_observation("A", 0, -60);
    backend_detection_observation_t b0 = fixture_named_observation("B", 0, -55);
    backend_detection_observation_t a1 = fixture_named_observation("A", 1, -40);
    backend_detection_router_ingest(&router, &a0, 1000);
    backend_detection_router_ingest(&router, &b0, 1100);
    backend_detection_router_ingest(&router, &a1, 1200);
    TEST_ASSERT_EQUAL_UINT32(
        2, backend_detection_router_tick(&router, 1600));
    backend_detection_observation_t upload = {0};
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_STRING("A", upload.detection.drone_id);
    TEST_ASSERT_EQUAL_HEX8(0x03, upload.detection.scanner_slots_seen);
    TEST_ASSERT_TRUE(backend_detection_router_next_upload(&router, &upload));
    TEST_ASSERT_EQUAL_STRING("B", upload.detection.drone_id);
    TEST_ASSERT_FALSE(backend_detection_router_next_upload(&router, &upload));
}
```

- [ ] **Step 2: Port only the badge-derived threat classifier**

Adapt the detection classification/live-window subset of
`badge_threat_policy.c` into `backend_threat_policy.*`. Rename symbols and
remove RGB565 colors, LCD visibility, display ranks/lanes, ticker/title/page
formatting, and every theme/display dependency. Preserve drone classification,
Meta Glasses classification, 15-second drone-SSID window, 90-second Meta
window, Remote ID liveness, and stale expiry.

```c
typedef struct {
    int64_t last_drone_ssid_ms;
    int64_t last_remote_id_ms;
    int64_t last_meta_ms;
    uint16_t drone_count;
    uint16_t meta_count;
} backend_threat_state_t;

typedef struct {
    bool drone_live;
    bool meta_live;
    uint16_t drone_count;
    uint16_t meta_count;
    int64_t drone_last_seen_age_ms;
    int64_t meta_last_seen_age_ms;
} backend_threat_snapshot_t;

void backend_threat_init(backend_threat_state_t *state);
void backend_threat_ingest(
    backend_threat_state_t *state,
    const drone_detection_t *detection,
    int64_t now_ms);
void backend_threat_snapshot(
    backend_threat_state_t *state,
    int64_t now_ms,
    backend_threat_snapshot_t *out);
```

Snapshot contains only `drone_live`, `meta_live`, counts, and last-seen ages.
An age is `-1` when that class has never been observed; otherwise it is
monotonic and nonnegative. Counts are the classifier's current live-window
counts and saturate at `UINT16_MAX`. `backend_threat_snapshot` may expire
state but does not ingest, enqueue, or mutate upload routing.

- [ ] **Step 3: Implement 500-ms cross-slot dedupe and health selection**

Use vendored detection identity/dedupe keys. Before routing, resolve the
observation timestamp through this pure interface:

```c
void backend_observation_resolve(
    const drone_detection_t *detection,
    const backend_scanner_stamp_t *scanner_stamp,
    int64_t uplink_epoch_ms,
    backend_detection_observation_t *out);
```

A scanner timestamp is valid only when `time_valid=true` and
`observed_epoch_ms >= 1700000000000`; it wins over the uplink clock. Otherwise
use the uplink epoch only when it meets that same lower bound. If neither is
valid, clear `timestamp_valid` and use zero so Task 8 omits the per-item API
timestamp. Never use monotonic uptime as epoch time. Dedupe/threat expiry uses
only `monotonic_now_ms`, never an epoch timestamp.

Merge masks and retain the strongest RSSI copy; union probed SSIDs without
truncating an existing valid record. For duplicate copies, retain the earliest
valid observation epoch while preserving the strongest record's remaining
fields. The upload builder receives the complete merged observation after the
500-ms bucket closes. The threat copy is independent and can never suppress
that upload.

The router is a bounded keyed table, not one global pending record:

```c
#define BACKEND_DEDUPE_BUCKET_CAPACITY 64
#define BACKEND_DEDUPE_READY_CAPACITY 64

typedef struct {
    bool used;
    char key[192];
    int64_t opened_monotonic_ms;
    uint64_t insertion_order;
    backend_detection_observation_t observation;
} backend_dedupe_bucket_t;

typedef struct {
    bool accepted_for_upload;
    bool update_local_threat;
    bool backpressure;
} backend_detection_route_result_t;

typedef struct {
    backend_dedupe_bucket_t buckets[BACKEND_DEDUPE_BUCKET_CAPACITY];
    backend_detection_observation_t ready[BACKEND_DEDUPE_READY_CAPACITY];
    uint8_t ready_head;
    uint8_t ready_count;
    uint64_t next_insertion_order;
} backend_detection_router_t;

void backend_detection_router_init(backend_detection_router_t *router);
backend_detection_route_result_t backend_detection_router_ingest(
    backend_detection_router_t *router,
    const backend_detection_observation_t *observation,
    int64_t monotonic_now_ms);
size_t backend_detection_router_tick(
    backend_detection_router_t *router,
    int64_t monotonic_now_ms);
bool backend_detection_router_next_upload(
    backend_detection_router_t *router,
    backend_detection_observation_t *out);
```

Keys come from the vendored dedupe-key encoder and must fit without truncation.
Tick closes every bucket aged at least 500 ms in insertion order and queues it
once; `next_upload` drains that order. On a 65th identity, close the oldest
bucket first. The coordinator drains ready items into Task 8 before every new
ingest/tick. If ready storage is nevertheless full, do not consume the new
detection: return `backpressure=true`, pause scanner detection flow, retain the
exact decoded item in its per-slot queue, drain, and retry. No bucket or upload
is silently evicted. Tests cover A/B/A interleaving, 64/65 identities, several
simultaneous expiries, ready backpressure/retry, and deterministic ordering.

The route result reports only whether the new input was consumed, whether it
updates local threat state, and whether bounded storage requires retry.
`accepted_for_upload=true` means the observation is now owned by a keyed
bucket; `backpressure=true` implies `accepted_for_upload=false` and the caller
retains the unchanged observation. Neither ingest nor tick returns an upload
payload. Tick returns the number of buckets newly moved to the ready queue;
`next_upload` is the only payload egress and returns each merged observation
exactly once. The 499/500-ms, repeated-tick, A/B/A, simultaneous-expiry, and
ready-queue tests drain only through `next_upload`, preventing a hidden
single-pending path from stranding or duplicating a detection.

`backend_health` maps two unusable scanners or fatal runtime failure to fatal;
one scanner or backend/Wi-Fi outage to degraded; then combines threat snapshot
with the LED priority from Task 5.

- [ ] **Step 4: Mirror LED state on change and every two seconds**

Coordinator increments generation on logical state change, encodes TTL 6000,
and sends the same command to each connected scanner immediately and at
two-second refresh. Equal-generation scanner acceptance is the Task-5
idempotent TTL refresh; it does not restart the pattern. A scanner status line
never fabricates threat state.

- [ ] **Step 5: Run router/threat tests and commit**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_detection_router -f test_backend_threat_policy -f test_backend_led_pattern`

Expected: PASS.

```bash
git add backend-firmware/shared backend-firmware/uplink/main/core backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: route threats without upload filtering"
```

---

### Task 11: Add Pull-Based BLE Investigation and Idempotent Results

**Files:**
- Create: `backend-firmware/shared/backend_ble_investigation.h`
- Create: `backend-firmware/shared/backend_ble_investigation.c`
- Create: `backend-firmware/test/test_backend_ble_investigation/test_main.c`
- Create: `backend-firmware/uplink/main/network/backend_command_client.h`
- Create: `backend-firmware/uplink/main/network/backend_command_client.c`
- Create: `backend-firmware/scanner/main/comms/backend_uart_rx.h`
- Create: `backend-firmware/scanner/main/comms/backend_uart_rx.c`
- Create: `backend-firmware/scanner/main/comms/backend_uart_tx.h`
- Create: `backend-firmware/scanner/main/comms/backend_uart_tx.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: plan-1 command endpoints, scanner role owner, vendored BLE investigator/protocol.
- Produces: stable command routing, bounded result sequences, retry-until-ack behavior, and cancellation.

- [ ] **Step 1: Write failing ownership/idempotency tests**

```c
void test_investigation_routes_to_current_ble_owner_and_duplicate_does_not_restart(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request =
        fixture_gatt_request("0123456789abcdef0123456789abcdef");
    TEST_ASSERT_TRUE(backend_ble_investigation_start(
        &state, request.request_id, &request, BACKEND_SCANNER_SLOT_WIFI, 1000));
    TEST_ASSERT_FALSE(backend_ble_investigation_start(
        &state, request.request_id, &request, BACKEND_SCANNER_SLOT_WIFI, 1100));
    TEST_ASSERT_EQUAL(BACKEND_SCANNER_SLOT_WIFI, state.scanner_slot);
}

void test_unacked_result_is_reposted_with_same_sequence(void)
{
    backend_ble_investigation_state_t state = completed_investigation_fixture();
    backend_command_result_t first = {0};
    backend_command_result_t retry = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &first));
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &retry));
    TEST_ASSERT_EQUAL_UINT32(first.sequence, retry.sequence);
    TEST_ASSERT_EQUAL_STRING(first.json, retry.json);
}

void test_only_matching_validated_result_ack_advances(void)
{
    backend_ble_investigation_state_t state = completed_investigation_fixture();
    backend_command_result_t result = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &result));
    TEST_ASSERT_FALSE(backend_ble_investigation_mark_acked(
        &state, "ffffffffffffffffffffffffffffffff", result.sequence));
    TEST_ASSERT_FALSE(backend_ble_investigation_mark_acked(
        &state, state.command_id, result.sequence + 1));
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, state.command_id, result.sequence));
}

void test_first_seen_cancel_emits_begin_then_cancelled_without_radio_start(void)
{
    backend_ble_investigation_state_t state;
    backend_ble_investigation_init(&state);
    ble_investigation_request_t request =
        fixture_passive_request("0123456789abcdef0123456789abcdef");
    TEST_ASSERT_TRUE(backend_ble_investigation_cancel_first_seen(
        &state, request.request_id, &request, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, state.radio_start_count);
    backend_command_result_t begin = {0};
    backend_command_result_t end = {0};
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &begin));
    TEST_ASSERT_EQUAL_UINT32(0, begin.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_begin", begin.type);
    TEST_ASSERT_TRUE(backend_ble_investigation_mark_acked(
        &state, state.command_id, begin.sequence));
    TEST_ASSERT_TRUE(backend_ble_investigation_next_result(&state, &end));
    TEST_ASSERT_EQUAL_UINT32(1, end.sequence);
    TEST_ASSERT_EQUAL_STRING("ble_inv_end", end.type);
    TEST_ASSERT_EQUAL_STRING("cancelled", end.state);
}
```

- [ ] **Step 2: Implement uplink investigation state**

```c
#define BACKEND_COMMAND_RESULT_QUEUE_CAPACITY 64
#define BACKEND_COMMAND_RESULT_MAX_JSON 512

typedef struct {
    uint32_t sequence;
    char type[24];
    char state[16];
    uint16_t json_length;
    char json[BACKEND_COMMAND_RESULT_MAX_JSON + 1];
} backend_command_result_t;

typedef struct {
    char command_id[33];
    ble_investigation_request_t request;
    backend_scanner_slot_t scanner_slot;
    bool active;
    bool terminal_queued;
    uint8_t queue_head;
    uint8_t queue_count;
    uint32_t next_sequence;
    uint32_t radio_start_count;
    backend_command_result_t queue[BACKEND_COMMAND_RESULT_QUEUE_CAPACITY];
} backend_ble_investigation_state_t;

bool backend_ble_investigation_start(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *request,
    backend_scanner_slot_t scanner_slot,
    int64_t now_ms);
bool backend_ble_investigation_accept_chunk(
    backend_ble_investigation_state_t *state,
    backend_scanner_slot_t scanner_slot,
    const ble_investigation_chunk_t *chunk);
bool backend_ble_investigation_next_result(
    backend_ble_investigation_state_t *state,
    backend_command_result_t *out);
bool backend_ble_investigation_mark_acked(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    uint32_t result_sequence);
bool backend_ble_investigation_cancel_first_seen(
    backend_ble_investigation_state_t *state,
    const char *command_id,
    const ble_investigation_request_t *original_request,
    int64_t now_ms);
```

Use the current `backend_scanner_ble_owner(plan)`, not a permanent slot. Enforce
the vendored limits: 16 services, 32 characteristics, 8 reads, 128 value-hex
characters, and 12000 ms. Duplicate command IDs return current state; a new
command conflicts while one is active.

The state is application-static (never a task-stack local on device). Its
64-entry queue holds the worst valid stream: begin, at most five monotonic
progress states, 16 services, 32 characteristics, 8 reads, and one terminal
event (63 total), while one head event remains pending HTTP ACK. Each accepted
chunk is encoded once into at most 512 bytes and gets one stable sequence;
retry returns the byte-identical head. Scanner/UART receive never waits on
HTTP. The 64th slot is reserved for a terminal `failed/result_overflow` event:
an extra progress rank, evidence beyond a declared limit, or an oversized
encoding is rejected and closes the command through that reserved event
without overwriting queued evidence. Tests fill the complete 63-event valid
stream, hold sequence 0 unacked while all later chunks arrive, replay it, then
ACK/drain every sequence in order; a 64th nonterminal chunk produces only the
reserved terminal failure and never writes past the array.

- [ ] **Step 3: Implement command polling/result posting**

Poll `GET /nodes/{device_id}/commands/next` every five seconds. HTTP 204 means
idle. Parse by the exact plan-1 discriminator. For `ble_investigate`, require
the same exact 32-hex `command_id` and `request_id`, known mode, bounded
timeout, and a canonical MAC in envelope field `target` for GATT (or JSON null
for passive capture); map only that `target` field to the Task-4 scanner wire
field `mac`. For `ble_investigate_cancel`, require exactly the complete eight
fields including persisted progress: `command_id`, `type`,
matching `request_id`, original `mode`, original `target`, original
`timeout_ms`, `next_sequence`, and `result_state`, with the same
validation/mapping rules. Investigate envelopes require those same two
persisted progress fields.
Reject unknown, missing, duplicate, or extra members. If cancellation is the
first envelope seen for that ID, queue a sequence-0 `ble_inv_begin` and then a
sequence-1 cancelled `ble_inv_end` without starting scanner/radio work; each
remains stable until its own ACK. If the investigation is already active,
retain its original request in uplink state and send only the exact Task-4
scanner line `{"type":"cancel","command_id":"<32hex>"}` to its current
BLE-owner slot; never add API target/mode/timeout to the scanner cancel line.
Retain the same ordered result stream. Round-trip tests assert API `target`
becomes scanner `mac`, scanner chunks restore API `request_id`, and neither
active nor first-seen cancellation invents a `target_mac` key. Send the
translated scanner command to the current BLE owner and post each result to
`POST /nodes/{device_id}/commands/{command_id}/result` with stable sequence.
Retry network/408/429/5xx; quarantine permanent validation errors in command
telemetry; only advance after the backend idempotent ACK.

On a fresh local state, `next_sequence==0` follows normal start/first-seen
cancel behavior. If persisted `next_sequence>0`, do not restart radio work or
replay sequence zero: reconstruct the original request, set local sequencing
to the supplied value, and emit exactly one terminal `ble_inv_end` at that
sequence. An investigate ends `failed` with `error:"device_restarted"`; a
cancel-pending envelope ends `cancelled`. Its prior begin is already durable on
the backend. Tests simulate reboot after sequences 1 and 37, require the exact
resume sequence/result state, and prove the active key clears after ACK.

The result ACK body matches plan 1 exactly: `ok:true`, the same 32-hex
`command_id`, `accepted_sequence` equal to the pending sequence,
`next_sequence == accepted_sequence + 1`, a known `result_state`, boolean
`terminal`, and boolean `duplicate`. The command client validates every member
with the local JSON reader before calling
`backend_ble_investigation_mark_acked`. Wrong ID, wrong accepted/next sequence,
`ok:false`, an unknown state, duplicate JSON keys, malformed 2xx, or an ACK for
an older result leaves the stable result pending. `duplicate:true` is valid
only for a byte-identical replay of the current pending result. `mark_acked`
itself rechecks ID/sequence and returns whether it advanced, making an
already-acked HTTP replay idempotent without acknowledging the next result.

- [ ] **Step 4: Wire scanner command ingress and vendored investigator**

Scanner ingress accepts role/time/flow/LED/investigate/cancel/health/recovery/
OTA only. It emits bounded `ble_inv_begin`, progress, service, characteristic,
read, and end chunks while retaining command ingress during flow control. GATT
authentication requirements and truncation flags pass through unchanged.

- [ ] **Step 5: Run tests and commit**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_ble_investigation`

Expected: PASS.

```bash
git add backend-firmware/shared backend-firmware/uplink/main/network backend-firmware/scanner/main/comms backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: add backend BLE investigations"
```

---

### Task 12: Enforce Backend-Only OTA, Scanner Relay, and Rollback

**Files:**
- Create: `backend-firmware/shared/backend_ota_identity.h`
- Create: `backend-firmware/shared/backend_ota_identity.c`
- Create: `backend-firmware/test/test_backend_ota_identity/test_main.c`
- Create: `backend-firmware/test/test_backend_scanner_relay/test_main.c`
- Create: `backend-firmware/test/test_backend_rollback_policy/test_main.c`
- Create: `backend-firmware/test/test_backend_ota_maintenance/test_main.c`
- Create: `backend-firmware/test/test_backend_ota_journal/test_main.c`
- Create: `backend-firmware/scanner/main/comms/uart_ota.h`
- Create: `backend-firmware/scanner/main/comms/uart_ota.c`
- Create: `backend-firmware/scanner/main/core/scanner_rollback.h`
- Create: `backend-firmware/scanner/main/core/scanner_rollback.c`
- Create: `backend-firmware/uplink/main/storage/backend_firmware_store.h`
- Create: `backend-firmware/uplink/main/storage/backend_firmware_store.c`
- Create: `backend-firmware/uplink/main/storage/backend_ota_journal.h`
- Create: `backend-firmware/uplink/main/storage/backend_ota_journal.c`
- Create: `backend-firmware/uplink/main/ota/backend_self_ota.h`
- Create: `backend-firmware/uplink/main/ota/backend_self_ota.c`
- Create: `backend-firmware/uplink/main/ota/backend_scanner_relay.h`
- Create: `backend-firmware/uplink/main/ota/backend_scanner_relay.c`
- Create: `backend-firmware/uplink/main/ota/backend_ota_maintenance.h`
- Create: `backend-firmware/uplink/main/ota/backend_ota_maintenance.c`
- Modify: `backend-firmware/platformio.ini`

**Interfaces:**
- Consumes: plan-1 catalog metadata/images and vendored image/version/relay primitives.
- Produces: exact backend admission, SHA/CRC/size/version checks, staged scanner relay, pending-verify rollback, and USB-triggered no-write/apply maintenance evidence for the hardware canary.

- [ ] **Step 1: Write the full rejection matrix first**

```c
void test_backend_ota_rejects_every_cross_family_identity(void)
{
    backend_ota_manifest_t scanner = valid_scanner_manifest();
    TEST_ASSERT_EQUAL(BACKEND_OTA_ADMIT,
        backend_ota_manifest_admit(
            &scanner, BACKEND_IMAGE_SCANNER, "0.0.9-backend", 0x200000));
    strcpy(scanner.target, "scanner-s3-combo-fof_badge");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_IDENTITY,
        backend_ota_manifest_admit(
            &scanner, BACKEND_IMAGE_SCANNER, "0.0.9-backend", 0x200000));
    scanner = valid_scanner_manifest();
    strcpy(scanner.project, "fof_scanner_seed");
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_IDENTITY,
        backend_ota_manifest_admit(
            &scanner, BACKEND_IMAGE_SCANNER, "0.0.9-backend", 0x200000));
    scanner = valid_scanner_manifest();
    scanner.image_size = 0x200001;
    TEST_ASSERT_EQUAL(BACKEND_OTA_REJECT_CAPACITY,
        backend_ota_manifest_admit(
            &scanner, BACKEND_IMAGE_SCANNER, "0.0.9-backend", 0x200000));
}
```

Extend that test with this exact rejection matrix:

| Mutation from `valid_scanner_manifest()` | Expected result |
|---|---|
| `hardware="xiao_esp32s3"` | reject |
| empty SHA or any non-hex SHA byte | reject |
| `generation=0` | reject |
| older version than the running image | reject |
| same version with `allow_same_version=false` | reject |
| embedded app project other than `fof_backend_scanner` | reject |
| embedded version other than the manifest version | reject |
| missing or duplicate structured `.fof_backend_identity` record | reject |
| structured record CRC/image kind/target/project/hardware mismatch | reject |
| `image_size > partition_capacity` | reject |

Add one acceptance case for an exact newer scanner image and one same-version
recovery case with `allow_same_version=true`; the recovery case still requires
all identity, digest, size, descriptor, and marker checks.

Add metadata-decoder cases proving a missing, duplicate, negative, fractional,
or greater-than-`UINT32_MAX` `crc32` member fails, while an explicitly present
zero is accepted. Add a structurally valid fixture whose computed whole-image
CRC32 is zero and prove image validation admits it only when the manifest also
contains zero; CRC validity is exact equality, never nonzero truthiness.

In `test_backend_ota_maintenance`, add these failing policy cases before the
ESP adapter exists:

```c
void test_probe_validates_complete_image_without_flash_writes(void)
{
    backend_ota_maintenance_t *state = maintenance_fixture();
    uint32_t before = backend_ota_image_write_count(state);
    backend_ota_evidence_t evidence = {0};
    TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
        state, BACKEND_OTA_COMPONENT_SCANNER0,
        "scanner-s3-combo-backend", fixture_scanner_sha(), &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_ADMIT, evidence.decision);
    TEST_ASSERT_EQUAL_UINT32(before, evidence.image_writes_before);
    TEST_ASSERT_EQUAL_UINT32(before, evidence.image_writes_after);
    TEST_ASSERT_TRUE(evidence.complete_image_validated);
}

void test_cross_family_probe_rejects_before_download_or_write(void)
{
    backend_ota_maintenance_t *state = maintenance_fixture();
    backend_ota_evidence_t evidence = {0};
    TEST_ASSERT_TRUE(backend_ota_maintenance_run_probe(
        state, BACKEND_OTA_COMPONENT_SCANNER0,
        "scanner-s3-combo-fof_badge", NULL, &evidence));
    TEST_ASSERT_EQUAL(BACKEND_OTA_DECISION_REJECT_IDENTITY, evidence.decision);
    TEST_ASSERT_EQUAL_UINT32(0, fixture_download_count());
    TEST_ASSERT_EQUAL_UINT32(0, evidence.image_writes_after);
}

void test_same_version_apply_requires_explicit_recovery_mode(void)
{
    backend_ota_maintenance_t *state = maintenance_fixture();
    backend_ota_request_t request = fixture_uplink_apply_request();
    request.apply_mode = BACKEND_OTA_NEWER_ONLY;
    TEST_ASSERT_FALSE(backend_ota_maintenance_request_apply(state, &request));
    request.apply_mode = BACKEND_OTA_SAME_VERSION_RECOVERY;
    TEST_ASSERT_TRUE(backend_ota_maintenance_request_apply(state, &request));
}
```

Also prove a scanner probe relays the complete image into scanner PSRAM and
receives the scanner's validated dry-run receipt without calling any scanner
partition erase/write function; malformed USB lines, wildcard SHA on apply,
parallel operations, changed catalog SHA, wrong component, and replayed
operation generation all fail closed.

Add a post-validation/pre-mutation fixture hook that changes exactly one live
binding member after the complete image has validated: target MAC, target boot
ID, or topology generation. Run it for uplink, scanner 0, and scanner 1. Every
case must return `REJECT_TARGET_BINDING`, leave self-OTA, scanner-store,
scanner-relay, partition-select, erase, and image-write counters unchanged,
write no `accepted` journal record, and emit no
`FOF_BACKEND_OTA_ACCEPTED`. A scanner-slot swap with the same image and a
reboot during download are separate regressions. A matching control proves
exactly one journal acceptance precedes the first mutation. These tests define
the TOCTOU boundary; validation only at command parse or download start is
insufficient.

- [ ] **Step 2: Implement immutable manifest admission**

```c
typedef struct {
    char target[40];
    char project[40];
    char hardware[40];
    char version[32];
    uint32_t image_size;
    uint32_t crc32;
    char sha256[65];
    uint32_t generation;
    bool allow_same_version;
} backend_ota_manifest_t;

typedef enum {
    BACKEND_OTA_ADMIT = 0,
    BACKEND_OTA_REJECT_ARGUMENT,
    BACKEND_OTA_REJECT_IDENTITY,
    BACKEND_OTA_REJECT_VERSION,
    BACKEND_OTA_REJECT_DIGEST,
    BACKEND_OTA_REJECT_SIZE,
    BACKEND_OTA_REJECT_GENERATION,
    BACKEND_OTA_REJECT_CAPACITY,
} backend_ota_admission_result_t;

typedef enum {
    BACKEND_OTA_IMAGE_OK = 0,
    BACKEND_OTA_IMAGE_READ_ERROR,
    BACKEND_OTA_IMAGE_FORMAT_ERROR,
    BACKEND_OTA_IMAGE_DIGEST_MISMATCH,
    BACKEND_OTA_IMAGE_CRC_MISMATCH,
    BACKEND_OTA_IMAGE_DESCRIPTOR_MISMATCH,
    BACKEND_OTA_IMAGE_IDENTITY_MISMATCH,
} backend_ota_image_result_t;

backend_ota_admission_result_t backend_ota_manifest_admit(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    const char *running_version,
    size_t partition_capacity);

typedef bool (*backend_ota_read_fn)(
    void *context, size_t offset, uint8_t *output, size_t length);

backend_ota_image_result_t backend_ota_image_validate(
    const backend_ota_manifest_t *manifest,
    backend_image_kind_t expected_kind,
    backend_ota_read_fn read_fn,
    void *read_context);
```

`backend_ota_admission_result_t` names identity, version, digest, size,
generation, and capacity rejection separately. Require exact backend manifest
identity, newer version (or exact same version only with
`allow_same_version=true`), nonzero size/generation, a present unsigned
32-bit CRC32 (including the valid value zero), 64 hex SHA, and capacity before
erase/write.

`backend_ota_image_validate` streams exactly `manifest.image_size` bytes through
the reader, computes SHA-256 and CRC32, parses the ESP image/app descriptor, and
requires exact project/version. It scans every image-byte offset for
`FOF_BACKEND_IDENTITY_MAGIC`, requires exactly one 164-byte structured record,
validates its CRC/schema/image kind, and requires target/project/hardware/
version to agree with both manifest and app descriptor. It rejects short reads,
trailing bytes, malformed segment bounds, digest mismatch, missing/duplicate
record, or any arbitrary string marker without a valid record. Native tests use
fragmented readers (1, 7, 511 bytes), read failure at every image region, and
the full mutation matrix.

- [ ] **Step 3: Port scanner UART OTA without legacy admission**

Adapt the proven generation/session/chunk CRC/selective NACK logic into the
local `uart_ota.c`. Remove legacy-ready and cross-target branches. Scanner
stages into PSRAM, verifies immutable manifest and complete image, writes only
the inactive 2-MB slot, sets pending verify, and reboots.

The relay state enum is exactly `IDLE`, `QUIET_REQUESTED`, `BEGIN_SENT`,
`STREAMING`, `IMAGE_STAGED`, `END_SENT`, `REBOOT_WAIT`,
`CONVERGENCE_WAIT`, `COMPLETE`, and `FAILED`. A session binds slot, old boot
ID, expected MAC, manifest generation/digest/size, chunk sequence, and retry
count. Each chunk is acknowledged by matching session ID and sequence; NACK
retries the same immutable bytes up to three times, timeout retries up to
three, and an out-of-order/wrong-session receipt cannot advance. `ota_staged`
is accepted only after all bytes; `ota_done` moves to reboot wait. Changed boot
then exact status identity/version, command ingress, current role ACK/profile/
radio health, and rollback-valid state are all required for completion.
Tests cover every illegal transition and stale receipt.

- [ ] **Step 4: Implement backend firmware store and relay convergence**

Uplink stages only `scanner-s3-combo-backend` into partition `fw_scanner_be`.
Relay binds session, generation, CRC32, SHA-256, expected scanner MAC and slot.
After reboot it requires changed boot ID plus exact target/project/hardware/
version, command ingress, role acknowledgment, correct radio profile, and
rollback-clear health before success. Relay one scanner at a time.

```c
bool backend_scanner_relay_can_begin(
    backend_scanner_slot_t slot,
    const backend_ota_manifest_t *manifest,
    const uint8_t expected_mac[6],
    uint32_t generation);
```

- [ ] **Step 5: Implement uplink self-OTA and rollback health**

Uplink accepts only `uplink-s3-backend`. Mark the pending image valid only
after NVS/config loads, LED/UART/coordinator workers run, and either AP or STA
is healthy. Backend availability is not required to clear rollback. Scanner
clears rollback only after role command, command ingress, and required radio
health.

Before any self-OTA boot-partition switch or scanner apply begins, persist one
CRC-protected canonical `backend_ota_journal` NVS record. Its logical fields
are exact, and its encoder writes fixed-width integers little-endian with no
raw-struct padding:

```c
typedef enum {
    BACKEND_OTA_COMPONENT_UPLINK = 0,
    BACKEND_OTA_COMPONENT_SCANNER0,
    BACKEND_OTA_COMPONENT_SCANNER1,
} backend_ota_component_t;

typedef enum {
    BACKEND_OTA_NEWER_ONLY = 0,
    BACKEND_OTA_SAME_VERSION_RECOVERY,
} backend_ota_apply_mode_t;

typedef enum {
    BACKEND_OTA_PHASE_ACCEPTED = 0,
    BACKEND_OTA_PHASE_WRITING,
    BACKEND_OTA_PHASE_REBOOT_PENDING,
    BACKEND_OTA_PHASE_CONVERGENCE_PENDING,
    BACKEND_OTA_PHASE_COMPLETE,
    BACKEND_OTA_PHASE_FAILED,
} backend_ota_phase_t;

typedef struct {
    uint32_t schema;
    uint32_t operation_id;
    backend_ota_component_t component;
    int8_t component_slot;
    backend_ota_apply_mode_t apply_mode;
    char catalog_name[40];
    backend_ota_manifest_t manifest;
    uint8_t uplink_mac[6];
    uint32_t uplink_boot_id;
    uint8_t expected_target_mac[6];
    uint8_t actual_target_mac[6];
    uint32_t expected_target_boot_id;
    uint32_t actual_target_boot_id;
    uint32_t expected_topology_generation;
    uint32_t actual_topology_generation;
    backend_ota_phase_t phase;
    uint32_t image_writes_before;
    uint32_t image_writes_after;
    uint32_t boot_id_after;
    bool rollback_clear;
    bool converged;
    uint32_t record_crc32;
} backend_ota_journal_record_t;
```

`schema` is 1. `component_slot` is `-1` for uplink, 0 for scanner 0, and 1
for scanner 1. The manifest supplies target/project/hardware/version,
SHA-256, whole-image CRC32/size, and catalog generation. All six expected/
actual target fields are mandatory for every apply. For uplink they bind the
uplink itself; for a scanner they bind the validated status for that physical
slot. Phases are `accepted`, `writing`, `reboot_pending`,
`convergence_pending`, `complete`, or `failed`; every transition is committed
before the destructive step it describes. The journal uses the safe Task-6
NVS adapter and never erases NVS.

Immediately before the first write, USB emits and flushes one bounded
`FOF_BACKEND_OTA_ACCEPTED` JSON line with schema, operation ID, component,
component slot, SHA-256, CRC32, `uplink_mac`, and the complete expected/actual
target binding. On uplink reboot, boot loads and validates
the journal before accepting another operation, resumes only the recorded
post-write health/convergence checks (never repeats download/write), and after
rollback clearance stores phase `complete` plus the final boot ID/write
counters and emits the final `FOF_BACKEND_OTA_EVIDENCE`. A failed CRC,
identity, rollback, or boot check stores/advertises `failed` and blocks OTA.
`FOF_BACKEND_OTA_STATUS` re-emits the durable complete/failed evidence after
USB re-enumeration; the record is replaced only when a later separately
authorized operation begins. Native power-cut tests restart after every phase
and prove no phase can repeat an image write, lose operation identity, or
report applied before rollback-clear convergence.

Its wire form has exactly these keys and no others:

```text
FOF_BACKEND_OTA_ACCEPTED {"schema":1,"operation_id":7,"component":"scanner0","component_slot":0,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","crc32":305419896,"uplink_mac":"AA:BB:CC:DD:EE:FF","expected_target_mac":"AA:BB:CC:DD:EE:01","actual_target_mac":"AA:BB:CC:DD:EE:01","expected_target_boot_id":305419896,"actual_target_boot_id":305419896,"expected_topology_generation":4,"actual_topology_generation":4,"boot_id_before":305419896}
```

Require schema 1, nonzero operation/target boot IDs/topology generations, the
component-to-slot mapping above, a known component, canonical 64-lowercase-hex
SHA, unsigned CRC32 including zero, and uppercase canonical MACs.
`boot_id_before` equals `actual_target_boot_id`. Firmware and canary tests
reject extra/missing/duplicate keys or any expected/actual mismatch.

The ordering is a hard mutation gate. After complete-image validation in the
fixed PSRAM arena, the worker takes the firmware-store/topology lock, reads one
atomic `backend_ota_target_binding_t`, and compares component, slot, MAC, boot
ID, and topology generation with the authorized expected values. It retains
the lock/target claim across this comparison and the first mutation. Only a
match may persist phase `accepted`; only after that durable commit may it emit
and flush `FOF_BACKEND_OTA_ACCEPTED`; only after the flush may it persist
`writing` and call any scanner-store copy, relay begin, self-OTA
begin/write/end, partition-select, erase, or boot-selection adapter. A
mismatch emits only non-accepted failure evidence and performs zero firmware
mutations. Automatic newer-only OTA captures its expected binding atomically
at operation admission and performs the same second snapshot/compare
immediately before mutation.

Adapt the pinned `scanner_rollback.c/.h` into the local files and replace its
old UART reporting with the Task-4 status codec. Its pure readiness input
contains `uptime_ms`, `command_ingress_healthy`, `current_boot_role_acked`,
`reported_profile`, `required_radio_healthy`, and watchdog-ready mask. Add a
regression proving 60000 ms of uptime alone does **not** mark valid; validity
requires all current-boot role/ingress/radio/watchdog conditions. Resetting the
boot ID resets readiness.

After network acquisition, uplink polls
`GET /nodes/firmware/latest/uplink-s3-backend` and
`GET /nodes/firmware/latest/scanner-s3-combo-backend` immediately and every
1800000 ms. Metadata must contain exact name/target/project/hardware/version/
size/SHA/CRC32/download URL; download uses
`GET /nodes/firmware/download/{exact-backend-target}` through the binary
streaming API, ignoring an untrusted alternate host in metadata. Retryable
metadata/download failures back off 5000, 10000, 20000, 40000, 80000,
160000, then 300000 ms, then resume
the 1800000-ms cadence after success/no-update. Only one self download or one
scanner relay may own the firmware-store lock; scanners relay one at a time.

Polling is always read-only while config `auto_update_enabled=false` (the
default): it may validate metadata and report `update_available`, but it does
not download to an OTA destination, erase, write, select a boot partition, or
start scanner relay. Only the USB maintenance command can perform a
challenge-gated canary apply in that state. When an operator explicitly
enables the AP setting after acceptance/soak, the automatic worker may admit
and apply strictly newer backend images; it never uses same-version recovery.
Tests begin with a newer catalog image and prove zero firmware-buffer
acquisitions/image-write calls/relay sessions while false, then prove one
newer-only operation after the explicit false-to-true config generation.

- [ ] **Step 6: Add the fail-closed USB maintenance trigger and evidence record**

The AP remains configuration-only. Physical canary OTA control is available
only on the uplink USB console through these exact newline commands:

```text
FOF_BACKEND_OTA_PROBE <uplink|scanner0|scanner1> <catalog-name> <64hex-sha|*>
FOF_BACKEND_OTA_APPLY <uplink|scanner0|scanner1> <64hex-backend-sha> <newer-only|same-version-recovery> <expected-mac> <expected-boot-id> <expected-topology-generation>
FOF_BACKEND_OTA_STATUS
```

`*` is legal only for a no-write probe. Apply always derives the exact backend
catalog name from the component and requires the caller's expected SHA to
match metadata before download. `same-version-recovery` is never used by the
automatic poller; it is accepted only from this USB command while no other OTA
operation owns the firmware-store lock.

Apply parsing requires an uppercase canonical MAC and nonzero expected boot ID
and topology generation. The component implies slot `-1/0/1`; there is no
wildcard target binding. `backend_ota_target_binding_matches` returns true only
when actual component and implied slot match and all three caller-supplied
expected values equal the atomic live snapshot. It performs no mutation.

Create these pure interfaces before the ESP USB adapter:

```c
typedef struct backend_ota_maintenance backend_ota_maintenance_t;

typedef struct {
    backend_ota_component_t component;
    int8_t component_slot;
    uint8_t target_mac[6];
    uint32_t target_boot_id;
    uint32_t topology_generation;
} backend_ota_target_binding_t;

typedef struct {
    bool probe;
    backend_ota_component_t component;
    char catalog_name[40];
    char expected_sha256[65];
    backend_ota_apply_mode_t apply_mode;
    uint8_t expected_mac[6];
    uint32_t expected_boot_id;
    uint32_t expected_topology_generation;
} backend_ota_request_t;

typedef enum {
    BACKEND_OTA_DECISION_ADMIT = 0,
    BACKEND_OTA_DECISION_NO_UPDATE,
    BACKEND_OTA_DECISION_REJECT_IDENTITY,
    BACKEND_OTA_DECISION_REJECT_VERSION,
    BACKEND_OTA_DECISION_REJECT_DIGEST,
    BACKEND_OTA_DECISION_REJECT_SIZE,
    BACKEND_OTA_DECISION_REJECT_CAPACITY,
    BACKEND_OTA_DECISION_REJECT_BUSY,
    BACKEND_OTA_DECISION_REJECT_TARGET_BINDING,
    BACKEND_OTA_DECISION_APPLIED,
    BACKEND_OTA_DECISION_FAILED,
} backend_ota_decision_t;

typedef struct {
    uint32_t operation_id;
    bool probe;
    backend_ota_component_t component;
    uint8_t uplink_mac[6];
    char catalog_name[40];
    backend_ota_manifest_t manifest;
    backend_ota_decision_t decision;
    size_t partition_capacity;
    int8_t component_slot;
    uint8_t expected_target_mac[6];
    uint8_t actual_target_mac[6];
    uint32_t expected_target_boot_id;
    uint32_t actual_target_boot_id;
    uint32_t expected_topology_generation;
    uint32_t actual_topology_generation;
    bool complete_image_validated;
    uint32_t image_writes_before;
    uint32_t image_writes_after;
    uint32_t boot_id_before;
    uint32_t boot_id_after;
    bool rollback_clear;
    bool converged;
} backend_ota_evidence_t;

bool backend_ota_maintenance_parse_usb(
    const char *line, size_t length, backend_ota_request_t *out);
bool backend_ota_target_binding_matches(
    const backend_ota_request_t *request,
    const backend_ota_target_binding_t *actual);
bool backend_ota_maintenance_run_probe(
    backend_ota_maintenance_t *state,
    backend_ota_component_t component,
    const char *catalog_name,
    const char *expected_sha256_or_null,
    backend_ota_evidence_t *out);
bool backend_ota_maintenance_request_apply(
    backend_ota_maintenance_t *state,
    const backend_ota_request_t *request);
size_t backend_ota_evidence_encode(
    const backend_ota_evidence_t *evidence, char *output, size_t capacity);
```

Evidence wire strings are exact. `component` is `"uplink"`, `"scanner0"`, or
`"scanner1"`. `mode` is `"probe"` for a probe and otherwise
`"newer-only"` or `"same-version-recovery"`. Decision mapping is
`ADMIT→"admit"`, `NO_UPDATE→"no_update"`,
`REJECT_IDENTITY→"reject_identity"`, `REJECT_VERSION→"reject_version"`,
`REJECT_DIGEST→"reject_digest"`, `REJECT_SIZE→"reject_size"`,
`REJECT_CAPACITY→"reject_capacity"`, `REJECT_BUSY→"reject_busy"`,
`REJECT_TARGET_BINDING→"reject_target_binding"`,
`APPLIED→"applied"`, and `FAILED→"failed"`. Firmware encoder and release
parser tests cover every value and reject unknown or case-variant spellings.

A probe fetches catalog metadata through plan 1, performs family/version/size/
capacity admission, acquires the exact Task-9 fixed 2-MiB PSRAM firmware
buffer, downloads the admitted complete image into it, and runs the same image
validator as apply. A missing arena returns `decision:"failed"`, exposes maintenance as
unavailable in health, and leaves sensing running without weakening admission.
For a scanner it uses the normal serialized
relay with a `dry_run=true` begin flag; the scanner validates the complete
PSRAM image and returns a receipt without calling `esp_ota_begin`, erase,
write, end, or boot-partition APIs. The uplink probe likewise validates from
PSRAM. Wrap every real self/scanner partition erase/write entry so
`image_writes_before/after` count image mutation calls. A successful probe
requires equal counters and unchanged boot IDs.

Every request emits one compact line prefixed `FOF_BACKEND_OTA_EVIDENCE `.
The JSON object contains exactly `schema`, `operation_id`, `mode`, `component`,
`component_slot`, `uplink_mac`, `expected_target_mac`, `actual_target_mac`,
`expected_target_boot_id`, `actual_target_boot_id`,
`expected_topology_generation`, `actual_topology_generation`,
`catalog_name`, `target`, `project`, `hardware`, `version`, `sha256`, `crc32`,
`size`, `partition_capacity`, `allow_same_version`, `decision`,
`complete_image_validated`, `image_writes_before`, `image_writes_after`,
`boot_id_before`, `boot_id_after`, `rollback_clear`, and `converged`. Probe
success is `decision:"admit"`, complete-image validation true, equal write
counters, equal boot IDs, and no rollback transition. Apply success is not
reported until the exact post-reboot identity, role/radio/command health,
rollback clearance, and scanner relay convergence checks already defined in
this task pass. `FOF_BACKEND_OTA_STATUS` repeats the last immutable evidence
record and cannot start work.

For a probe, maintenance snapshots the addressed live target at probe start
and copies that binding into both expected and actual evidence fields; this is
diagnostic and grants no later apply authority. For an apply, evidence retains
the caller/admission snapshot as `expected_*` and the final mutation-gate
snapshot as `actual_*`, including on rejection. `component_slot` follows the
`-1/0/1` mapping in journal, ACCEPTED, and evidence. Encoder tests cover
uplink and both scanner slots, swapped MACs, changed boots, topology drift,
canonical MAC/hex spelling, exact key order/set, and zero mutation counters on
`reject_target_binding`.

- [ ] **Step 7: Run OTA/relay tests and commit**

Run: `cd backend-firmware && pio test -e backend-native -f test_backend_ota_identity -f test_backend_scanner_relay -f test_backend_rollback_policy -f test_backend_ota_maintenance -f test_backend_ota_journal`

Expected: PASS.

```bash
git add backend-firmware/shared backend-firmware/scanner/main backend-firmware/uplink/main/storage backend-firmware/uplink/main/ota backend-firmware/test backend-firmware/platformio.ini
git commit -m "backend-fw: enforce backend-only OTA recovery"
```

---

### Task 13: Compose the Scanner and Uplink Applications and Build Both Images

**Files:**
- Create: `backend-firmware/scanner/CMakeLists.txt`
- Create: `backend-firmware/scanner/platformio.ini`
- Create: `backend-firmware/scanner/sdkconfig.defaults`
- Create: `backend-firmware/scanner/partitions_backend_scanner_8mb.csv`
- Create: `backend-firmware/scanner/main/CMakeLists.txt`
- Create: `backend-firmware/scanner/main/Kconfig.projbuild`
- Create: `backend-firmware/scanner/main/main.c`
- Create: `backend-firmware/uplink/CMakeLists.txt`
- Create: `backend-firmware/uplink/platformio.ini`
- Create: `backend-firmware/uplink/sdkconfig.defaults`
- Create: `backend-firmware/uplink/partitions_backend_uplink_8mb.csv`
- Create: `backend-firmware/uplink/main/CMakeLists.txt`
- Create: `backend-firmware/uplink/main/main.c`
- Create: `backend-firmware/tools/tests/test_backend_build_contract.py`
- Modify: `backend-firmware/README.md`

**Interfaces:**
- Consumes: all pure policies and ESP-IDF adapters from Tasks 1-12.
- Produces: `scanner-s3-combo-backend` and `uplink-s3-backend` images with exact ESP app descriptors.

- [ ] **Step 1: Write build-contract tests before the projects exist**

Create `tools/tests/test_backend_build_contract.py` with exact assertions:

```python
from configparser import ConfigParser
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def load_ini(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    parser = ConfigParser(interpolation=None)
    parser.read_string(text)
    assert parser.sections()
    return text


def test_platformio_environments_and_projects_are_backend_only():
    scanner = load_ini(ROOT / "scanner/platformio.ini")
    uplink = load_ini(ROOT / "uplink/platformio.ini")
    assert "env:scanner-s3-combo-backend" in scanner
    assert "env:uplink-s3-backend" in uplink
    assert "FOF_BADGE_VARIANT" not in scanner + uplink
    assert "scanner-s3-combo-fof_badge" not in scanner
    assert "uplink-s3-fof_badge" not in uplink
    assert "project(fof_backend_scanner)" in (ROOT / "scanner/CMakeLists.txt").read_text()
    assert "project(fof_backend_uplink)" in (ROOT / "uplink/CMakeLists.txt").read_text()
```

Run: `cd backend-firmware && python -m pytest tools/tests/test_backend_build_contract.py -q`

Expected: FAIL because the projects are absent.

- [ ] **Step 2: Create exact partition tables and rollback sdkconfig**

Scanner table:

```csv
nvs,data,nvs,0x9000,0x6000,
otadata,data,ota,0xf000,0x2000,
phy_init,data,phy,0x11000,0x1000,
ota_0,app,ota_0,0x20000,0x200000,
ota_1,app,ota_1,0x220000,0x200000,
storage,data,spiffs,0x420000,0x100000,
reserved,data,fat,0x520000,0x2e0000,
```

Uplink table:

```csv
nvs,data,nvs,0x9000,0x6000,
otadata,data,ota,0xf000,0x2000,
phy_init,data,phy,0x11000,0x1000,
ota_0,app,ota_0,0x20000,0x200000,
ota_1,app,ota_1,0x220000,0x200000,
fw_scanner_be,data,0x40,0x420000,0x200000,
storage,data,spiffs,0x620000,0x100000,
reserved,data,fat,0x720000,0xe0000,
```

Both sdkconfigs select 8-MB flash, octal PSRAM, USB serial-JTAG console,
custom partition file, 1000-Hz FreeRTOS, 30-second watchdog, and
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Scanner enables NimBLE/Wi-Fi/coexist;
uplink sets `CONFIG_BT_ENABLED=n` and enables STA/AP HTTP server.

Scanner defaults explicitly include
`CONFIG_BT_ENABLED=y`, `CONFIG_BT_NIMBLE_ENABLED=y`,
`CONFIG_BT_NIMBLE_ROLE_OBSERVER=y`, `CONFIG_BT_NIMBLE_ROLE_CENTRAL=y`,
`CONFIG_BT_NIMBLE_EXT_ADV=y`, Wi-Fi promiscuous support, coexistence, and
octal PSRAM. `main/Kconfig.projbuild` defines only
`CONFIG_FOF_BACKEND_GLASSES_DETECTION` default `y`; the adapted classifier/
settings use that symbol and never the badge symbol. Uplink defaults explicitly
disable both Bluedroid and NimBLE. Build-contract tests parse sdkconfig and
fail if scanner extended advertisements/central role are absent or if uplink
Bluetooth is enabled.

- [ ] **Step 3: Create exact PlatformIO environments**

Both use `platform = espressif32@6.13.0`, `board = seeed_xiao_esp32s3`,
`framework = espidf`, `monitor_speed = 921600`, flash size 8 MB, upload offset
`0x20000`, `board_build.flash_mode = dio`, and
`board_build.f_flash = 80000000L`, with only local pre/post scripts. Scanner defines
`FOF_BACKEND_SCANNER=1`; uplink defines `FOF_BACKEND_UPLINK=1`; both define
`FOF_BACKEND_FIRMWARE=1` and `BOARD_HAS_PSRAM`.

The build-contract test parses both environments and requires DIO, 80 MHz,
8 MB, and app offset `0x20000`; it also parses generated `flasher_args.json`
after each build and requires the same `dio`/`80m`/`8MB` values consumed by
the release verifier.

- [ ] **Step 4: Compose scanner main without presentation code**

Scanner boot order is NVS/rollback, GPIO21 LED, UART command ingress, identity
status, quiescent role runtime, then radios only after a current-boot role.
It runs BLE/Wi-Fi detectors according to profile, emits complete detections and
health, services BLE investigations, honors flow/LED/time, and retains OTA
ingress. No LCD/OLED, button, theme, Easter egg, game, advertiser, or WS2812
source appears in the explicit `SRCS` list.

On the initial direct-USB image, require
`esp_ota_get_state_partition(running) == ESP_OTA_IMG_VALID`; the pinned
bootloader converts erased initial otadata to a valid ota_0 record, so firmware
must never claim PENDING_VERIFY for this path. Immediately after immutable
identity, NVS, GPIO21, and UART command ingress are ready—but before a
role-dependent health decision—it prints this provisional
USB line with runtime MAC and nonzero boot ID substituted:

```text
FOF_BACKEND_BOOT {"target":"scanner-s3-combo-backend","project":"fof_backend_scanner","hardware":"seeed_xiao_esp32s3","version":"0.1.0-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"nvs_erased":false,"uart_ingress":true,"ota_state":"valid"}
```

This provisional record does not claim radio or rollback health. After a
current-boot role is acknowledged, its required radio is healthy, command
ingress remains bound to the same boot ID, watchdog readiness passes, and any
future OTA pending image is marked valid, print:

```text
FOF_BACKEND_HEALTH {"target":"scanner-s3-combo-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"nvs_erased":false,"role":"ble_primary","command_ingress_boot_id":305419896,"radio_healthy":true,"rollback_clear":true}
```

The role field is the actual assigned `ble_primary`, `wifi_primary`, or
`hybrid_failover`; the uplink's scanner-status snapshot carries the same MAC,
boot ID, role, command/radio health, and rollback state.

Every board's USB console accepts the exact read-only newline command
`FOF_BACKEND_STATUS`. It re-emits the current immutable `FOF_BACKEND_BOOT`
record and, if available, the latest `FOF_BACKEND_HEALTH` record without
changing NVS, roles, radios, OTA, or counters. Unknown commands fail closed.
Host/parser tests prove repeated status requests are byte-equivalent within a
boot and contain the live MAC/boot ID; canary verification never depends on
catching a one-shot startup print.

- [ ] **Step 5: Compose uplink main without display or uplink BLE**

Uplink boot order is NVS/rollback, GPIO21 LED, dual UART, coordinator/config,
Wi-Fi/AP, time, uploader, commands, and OTA workers. Every valid detection goes
to router/upload and a separate threat copy. It sends role/time/LED cadence,
handles failover, polls commands, and never starts a BLE controller, GPS,
battery, LCD/OLED, badge runtime, game, or display task.

For every decoded UART detection, the coordinator passes the decoder's exact
`backend_scanner_stamp_t` plus the current validated uplink epoch to
`backend_observation_resolve`, then passes the resulting observation and a
separate monotonic clock to the router. This is the only path into the batch
builder and is covered by the Task-10 scanner-time/fallback tests.

The initial uplink boot requires the same actual `ESP_OTA_IMG_VALID` state.
After NVS, both UART workers, coordinator, and either AP or STA are ready, it
prints this provisional USB line with
runtime MAC, boot ID, and actual `network_state` substituted:

```text
FOF_BACKEND_BOOT {"target":"uplink-s3-backend","project":"fof_backend_uplink","hardware":"seeed_xiao_esp32s3","version":"0.1.0-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"device_id":"uplink_CB77A4","config_state":"loaded","config_generation":9,"nvs_erased":false,"auto_update_enabled":false,"uart0_started":true,"uart1_started":true,"network_state":"ap","ota_state":"valid"}
```

After uplink rollback clearance it prints:

```text
FOF_BACKEND_HEALTH {"target":"uplink-s3-backend","mac":"AA:BB:CC:DD:EE:FF","boot_id":305419896,"device_id":"uplink_CB77A4","config_state":"loaded","config_generation":9,"nvs_loaded":true,"nvs_erased":false,"auto_update_enabled":false,"uart0_started":true,"uart1_started":true,"coordinator_started":true,"network_state":"ap","rollback_clear":true}
```

- [ ] **Step 6: Use explicit local CMake source lists and declared components**

Top-level CMake parses `FOF_VERSION_BACKEND` and calls exactly
`project(fof_backend_scanner)` or `project(fof_backend_uplink)`. Component
CMake enumerates every `main` source and every required shared source by
filename; the only sibling paths allowed are explicit
`../../shared/<backend-owned-file>.c` and `../../shared` include paths, which
canonicalize inside `backend-firmware`. It does not use `SRC_DIRS`,
wildcards, `EXTRA_COMPONENT_DIRS`, any `../../../` donor path, or a source
under `vendor/`. Scanner declares only the ESP-IDF components it consumes
(`bt`, `esp_wifi`, `nvs_flash`, `esp_timer`, `app_update`, `mbedtls`,
`esp_psram`, `driver`); uplink declares `esp_wifi`, `esp_http_server`,
`esp_netif`, `esp_event`, `nvs_flash`, `esp_timer`, `app_update`,
`mbedtls`, `lwip`, `esp_psram`, and `driver`. The local JSON reader/writer
means neither device declares an ESP-IDF cJSON/json component.

- [ ] **Step 7: Run native suite, clean builds, and isolation audit**

Run:

```bash
cd backend-firmware
pio test -e backend-native
python -m pytest tools/tests -q
python tools/check_source_isolation.py --root .

cd scanner
pio run -e scanner-s3-combo-backend -t clean
pio run -e scanner-s3-combo-backend

cd ../uplink
pio run -e uplink-s3-backend -t clean
pio run -e uplink-s3-backend
```

Expected: all tests and both builds pass; each `firmware.bin` is no larger than
`0x200000`; compile databases contain only paths under `backend-firmware`
plus PlatformIO/ESP-IDF toolchain paths.

- [ ] **Step 8: Inspect binary identities and forbidden symbols**

Run the local identity verifier from plan 3 or, before that tool exists, use
`strings` and `size`:

```bash
cd ..
strings scanner/.pio/build/scanner-s3-combo-backend/firmware.bin | rg 'scanner-s3-combo-backend|fof_backend_scanner|seeed_xiao_esp32s3|0.1.0-backend'
strings uplink/.pio/build/uplink-s3-backend/firmware.bin | rg 'uplink-s3-backend|fof_backend_uplink|seeed_xiao_esp32s3|0.1.0-backend'
strings scanner/.pio/build/scanner-s3-combo-backend/firmware.bin uplink/.pio/build/uplink-s3-backend/firmware.bin | rg 'fof_badge|badge_con|display_st7735|oled_display|badge_easter|CON CRUD'
```

Expected: all four identity markers appear in its corresponding image; the final
forbidden-symbol command exits 1 with no matches.

- [ ] **Step 9: Commit the buildable applications**

```bash
git add backend-firmware
git commit -m "backend-fw: build backend uplink and scanner images"
```

## Plan 2 Completion Gate

Run the Task 13 native tests, both clean builds, binary identity checks, and
source isolation audit from a clean worktree. The output is software-complete
but not authorized for hardware flashing; plan 3 must package and verify it,
then the operator must explicitly approve the direct-USB canary.
